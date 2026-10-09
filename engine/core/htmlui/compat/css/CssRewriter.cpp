// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CSS -> RCSS rewriter (phase-2 §3.9): one session per document. Phase A parses every
// sheet and flattens it (@import inlined, @supports evaluated, nesting resolved, @layer
// ordered, media normalized) into style rules in cascade order; a pre-pass rewrites the
// selectors and collects what the declaration pipeline needs document-wide (custom property
// index, companions in use, keyframe names, base rules); phase B runs every declaration
// through the pipeline of §3.9.6 and writes the RCSS with its line map.

#include "htmlui/compat/css/CssRewriter.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/Hash.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/AtRules.h"
#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/css/CascadeKeywords.h"
#include "htmlui/compat/css/Companions.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/Import.h"
#include "htmlui/compat/css/Nesting.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/RcssWriter.h"
#include "htmlui/compat/css/RewriteSession.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"
#include "htmlui/compat/css/props/Rewriters.h"

#include <algorithm>
#include <cstring>
#include <set>

using namespace jasperengine::htmlui::compat;

// ---- DeclContext

void DeclContext::emit(const std::string& property, const std::string& value){
    emit(property, value, declaration.ref);
}

void DeclContext::emit(const std::string& property, const std::string& value, SourceRef ref){
    OutDecl out;
    out.name = property;
    out.value = value;
    out.ref = ref.valid() ? ref : declaration.ref;
    out.important = important;
    output.declarations.push_back(std::move(out));
}

void DeclContext::generate(const std::string& suffix, const std::string& property, const std::string& value){
    for (GeneratedRule& rule : output.generated){
        if (rule.suffix == suffix){
            OutDecl out{property, value, declaration.ref, false, false};
            rule.declarations.push_back(out);
            return;
        }
    }
    GeneratedRule rule;
    rule.suffix = suffix;
    rule.declarations.push_back(OutDecl{property, value, declaration.ref, false, false});
    output.generated.push_back(std::move(rule));
}

void DeclContext::report(DiagCode code, const std::string& message, const std::string& suggestion, const std::string& replacement){
    session.sink->report(code, declaration.ref, message, suggestion, replacement, name);
}

void DeclContext::reportAs(Severity severity, DiagCode code, const std::string& message, const std::string& suggestion,
                           const std::string& subject){
    // the subject keeps several findings of one declaration apart (the sink drops repeated (code, ref, subject))
    session.sink->reportAs(severity, code, declaration.ref, message, suggestion, std::string(), subject.empty() ? name : name + ": " + subject);
}

void DeclContext::reportAt(SourceRef ref, Severity severity, DiagCode code, const std::string& message, const std::string& suggestion){
    session.sink->reportAs(severity, code, ref.valid() ? ref : declaration.ref, message, suggestion, std::string(), name);
}

SourceRef DeclContext::valueRef() const{
    return declaration.valueRef.valid() ? declaration.valueRef : declaration.ref;
}

std::string DeclContext::valueText() const{
    return serialize(value);
}

std::string DeclContext::sourceText() const{
    return declaration.name + ": " + serialize(trimWhitespace(declaration.value));
}

bool DeclContext::fork(ForkFeature feature) const{
    return session.options.fork.has(feature);
}

// ---- Session

Session::Session(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, const RewriteContext& context):
    options(options), files(files), sink(&sink), context(context), urls(context.files, context.manifest) {
    selectorOptions.fork = options.fork;
    for (const auto& attribute : context.rootAttributes){
        const std::string name = lowerAscii(attribute.first);
        if (name == "id"){
            selectorOptions.rootId = attribute.second;
        }else if (name == "class"){
            std::string current;
            for (char c : attribute.second + " "){
                if (c == ' ' || c == '\t' || c == '\n'){
                    if (!current.empty())
                        selectorOptions.rootClasses.push_back(current);
                    current.clear();
                }else{
                    current += c;
                }
            }
        }else if (name == "dir"){
            rootDirection = lowerAscii(attribute.second);
        }
        selectorOptions.rootAttributes.push_back(name);
    }
}

const ImportManifest& Session::manifest() const{
    return urls.manifest();
}

std::string Session::keyframeName(const std::string& css){
    const auto it = keyframeNames.find(css);
    if (it != keyframeNames.end())
        return it->second;
    const std::string name = AtRules::validKeyframesName(css) ? css : "kf-" + sha1Hex(css).substr(0, 8);
    keyframeNames[css] = name;
    return name;
}

bool Session::staticVar(const ComponentValue& var, std::vector<ComponentValue>& out) const{
    return vars.resolve(var, out);
}

std::optional<TypedVar> Session::typedVar(const ComponentValue& var, const std::string& family){
    std::vector<ComponentValue> fallback;
    bool hasFallback = false;
    const std::string name = CustomPropertyIndex::varName(var, &fallback, &hasFallback);
    const VarFamily* f = findVarFamily(family);
    if (name.empty() || !f)
        return std::nullopt;
    std::vector<std::vector<ComponentValue>> values;
    if (!vars.declared(name)){
        if (!hasFallback)
            return std::nullopt;
        values.push_back(fallback);
    }else if (!vars.expandedValues(name, values)){
        return std::nullopt;
    }
    std::string kind;
    for (const std::vector<ComponentValue>& value : values){
        const std::string k = f->classify(*this, value);
        if (k.empty())
            return std::nullopt;
        if (!kind.empty() && k != kind)
            return std::nullopt;
        kind = k;
    }
    // a declaration that itself references a variable cannot be written in another kind
    if (const std::vector<CustomPropertyIndex::Entry>* entries = vars.find(name)){
        for (const CustomPropertyIndex::Entry& entry : *entries){
            std::vector<std::string> nested;
            CustomPropertyIndex::referencedVariables(entry.value, nested);
            if (!nested.empty() && kind != "color")
                return std::nullopt;
        }
    }
    TypedVar typed;
    typed.kind = kind;
    // colors are written the same in every family: the variable itself
    std::string reference = "var(" + (vars.declared(name) && kind != "color" ? vars.typedName(name, family) : name);
    if (hasFallback && !fallback.empty()){
        std::optional<std::string> transformed = f->transform(*this, fallback, kind, SourceRef());
        if (transformed)
            reference += ", " + *transformed;
    }
    typed.reference = reference + ")";
    return typed;
}

namespace {

    // ---- Damerau-Levenshtein for unknown property suggestions

    size_t editDistance(const std::string& a, const std::string& b){
        const size_t n = a.size(), m = b.size();
        std::vector<std::vector<size_t>> d(n + 1, std::vector<size_t>(m + 1));
        for (size_t i = 0; i <= n; i++)
            d[i][0] = i;
        for (size_t j = 0; j <= m; j++)
            d[0][j] = j;
        for (size_t i = 1; i <= n; i++){
            for (size_t j = 1; j <= m; j++){
                const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
                d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
                if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                    d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            }
        }
        return d[n][m];
    }

    std::string closestProperty(const std::string& name){
        std::string best;
        size_t bestDistance = 3;
        for (const PropertySupport& row : propertyTable()){
            if (row.name[0] == '-' && name[0] != '-')
                continue;
            const std::string candidate = row.name;
            if (candidate == "--*")
                continue;
            const size_t lengthDifference = candidate.size() > name.size() ? candidate.size() - name.size() : name.size() - candidate.size();
            if (lengthDifference > 2)
                continue;
            const size_t distance = editDistance(name, candidate);
            if (distance < bestDistance){
                bestDistance = distance;
                best = candidate;
            }
        }
        return best;
    }

