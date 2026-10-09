// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/SelectorRewriter.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/Units.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;

namespace {

    using Kind = SimpleSelector::Kind;

    constexpr size_t kExpansionLimit = 256;
    constexpr const char* kRootId = "htmlui-root";

    bool isWs(const ComponentValue& v){
        return v.isWhitespace() || v.isToken(TokenType::Comment);
    }

    bool isSelectorListPseudo(const std::string& name){
        return name == "is" || name == "where" || name == "matches" || name == "-webkit-any" || name == "-moz-any" ||
               name == "not" || name == "has" || name == "host" || name == "host-context";
    }

    // ---- parsing

    class Parser {
    public:
        Parser(const std::vector<ComponentValue>& values, uint32_t file): file(file) {
            for (const ComponentValue& v : values){
                if (!v.isToken(TokenType::Comment))
                    items.push_back(&v);
            }
        }

        std::optional<std::vector<ComplexSelector>> list(bool forgiving){
            std::vector<ComplexSelector> result;
            std::vector<std::vector<const ComponentValue*>> groups(1);
            for (const ComponentValue* v : items){
                if (v->isToken(TokenType::Comma)){
                    groups.emplace_back();
                    continue;
                }
                groups.back().push_back(v);
            }
            for (const std::vector<const ComponentValue*>& group : groups){
                std::optional<ComplexSelector> selector = complex(group);
                if (!selector){
                    if (forgiving){
                        error.clear();
                        continue;
                    }
                    return std::nullopt;
                }
                result.push_back(std::move(*selector));
            }
            if (result.empty() && !forgiving){
                fail(nullptr, "empty selector");
                return std::nullopt;
            }
            return result;
        }

        std::string error;
        SourceRef errorRef;

    private:
        SourceRef refOf(const ComponentValue* v) const {
            return v ? SourceRef{file, v->offset, v->length} : SourceRef{};
        }

        void fail(const ComponentValue* at, const std::string& message){
            if (error.empty()){
                error = message;
                errorRef = refOf(at);
            }
        }

        std::optional<ComplexSelector> complex(const std::vector<const ComponentValue*>& tokens){
            ComplexSelector selector;
            size_t i = 0;
            // trim
            size_t end = tokens.size();
            while (i < end && tokens[i]->isWhitespace())
                i++;
            while (end > i && tokens[end - 1]->isWhitespace())
                end--;
            if (i >= end){
                fail(nullptr, "empty selector");
                return std::nullopt;
            }
            selector.ref = SourceRef{file, tokens[i]->offset, tokens[end - 1]->offset + tokens[end - 1]->length - tokens[i]->offset};
            char combinator = 0;
            bool first = true;
            while (i < end){
                bool sawWhitespace = false;
                while (i < end && tokens[i]->isWhitespace()){
                    sawWhitespace = true;
                    i++;
                }
                if (i >= end)
                    break;
                const ComponentValue* t = tokens[i];
                char explicitCombinator = 0;
                if (t->isToken(TokenType::Delim)){
                    const std::string& d = t->token.value;
                    if (d == ">" || d == "+" || d == "~")
                        explicitCombinator = d[0];
                    else if (d == "|" && i + 1 < end && tokens[i + 1]->isToken(TokenType::Delim) && tokens[i + 1]->token.value == "|"){
                        explicitCombinator = '|';
                        i++;
                    }
                }
                if (explicitCombinator){
                    if (combinator && combinator != ' '){
                        fail(t, "two combinators in a row");
                        return std::nullopt;
                    }
                    combinator = explicitCombinator;
                    i++;
                    if (first)
                        selector.relative = true;
                    continue;
                }
                if (sawWhitespace && !first && !combinator)
                    combinator = ' ';
                Compound compound;
                if (!parseCompound(tokens, i, end, compound))
                    return std::nullopt;
                compound.combinator = first ? (selector.relative ? combinator : 0) : (combinator ? combinator : ' ');
                if (first && selector.relative && !combinator)
                    compound.combinator = 0;
                selector.compounds.push_back(std::move(compound));
                combinator = 0;
                first = false;
            }
            if (selector.compounds.empty() || (combinator && combinator != ' ')){
                fail(tokens[end - 1], "selector ends with a combinator");
                return std::nullopt;
            }
            return selector;
        }

