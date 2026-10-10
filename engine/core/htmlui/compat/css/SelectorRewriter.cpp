// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/SelectorRewriter.h"
#include "htmlui/compat/LiteralCompare.h"

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

    // selector text (defined with SelectorRewriter::serialize below); `prefixed`: written after a
    // compound of its own, so the first compound gets its combinator
    void appendComplex(std::string& text, const ComplexSelector& selector, bool prefixed = false);
    constexpr const char* kRootId = "htmlui-root";

    // a Delim token of that character
    inline bool isDelimChar(const ComponentValue& v, char c){
        return v.kind == ComponentValue::Kind::Token && v.token.type == TokenType::Delim && v.token.value.size() == 1 && v.token.value[0] == c;
    }

    // out = lowerAscii(text): the text itself when it has no upper case letter
    inline void assignLower(Text& out, const Text& text){
        size_t upper = 0;
        while (upper < text.size() && !(text[upper] >= 'A' && text[upper] <= 'Z'))
            upper++;
        if (upper == text.size()){
            out = text;
            return;
        }
        char* lowered = (char*)Arena::current().allocate(text.size());
        for (size_t i = 0; i < text.size(); i++){
            const char c = text[i];
            lowered[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        out = Text::view(std::string_view(lowered, text.size()));
    }

    bool isWs(const ComponentValue& v){
        return v.isWhitespace() || v.isToken(TokenType::Comment);
    }

    bool isSelectorListPseudo(std::string_view name){
        return name == "is" || name == "where" || name == "matches" || name == "-webkit-any" || name == "-moz-any" ||
               name == "not" || name == "has" || name == "host" || name == "host-context";
    }

    // ---- parsing

    class Parser {
    public:
        using Tokens = Span<const ComponentValue* const>;

        Parser(const ValueList& values, uint32_t file): file(file) {
            // (a selector's few tokens fit the inline buffer)
            if (values.size() > kInlineItems){
                heapItems.reserve(values.size());
                for (const ComponentValue& v : values){
                    if (!v.isToken(TokenType::Comment))
                        heapItems.push_back(&v);
                }
                items = Tokens(heapItems.data(), heapItems.size());
            }else{
                size_t count = 0;
                for (const ComponentValue& v : values){
                    if (!v.isToken(TokenType::Comment))
                        inlineItems[count++] = &v;
                }
                items = Tokens(inlineItems, count);
            }
        }

        std::optional<SelectorList> list(bool forgiving){
            SelectorList result;
            // the comma-separated groups, as ranges of items
            size_t commas = 0;
            for (size_t i = 0; i < items.size(); i++)
                commas += items[i]->isToken(TokenType::Comma) ? 1 : 0;
            result.reserve(commas + 1);
            size_t begin = 0;
            for (size_t g = 0; g <= items.size(); g++){
                if (g < items.size() && !items[g]->isToken(TokenType::Comma))
                    continue;
                const Tokens group(items.data() + begin, g - begin);
                begin = g + 1;
                if (!complex(group, result.emplace_back())){
                    result.pop_back();
                    if (forgiving){
                        error.clear();
                        continue;
                    }
                    return std::nullopt;
                }
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

        // into a default-constructed selector
        bool complex(const Tokens& tokens, ComplexSelector& selector){
            size_t i = 0;
            // trim
            size_t end = tokens.size();
            while (i < end && tokens[i]->isWhitespace())
                i++;
            while (end > i && tokens[end - 1]->isWhitespace())
                end--;
            if (i >= end){
                fail(nullptr, "empty selector");
                return false;
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
                    const Text& d = t->token.value;
                    if (d == ">" || d == "+" || d == "~")
                        explicitCombinator = d[0];
                    else if (d == "|" && i + 1 < end && isDelimChar(*tokens[i + 1], '|')){
                        explicitCombinator = '|';
                        i++;
                    }
                }
                if (explicitCombinator){
                    if (combinator && combinator != ' '){
                        fail(t, "two combinators in a row");
                        return false;
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
                {
                    // at most one simple selector per token up to the next whitespace
                    size_t j = i;
                    while (j < end && !tokens[j]->isWhitespace())
                        j++;
                    compound.simples.reserve(j - i);
                }
                if (selector.compounds.empty()){
                    // at most one compound per whitespace- or combinator-separated run
                    size_t runs = 1;
                    for (size_t k = i + 1; k < end; k++){
                        const ComponentValue& v = *tokens[k];
                        if (v.isWhitespace() || isDelimChar(v, '>') || isDelimChar(v, '+') || isDelimChar(v, '~') || isDelimChar(v, '|'))
                            runs++;
                    }
                    selector.compounds.reserve(runs < 3 ? runs : 3);
                }
                if (!parseCompound(tokens, i, end, compound))
                    return false;
                compound.combinator = first ? (selector.relative ? combinator : 0) : (combinator ? combinator : ' ');
                if (first && selector.relative && !combinator)
                    compound.combinator = 0;
                selector.compounds.push_back(std::move(compound));
                combinator = 0;
                first = false;
            }
            if (selector.compounds.empty() || (combinator && combinator != ' ')){
                fail(tokens[end - 1], "selector ends with a combinator");
                return false;
            }
            return true;
        }

        bool parseCompound(const Tokens& tokens, size_t& i, size_t end, Compound& compound){
            bool any = false;
            while (i < end){
                const ComponentValue* t = tokens[i];
                if (t->isWhitespace())
                    break;
                if ((isDelimChar(*t, '>') || isDelimChar(*t, '+') || isDelimChar(*t, '~')))
                    break;
                if (isDelimChar(*t, '|') && i + 1 < end && isDelimChar(*tokens[i + 1], '|'))
                    break;
                SimpleSelector simple;
                simple.ref = refOf(t);
                // type or universal, with an optional namespace prefix
                if ((t->isToken(TokenType::Ident) || ((isDelimChar(*t, '*') || isDelimChar(*t, '|')))) &&
                    !any){
                    bool hasNs = false;
                    const ComponentValue* name = t;
                    if (isDelimChar(*t, '|')){
                        hasNs = true;
                        if (i + 1 >= end){
                            fail(t, "namespace without a name");
                            return false;
                        }
                        name = tokens[++i];
                    }else if (i + 2 < end && isDelimChar(*tokens[i + 1], '|') &&
                              !(isDelimChar(*tokens[i + 2], '|'))){
                        hasNs = true;
                        i += 2;
                        name = tokens[i];
                    }
                    if (name->isToken(TokenType::Ident)){
                        simple.kind = Kind::Type;
                        assignLower(simple.name, name->token.value);
                    }else if (isDelimChar(*name, '*')){
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
                }else if (isDelimChar(*t, '.')){
                    if (i + 1 >= end || !tokens[i + 1]->isToken(TokenType::Ident) || tokens[i + 1]->offset != t->offset + 1){
                        fail(t, "'.' without a class name");
                        return false;
                    }
                    simple.kind = Kind::Class;
                    simple.name = tokens[i + 1]->token.value;
                    simple.ref = refOf(t).cover(refOf(tokens[i + 1]));
                    i += 2;
                }else if (isDelimChar(*t, '&')){
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
                        assignLower(simple.name, n->token.value);
                    }else if (n->kind == ComponentValue::Kind::Function){
                        assignLower(simple.name, n->name());
                        simple.function = true;
                        simple.arguments = n->children;
                        if (isSelectorListPseudo(simple.name)){
                            Parser inner(n->children, file);
                            const bool forgiving = simple.name == "is" || simple.name == "where";
                            std::optional<SelectorList> parsed = inner.list(forgiving);
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
            if (i + 2 < t.size() + 0 && t.size() >= 3 && isDelimChar(*t[1], '|') &&
                !(t.size() > 2 && isDelimChar(*t[2], '='))){
                simple.hasNamespace = true;
                i = 2;
            }else if (!t.empty() && isDelimChar(*t[0], '|')){
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
                          isDelimChar(*t[i + 1], '=')){
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

        static constexpr size_t kInlineItems = 24;
        const ComponentValue* inlineItems[kInlineItems];
        std::vector<const ComponentValue*> heapItems;
        Tokens items;
        uint32_t file;
    };

    // ---- serialization

    // An+B as RmlUi reads it ("2n+1"): the signs of the B part kept
    std::string anbText(const ValueList& arguments){
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

    bool hasOf(const ValueList& arguments){
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
        static bool expandable(std::string_view n){
            return n == "is" || n == "where" || n == "matches" || n == "-webkit-any" || n == "-moz-any";
        }

        // pseudo-classes RmlUi matches as written (pseudoClass() keeps them)
        static bool keptPseudoClass(const SimpleSelector& simple){
            const Text& n = simple.name;
            return !simple.function &&
                   (n == "hover" || n == "active" || n == "checked" || n == "disabled" || n == "placeholder-shown" || n == "scope" ||
                    n == "first-child" || n == "last-child" || n == "only-child" || n == "first-of-type" || n == "last-of-type" ||
                    n == "only-of-type" || n == "empty" || n == "selected" || n == "drag" || n == "html-focus" || n == "html-focus-visible");
        }

        // Would mapSelector() return the selector unchanged (and report nothing)? Types other
        // than html/body/template, classes, ids other than the reserved and the root's,
        // attributes without a flag, kept pseudo-classes, the universal selector; no namespace,
        // column combinator, pseudo-element, nesting or root marker.
        bool mapsToItself(const ComplexSelector& selector) const {
            if (selector.compounds.empty())
                return false;
            for (const Compound& compound : selector.compounds){
                if (compound.combinator == '|' || compound.simples.empty())
                    return false;
                for (const SimpleSelector& simple : compound.simples){
                    if (simple.hasNamespace)
                        return false;
                    switch (simple.kind){
                        case Kind::Type:
                            if (simple.name == "html" || simple.name == "body" || simple.name == "template")
                                return false;
                            break;
                        case Kind::Universal:
                        case Kind::Class:
                            break;
                        case Kind::Id:
                            if ((author() && simple.name == kRootId) || (!options.rootId.empty() && simple.name == options.rootId))
                                return false;
                            break;
                        case Kind::Attribute:
                            if (simple.flag)
                                return false;
                            break;
                        case Kind::PseudoClass:
                            if (!keptPseudoClass(simple))
                                return false;
                            break;
                        default:
                            return false;
                    }
                }
            }
            return true;
        }

        // does expand() change the selector (an :is()/:where() to expand)?
        bool needsExpansion(const ComplexSelector& selector) const {
            if (options.fork.has(ForkFeature::RootIsWhere))
                return false;
            for (const Compound& compound : selector.compounds){
                for (const SimpleSelector& simple : compound.simples){
                    if (simple.kind == Kind::PseudoClass && expandable(simple.name))
                        return true;
                }
            }
            return false;
        }

        SelectorList expand(const ComplexSelector& selector){
            // nothing to expand (the common case): the selector itself
            if (!needsExpansion(selector))
                return SelectorList{selector};
            SelectorList work{selector};
            SelectorList done;
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
                        const Text& n = simple.name;
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
            ArenaList<SimpleSelector> simples;
            for (const SimpleSelector& s : last.simples){
                if (s.kind == Kind::Type || s.kind == Kind::Universal)
                    simples.push_back(s);
            }
            for (const SimpleSelector& s : merged.simples){
                if (s.kind == Kind::Type || s.kind == Kind::Universal)
                    simples.push_back(s);
            }
            for (const SimpleSelector& s : merged.simples){
                if (s.kind != Kind::Type && s.kind != Kind::Universal)
                    simples.push_back(s);
            }
            for (const SimpleSelector& s : last.simples){
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

        // the selector's compounds rewritten into `out`; Owned: the input's simple selectors are
        // moved from (the input is replaced by the result), else copied
        template<bool Owned, typename Selector>
        bool mapCompounds(Selector& selector, ArenaList<Compound>& out, bool& placeholder, bool& pseudoElement, int depth){
            out.reserve(selector.compounds.size());
            for (size_t c = 0; c < selector.compounds.size(); c++){
                auto& source = selector.compounds[c];
                Compound compound;
                compound.combinator = source.combinator;
                if (compound.combinator == '|'){
                    report(codes::UnsupportedSelector, source.simples.empty() ? nullptr : &source.simples[0],
                           "the column combinator '||' is not supported; selector dropped", "select the cells by class", "||");
                    return false;
                }
                ArenaList<SimpleSelector> simples;
                simples.reserve(source.simples.size());
                ArenaList<Compound> appended;     // compounds a pseudo-element adds after this one
                bool removeSelf = false;            // a pseudo-element that replaces the compound
                for (auto& simple : source.simples){
                    if (simple.hasNamespace){
                        report(codes::UnsupportedSelector, &simple, "namespaced selector '" + SelectorRewriter::serialize(simple) + "' is not supported; selector dropped",
                               "remove the namespace prefix", "namespace");
                        return false;
                    }
                    // simples after a pseudo-element belong to the element it generates
                    ArenaList<SimpleSelector>& target = appended.empty() ? simples : appended.back().simples;
                    switch (simple.kind){
                        case Kind::Type:
                            if (simple.name == "html")
                                target.push_back(rootMarker());
                            else if (simple.name == "body")
                                target.push_back(makeType("htmlbody"));
                            else if (simple.name == "template")
                                target.push_back(makeType("htmltemplate"));
                            else
                                target.push_back(forward<Owned>(simple));
                            break;
                        case Kind::Universal:
                        case Kind::Class:
                        case Kind::RootMarker:
                            target.push_back(forward<Owned>(simple));
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
                                target.push_back(forward<Owned>(simple));
                            break;
                        case Kind::Attribute: {
                            SimpleSelector copy = forward<Owned>(simple);
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
            return true;
        }

        template<bool Owned, typename T>
        static decltype(auto) forward(T& value){
            if constexpr (Owned)
                return std::move(value);
            else
                return static_cast<const T&>(value);
        }

        // Rewrites the simples of one selector in place. Returns false to drop the selector;
        // sets invalidRule for a selector browsers reject (the whole rule goes).
        bool mapSelector(ComplexSelector& selector, bool& placeholder, bool& pseudoElement, int depth){
            ArenaList<Compound> out;
            if (!mapCompounds<true>(selector, out, placeholder, pseudoElement, depth))
                return false;
            selector.compounds = std::move(out);
            return !selector.compounds.empty();
        }

        // the same into `result`, `selector` left as it is
        bool mapSelector(const ComplexSelector& selector, ComplexSelector& result, bool& placeholder, bool& pseudoElement, int depth){
            result.ref = selector.ref;
            result.relative = selector.relative;
            ArenaList<Compound> out;
            if (!mapCompounds<false>(selector, out, placeholder, pseudoElement, depth))
                return false;
            result.compounds = std::move(out);
            return !result.compounds.empty();
        }

        static SimpleSelector rootMarker(){
            SimpleSelector s;
            s.kind = Kind::RootMarker;
            return s;
        }

        bool pseudoClass(const SimpleSelector& simple, ArenaList<SimpleSelector>& target, int depth){
            const Text& n = simple.name;
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
                        SelectorList expanded = expand(argument);
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
            // ":name" or ":name()" for the messages (the supported pseudo-classes above return first)
            const std::string display = ":" + n + (simple.function ? "()" : "");
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
        bool pseudoElementOf(const SimpleSelector& simple, ArenaList<SimpleSelector>& simples, ArenaList<Compound>& appended,
                             bool& removeSelf, bool& placeholder, bool& pseudoElement){
            const Text& n = simple.name;
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
                ArenaList<SimpleSelector> simples;
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
                        if (s.name == "is" || s.name == "where")
                            return false;
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

        // The RCSS selectors of a rewritten selector (text and specificity): author selectors go
        // under the #htmlui-root band (written as a prefix: no copy of the selector), plus the
        // root variant when the first compound could match the root element
        template<typename Emit>
        void band(const ComplexSelector& selector, const Emit& emit){
            // (one buffer for every selector of the thread: the emitter copies what it keeps)
            static thread_local std::string buffer;
            std::string& text = buffer;
            text.clear();
            if (!author() || startsAtRoot(selector)){
                appendComplex(text, selector);
                emit(std::string_view(text), SelectorRewriter::specificity(selector));
                return;
            }
            text += '#';
            text += kRootId;
            appendComplex(text, selector, true);
            // the band compound is one id selector
            emit(std::string_view(text), 1000000 + SelectorRewriter::specificity(selector));
            if (!selector.compounds.empty() && couldBeRoot(selector.compounds.front())){
                SimpleSelector id;
                id.kind = Kind::Id;
                id.name = kRootId;
                ComplexSelector rootVariant = selector;
                Compound& first = rootVariant.compounds.front();
                ArenaList<SimpleSelector> simples;
                simples.push_back(makeType("body"));
                simples.push_back(id);
                for (const SimpleSelector& s : first.simples){
                    if (s.kind != Kind::Universal)
                        simples.push_back(s);
                }
                first.simples = std::move(simples);
                text.clear();
                appendComplex(text, rootVariant);
                emit(std::string_view(text), SelectorRewriter::specificity(rootVariant));
            }
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

std::optional<SelectorList> SelectorRewriter::parseList(const ValueList& prelude, uint32_t file,
                                                                        std::string* error, SourceRef* errorRef){
    Parser parser(prelude, file);
    std::optional<SelectorList> result = parser.list(false);
    if (!result){
        if (error)
            *error = parser.error;
        if (errorRef)
            *errorRef = parser.errorRef;
    }
    return result;
}

std::optional<SelectorList> SelectorRewriter::parseList(std::string_view text, std::string* error){
    ValueList values = parseComponentValueList(text);
    return parseList(values, 0, error, nullptr);
}

SelectorRewriteResult SelectorRewriter::rewrite(const SelectorList& list, const SelectorOptions& options,
                                                DiagnosticSink* sink, SourceRef ruleRef){
    SelectorRewriteResult result;
    result.selectors.reserve(list.size());
    Rewriter rewriter(options, sink, ruleRef);
    for (size_t sourceIndex = 0; sourceIndex < list.size(); sourceIndex++){
        const ComplexSelector& source = list[sourceIndex];
        if (source.relative){
            if (sink)
                sink->report(codes::InvalidSelector, source.ref.valid() ? source.ref : ruleRef,
                             "selector '" + serialize(source) + "' starts with a combinator outside a nested rule; the rule is dropped");
            result.invalid = true;
            result.selectors.clear();
            return result;
        }
        // without :is()/:where() to expand, the source is mapped as it is (no copy)
        const bool expands = rewriter.needsExpansion(source);
        SelectorList variants;
        if (expands)
            variants = rewriter.expand(source);
        if (rewriter.limitReached){
            if (sink)
                sink->report(codes::SelectorExpansionLimit, ruleRef, "the selector list expands to more than 256 selectors; the rule is dropped",
                             "write fewer :is() / nested alternatives", std::string(), ":is()");
            result.invalid = true;
            result.selectors.clear();
            return result;
        }
        if (!expands && rewriter.mapsToItself(source)){
            // nothing to map, no root marker to resolve: the source is banded as it is
            rewriter.pendingAlternate.clear();
            rewriter.band(source, [&](std::string_view text, uint32_t specificity){
                for (const RewrittenSelector& kept : result.selectors){
                    if (kept.text == text)
                        return;
                }
                RewrittenSelector out;
                out.text = Text(text);
                out.specificity = specificity;
                out.source = (uint32_t)sourceIndex;
                out.placeholder = false;
                out.pseudoElement = false;
                result.selectors.push_back(std::move(out));
            });
            continue;
        }
        for (size_t v = 0; v < (expands ? variants.size() : 1); v++){
            bool placeholder = false, pseudoElement = false;
            rewriter.pendingAlternate.clear();
            ComplexSelector mapped;
            bool kept;
            if (expands){
                mapped = std::move(variants[v]);
                kept = rewriter.mapSelector(mapped, placeholder, pseudoElement, 0);
            }else{
                kept = rewriter.mapSelector(source, mapped, placeholder, pseudoElement, 0);
            }
            if (rewriter.invalidRule){
                result.invalid = true;
                result.selectors.clear();
                return result;
            }
            if (!kept)
                continue;
            auto write = [&](ComplexSelector& form){
                if (!rewriter.resolveRoot(form))
                    return;
                rewriter.band(form, [&](std::string_view text, uint32_t specificity){
                    for (const RewrittenSelector& kept : result.selectors){
                        if (kept.text == text)
                            return;
                    }
                    RewrittenSelector out;
                    out.text = Text(text);
                    out.specificity = specificity;
                    out.source = (uint32_t)sourceIndex;
                    out.placeholder = placeholder;
                    out.pseudoElement = pseudoElement;
                    result.selectors.push_back(std::move(out));
                });
            };
            if (rewriter.pendingAlternate.empty()){
                write(mapped);
                continue;
            }
            // the scrollbar mappings make two selectors (vertical and horizontal, dec and inc)
            ComplexSelector alternate = mapped;
            for (Compound& compound : alternate.compounds){
                for (SimpleSelector& s : compound.simples){
                    if (s.kind == Kind::Type && (s.name == "scrollbarvertical" || s.name == "sliderarrowdec"))
                        s.name = rewriter.pendingAlternate;
                }
            }
            write(mapped);
            write(alternate);
        }
    }
    return result;
}

namespace {

    void appendSimple(std::string& text, const SimpleSelector& s);

    void appendCompound(std::string& text, const Compound& compound){
        // a universal selector only when the compound has nothing else
        bool onlyUniversal = true;
        for (const SimpleSelector& s : compound.simples){
            if (s.kind != Kind::Universal)
                onlyUniversal = false;
        }
        for (const SimpleSelector& s : compound.simples){
            if (s.kind == Kind::Universal && !onlyUniversal)
                continue;
            appendSimple(text, s);
        }
    }

    void appendCombinator(std::string& text, char combinator, bool first){
        switch (combinator){
            case '>': text += !first ? " > " : "> "; break;
            case '+': text += !first ? " + " : "+ "; break;
            case '~': text += !first ? " ~ " : "~ "; break;
            case '|': text += " || "; break;
            default: text += " "; break;
        }
    }

    // `prefixed`: the selector is written after another compound (the #htmlui-root band), so its
    // first compound gets its combinator (a descendant one when it has none)
    void appendComplex(std::string& text, const ComplexSelector& selector, bool prefixed){
        for (size_t i = 0; i < selector.compounds.size(); i++){
            const Compound& compound = selector.compounds[i];
            if (prefixed && i == 0)
                appendCombinator(text, compound.combinator ? compound.combinator : ' ', false);
            else if (i > 0 || (selector.relative && compound.combinator))
                appendCombinator(text, compound.combinator, i == 0);
            appendCompound(text, compound);
        }
    }

    void appendSimple(std::string& text, const SimpleSelector& s){
        switch (s.kind){
            case Kind::Type:
                if (s.hasNamespace)
                    text += '|';
                appendIdent(text, s.name);
                return;
            case Kind::Universal: text += '*'; return;
            case Kind::Id: text += '#'; appendIdent(text, s.name); return;
            case Kind::Class: text += '.'; appendIdent(text, s.name); return;
            case Kind::Nesting: text += '&'; return;
            case Kind::RootMarker: text += ":root"; return;
            case Kind::Attribute: {
                text += '[';
                appendIdent(text, s.name);
                if (!s.op.empty()){
                    text += s.op;
                    appendString(text, s.value);
                }
                if (s.flag){
                    text += ' ';
                    text += s.flag;
                }
                text += ']';
                return;
            }
            case Kind::PseudoClass:
            case Kind::PseudoElement: {
                text += s.kind == Kind::PseudoElement && !s.legacyColon ? "::" : ":";
                text += s.name;
                if (s.function){
                    text += "(";
                    if (!s.selectors.empty()){
                        for (size_t i = 0; i < s.selectors.size(); i++){
                            if (i > 0)
                                text += ", ";
                            appendComplex(text, s.selectors[i]);
                        }
                    }else if (s.name.compare(0, 4, "nth-") == 0){
                        text += anbText(s.arguments);
                    }else{
                        appendSerialized(text, trimWhitespace(s.arguments));
                    }
                    text += ")";
                }
                return;
            }
        }
    }

}

std::string SelectorRewriter::serialize(const SimpleSelector& s){
    std::string text;
    appendSimple(text, s);
    return text;
}

std::string SelectorRewriter::serialize(const Compound& compound){
    std::string text;
    appendCompound(text, compound);
    return text;
}

std::string SelectorRewriter::serialize(const ComplexSelector& selector){
    std::string text;
    appendComplex(text, selector);
    return text;
}

uint32_t SelectorRewriter::specificity(const ComplexSelector& selector){
    uint32_t total = 0;
    for (const Compound& compound : selector.compounds)
        total += specificityOf(compound);
    return total;
}

bool SelectorRewriter::supported(const SelectorList& list, const SelectorOptions& options){
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
        ArenaList<SimpleSelector> kept;
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