    // ---- flattening (phase A)

    struct FlattenScope {
        uint32_t sheet = 0;
        uint32_t file = 0;
        RuleOrigin origin = RuleOrigin::Author;
        MediaResult media = MediaResult::unconditional();
        int layer = -1;
        std::string layerName;                  // dotted name of the enclosing layer
        const std::vector<ComplexSelector>* parents = nullptr;
        std::vector<uint32_t> importChain;
    };

    int registerLayer(Session& session, const std::string& name){
        for (size_t i = 0; i < session.layerOrder.size(); i++){
            if (session.layerOrder[i] == name)
                return (int)i;
        }
        session.layerOrder.push_back(name);
        return (int)session.layerOrder.size() - 1;
    }

    void addRule(Session& session, const FlattenScope& scope, const std::vector<ComplexSelector>& selectors,
                 std::vector<Declaration> declarations, SourceRef ref, SourceRef preludeRef){
        if (declarations.empty() || scope.media.never)
            return;
        FlatRule rule;
        rule.index = (uint32_t)session.rules.size();
        rule.sheet = scope.sheet;
        rule.file = scope.file;
        rule.origin = scope.origin;
        rule.selectors = selectors;
        for (size_t i = 0; i < selectors.size(); i++)
            rule.selectorText += (i ? ", " : "") + SelectorRewriter::serialize(selectors[i]);
        rule.ref = ref;
        rule.preludeRef = preludeRef;
        rule.media = scope.media;
        rule.declarations = std::move(declarations);
        rule.layer = scope.layer;
        rule.order = session.nextOrder++;
        session.sheets[scope.sheet].push_back(SheetItem{false, rule.index});
        session.rules.push_back(std::move(rule));
    }

    void flatten(Session& session, const std::vector<Rule>& rules, const FlattenScope& scope, bool topLevel);