        bool parseCompound(const std::vector<const ComponentValue*>& tokens, size_t& i, size_t end, Compound& compound){
            bool any = false;
            while (i < end){
                const ComponentValue* t = tokens[i];
                if (t->isWhitespace())
                    break;
                if (t->isToken(TokenType::Delim) && (t->token.value == ">" || t->token.value == "+" || t->token.value == "~"))
                    break;
                if (t->isToken(TokenType::Delim) && t->token.value == "|" && i + 1 < end && tokens[i + 1]->isToken(TokenType::Delim) &&
                    tokens[i + 1]->token.value == "|")
                    break;
                SimpleSelector simple;
                simple.ref = refOf(t);
                // type or universal, with an optional namespace prefix
                if ((t->isToken(TokenType::Ident) || (t->isToken(TokenType::Delim) && (t->token.value == "*" || t->token.value == "|"))) &&
                    !any){
                    bool hasNs = false;
                    const ComponentValue* name = t;
                    if (t->isToken(TokenType::Delim) && t->token.value == "|"){
                        hasNs = true;
                        if (i + 1 >= end){
                            fail(t, "namespace without a name");
                            return false;
                        }
                        name = tokens[++i];
                    }else if (i + 2 < end && tokens[i + 1]->isToken(TokenType::Delim) && tokens[i + 1]->token.value == "|" &&
                              !(tokens[i + 2]->isToken(TokenType::Delim) && tokens[i + 2]->token.value == "|")){
                        hasNs = true;
                        i += 2;
                        name = tokens[i];
                    }
                    if (name->isToken(TokenType::Ident)){
                        simple.kind = Kind::Type;
                        simple.name = lowerAscii(name->token.value);
                    }else if (name->isToken(TokenType::Delim) && name->token.value == "*"){
                        simple.kind = Kind::Universal;
                    }else{
                        fail(name, "invalid type selector");
                        return false;
                    }
                    simple.hasNamespace = hasNs;
                    i++;
                    compound.simples.push_back(std::move(simple));
                    any = true;
                    continue;
                }
                if (t->isToken(TokenType::Hash)){
                    if (!t->token.hashIsId){
                        fail(t, "invalid id selector '#" + t->token.value + "'");
                        return false;
                    }
                    simple.kind = Kind::Id;
                    simple.name = t->token.value;
                    i++;
                }else if (t->isToken(TokenType::Delim) && t->token.value == "."){
                    if (i + 1 >= end || !tokens[i + 1]->isToken(TokenType::Ident) || tokens[i + 1]->offset != t->offset + 1){
                        fail(t, "'.' without a class name");
                        return false;
                    }
                    simple.kind = Kind::Class;
                    simple.name = tokens[i + 1]->token.value;
                    simple.ref = refOf(t).cover(refOf(tokens[i + 1]));
                    i += 2;
                }else if (t->isToken(TokenType::Delim) && t->token.value == "&"){
                    simple.kind = Kind::Nesting;
                    i++;
                }else if (t->isBlock(TokenType::LeftBracket)){
                    if (!attribute(*t, simple))
                        return false;
                    i++;
                }else if (t->isToken(TokenType::Colon)){
                    bool element = false;
                    size_t j = i + 1;
                    if (j < end && tokens[j]->isToken(TokenType::Colon)){
                        element = true;
                        j++;
                    }
                    if (j >= end){
                        fail(t, "':' without a pseudo-class name");
                        return false;
                    }
                    const ComponentValue* n = tokens[j];
                    if (n->isToken(TokenType::Ident)){
                        simple.name = lowerAscii(n->token.value);
                    }else if (n->kind == ComponentValue::Kind::Function){
                        simple.name = lowerAscii(n->name());
                        simple.function = true;
                        simple.arguments = n->children;
                        if (isSelectorListPseudo(simple.name)){
                            Parser inner(n->children, file);
                            const bool forgiving = simple.name == "is" || simple.name == "where";
                            std::optional<std::vector<ComplexSelector>> parsed = inner.list(forgiving);
                            if (!parsed){
                                fail(n, "invalid selector in :" + simple.name + "()");
                                return false;
                            }
                            simple.selectors = std::move(*parsed);
                        }
                    }else{
                        fail(n, "invalid pseudo-class");
                        return false;
                    }
                    simple.kind = element ? Kind::PseudoElement : Kind::PseudoClass;
                    if (!element && (simple.name == "before" || simple.name == "after" || simple.name == "first-line" ||
                                     simple.name == "first-letter")){
                        simple.kind = Kind::PseudoElement;
                        simple.legacyColon = true;
                    }
                    simple.ref = refOf(t).cover(refOf(n));
                    i = j + 1;
                }else{
                    fail(t, "unexpected '" + serialize(*t) + "' in a selector");
                    return false;
                }
                compound.simples.push_back(std::move(simple));
                any = true;
            }
            if (!any){
                fail(i < end ? tokens[i] : nullptr, "empty compound selector");
                return false;
            }
            return true;
        }

        bool attribute(const ComponentValue& block, SimpleSelector& simple){
            simple.kind = Kind::Attribute;
            std::vector<const ComponentValue*> t;
            for (const ComponentValue& v : block.children){
                if (!isWs(v))
                    t.push_back(&v);
            }
            size_t i = 0;
            // [ns|name], [*|name], [|name]
            if (i + 2 < t.size() + 0 && t.size() >= 3 && t[1]->isToken(TokenType::Delim) && t[1]->token.value == "|" &&
                !(t.size() > 2 && t[2]->isToken(TokenType::Delim) && t[2]->token.value == "=")){
                simple.hasNamespace = true;
                i = 2;
            }else if (!t.empty() && t[0]->isToken(TokenType::Delim) && t[0]->token.value == "|"){
                simple.hasNamespace = true;
                i = 1;
            }
            if (i >= t.size() || !t[i]->isToken(TokenType::Ident)){
                fail(&block, "invalid attribute selector");
                return false;
            }
            simple.name = t[i]->token.value;
            i++;
            if (i == t.size())
                return true;
            // operator
            if (t[i]->isToken(TokenType::Delim)){
                const std::string c = t[i]->token.value;
                if (c == "="){
                    simple.op = "=";
                    i++;
                }else if ((c == "~" || c == "|" || c == "^" || c == "$" || c == "*") && i + 1 < t.size() &&
                          t[i + 1]->isToken(TokenType::Delim) && t[i + 1]->token.value == "="){
                    simple.op = c + "=";
                    i += 2;
                }else{
                    fail(&block, "invalid attribute selector operator");
                    return false;
                }
            }else{
                fail(&block, "invalid attribute selector");
                return false;
            }
            if (i >= t.size() || (!t[i]->isToken(TokenType::Ident) && !t[i]->isToken(TokenType::String))){
                fail(&block, "attribute selector without a value");
                return false;
            }
            simple.value = t[i]->token.value;
            i++;
            if (i < t.size()){
                if (!t[i]->isToken(TokenType::Ident) || (!equalsNoCase(t[i]->token.value, "i") && !equalsNoCase(t[i]->token.value, "s"))){
                    fail(&block, "invalid attribute selector flag");
                    return false;
                }
                simple.flag = (char)(t[i]->token.value[0] | 0x20);
                i++;
            }
            if (i != t.size()){
                fail(&block, "invalid attribute selector");
                return false;
            }
            return true;
        }

        std::vector<const ComponentValue*> items;
        uint32_t file;
    };

    // ---- serialization

    // An+B as RmlUi reads it ("2n+1"): the signs of the B part kept
    std::string anbText(const std::vector<ComponentValue>& arguments){
        std::string text;
        for (const ComponentValue& v : arguments){
            if (v.isWhitespace() || v.isToken(TokenType::Comment))
                continue;
            if (v.isToken(TokenType::Number)){
                if (!text.empty() && v.token.number >= 0 && text.back() != '+' && text.back() != '-')
                    text += '+';
                text += formatNumber(v.token.number);
            }else if (v.isToken(TokenType::Dimension)){
                text += formatNumber(v.token.number) + lowerAscii(v.token.unit);
            }else{
                text += lowerAscii(serialize(v));
            }
        }
        return text;
    }

    bool hasOf(const std::vector<ComponentValue>& arguments){
        for (const ComponentValue& v : arguments){
            if (v.isToken(TokenType::Ident) && equalsNoCase(v.token.value, "of"))
                return true;
        }
        return false;
    }

    // ---- rewriting

    struct Rewriter {
        Rewriter(const SelectorOptions& options, DiagnosticSink* sink, SourceRef ruleRef): options(options), sink(sink), ruleRef(ruleRef) {}

        const SelectorOptions& options;
        DiagnosticSink* sink;
        SourceRef ruleRef;
        bool invalidRule = false;
        bool limitReached = false;

        bool author() const { return options.origin == SelectorOptions::Origin::Author; }

        void report(DiagCode code, const SimpleSelector* at, const std::string& message, const std::string& suggestion,
                    const std::string& subject, Severity severity = Severity::Warning){
            if (!sink)
                return;
            const SourceRef ref = (at && at->ref.valid()) ? at->ref : ruleRef;
            sink->reportAs(severity, code, ref, message, suggestion, std::string(), subject);
        }

        // :is()/:where() expanded into the cross product (until RootIsWhere)
        std::vector<ComplexSelector> expand(const ComplexSelector& selector){
            std::vector<ComplexSelector> work{selector};
            std::vector<ComplexSelector> done;
            while (!work.empty()){
                ComplexSelector current = std::move(work.back());
                work.pop_back();
                bool expanded = false;
                for (size_t c = 0; c < current.compounds.size() && !expanded; c++){
                    Compound& compound = current.compounds[c];
                    for (size_t s = 0; s < compound.simples.size(); s++){
                        const SimpleSelector& simple = compound.simples[s];
                        if (simple.kind != Kind::PseudoClass)
                            continue;
                        const std::string& n = simple.name;
                        if (n != "is" && n != "where" && n != "matches" && n != "-webkit-any" && n != "-moz-any")
                            continue;
                        if (options.fork.has(ForkFeature::RootIsWhere))
                            continue;
                        if (n == "where")
                            report(codes::IsWhereExpanded, &simple, "':where()' is expanded into separate selectors; it keeps the specificity of its arguments instead of zero",
                                   "", ":where()", Severity::Info);
                        else
                            report(codes::IsWhereExpanded, &simple, "':" + n + "()' is expanded into separate selectors", "", ":is()",
                                   Severity::Info);
                        SimpleSelector removed = simple;
                        compound.simples.erase(compound.simples.begin() + (std::ptrdiff_t)s);
                        for (const ComplexSelector& argument : removed.selectors){
                            work.push_back(substitute(current, c, argument));
                            if (work.size() + done.size() > kExpansionLimit){
                                limitReached = true;
                                return {};
                            }
                        }
                        expanded = true;
                        break;
                    }
                }
                if (!expanded)
                    done.push_back(std::move(current));
                if (done.size() > kExpansionLimit){
                    limitReached = true;
                    return {};
                }
            }
            std::reverse(done.begin(), done.end());
            return done;
        }

        // current with `argument` merged into compound c (argument's subject compound merged,
        // its earlier compounds placed before c)
        static ComplexSelector substitute(const ComplexSelector& current, size_t c, const ComplexSelector& argument){
            ComplexSelector result;
            result.ref = current.ref;
            for (size_t i = 0; i < c; i++)
                result.compounds.push_back(current.compounds[i]);
            const size_t n = argument.compounds.size();
            for (size_t i = 0; i + 1 < n; i++){
                Compound copy = argument.compounds[i];
                if (i == 0)
                    copy.combinator = c == 0 ? 0 : current.compounds[c].combinator;
                result.compounds.push_back(std::move(copy));
            }
            Compound merged = current.compounds[c];
            const Compound& last = argument.compounds[n - 1];
            if (n > 1)
                merged.combinator = last.combinator;
            // type selectors first
            std::vector<SimpleSelector> simples;
            for (const SimpleSelector& s : last.simples){
                if (s.kind == Kind::Type || s.kind == Kind::Universal)
                    simples.push_back(s);
            }
            for (const SimpleSelector& s : merged.simples){
                if (s.kind == Kind::Type || s.kind == Kind::Universal)
                    simples.push_back(s);
            }
            for (const SimpleSelector& s : last.simples){
                if (s.kind != Kind::Type && s.kind != Kind::Universal)
                    simples.push_back(s);
            }
            for (const SimpleSelector& s : merged.simples){
                if (s.kind != Kind::Type && s.kind != Kind::Universal)
                    simples.push_back(s);
            }
            merged.simples = std::move(simples);
            result.compounds.push_back(std::move(merged));
            for (size_t i = c + 1; i < current.compounds.size(); i++)
                result.compounds.push_back(current.compounds[i]);
            return result;
        }