    // a style rule: its declarations and nested rules in source order (CSS Nesting)
    void flattenStyleRule(Session& session, const Rule& rule, const FlattenScope& scope){
        std::string error;
        SourceRef errorRef;
        std::optional<std::vector<ComplexSelector>> parsed = SelectorRewriter::parseList(rule.prelude, scope.file, &error, &errorRef);
        if (!parsed){
            session.sink->report(codes::InvalidSelector, rule.preludeRef.valid() ? rule.preludeRef : rule.ref,
                                 "invalid selector '" + serialize(trimWhitespace(rule.prelude)) + "' (" + error + "); the rule is dropped",
                                 "fix the selector syntax", std::string(), "selector");
            return;
        }
        std::vector<ComplexSelector> selectors = std::move(*parsed);
        if (scope.parents){
            std::optional<std::vector<ComplexSelector>> combined = Nesting::combine(*scope.parents, selectors, session.options.fork);
            if (!combined){
                session.sink->report(codes::SelectorExpansionLimit, rule.preludeRef, "nested selectors expand to more than 256 selectors; the rule is dropped",
                                     "flatten the nesting or use fewer selectors in the parent list", std::string(), "&");
                return;
            }
            selectors = std::move(*combined);
            if (!session.nestingReported){
                session.nestingReported = true;
                session.sink->report(codes::NestingFlattened, rule.preludeRef, "nested rules are flattened (& replaced by the parent selector)");
            }
        }

        // declarations and nested rules in source order
        struct Item {
            uint32_t offset;
            const Declaration* declaration;
            const Rule* rule;
        };
        std::vector<Item> items;
        for (const Declaration& d : rule.declarations)
            items.push_back(Item{d.ref.offset, &d, nullptr});
        for (const Rule& r : rule.rules)
            items.push_back(Item{r.ref.offset, nullptr, &r});
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b){ return a.offset < b.offset; });

        std::vector<Declaration> group;
        FlattenScope nested = scope;
        nested.parents = &selectors;
        for (const Item& item : items){
            if (item.declaration){
                group.push_back(*item.declaration);
                continue;
            }
            addRule(session, scope, selectors, std::move(group), rule.ref, rule.preludeRef);
            group.clear();
            std::vector<Rule> one{*item.rule};
            flatten(session, one, nested, false);
        }
        addRule(session, scope, selectors, std::move(group), rule.ref, rule.preludeRef);
    }

    void flatten(Session& session, const std::vector<Rule>& rules, const FlattenScope& scope, bool topLevel){
        bool importsAllowed = topLevel;
        for (const Rule& rule : rules){
            if (rule.kind == Rule::Kind::Style){
                importsAllowed = false;
                flattenStyleRule(session, rule, scope);
                continue;
            }
            const std::string& name = rule.atName;
            if (name == "import"){
                if (!importsAllowed){
                    session.sink->reportAs(Severity::Warning, codes::ImportProblem, rule.ref,
                                           "@import after other rules is ignored (CSS only allows it at the start)",
                                           "move the @import to the top of the stylesheet");
                    continue;
                }
                std::vector<uint32_t> chain = scope.importChain;
                chain.push_back(scope.file);
                std::optional<ImportedSheet> imported = Import::load(session, rule, scope.file, chain);
                if (!imported)
                    continue;
                if (!imported->supports.empty()){
                    // supports(<declaration>) is a condition in parentheses
                    std::vector<ComponentValue> condition = trimWhitespace(imported->supports);
                    if (!condition.empty() && condition[0].isToken(TokenType::Ident) && !equalsNoCase(condition[0].token.value, "not")){
                        ComponentValue block;
                        block.kind = ComponentValue::Kind::Block;
                        block.token.type = TokenType::LeftParen;
                        block.children = condition;
                        condition = {block};
                    }
                    imported->supports = condition;
                }
                if (!imported->supports.empty() && !AtRules::evaluateCondition(session, imported->supports, scope.file)){
                    session.sink->report(codes::SupportsEvaluated, rule.ref, "@import supports(" + serialize(trimWhitespace(imported->supports)) +
                                         ") is false: the sheet is not imported");
                    continue;
                }
                FlattenScope inner = scope;
                inner.file = imported->file;
                inner.importChain = chain;
                inner.media = MediaResult::conjoin(scope.media, imported->media);
                if (imported->layered){
                    const std::string layerName = imported->layer.empty()
                        ? "@anonymous-" + std::to_string(session.layerOrder.size())
                        : (scope.layerName.empty() ? imported->layer : scope.layerName + "." + imported->layer);
                    inner.layer = registerLayer(session, layerName);
                    inner.layerName = layerName;
                }
                flatten(session, imported->sheet.rules, inner, true);
                continue;
            }
            if (name == "charset"){
                AtRules::reportDropped(session, rule);
                continue;
            }
            if (name == "layer"){
                // statement: declares the order; block: its rules in that layer
                std::vector<std::string> names;
                for (const std::vector<ComponentValue>& group : splitAtCommas(rule.prelude)){
                    const std::string n = serialize(trimWhitespace(group));
                    if (!n.empty())
                        names.push_back(scope.layerName.empty() ? n : scope.layerName + "." + n);
                }
                if (!rule.hasBlock){
                    for (const std::string& n : names)
                        registerLayer(session, n);
                    continue;
                }
                importsAllowed = false;
                FlattenScope inner = scope;
                const std::string layerName = names.empty() ? "@anonymous-" + std::to_string(session.layerOrder.size()) : names.front();
                inner.layer = registerLayer(session, layerName);
                inner.layerName = layerName;
                if (scope.parents && !rule.declarations.empty())
                    addRule(session, inner, *scope.parents, rule.declarations, rule.ref, rule.preludeRef);
                flatten(session, rule.rules, inner, false);
                continue;
            }
            importsAllowed = false;
            if (name == "media"){
                FlattenScope inner = scope;
                const MediaResult media = MediaQuery::normalize(rule.prelude, rule.preludeRef, session.options, session.sink);
                inner.media = MediaResult::conjoin(scope.media, media);
                if (inner.media.never)
                    continue;
                if (scope.parents && !rule.declarations.empty())
                    addRule(session, inner, *scope.parents, rule.declarations, rule.ref, rule.preludeRef);
                flatten(session, rule.rules, inner, false);
                continue;
            }
            if (name == "supports"){
                if (!AtRules::evaluateSupports(session, rule))
                    continue;
                if (scope.parents && !rule.declarations.empty())
                    addRule(session, scope, *scope.parents, rule.declarations, rule.ref, rule.preludeRef);
                flatten(session, rule.rules, scope, false);
                continue;
            }
            if (name == "font-face" || name == "keyframes" || name == "-webkit-keyframes" || name == "-moz-keyframes" ||
                name == "-o-keyframes" || name == "decorator" || name == "spritesheet"){
                if (scope.parents){
                    session.sink->reportAs(Severity::Warning, codes::UnsupportedAtRule, rule.ref, "@" + name + " inside a style rule is dropped",
                                           "move it to the top level");
                    continue;
                }
                if (scope.media.never)
                    continue;
                FlatAtRule at;
                at.sheet = scope.sheet;
                at.file = scope.file;
                at.origin = scope.origin;
                at.rule = rule;
                at.media = scope.media;
                at.layer = scope.layer;
                at.order = session.nextOrder++;
                session.sheets[scope.sheet].push_back(SheetItem{true, (uint32_t)session.atRules.size()});
                session.atRules.push_back(std::move(at));
                continue;
            }
            if (name == "property"){
                // @property --x { initial-value: v; inherits: ...; } -> the root gets --x: v
                const std::string property = serialize(trimWhitespace(rule.prelude));
                const Declaration* initial = nullptr;
                bool inheritsFalse = false;
                for (const Declaration& d : rule.declarations){
                    if (d.name == "initial-value")
                        initial = &d;
                    else if (d.name == "inherits" && !d.value.empty() && lowerAscii(serialize(trimWhitespace(d.value))) == "false")
                        inheritsFalse = true;
                }
                if (property.size() < 3 || property.compare(0, 2, "--") != 0){
                    session.sink->report(codes::PropertyApproximated, rule.ref, "@property needs a custom property name; dropped");
                    continue;
                }
                if (inheritsFalse){
                    session.sink->report(codes::PropertyApproximated, rule.ref, "@property " + property +
                                         " { inherits: false } cannot be honored: the value inherits", "set the property on the elements that use it");
                }
                if (!initial)
                    continue;
                Declaration declaration = *initial;
                declaration.name = property;
                std::optional<std::vector<ComplexSelector>> root = SelectorRewriter::parseList(":root");
                if (root)
                    addRule(session, scope, *root, {declaration}, rule.ref, rule.preludeRef);
                continue;
            }
            AtRules::reportDropped(session, rule);
        }
    }

    // ---- pre-pass helpers

    bool hasIdent(const std::vector<ComponentValue>& values, const char* lowercase){
        for (const ComponentValue& v : values){
            if (v.isToken(TokenType::Ident) && equalsNoCase(v.token.value, lowercase))
                return true;
            if (v.kind != ComponentValue::Kind::Token && hasIdent(v.children, lowercase))
                return true;
        }
        return false;
    }

    // animation names of an `animation` shorthand or `animation-name` value
    std::vector<std::string> animationNames(const std::string& property, const std::vector<ComponentValue>& value){
        static const std::set<std::string> keywords = {
            "none", "infinite", "normal", "reverse", "alternate", "alternate-reverse", "forwards", "backwards", "both", "running",
            "paused", "linear", "ease", "ease-in", "ease-out", "ease-in-out", "step-start", "step-end", "initial", "inherit", "unset"};
        std::vector<std::string> names;
        for (const std::vector<ComponentValue>& group : splitAtCommas(value)){
            for (const ComponentValue& v : group){
                if (v.isToken(TokenType::String)){
                    names.push_back(v.token.value);
                    break;
                }
                if (v.isToken(TokenType::Ident) && (property == "animation-name" || !keywords.count(lowerAscii(v.token.value)))){
                    if (lowerAscii(v.token.value) != "none")
                        names.push_back(v.token.value);
                    break;
                }
            }
        }
        return names;
    }

    std::vector<std::string> ruleAnimationNames(const FlatRule& rule){
        std::vector<std::string> names;
        for (const Declaration& d : rule.declarations){
            if (d.name == "animation" || d.name == "-webkit-animation" || d.name == "animation-name" || d.name == "-webkit-animation-name"){
                const std::string property = d.name.find("name") != std::string::npos ? "animation-name" : "animation";
                names = animationNames(property, d.value);
            }
        }
        return names;
    }

    void prePass(Session& session, const std::vector<std::vector<Declaration>>& styleAttributes){
        // selectors
        for (FlatRule& rule : session.rules){
            SelectorOptions options = session.selectorOptions;
            options.origin = rule.origin == RuleOrigin::UserAgent ? SelectorOptions::Origin::UserAgent : SelectorOptions::Origin::Author;
            SelectorRewriteResult result = SelectorRewriter::rewrite(rule.selectors, options, session.sink, rule.preludeRef.valid() ? rule.preludeRef : rule.ref);
            rule.rewritten = std::move(result.selectors);
            if (result.invalid || rule.rewritten.empty())
                rule.dropped = true;
        }
        BaseRules::index(session);

        // the custom property index (values normalized quietly; phase B reports)
        auto normalizeQuietly = [&](const std::vector<ComponentValue>& value, const std::string& name){
            NormalizeContext context;
            context.property = name;
            context.customProperty = true;
            context.options = session.options;
            NormalizeResult result = ValueNormalizer::normalize(value, context);
            return result.ok ? trimWhitespace(result.value) : trimWhitespace(value);
        };
        auto indexDeclarations = [&](const std::vector<Declaration>& declarations, uint32_t ruleId){
            for (const Declaration& d : declarations){
                if (!d.isCustomProperty())
                    continue;
                CustomPropertyIndex::Entry entry;
                entry.value = normalizeQuietly(d.value, d.name);
                entry.ref = d.ref;
                entry.rule = ruleId;
                session.vars.add(d.name, std::move(entry));
            }
        };
        for (const FlatRule& rule : session.rules){
            if (!rule.dropped)
                indexDeclarations(rule.declarations, rule.index + 1);
        }
        for (const std::vector<Declaration>& declarations : styleAttributes)
            indexDeclarations(declarations, 0);

        // uses, flags, conflicts index
        auto scan = [&](const std::vector<Declaration>& declarations, const FlatRule* rule){
            for (const Declaration& d : declarations){
                std::vector<std::string> referenced;
                CustomPropertyIndex::referencedVariables(d.value, referenced);
                if (d.isCustomProperty()){
                    for (const std::string& name : referenced)
                        session.vars.addRawUse(name);
                    continue;
                }
                std::string name = d.name;
                if (!findProperty(name)){
                    for (const char* prefix : {"-webkit-", "-moz-", "-ms-", "-o-"}){
                        const std::string p(prefix);
                        if (name.compare(0, p.size(), p) == 0 && findProperty(name.substr(p.size())))
                            name = name.substr(p.size());
                    }
                }
                const std::string family = varFamilyOf(name);
                const std::vector<ComponentValue> trimmed = trimWhitespace(d.value);
                const bool whole = trimmed.size() == 1 && trimmed[0].isFunction("var");
                const bool timing = name == "transition" || name == "animation" || name == "transition-timing-function" ||
                                    name == "animation-timing-function";
                for (const std::string& variable : referenced){
                    if (!family.empty() && whole && family != "easing"){
                        session.vars.addUse(variable, family);
                        continue;
                    }
                    if (timing){
                        // a variable in an easing position is written as a tween name
                        std::vector<std::vector<ComponentValue>> values;
                        bool easing = session.vars.expandedValues(variable, values) && !values.empty();
                        for (const std::vector<ComponentValue>& value : values)
                            easing = easing && !kEasingFamily.classify(session, value).empty();
                        if (easing){
                            session.vars.addUse(variable, "easing");
                            continue;
                        }
                    }
                    session.vars.addRawUse(variable);
                }
                if (name.compare(0, 12, "border-image") == 0 && !hasIdent(d.value, "none"))
                    session.usesBorderImage = true;
                if (name == "text-shadow" && !hasIdent(d.value, "none"))
                    session.usesTextShadow = true;
                if (name.compare(0, 19, "-webkit-text-stroke") == 0)
                    session.usesTextStroke = true;
                if (CascadeKeywords::keywordOf(d.value) == "inherit" && !session.options.fork.has(ForkFeature::CssWideKeywords)){
                    for (const std::string& p : CascadeKeywords::outputProperties(name)){
                        const PropertySupport* row = findProperty(name);
                        const bool inherited = row ? row->inherited : CascadeKeywords::inherited(p);
                        if (!inherited && !CascadeKeywords::inherited(p))
                            session.inheritCompanions.insert(p);
                    }
                }
                if ((name == "animation" || name == "animation-direction") && (hasIdent(d.value, "reverse") || hasIdent(d.value, "alternate-reverse"))){
                    std::vector<std::string> names = name == "animation" ? animationNames("animation", d.value) : std::vector<std::string>();
                    if (names.empty() && rule){
                        names = ruleAnimationNames(*rule);
                        if (names.empty()){
                            const FlatRule* base = BaseRules::find(session, *rule, [](const FlatRule& candidate){
                                return !ruleAnimationNames(candidate).empty();
                            });
                            if (base)
                                names = ruleAnimationNames(*base);
                        }
                    }
                    for (const std::string& n : names)
                        session.reversedKeyframes.insert(session.keyframeName(n));
                }
                if (rule && rule->origin == RuleOrigin::Author){
                    Session::PropertyUse use;
                    use.rule = rule->index;
                    use.important = d.important;
                    use.ref = d.ref;
                    session.propertyRules[name].push_back(use);
                }
            }
        };
        for (const FlatRule& rule : session.rules){
            if (!rule.dropped)
                scan(rule.declarations, &rule);
        }
        for (const std::vector<Declaration>& declarations : styleAttributes)
            scan(declarations, nullptr);
        for (const FlatAtRule& at : session.atRules){
            if (at.rule.atName.find("keyframes") != std::string::npos){
                const std::string name = AtRules::keyframesName(at.rule);
                if (!name.empty())
                    session.keyframeName(name);
                for (const Rule& frame : at.rule.rules)
                    scan(frame.declarations, nullptr);
            }
        }

        // the root font size (rem, root em) from static root rules
        for (const FlatRule& rule : session.rules){
            if (rule.dropped || rule.origin != RuleOrigin::Author)
                continue;
            bool root = false;
            for (const ComplexSelector& s : rule.selectors){
                if (SelectorRewriter::isRootRule(s, session.selectorOptions))
                    root = true;
            }
            if (!root)
                continue;
            for (const Declaration& d : rule.declarations){
                if (d.name != "font-size")
                    continue;
                std::vector<ComponentValue> v = trimWhitespace(d.value);
                if (v.size() != 1)
                    continue;
                if (v[0].isToken(TokenType::Percentage))
                    session.rootFontSize = 16.0 * v[0].token.number / 100.0;
                else if (v[0].isToken(TokenType::Dimension)){
                    const std::string unit = lowerAscii(v[0].token.unit);
                    if (unit == "em" || unit == "rem")
                        session.rootFontSize = 16.0 * v[0].token.number;
                    else if (std::optional<double> factor = canonicalFactor(unit); factor && unitCategory(unit) == UnitCategory::AbsoluteLength)
                        session.rootFontSize = v[0].token.number * *factor;
                }
            }
        }

        // @layer: a layered rule that outranks an unlayered one by specificity (RmlUi) loses in CSS
        if (!session.layerOrder.empty()){
            for (const auto& entry : session.propertyRules){
                for (const Session::PropertyUse& layered : entry.second){
                    const FlatRule& l = session.rules[layered.rule];
                    if (l.layer < 0 || l.dropped)
                        continue;
                    for (const Session::PropertyUse& plain : entry.second){
                        const FlatRule& u = session.rules[plain.rule];
                        if (u.layer >= 0 || u.dropped)
                            continue;
                        bool conflict = false;
                        for (const RewrittenSelector& sl : l.rewritten){
                            for (const RewrittenSelector& su : u.rewritten){
                                if (sl.specificity > su.specificity && SelectorRewriter::subjectsOverlap(sl.source, su.source))
                                    conflict = true;
                            }
                        }
                        if (conflict){
                            session.sink->report(codes::LayerFlattened, layered.ref,
                                                 "'" + entry.first + "' in @layer " + session.layerOrder[(size_t)l.layer] +
                                                 " has a higher specificity than the unlayered rule at " + BaseRules::location(session, u) +
                                                 "; in the engine it wins, in CSS the unlayered rule does",
                                                 "lower the layered selector's specificity or move the rule out of the layer");
                            break;
                        }
                    }
                }
            }
        }
    }

    // ---- writing

    std::string replaceViewportOrientation(const std::string& value, bool landscape){
        std::vector<ComponentValue> values = parseComponentValueList(value);
        std::function<void(std::vector<ComponentValue>&)> walk = [&](std::vector<ComponentValue>& list){
            for (ComponentValue& v : list){
                if (v.isToken(TokenType::Dimension)){
                    const std::string unit = lowerAscii(v.token.unit);
                    if (unit == "vmin")
                        v.token.unit = landscape ? "vh" : "vw";
                    else if (unit == "vmax")
                        v.token.unit = landscape ? "vw" : "vh";
                }
                if (v.kind != ComponentValue::Kind::Token)
                    walk(v.children);
            }
        };
        walk(values);
        return serialize(values);
    }

    bool hasViewportOrientation(const std::string& value){
        std::vector<ComponentValue> values = parseComponentValueList(value);
        std::function<bool(const std::vector<ComponentValue>&)> walk = [&](const std::vector<ComponentValue>& list){
            for (const ComponentValue& v : list){
                if (v.isToken(TokenType::Dimension)){
                    const std::string unit = lowerAscii(v.token.unit);
                    if (unit == "vmin" || unit == "vmax")
                        return true;
                }
                if (v.kind != ComponentValue::Kind::Token && walk(v.children))
                    return true;
            }
            return false;
        };
        return walk(values);
    }

    // the declarations that take effect: in one rule the last declaration of a property wins,
    // so earlier ones are not written
    std::vector<const OutDecl*> effective(const std::vector<OutDecl>& declarations){
        std::vector<const OutDecl*> out;
        std::set<std::string> seen;
        for (size_t i = declarations.size(); i-- > 0;){
            if (seen.insert(declarations[i].name).second)
                out.push_back(&declarations[i]);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    void writeBlock(RcssWriter& writer, const std::string& selectors, const std::vector<OutDecl>& declarations, SourceRef ref){
        if (declarations.empty() || selectors.empty())
            return;
        writer.beginRule(selectors, ref);
        for (const OutDecl* d : effective(declarations))
            writer.declaration(d->name, d->value, d->ref);
        writer.endRule();
    }

    std::string joinSelectors(const std::vector<std::string>& selectors, const std::string& suffix = std::string()){
        std::string text;
        for (const std::string& s : selectors)
            text += (text.empty() ? "" : ", ") + s + suffix;
        return text;
    }

    // a rule (and its generated rules) under each block of a media result
    void writeUnderMedia(RcssWriter& writer, const MediaResult& media, const std::function<void()>& body){
        if (media.never)
            return;
        if (media.always){
            body();
            return;
        }
        for (const MediaConjunction& block : media.blocks){
            writer.beginMedia(block.toRcss());
            body();
            writer.endMedia();
        }
    }

    bool placeholderProperty(const std::string& name){
        return name == "color" || name == "opacity" || name.compare(0, 5, "font-") == 0 || name == "--html-current-color" ||
               name == "font-effect" || name == "letter-spacing";
    }

    void writeRule(Session& session, const FlatRule& rule, RcssWriter& writer){
        RuleOutput output;
        processDeclarations(session, &rule, rule.origin, false, rule.declarations, output);

        std::vector<OutDecl> normal, vmin;
        Session::ImportantRule important;
        important.rule = &rule;
        for (OutDecl& d : output.declarations){
            if (d.important){
                important.declarations.push_back(d);
                continue;
            }
            if (rule.origin != RuleOrigin::StyleAttribute && hasViewportOrientation(d.value))
                vmin.push_back(d);
            else
                normal.push_back(d);
        }
        if (!important.declarations.empty())
            session.important.push_back(std::move(important));

        // ::placeholder selectors take only text properties
        std::vector<std::string> plain, placeholder;
        for (const RewrittenSelector& s : rule.rewritten)
            (s.placeholder ? placeholder : plain).push_back(s.text);
        std::vector<OutDecl> placeholderDeclarations;
        if (!placeholder.empty()){
            for (const OutDecl& d : normal){
                if (placeholderProperty(d.name))
                    placeholderDeclarations.push_back(d);
                else
                    session.sink->reportAs(Severity::Info, codes::UnsupportedProperty, d.ref, "'" + d.name +
                                           "' has no effect in ::placeholder (only color, opacity and font properties apply); dropped there");
            }
        }

        bool any = !normal.empty() || !placeholderDeclarations.empty();
        for (const GeneratedRule& generated : output.generated)
            any = any || !generated.declarations.empty();
        if (any)
        writeUnderMedia(writer, rule.media, [&](){
            writeBlock(writer, joinSelectors(plain), normal, rule.ref);
            writeBlock(writer, joinSelectors(placeholder), placeholderDeclarations, rule.ref);
            for (const GeneratedRule& generated : output.generated)
                writeBlock(writer, joinSelectors(plain, generated.suffix), generated.declarations, rule.ref);
        });

        if (!vmin.empty()){
            // vmin/vmax: one copy per orientation (landscape: vmin = vh; portrait: vmin = vw)
            for (int pass = 0; pass < 2; pass++){
                const bool landscape = pass == 0;
                MediaConjunction orientation;
                orientation.orientation = landscape ? "landscape" : "portrait";
                MediaResult orientationMedia;
                orientationMedia.blocks.push_back(orientation);
                const MediaResult media = MediaResult::conjoin(rule.media, orientationMedia);
                std::vector<OutDecl> converted = vmin;
                for (OutDecl& d : converted)
                    d.value = replaceViewportOrientation(d.value, landscape);
                writeUnderMedia(writer, media, [&](){
                    writeBlock(writer, joinSelectors(plain), converted, rule.ref);
                });
            }
        }
    }

    std::vector<size_t> writeOrder(const Session& session, uint32_t sheet){
        const std::vector<SheetItem>& items = session.sheets[sheet];
        std::vector<size_t> order(items.size());
        for (size_t i = 0; i < order.size(); i++)
            order[i] = i;
        auto layerOf = [&](const SheetItem& item){
            return item.atRule ? session.atRules[item.index].layer : session.rules[item.index].layer;
        };
        auto orderOf = [&](const SheetItem& item){
            return item.atRule ? session.atRules[item.index].order : session.rules[item.index].order;
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b){
            const int la = layerOf(items[a]);
            const int lb = layerOf(items[b]);
            // named layers first in declaration order, unlayered last
            const int ka = la < 0 ? 1 << 30 : la;
            const int kb = lb < 0 ? 1 << 30 : lb;
            if (ka != kb)
                return ka < kb;
            return orderOf(items[a]) < orderOf(items[b]);
        });
        return order;
    }

    void importantConflicts(Session& session, const FlatRule& rule, const OutDecl&, const std::string& property, SourceRef ref){
        const auto it = session.propertyRules.find(property);
        if (it == session.propertyRules.end())
            return;
        for (const Session::PropertyUse& use : it->second){
            if (use.rule == rule.index || use.important)
                continue;
            const FlatRule& other = session.rules[use.rule];
            if (other.dropped)
                continue;
            for (const RewrittenSelector& mine : rule.rewritten){
                for (const RewrittenSelector& theirs : other.rewritten){
                    if (theirs.specificity > mine.specificity && SelectorRewriter::subjectsOverlap(mine.source, theirs.source)){
                        session.sink->report(codes::ImportantMayLose, ref, "'" + property + " !important' may lose to the more specific rule '" +
                                             SelectorRewriter::serialize(theirs.source) + "' at " + BaseRules::location(session, other) +
                                             " (the engine has no !important)", "raise this selector's specificity or lower the other one");
                        return;
                    }
                }
            }
        }
    }

    // the property name a declaration is read as (vendor prefixes resolved); "" + reported when dropped
    std::string effectiveName(Session& session, const Declaration& d, const std::vector<Declaration>& block, bool& silentlyDropped){
        silentlyDropped = false;
        if (findProperty(d.name))
            return d.name;
        for (const char* prefix : {"-webkit-", "-moz-", "-ms-", "-o-"}){
            const std::string p(prefix);
            if (d.name.compare(0, p.size(), p) != 0)
                continue;
            const std::string base = d.name.substr(p.size());
            if (!findProperty(base))
                continue;
            for (const Declaration& other : block){
                if (other.name == base){
                    silentlyDropped = true;
                    return std::string();
                }
            }
            session.sink->report(codes::VendorPrefix, d.nameRef.valid() ? d.nameRef : d.ref, "'" + d.name + "' is read as '" + base + "'",
                                 "write '" + base + "'", base, d.name);
            return base;
        }
        return std::string();
    }

    bool isInfoNote(const char* note){
        return note && std::strstr(note, "(Info)") != nullptr;
    }

    // one declaration through the pipeline of §3.9.6
    void processOne(Session& session, const FlatRule* rule, RuleOrigin origin, bool keyframe, const Declaration& d,
                    const std::vector<Declaration>& block, RuleOutput& output){
        if (session.options.dropTransitionsForTesting &&
            (d.name == "transition" || d.name.compare(0, 11, "transition-") == 0 || d.name.compare(0, 18, "-webkit-transition") == 0))
            return;
        const size_t first = output.declarations.size();

        // 1. custom properties
        if (d.isCustomProperty()){
            NormalizeContext context;
            context.property = d.name;
            context.customProperty = true;
            context.options = session.options;
            context.inlineStyle = origin == RuleOrigin::StyleAttribute;
            context.report = [&](DiagCode code, Severity severity, const std::string& message, const std::string& suggestion){
                session.sink->reportAs(severity, code, d.ref, message, suggestion, std::string(), d.name);
            };
            NormalizeResult normalized = ValueNormalizer::normalize(d.value, context);
            if (!normalized.ok)
                return;
            if (normalized.dependsOnViewport)
                session.result->dependsOnViewport = true;
            std::vector<ComponentValue> value = trimWhitespace(normalized.value);
            for (const ComponentValue& v : value){
                if (v.isBlock(TokenType::LeftBrace)){
                    session.sink->report(codes::UnsupportedValue, d.ref, "custom property '" + d.name +
                                         "' holds a {} block, which the engine's style sheets cannot hold; dropped", std::string(), std::string(), d.name);
                    return;
                }
            }
            // RmlUi reads a quoted string only as a whole value: in a list ("Inter", sans-serif)
            // the strings are written unquoted (exact for font lists, the common case)
            size_t components = 0;
            bool hasString = false;
            for (const ComponentValue& v : value){
                if (!v.isWhitespace())
                    components++;
                if (v.isToken(TokenType::String))
                    hasString = true;
            }
            if (hasString && components > 1){
                for (ComponentValue& v : value){
                    if (v.isToken(TokenType::String)){
                        std::string inner;
                        for (char c : v.token.value){
                            if (c != '"' && c != '\\' && c != ';' && c != '}' && c != '{')
                                inner += c;
                        }
                        // written raw (an ident would escape the spaces)
                        v.token.type = TokenType::Delim;
                        v.token.value = inner;
                    }
                }
            }
            std::string text = serialize(value);
            std::vector<std::pair<std::string, std::string>> copies;
            for (const std::string& familyName : session.vars.families(d.name)){
                const VarFamily* family = findVarFamily(familyName);
                if (!family)
                    continue;
                // only a variable whose every declaration has one kind is written in that kind
                const ComponentValue reference = ValueNormalizer::function("var", {ValueNormalizer::ident(d.name)});
                const std::optional<TypedVar> typed = session.typedVar(reference, familyName);
                if (!typed || typed->kind == "color")
                    continue;
                const std::string kind = family->classify(session, value);
                if (kind != typed->kind)
                    continue;
                std::optional<std::string> transformed = family->transform(session, value, kind, d.ref);
                if (!transformed)
                    continue;
                if (session.vars.inPlace(d.name))
                    text = *transformed;
                else
                    copies.emplace_back(session.vars.typedName(d.name, familyName), *transformed);
            }
            if (text.empty())
                text = " ";
            OutDecl out{d.name, text, d.ref, false, false};
            output.declarations.push_back(out);
            for (const auto& copy : copies)
                output.declarations.push_back(OutDecl{copy.first, copy.second, d.ref, false, false});
            return;
        }

        // 2. vendor prefixes, 3. lookup
        bool silentlyDropped = false;
        std::string name = effectiveName(session, d, block, silentlyDropped);
        if (silentlyDropped)
            return;
        if (name.empty()){
            const std::string closest = closestProperty(d.name);
            session.sink->report(codes::UnknownProperty, d.nameRef.valid() ? d.nameRef : d.ref, "'" + d.name + "' is not a CSS property; dropped",
                                 closest.empty() ? std::string() : "did you mean '" + closest + "'?", closest, d.name);
            return;
        }
        const PropertySupport* row = findProperty(name);
        const Support support = effectiveSupport(*row, session.options.fork);
        if (support == Support::Unsupported){
            Severity severity = isInfoNote(row->note) ? Severity::Info : Severity::Warning;
            std::string suggestion = row->suggestion;
            if (name == "aspect-ratio" && rule){
                for (const ComplexSelector& s : rule->selectors){
                    const std::string tag = SelectorRewriter::subjectTag(s);
                    if (tag == "img" || tag == "svg" || tag == "video")
                        severity = Severity::Info;
                }
            }
            if ((name == "counter-reset" || name == "counter-increment" || name == "counter-set")){
                bool onlyListItem = true;
                for (const ComponentValue& v : d.value){
                    if (v.isToken(TokenType::Ident) && !equalsNoCase(v.token.value, "list-item") && !equalsNoCase(v.token.value, "none"))
                        onlyListItem = false;
                }
                if (onlyListItem)
                    severity = Severity::Info;
            }
            session.sink->reportAs(severity, codes::UnsupportedProperty, d.ref, "'" + name + "' is not supported; dropped", suggestion,
                                   std::string(), name);
            return;
        }
        if (support == Support::Ignored){
            session.sink->report(codes::IgnoredProperty, d.ref, "'" + name + "' has no effect in the engine; dropped", row->suggestion,
                                 std::string(), name);
            return;
        }

        DeclContext context{session, rule, origin, keyframe, d, name, row, {}, false, output, false};
        context.important = d.important && !keyframe;

        // the color property's own currentColor is `inherit`
        std::vector<ComponentValue> trimmed = trimWhitespace(d.value);
        if (name == "color" && trimmed.size() == 1 && trimmed[0].isToken(TokenType::Ident) && equalsNoCase(trimmed[0].token.value, "currentcolor")){
            context.emit("color", std::string("var(") + companions::kUnset + ")");
        }else if (name != "all" && !CascadeKeywords::keywordOf(d.value).empty()){
            // 5. CSS-wide keywords
            CascadeKeywords::apply(context, CascadeKeywords::keywordOf(d.value));
        }else{
            // 4. value normalization
            NormalizeContext normalize;
            normalize.property = name;
            normalize.colors = ValueNormalizer::colorKeywordsApply(name);
            normalize.keepCurrentColorKeyword = row->rewrite == RewriteId::SvgPaint;
            normalize.inlineStyle = origin == RuleOrigin::StyleAttribute;
            normalize.percentBasis = ValueNormalizer::percentBasisOf(name, &normalize.vertical);
            normalize.options = session.options;
            if (rule && !keyframe){
                const std::string staticColor = props::ruleStaticColor(session, *rule);
                if (name != "color")
                    normalize.staticColor = staticColor;
                if (std::optional<double> fontSize = props::ruleFontSizePx(session, *rule); fontSize && name != "font-size")
                    normalize.emPx = *fontSize;
            }
            normalize.remPx = session.rootFontSize.value_or(16.0);
            normalize.resolveVar = [&session](const ComponentValue& var, std::vector<ComponentValue>& out){ return session.staticVar(var, out); };
            normalize.report = [&](DiagCode code, Severity severity, const std::string& message, const std::string& suggestion){
                session.sink->reportAs(severity, code, d.valueRef.valid() ? d.valueRef : d.ref, message, suggestion, std::string(), name);
            };
            NormalizeResult normalized = ValueNormalizer::normalize(d.value, normalize);
            if (!normalized.ok)
                return;
            if (normalized.dependsOnViewport)
                session.result->dependsOnViewport = true;
            context.value = trimWhitespace(normalized.value);
            if (context.value.empty())
                return;

            // 6. the property rewrite
            if (name == "transform" && rule && ruleHasIndividualTransform(*rule)){
                // written with translate / rotate / scale (Transform.cpp)
            }else if (row->rewrite != RewriteId::None){
                if (RewriteFunction function = findRewrite(row->rewrite))
                    function(context);
            }else{
                if (name == "transform-origin" || name == "perspective-origin"){
                    // RmlUi reads the horizontal keyword first ("top left" -> "left top")
                    std::vector<ComponentValue> w;
                    for (const ComponentValue& v : context.value){
                        if (!v.isWhitespace())
                            w.push_back(v);
                    }
                    auto vertical = [](const ComponentValue& v){ return v.isToken(TokenType::Ident) && (equalsNoCase(v.token.value, "top") || equalsNoCase(v.token.value, "bottom")); };
                    auto horizontal = [](const ComponentValue& v){ return v.isToken(TokenType::Ident) && (equalsNoCase(v.token.value, "left") || equalsNoCase(v.token.value, "right")); };
                    if (w.size() >= 2 && (vertical(w[0]) || horizontal(w[1])) && !(vertical(w[1]) || horizontal(w[0])))
                        std::swap(w[0], w[1]);
                    else if (w.size() == 1 && vertical(w[0]))
                        w.insert(w.begin(), ValueNormalizer::ident("center"));
                    context.value.clear();
                    for (size_t i = 0; i < w.size(); i++){
                        if (i)
                            context.value.push_back(ValueNormalizer::whitespace());
                        context.value.push_back(w[i]);
                    }
                }
                const std::string text = context.valueText();
                // an unsupported keyword from the value table
                if (context.value.size() == 1 && context.value[0].isToken(TokenType::Ident)){
                    if (const ValueSupport* value = findValue(name, lowerAscii(context.value[0].token.value))){
                        if (effectiveSupport(*value, session.options.fork) == Support::Unsupported){
                            context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + name + ": " + text + "' is not supported; declaration dropped",
                                             value->suggestion);
                            return;
                        }
                    }
                }
                if (!RcssGrammar::known(name)){
                    context.reportAs(Severity::Warning, codes::UnsupportedProperty, "'" + name + "' is not supported by the engine; dropped", row->suggestion);
                    return;
                }
                if (!RcssGrammar::accepts(name, text)){
                    std::string suggestion;
                    const std::string keywords = RcssGrammar::keywords(name);
                    if (!keywords.empty())
                        suggestion = "use one of: " + keywords;
                    context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + name + ": " + text + "' is not supported; declaration dropped",
                                     suggestion);
                    return;
                }
                context.emit(name, text);
            }
        }
        if (context.dropped)
            output.declarations.resize(first);

        // 7. !important, companions
        std::vector<OutDecl> extra;
        for (size_t i = first; i < output.declarations.size(); i++){
            OutDecl& out = output.declarations[i];
            if (out.final)
                continue;
            out.final = true;
            if (d.important){
                if (keyframe){
                    out.important = false;
                }else if (session.options.fork.has(ForkFeature::Important)){
                    out.important = false;
                    out.value += " !important";
                }else if (origin == RuleOrigin::StyleAttribute){
                    out.important = false;
                }else{
                    out.important = true;
                }
            }
            if (keyframe)
                continue;
            const bool keyword = !CascadeKeywords::keywordOf(parseComponentValueList(out.value)).empty();
            if (out.name == "color" && out.value.compare(0, 4, "var(") != 0 && !keyword)
                extra.push_back(OutDecl{companions::kCurrentColor, out.value, out.ref, out.important, false, true});
            if (session.inheritCompanions.count(out.name) && out.value.find("--html-inh-") == std::string::npos && !keyword)
                extra.push_back(OutDecl{companions::inheritName(out.name), out.value, out.ref, out.important, false, true});
        }
        for (OutDecl& e : extra)
            output.declarations.push_back(std::move(e));

        if (d.important && rule && !keyframe && origin == RuleOrigin::Author && !session.options.fork.has(ForkFeature::Important) &&
            output.declarations.size() > first)
            importantConflicts(session, *rule, output.declarations[first], name, d.ref);
        if (d.important && origin == RuleOrigin::StyleAttribute){
            const auto it = session.propertyRules.find(name);
            if (it != session.propertyRules.end()){
                for (const Session::PropertyUse& use : it->second){
                    if (use.important){
                        session.sink->reportAs(Severity::Info, codes::ImportantMayLose, d.ref,
                                               "the inline '" + name + "' wins over the author rule's !important at " +
                                               BaseRules::location(session, session.rules[use.rule]) + " here (in CSS the !important rule wins)");
                        break;
                    }
                }
            }
        }
    }

}

void jasperengine::htmlui::compat::processDeclarations(Session& session, const FlatRule* rule, RuleOrigin origin, bool keyframe,
                                                       const std::vector<Declaration>& declarations, RuleOutput& output){
    for (const Declaration& d : declarations)
        processOne(session, rule, origin, keyframe, d, declarations, output);
}

void jasperengine::htmlui::compat::processSynthetic(DeclContext& context, const std::string& property, const std::string& value){
    Declaration synthetic;
    synthetic.name = property;
    synthetic.value = trimWhitespace(parseComponentValueList(value));
    synthetic.ref = context.declaration.ref;
    synthetic.nameRef = context.declaration.nameRef;
    synthetic.valueRef = context.declaration.valueRef;
    synthetic.important = context.declaration.important;
    RuleOutput output;
    processOne(context.session, context.rule, context.origin, context.keyframe, synthetic, {synthetic}, output);
    for (OutDecl& d : output.declarations){
        d.ref = context.declaration.ref;
        context.output.declarations.push_back(std::move(d));
    }
    for (GeneratedRule& g : output.generated)
        context.output.generated.push_back(std::move(g));
}

// ---- CssRewriter

struct CssRewriter::Impl {
    Impl(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, RewriteContext context):
        options(options), files(files), sink(sink), context(std::move(context)) {}