        static SimpleSelector makeType(const std::string& name){
            SimpleSelector s;
            s.kind = Kind::Type;
            s.name = name;
            return s;
        }
        static SimpleSelector makePseudo(const std::string& name){
            SimpleSelector s;
            s.kind = Kind::PseudoClass;
            s.name = name;
            return s;
        }
        static SimpleSelector makeAttribute(const std::string& name, const std::string& op = std::string(), const std::string& value = std::string()){
            SimpleSelector s;
            s.kind = Kind::Attribute;
            s.name = name;
            s.op = op;
            s.value = value;
            return s;
        }
        static SimpleSelector makeNot(const SimpleSelector& inner){
            SimpleSelector s;
            s.kind = Kind::PseudoClass;
            s.name = "not";
            s.function = true;
            ComplexSelector argument;
            Compound compound;
            compound.simples.push_back(inner);
            argument.compounds.push_back(std::move(compound));
            s.selectors.push_back(std::move(argument));
            return s;
        }

        static bool isVendor(const std::string& name){
            return name.size() > 1 && name[0] == '-';
        }

        // Rewrites the simples of one selector in place. Returns false to drop the selector;
        // sets invalidRule for a selector browsers reject (the whole rule goes).
        bool mapSelector(ComplexSelector& selector, bool& placeholder, bool& pseudoElement, int depth){
            std::vector<Compound> out;
            for (size_t c = 0; c < selector.compounds.size(); c++){
                Compound compound = selector.compounds[c];
                if (compound.combinator == '|'){
                    report(codes::UnsupportedSelector, compound.simples.empty() ? nullptr : &compound.simples[0],
                           "the column combinator '||' is not supported; selector dropped", "select the cells by class", "||");
                    return false;
                }
                std::vector<SimpleSelector> simples;
                std::vector<Compound> appended;     // compounds a pseudo-element adds after this one
                bool removeSelf = false;            // a pseudo-element that replaces the compound
                for (const SimpleSelector& simple : compound.simples){
                    if (simple.hasNamespace){
                        report(codes::UnsupportedSelector, &simple, "namespaced selector '" + SelectorRewriter::serialize(simple) + "' is not supported; selector dropped",
                               "remove the namespace prefix", "namespace");
                        return false;
                    }
                    // simples after a pseudo-element belong to the element it generates
                    std::vector<SimpleSelector>& target = appended.empty() ? simples : appended.back().simples;
                    switch (simple.kind){
                        case Kind::Type:
                            if (simple.name == "html")
                                target.push_back(rootMarker());
                            else if (simple.name == "body")
                                target.push_back(makeType("htmlbody"));
                            else if (simple.name == "template")
                                target.push_back(makeType("htmltemplate"));
                            else
                                target.push_back(simple);
                            break;
                        case Kind::Universal:
                        case Kind::Class:
                        case Kind::RootMarker:
                            target.push_back(simple);
                            break;
                        case Kind::Nesting:
                            // a top-level '&' is :scope, the root
                            target.push_back(rootMarker());
                            break;
                        case Kind::Id:
                            if (author() && simple.name == kRootId){
                                report(codes::UnsupportedSelector, &simple, "'#htmlui-root' is a reserved id; selector dropped",
                                       "use another id", "#id");
                                return false;
                            }
                            if (!options.rootId.empty() && simple.name == options.rootId)
                                target.push_back(rootMarker());
                            else
                                target.push_back(simple);
                            break;
                        case Kind::Attribute: {
                            SimpleSelector copy = simple;
                            if (copy.flag){
                                report(codes::SelectorRewritten, &simple,
                                       std::string("attribute selector flag '") + copy.flag + "' is removed" + (copy.flag == 'i' ? "; matching is case-sensitive" : ""),
                                       "", copy.flag == 'i' ? "[attr i]" : "[attr s]", Severity::Info);
                                copy.flag = 0;
                            }
                            target.push_back(std::move(copy));
                            break;
                        }
                        case Kind::PseudoClass:
                            if (!pseudoClass(simple, target, depth))
                                return false;
                            break;
                        case Kind::PseudoElement: {
                            if (!pseudoElementOf(simple, simples, appended, removeSelf, placeholder, pseudoElement))
                                return false;
                            break;
                        }
                    }
                }
                compound.simples = std::move(simples);
                if (removeSelf){
                    // ::-webkit-scrollbar alone: the generated element replaces the empty compound
                    if (!appended.empty()){
                        appended.front().combinator = compound.combinator;
                        if (out.empty())
                            appended.front().combinator = 0;
                    }
                }else{
                    if (compound.simples.empty()){
                        SimpleSelector universal;
                        universal.kind = Kind::Universal;
                        compound.simples.push_back(universal);
                    }
                    out.push_back(std::move(compound));
                }
                for (Compound& extra : appended){
                    if (out.empty())
                        extra.combinator = 0;
                    out.push_back(std::move(extra));
                }
            }
            selector.compounds = std::move(out);
            return !selector.compounds.empty();
        }

        static SimpleSelector rootMarker(){
            SimpleSelector s;
            s.kind = Kind::RootMarker;
            return s;
        }