    CompatOptions options;
    SourceFiles& files;
    DiagnosticSink& sink;
    RewriteContext context;
};

CssRewriter::CssRewriter(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, RewriteContext context):
    impl(std::make_unique<Impl>(options, files, sink, std::move(context))) {
}

CssRewriter::~CssRewriter() = default;

RewriteResult CssRewriter::rewrite(Span<const StylesheetInput> inputs){
    RewriteResult result;
    result.importantSheet.kind = StylesheetInput::Kind::Inline;
    result.importantSheet.sourcePath = impl->context.documentPath;

    Session session(impl->options, impl->files, impl->sink, impl->context);
    session.result = &result;
    session.sheets.resize(inputs.size());

    // ---- phase A: parse and flatten
    std::vector<std::vector<Declaration>> styleAttributes(inputs.size());
    std::vector<std::vector<Declaration>> styleAttributeList;
    for (size_t i = 0; i < inputs.size(); i++){
        const StylesheetInput& input = inputs[i];
        session.inputs.push_back(&input);
        if (input.kind == StylesheetInput::Kind::StyleAttribute){
            styleAttributes[i] = parseDeclarationList(input.file, input.text, input.baseOffset, impl->sink);
            styleAttributeList.push_back(styleAttributes[i]);
            continue;
        }
        if (input.kind == StylesheetInput::Kind::File){
            const std::string path = impl->files.path(input.file);
            if (!path.empty())
                result.dependencies.push_back(path);
        }
        const Stylesheet sheet = parseStylesheet(input.file, input.text, impl->sink, input.baseOffset);
        FlattenScope scope;
        scope.sheet = (uint32_t)i;
        scope.file = input.file;
        scope.origin = input.kind == StylesheetInput::Kind::UserAgent ? RuleOrigin::UserAgent : RuleOrigin::Author;
        if (!input.media.empty()){
            SourceRef ref{input.file, input.baseOffset, 0};
            scope.media = MediaQuery::normalize(input.media, impl->options, &impl->sink, ref);
        }
        flatten(session, sheet.rules, scope, true);
    }

    // ---- pre-pass
    prePass(session, styleAttributeList);

    // ---- phase B: rewrite and write
    for (size_t i = 0; i < inputs.size(); i++){
        const StylesheetInput& input = inputs[i];
        StylesheetOutput output;
        output.kind = input.kind;
        output.elementLoc = input.elementLoc;
        output.sourcePath = input.kind == StylesheetInput::Kind::File ? impl->files.path(input.file)
                                                                       : (impl->context.documentPath.empty() ? impl->files.path(input.file)
                                                                                                             : impl->context.documentPath);
        if (input.kind == StylesheetInput::Kind::StyleAttribute){
            RuleOutput out;
            processDeclarations(session, nullptr, RuleOrigin::StyleAttribute, false, styleAttributes[i], out);
            RcssWriter writer(RcssWriter::Mode::DeclarationList);
            for (const OutDecl* d : effective(out.declarations))
                writer.declaration(d->name, d->value, d->ref);
            output.rcss = writer.take(output.lines);
            if (!out.generated.empty()){
                // rules a style attribute generates (grid children, scrollbars) select the element by its location
                const std::string selector = "#htmlui-root [data-htmlui-loc=\"" + std::to_string(input.elementLoc) + "\"]";
                for (const GeneratedRule& g : out.generated){
                    StylesheetOutput& sheet = result.importantSheet;
                    RcssWriter w;
                    writeBlock(w, selector + g.suffix, g.declarations, g.declarations.empty() ? SourceRef() : g.declarations.front().ref);
                    RcssLineMap lines;
                    const std::string text = w.take(lines);
                    sheet.rcss += text;
                    sheet.lines.lines.insert(sheet.lines.lines.end(), lines.lines.begin(), lines.lines.end());
                }
            }
            result.sheets.push_back(std::move(output));
            continue;
        }
        RcssWriter writer;
        for (size_t index : writeOrder(session, (uint32_t)i)){
            const SheetItem& item = session.sheets[i][index];
            if (item.atRule){
                const FlatAtRule& at = session.atRules[item.index];
                if (!at.media.never)
                    AtRules::write(session, at, writer);
                continue;
            }
            const FlatRule& rule = session.rules[item.index];
            if (rule.dropped)
                continue;
            writeRule(session, rule, writer);
        }
        output.rcss = writer.take(output.lines);
        if (input.kind == StylesheetInput::Kind::File && !impl->context.virtualDirectory.empty())
            output.virtualPath = impl->context.virtualDirectory + output.sourcePath + ".rcss";
        result.sheets.push_back(std::move(output));
    }

    // ---- the !important sheet
    if (!session.important.empty()){
        RcssWriter writer;
        for (const Session::ImportantRule& important : session.important){
            const FlatRule& rule = *important.rule;
            std::vector<std::string> selectors;
            for (const RewrittenSelector& s : rule.rewritten)
                selectors.push_back(s.text);
            writeUnderMedia(writer, rule.media, [&](){
                writeBlock(writer, joinSelectors(selectors), important.declarations, rule.ref);
            });
        }
        RcssLineMap lines;
        std::string text = writer.take(lines);
        result.importantSheet.rcss = text + result.importantSheet.rcss;
        lines.lines.insert(lines.lines.end(), result.importantSheet.lines.lines.begin(), result.importantSheet.lines.lines.end());
        result.importantSheet.lines = std::move(lines);
    }

    // keyframes needing AnimationFill
    for (const std::string& name : session.fillKeyframes)
        result.animationFillNames.push_back(name);

    std::sort(result.dependencies.begin(), result.dependencies.end());
    result.dependencies.erase(std::unique(result.dependencies.begin(), result.dependencies.end()), result.dependencies.end());
    auto unique = [](std::vector<std::string>& list){
        std::vector<std::string> out;
        for (const std::string& s : list){
            if (std::find(out.begin(), out.end(), s) == out.end())
                out.push_back(s);
        }
        list = std::move(out);
    };
    unique(result.fontFamilies);
    unique(result.declaredFontFamilies);
    unique(result.animationFillNames);
    return result;
}

std::string CssRewriter::rewriteSelector(std::string_view selector, DiagnosticSink* sink, const CompatOptions& options){
    SourceFiles files;
    const uint32_t file = files.add("[selector]", std::string(selector));
    DiagnosticSink local(files);
    std::string error;
    std::optional<std::vector<ComplexSelector>> list = SelectorRewriter::parseList(parseComponentValueList(files.text(file)), file, &error);
    std::string result;
    if (!list){
        local.report(codes::InvalidSelector, SourceRef{file, 0, (uint32_t)selector.size()}, "invalid selector '" + std::string(selector) + "' (" + error + ")");
    }else{
        SelectorOptions selectorOptions;
        selectorOptions.origin = SelectorOptions::Origin::Query;
        selectorOptions.fork = options.fork;
        SelectorRewriteResult rewritten = SelectorRewriter::rewrite(*list, selectorOptions, &local, SourceRef{file, 0, (uint32_t)selector.size()});
        if (!rewritten.invalid){
            for (const RewrittenSelector& s : rewritten.selectors)
                result += (result.empty() ? "" : ", ") + s.text;
        }
    }
    if (sink){
        for (Diagnostic d : local.diagnostics()){
            d.file.clear();
            d.line = d.column = d.length = 0;
            sink->report(d);
        }
    }
    return result;
}

std::string CssRewriter::rewriteDeclarations(std::string_view css, DiagnosticSink* sink, const CompatOptions& options){
    SourceFiles files;
    const uint32_t file = files.add("[style]", std::string(css));
    DiagnosticSink local(files);
    RewriteContext context;
    CssRewriter rewriter(options, files, local, context);
    StylesheetInput input;
    input.kind = StylesheetInput::Kind::StyleAttribute;
    input.file = file;
    input.text = files.text(file);
    RewriteResult result = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
    if (sink){
        for (Diagnostic d : local.diagnostics()){
            d.file.clear();
            sink->report(d);
        }
    }
    return result.sheets.empty() ? std::string() : result.sheets[0].rcss;
}