        bool pseudoClass(const SimpleSelector& simple, std::vector<SimpleSelector>& target, int depth){
            const std::string& n = simple.name;
            const std::string display = ":" + n + (simple.function ? "()" : "");
            if (!simple.function){
                if (n == "hover" || n == "active" || n == "checked" || n == "disabled" || n == "placeholder-shown" || n == "scope" ||
                    n == "first-child" || n == "last-child" || n == "only-child" || n == "first-of-type" || n == "last-of-type" ||
                    n == "only-of-type" || n == "empty" || n == "selected" || n == "drag" ||
                    n == "html-focus" || n == "html-focus-visible"){
                    target.push_back(simple);
                    return true;
                }
                if (n == "focus"){
                    target.push_back(makePseudo("html-focus"));
                    return true;
                }
                if (n == "focus-visible"){
                    target.push_back(makePseudo("html-focus-visible"));
                    return true;
                }
                if (n == "focus-within"){
                    target.push_back(makePseudo("focus"));
                    return true;
                }
                if (n == "enabled"){
                    target.push_back(makeNot(makePseudo("disabled")));
                    return true;
                }
                if (n == "link" || n == "any-link" || n == "-webkit-any-link"){
                    target.push_back(makeAttribute("href"));
                    return true;
                }
                if (n == "visited"){
                    report(codes::VisitedDropped, &simple, "':visited' never matches (no browsing history); selector dropped",
                           "style links with :link or [href]", ":visited", Severity::Info);
                    return false;
                }
                if (n == "required"){
                    target.push_back(makeAttribute("required"));
                    return true;
                }
                if (n == "optional"){
                    target.push_back(makeNot(makeAttribute("required")));
                    return true;
                }
                if (n == "read-only" || n == "-moz-read-only"){
                    target.push_back(makeAttribute("readonly"));
                    return true;
                }
                if (n == "read-write" || n == "-moz-read-write"){
                    target.push_back(makeNot(makeAttribute("readonly")));
                    return true;
                }
                if (n == "root"){
                    target.push_back(rootMarker());
                    return true;
                }
                if (n == "defined"){
                    report(codes::SelectorRewritten, &simple, "':defined' is always true here and is removed", "", ":defined", Severity::Info);
                    return true;
                }
                if (n == "before" || n == "after")
                    return true;   // handled as pseudo-elements
            }else{
                if (n == "nth-child" || n == "nth-last-child" || n == "nth-of-type" || n == "nth-last-of-type"){
                    if (hasOf(simple.arguments)){
                        report(codes::UnsupportedSelector, &simple, "':" + n + "(An+B of S)' is not supported; selector dropped",
                               "add a class to the elements to count", ":nth-child(of S)");
                        return false;
                    }
                    target.push_back(simple);
                    return true;
                }
                if (n == "not"){
                    SimpleSelector copy = simple;
                    copy.selectors.clear();
                    for (const ComplexSelector& argument : simple.selectors){
                        std::vector<ComplexSelector> expanded = expand(argument);
                        for (ComplexSelector& variant : expanded){
                            bool placeholder = false, pseudo = false;
                            if (!mapSelector(variant, placeholder, pseudo, depth + 1) || placeholder || pseudo){
                                report(codes::UnsupportedSelector, &simple, "':not()' with an argument the engine cannot match; selector dropped",
                                       "simplify the :not() argument", ":not()");
                                return false;
                            }
                            if (!resolveRoot(variant))
                                continue;
                            copy.selectors.push_back(std::move(variant));
                        }
                    }
                    if (copy.selectors.empty())
                        return true;   // :not() of nothing matches everything
                    target.push_back(std::move(copy));
                    return true;
                }
                if ((n == "is" || n == "where" || n == "matches" || n == "-webkit-any" || n == "-moz-any") && options.fork.has(ForkFeature::RootIsWhere)){
                    SimpleSelector copy = simple;
                    if (n != "where")
                        copy.name = "is";
                    copy.selectors.clear();
                    for (const ComplexSelector& argument : simple.selectors){
                        ComplexSelector variant = argument;
                        bool placeholder = false, pseudo = false;
                        if (!mapSelector(variant, placeholder, pseudo, depth + 1))
                            continue;
                        if (!resolveRoot(variant))
                            continue;
                        copy.selectors.push_back(std::move(variant));
                    }
                    if (copy.selectors.empty())
                        return false;
                    target.push_back(std::move(copy));
                    return true;
                }
                if (n == "lang"){
                    const std::string language = serialize(trimWhitespace(simple.arguments));
                    std::string value = language;
                    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                        value = value.substr(1, value.size() - 2);
                    report(codes::SelectorRewritten, &simple, "':lang(" + value + ")' matches the lang attribute of the element itself (no inheritance)",
                           "", ":lang()", Severity::Info);
                    target.push_back(makeAttribute("lang", "|=", value));
                    return true;
                }
                if (n == "dir"){
                    report(codes::SelectorIgnored, &simple, "':dir()' is not supported; selector dropped", "select on the dir attribute: [dir=rtl]",
                           ":dir()", Severity::Info);
                    return false;
                }
            }
            if (n == "has"){
                report(codes::UnsupportedSelector, &simple, "':has()' is not supported; selector dropped", "set a class from Lua", ":has()");
                return false;
            }
            if (const SelectorSupport* row = findPseudoClass(display)){
                if (row->support == Support::Unsupported){
                    report(codes::UnsupportedSelector, &simple, "'" + display + "' is not supported; selector dropped", row->suggestion, row->name);
                    return false;
                }
            }
            if (isVendor(n)){
                report(codes::SelectorIgnored, &simple, "unknown vendor pseudo-class '" + display + "'; the rule is dropped, as Chromium drops it",
                       "", display, Severity::Info);
                invalidRule = true;
                return false;
            }
            report(codes::InvalidSelector, &simple, "unknown pseudo-class '" + display + "'; the rule is dropped, as browsers drop it",
                   "", display);
            invalidRule = true;
            return false;
        }

        static Compound elementCompound(const std::string& tag, char combinator){
            Compound compound;
            compound.combinator = combinator;
            compound.simples.push_back(makeType(tag));
            return compound;
        }

        // the pseudo-element mappings of Appendix B
        bool pseudoElementOf(const SimpleSelector& simple, std::vector<SimpleSelector>& simples, std::vector<Compound>& appended,
                             bool& removeSelf, bool& placeholder, bool& pseudoElement){
            const std::string& n = simple.name;
            const std::string display = "::" + n + (simple.function ? "()" : "");
            if (!appended.empty()){
                report(codes::InvalidSelector, &simple, "two pseudo-elements in one selector; the rule is dropped", "", display);
                invalidRule = true;
                return false;
            }
            auto ownCompoundEmpty = [&](){
                for (const SimpleSelector& s : simples){
                    if (s.kind != Kind::Universal)
                        return false;
                }
                return true;
            };
            if (n == "before" || n == "after"){
                if (options.fork.has(ForkFeature::PseudoElements)){
                    SimpleSelector copy = simple;
                    copy.legacyColon = false;
                    simples.push_back(copy);
                    pseudoElement = true;
                    return true;
                }
                report(codes::PseudoElementPending, &simple, "'" + std::string(simple.legacyColon ? ":" : "::") + n +
                       "' needs the fork's generated content; selector dropped", "add a real element (a span) for the content", "::" + n);
                return false;
            }
            if (n == "placeholder" || n == "-webkit-input-placeholder" || n == "-moz-placeholder" || n == "-ms-input-placeholder"){
                simples.push_back(makePseudo("placeholder-shown"));
                placeholder = true;
                return true;
            }
            if (n == "selection" || n == "-moz-selection"){
                appended.push_back(elementCompound("selection", ' '));
                return true;
            }
            if (n == "marker"){
                appended.push_back(elementCompound("marker", '>'));
                return true;
            }
            if (n == "-webkit-progress-bar")
                return true;
            struct Mapping { const char* name; const char* element; char combinator; bool scrollbar; };
            static const Mapping mappings[] = {
                {"-webkit-scrollbar", nullptr, '>', true},
                {"-webkit-scrollbar-thumb", "sliderbar", '>', true},
                {"-webkit-scrollbar-track", "slidertrack", '>', true},
                {"-webkit-scrollbar-track-piece", "slidertrack", '>', true},
                {"-webkit-scrollbar-button", nullptr, ' ', false},
                {"-webkit-scrollbar-corner", "scrollbarcorner", ' ', false},
                {"-webkit-slider-thumb", "sliderbar", ' ', false},
                {"-moz-range-thumb", "sliderbar", ' ', false},
                {"-webkit-slider-runnable-track", "slidertrack", ' ', false},
                {"-moz-range-track", "slidertrack", ' ', false},
                {"-moz-range-progress", "sliderprogress", ' ', false},
                {"-webkit-progress-value", "fill", ' ', false},
                {"-moz-progress-bar", "fill", ' ', false},
            };
            for (const Mapping& m : mappings){
                if (n != m.name)
                    continue;
                if (n == "-webkit-scrollbar-button"){
                    // two generated elements: the caller duplicates the selector per variant
                    appended.push_back(elementCompound("sliderarrowdec", ' '));
                    pendingAlternate = "sliderarrowinc";
                    removeSelf = ownCompoundEmpty();
                    return true;
                }
                if (m.scrollbar){
                    const char combinator = ownCompoundEmpty() ? ' ' : '>';
                    appended.push_back(elementCompound("scrollbarvertical", combinator));
                    pendingAlternate = "scrollbarhorizontal";
                    if (m.element){
                        Compound child = elementCompound(m.element, '>');
                        appended.push_back(std::move(child));
                    }
                    removeSelf = ownCompoundEmpty();
                    return true;
                }
                appended.push_back(elementCompound(m.element, m.combinator));
                removeSelf = ownCompoundEmpty();
                return true;
            }
            if (const SelectorSupport* row = findPseudoElement(display)){
                if (row->support == Support::Unsupported){
                    report(codes::UnsupportedSelector, &simple, "'" + display + "' is not supported; selector dropped", row->suggestion, row->name);
                    return false;
                }
            }
            if (n.compare(0, 16, "view-transition-") == 0 || n == "view-transition"){
                report(codes::UnsupportedSelector, &simple, "'" + display + "' is not supported; selector dropped",
                       "animate with transitions or @keyframes", "::view-transition");
                return false;
            }
            if (isVendor(n)){
                report(codes::SelectorIgnored, &simple, "unknown vendor pseudo-element '" + display + "'; the rule is dropped, as Chromium drops it",
                       "", display, Severity::Info);
                invalidRule = true;
                return false;
            }
            report(codes::InvalidSelector, &simple, "unknown pseudo-element '" + display + "'; the rule is dropped, as browsers drop it", "", display);
            invalidRule = true;
            return false;
        }

        // the second element of a mapping that generates two (scrollbars, scrollbar buttons)
        std::string pendingAlternate;

        // replaces root markers by body / body#htmlui-root; false when a compound cannot be the root
        bool resolveRoot(ComplexSelector& selector) const {
            for (Compound& compound : selector.compounds){
                bool root = false;
                for (const SimpleSelector& s : compound.simples){
                    if (s.kind == Kind::RootMarker)
                        root = true;
                }
                if (!root)
                    continue;
                std::vector<SimpleSelector> simples;
                simples.push_back(makeType("body"));
                if (author()){
                    SimpleSelector id;
                    id.kind = Kind::Id;
                    id.name = kRootId;
                    simples.push_back(id);
                }
                for (const SimpleSelector& s : compound.simples){
                    if (s.kind == Kind::RootMarker || s.kind == Kind::Universal)
                        continue;
                    if (s.kind == Kind::Type)
                        return false;   // another element type can never be the root
                    if (s.kind == Kind::Id && author())
                        return false;
                    simples.push_back(s);
                }
                compound.simples = std::move(simples);
            }
            return true;
        }

        bool startsAtRoot(const ComplexSelector& selector) const {
            if (selector.compounds.empty())
                return false;
            for (const SimpleSelector& s : selector.compounds.front().simples){
                if (s.kind == Kind::Id && s.name == kRootId)
                    return true;
                if (s.kind == Kind::Type && s.name == "body" && !author())
                    return true;
            }
            return false;
        }

        // could the first compound (written without a root reference) also match the root element?
        bool couldBeRoot(const Compound& compound) const {
            for (const SimpleSelector& s : compound.simples){
                switch (s.kind){
                    case Kind::Type:
                        return false;
                    case Kind::Id:
                        return false;
                    case Kind::Class:
                        if (std::find(options.rootClasses.begin(), options.rootClasses.end(), s.name) == options.rootClasses.end())
                            return false;
                        break;
                    case Kind::Attribute: {
                        bool found = false;
                        for (const std::string& a : options.rootAttributes){
                            if (equalsNoCase(a, s.name))
                                found = true;
                        }
                        if (!found)
                            return false;
                        break;
                    }
                    case Kind::PseudoClass:
                        if (s.name == "first-child" || s.name == "last-child" || s.name == "only-child" ||
                            s.name.compare(0, 4, "nth-") == 0 || s.name == "first-of-type" || s.name == "last-of-type" ||
                            s.name == "only-of-type")
                            return false;
                        break;
                    default:
                        break;
                }
            }
            return true;
        }

        std::vector<ComplexSelector> band(const ComplexSelector& selector){
            std::vector<ComplexSelector> result;
            if (!author() || startsAtRoot(selector)){
                result.push_back(selector);
                return result;
            }
            ComplexSelector banded = selector;
            Compound root;
            SimpleSelector id;
            id.kind = Kind::Id;
            id.name = kRootId;
            root.simples.push_back(id);
            if (!banded.compounds.empty() && banded.compounds.front().combinator == 0)
                banded.compounds.front().combinator = ' ';
            banded.compounds.insert(banded.compounds.begin(), root);
            result.push_back(std::move(banded));
            if (!selector.compounds.empty() && couldBeRoot(selector.compounds.front())){
                ComplexSelector rootVariant = selector;
                Compound& first = rootVariant.compounds.front();
                std::vector<SimpleSelector> simples;
                simples.push_back(makeType("body"));
                simples.push_back(id);
                for (const SimpleSelector& s : first.simples){
                    if (s.kind != Kind::Universal)
                        simples.push_back(s);
                }
                first.simples = std::move(simples);
                result.push_back(std::move(rootVariant));
            }
            return result;
        }
    };

    uint32_t specificityOf(const Compound& compound){
        uint32_t total = 0;
        for (const SimpleSelector& s : compound.simples){
            switch (s.kind){
                case Kind::Type: total += 10000; break;
                case Kind::Id: total += 1000000; break;
                case Kind::Class:
                case Kind::Attribute: total += 100000; break;
                case Kind::PseudoClass:
                    if (s.name == "not" || s.name == "is" || s.name == "where"){
                        uint32_t best = 0;
                        for (const ComplexSelector& argument : s.selectors)
                            best = std::max(best, SelectorRewriter::specificity(argument));
                        total += best;
                    }else{
                        total += 100000;
                    }
                    break;
                case Kind::PseudoElement: total += 100000; break;
                default: break;
            }
        }
        return total;
    }

}

std::optional<std::vector<ComplexSelector>> SelectorRewriter::parseList(const std::vector<ComponentValue>& prelude, uint32_t file,
                                                                        std::string* error, SourceRef* errorRef){
    Parser parser(prelude, file);
    std::optional<std::vector<ComplexSelector>> result = parser.list(false);
    if (!result){
        if (error)
            *error = parser.error;
        if (errorRef)
            *errorRef = parser.errorRef;
    }
    return result;
}

std::optional<std::vector<ComplexSelector>> SelectorRewriter::parseList(std::string_view text, std::string* error){
    std::vector<ComponentValue> values = parseComponentValueList(text);
    return parseList(values, 0, error, nullptr);
}

SelectorRewriteResult SelectorRewriter::rewrite(const std::vector<ComplexSelector>& list, const SelectorOptions& options,
                                                DiagnosticSink* sink, SourceRef ruleRef){
    SelectorRewriteResult result;
    Rewriter rewriter(options, sink, ruleRef);
    std::vector<std::string> seen;
    for (const ComplexSelector& source : list){
        if (source.relative){
            if (sink)
                sink->report(codes::InvalidSelector, source.ref.valid() ? source.ref : ruleRef,
                             "selector '" + serialize(source) + "' starts with a combinator outside a nested rule; the rule is dropped");
            result.invalid = true;
            result.selectors.clear();
            return result;
        }
        std::vector<ComplexSelector> variants = rewriter.expand(source);
        if (rewriter.limitReached){
            if (sink)
                sink->report(codes::SelectorExpansionLimit, ruleRef, "the selector list expands to more than 256 selectors; the rule is dropped",
                             "write fewer :is() / nested alternatives", std::string(), ":is()");
            result.invalid = true;
            result.selectors.clear();
            return result;
        }
        for (ComplexSelector& variant : variants){
            bool placeholder = false, pseudoElement = false;
            rewriter.pendingAlternate.clear();
            ComplexSelector mapped = variant;
            const bool kept = rewriter.mapSelector(mapped, placeholder, pseudoElement, 0);
            if (rewriter.invalidRule){
                result.invalid = true;
                result.selectors.clear();
                return result;
            }
            if (!kept)
                continue;
            std::vector<ComplexSelector> forms{mapped};
            if (!rewriter.pendingAlternate.empty()){
                // the scrollbar mappings make two selectors (vertical and horizontal, dec and inc)
                ComplexSelector alternate = mapped;
                for (Compound& compound : alternate.compounds){
                    for (SimpleSelector& s : compound.simples){
                        if (s.kind == Kind::Type && (s.name == "scrollbarvertical" || s.name == "sliderarrowdec"))
                            s.name = rewriter.pendingAlternate;
                    }
                }
                forms.push_back(std::move(alternate));
            }
            for (ComplexSelector& form : forms){
                if (!rewriter.resolveRoot(form))
                    continue;
                for (ComplexSelector& banded : rewriter.band(form)){
                    RewrittenSelector out;
                    out.text = serialize(banded);
                    if (std::find(seen.begin(), seen.end(), out.text) != seen.end())
                        continue;
                    seen.push_back(out.text);
                    out.specificity = specificity(banded);
                    out.selector = std::move(banded);
                    out.source = source;
                    out.placeholder = placeholder;
                    out.pseudoElement = pseudoElement;
                    result.selectors.push_back(std::move(out));
                }
            }
        }
    }
    return result;
}

std::string SelectorRewriter::serialize(const SimpleSelector& s){
    switch (s.kind){
        case Kind::Type: return (s.hasNamespace ? "|" : "") + serializeIdent(s.name);
        case Kind::Universal: return "*";
        case Kind::Id: return "#" + serializeIdent(s.name);
        case Kind::Class: return "." + serializeIdent(s.name);
        case Kind::Nesting: return "&";
        case Kind::RootMarker: return ":root";
        case Kind::Attribute: {
            std::string text = "[" + serializeIdent(s.name);
            if (!s.op.empty())
                text += s.op + serializeString(s.value);
            if (s.flag){
                text += ' ';
                text += s.flag;
            }
            return text + "]";
        }
        case Kind::PseudoClass:
        case Kind::PseudoElement: {
            std::string text = s.kind == Kind::PseudoElement && !s.legacyColon ? "::" : ":";
            text += s.name;
            if (s.function){
                text += "(";
                if (!s.selectors.empty()){
                    for (size_t i = 0; i < s.selectors.size(); i++){
                        if (i > 0)
                            text += ", ";
                        text += serialize(s.selectors[i]);
                    }
                }else if (s.name.compare(0, 4, "nth-") == 0){
                    text += anbText(s.arguments);
                }else{
                    text += jasperengine::htmlui::compat::serialize(trimWhitespace(s.arguments));
                }
                text += ")";
            }
            return text;
        }
    }
    return std::string();
}

std::string SelectorRewriter::serialize(const Compound& compound){
    std::string text;
    // a universal selector only when the compound has nothing else
    bool onlyUniversal = true;
    for (const SimpleSelector& s : compound.simples){
        if (s.kind != Kind::Universal)
            onlyUniversal = false;
    }
    for (const SimpleSelector& s : compound.simples){
        if (s.kind == Kind::Universal && !onlyUniversal)
            continue;
        text += serialize(s);
    }
    return text;
}

std::string SelectorRewriter::serialize(const ComplexSelector& selector){
    std::string text;
    for (size_t i = 0; i < selector.compounds.size(); i++){
        const Compound& compound = selector.compounds[i];
        if (i > 0 || (selector.relative && compound.combinator)){
            switch (compound.combinator){
                case '>': text += i > 0 ? " > " : "> "; break;
                case '+': text += i > 0 ? " + " : "+ "; break;
                case '~': text += i > 0 ? " ~ " : "~ "; break;
                case '|': text += " || "; break;
                default: text += " "; break;
            }
        }
        text += serialize(compound);
    }
    return text;
}

uint32_t SelectorRewriter::specificity(const ComplexSelector& selector){
    uint32_t total = 0;
    for (const Compound& compound : selector.compounds)
        total += specificityOf(compound);
    return total;
}

bool SelectorRewriter::supported(const std::vector<ComplexSelector>& list, const SelectorOptions& options){
    SelectorRewriteResult result = rewrite(list, options, nullptr, SourceRef());
    return !result.invalid && result.selectors.size() > 0;
}

const Compound* SelectorRewriter::subject(const ComplexSelector& selector){
    return selector.compounds.empty() ? nullptr : &selector.compounds.back();
}

bool SelectorRewriter::subjectsOverlap(const ComplexSelector& a, const ComplexSelector& b){
    const Compound* x = subject(a);
    const Compound* y = subject(b);
    if (!x || !y)
        return false;
    auto keys = [](const Compound& c, bool& universal){
        std::vector<std::string> result;
        universal = true;
        for (const SimpleSelector& s : c.simples){
            if (s.kind == Kind::Type){
                result.push_back("t:" + s.name);
                universal = false;
            }else if (s.kind == Kind::Id){
                result.push_back("i:" + s.name);
                universal = false;
            }else if (s.kind == Kind::Class){
                result.push_back("c:" + s.name);
                universal = false;
            }else if (s.kind == Kind::RootMarker){
                result.push_back("t:html");
                universal = false;
            }
        }
        return result;
    };
    bool ux = false, uy = false;
    const std::vector<std::string> kx = keys(*x, ux);
    const std::vector<std::string> ky = keys(*y, uy);
    if (ux || uy)
        return true;
    for (const std::string& k : kx){
        if (std::find(ky.begin(), ky.end(), k) != ky.end())
            return true;
    }
    return false;
}

std::string SelectorRewriter::baseKey(const ComplexSelector& selector){
    ComplexSelector copy = selector;
    if (!copy.compounds.empty()){
        Compound& last = copy.compounds.back();
        std::vector<SimpleSelector> kept;
        for (const SimpleSelector& s : last.simples){
            if (s.kind == Kind::PseudoClass || s.kind == Kind::Attribute || s.kind == Kind::PseudoElement)
                continue;
            kept.push_back(s);
        }
        if (kept.empty()){
            SimpleSelector universal;
            universal.kind = Kind::Universal;
            kept.push_back(universal);
        }
        last.simples = std::move(kept);
    }
    return serialize(copy);
}

std::string SelectorRewriter::subjectTag(const ComplexSelector& selector){
    const Compound* s = subject(selector);
    if (!s)
        return std::string();
    for (const SimpleSelector& simple : s->simples){
        if (simple.kind == Kind::Type)
            return simple.name;
    }
    return std::string();
}

bool SelectorRewriter::isRootRule(const ComplexSelector& selector, const SelectorOptions& options){
    if (selector.compounds.size() != 1)
        return false;
    for (const SimpleSelector& s : selector.compounds.front().simples){
        if (s.kind == Kind::Type && s.name == "html")
            return true;
        if (s.kind == Kind::PseudoClass && s.name == "root" && !s.function)
            return true;
        if (s.kind == Kind::RootMarker)
            return true;
        if (s.kind == Kind::Id && !options.rootId.empty() && s.name == options.rootId)
            return true;
    }
    return false;
}
