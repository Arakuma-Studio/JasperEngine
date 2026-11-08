// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CSS -> RCSS rewriter (phase-2 §3.9): one session per document. Phase A parses every
// sheet and flattens it (@import inlined, @supports evaluated, nesting resolved, @layer
// ordered, media normalized) into style rules in cascade order; a pre-pass rewrites the
// selectors and collects what the declaration pipeline needs document-wide (custom property
// index, companions in use, keyframe names, base rules); phase B runs every declaration
// through the pipeline of §3.9.6 and writes the RCSS with its line map.

#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/LiteralCompare.h"
#include "htmlui/compat/CompatAtProperty.h"

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
#include <cctype>
#include <functional>
#include <atomic>
#include <array>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

using namespace jasperengine::htmlui::compat;

namespace {

    // the declaration memo's mode (-1: not read from the environment yet) and what Check found
    std::atomic<int> gMemoMode{-1};
    std::atomic<uint64_t> gMemoMismatches{0};
    std::atomic<uint64_t> gMemoHits{0};

}

void CssRewriter::setDeclarationMemo(DeclarationMemo mode){
    gMemoMode.store((int)mode, std::memory_order_relaxed);
}

CssRewriter::DeclarationMemo CssRewriter::declarationMemo(){
    int mode = gMemoMode.load(std::memory_order_relaxed);
    if (mode < 0){
        const char* value = std::getenv("JASPER_HTMLUI_CSS_MEMO");
        mode = (int)DeclarationMemo::On;
        if (value && std::strcmp(value, "off") == 0)
            mode = (int)DeclarationMemo::Off;
        else if (value && std::strcmp(value, "check") == 0)
            mode = (int)DeclarationMemo::Check;
        gMemoMode.store(mode, std::memory_order_relaxed);
    }
    return (DeclarationMemo)mode;
}

uint64_t CssRewriter::declarationMemoMismatches(){
    return gMemoMismatches.load(std::memory_order_relaxed);
}

uint64_t CssRewriter::declarationMemoHits(){
    return gMemoHits.load(std::memory_order_relaxed);
}

namespace {
    void boxShorthandCompanions(Session& session, const OutDecl& out, ArenaList<OutDecl>& extra);
}

// ---- DeclContext

void DeclContext::emit(Text property, Text value){
    emit(property, value, declaration.ref);
}

void DeclContext::emit(Text property, Text value, SourceRef ref){
    OutDecl& out = output.declarations.emplace_back();
    out.name = property;
    out.value = value;
    out.ref = ref.valid() ? ref : declaration.ref;
    out.important = important;
}

void DeclContext::generate(const std::string& suffix, const std::string& property, const std::string& value){
    session.memoTaint++;   // (a generated rule: not a declaration of the rule's own block)
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
    session.memoTaint++;   // (of the declaration as written, which a made declaration does not have)
    return declaration.name + ": " + serialize(trimWhitespace(declaration.value));
}

bool DeclContext::fork(ForkFeature feature) const{
    return session.options.fork.has(feature);
}

// ---- FlatRule

const SelectorList& FlatRule::selectors() const{
    if (!selectorsParsed){
        // (a scanned list: valid, so it parses; a deferred prelude is read from its text)
        std::optional<SelectorList> parsed = source ? SelectorRewriter::parseList(preludeOf(*source), file) : std::nullopt;
        if (parsed)
            parsedSelectors = std::move(*parsed);
        selectorsParsed = true;
    }
    return parsedSelectors;
}

std::string_view FlatRule::selectorText(size_t i, std::string& buffer) const{
    if (scanned)
        return selectorTexts[i].view();
    buffer.clear();
    SelectorRewriter::appendSerialized(buffer, selectors()[i]);
    return buffer;
}

// ---- Session

Session::Session(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, const RewriteContext& context):
    options(options), files(files), sink(&sink), context(context), urls(context.files, context.manifest) {
    // (the storage the thread's previous session left, if any: in use, no longer kept)
    auto take = [](auto& list, auto& spare){
        SpareStorage::take(spare.capacity() * sizeof(spare[0]));
        list.swap(spare);
    };
    take(rules, sessionSpare<FlatRule, 0>());
    take(baseEntries, sessionSpare<BaseEntry, 0>());
    take(baseSlots, sessionSpare<uint32_t, 0>());
    take(propertyUses, sessionSpare<PendingUse, 0>());
    take(memoEntries, sessionSpare<MemoEntry, 0>());
    take(memoSlots, sessionSpare<uint32_t, 1>());
    mainSink = &sink;
    const CssRewriter::DeclarationMemo memoMode = CssRewriter::declarationMemo();
    memoDisabled = memoMode == CssRewriter::DeclarationMemo::Off;
    memoCheck = memoMode == CssRewriter::DeclarationMemo::Check;
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
    // p2-html's fields: the <html> id and dir
    if (!context.rootId.empty()){
        selectorOptions.rootId = context.rootId;
        if (std::find(selectorOptions.rootAttributes.begin(), selectorOptions.rootAttributes.end(), "id") == selectorOptions.rootAttributes.end())
            selectorOptions.rootAttributes.push_back("id");
    }
    if (!context.rootDirection.empty()){
        rootDirection = lowerAscii(context.rootDirection);
        if (std::find(selectorOptions.rootAttributes.begin(), selectorOptions.rootAttributes.end(), "dir") == selectorOptions.rootAttributes.end())
            selectorOptions.rootAttributes.push_back("dir");
    }
    userAgentSelectorOptions = selectorOptions;
    userAgentSelectorOptions.origin = SelectorOptions::Origin::UserAgent;
}

Session::~Session(){
    // the lists' storage is left to the thread's next session, within kSpareKeptBytes (what is
    // not kept is freed with the session)
    auto keep = [](auto& list, auto& spare){
        list.clear();
        if (list.capacity() > spare.capacity() &&
            SpareStorage::keep(spare.capacity() * sizeof(spare[0]), list.capacity() * sizeof(list[0])))
            spare.swap(list);
    };
    keep(rules, sessionSpare<FlatRule, 0>());
    keep(baseEntries, sessionSpare<BaseEntry, 0>());
    keep(baseSlots, sessionSpare<uint32_t, 0>());
    keep(propertyUses, sessionSpare<PendingUse, 0>());
    keep(memoEntries, sessionSpare<MemoEntry, 0>());
    keep(memoSlots, sessionSpare<uint32_t, 1>());
}

const PropertyFacts& Session::facts(std::string_view property){
    return propertyFacts.get(property, [](std::string_view name){
        PropertyFacts f;
        f.row = findProperty(name);
        const std::string text(name);
        f.colorKeywords = ValueNormalizer::colorKeywordsApply(text);
        f.percentBasis = ValueNormalizer::percentBasisOf(text, &f.vertical);
        f.rcssKnown = RcssGrammar::known(name);
        return f;
    });
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

bool Session::staticVar(const ComponentValue& var, ValueList& out) const{
    memoVarReads++;
    // a runtime variable where a rewrite needs one static value: 0px (spec: safe-area 6.4)
    if (CustomPropertyIndex::isDynamic(CustomPropertyIndex::varName(var))){
        dynamicVarsFolded++;
        out = CustomPropertyIndex::dynamicStaticValue();
        return true;
    }
    return vars.resolve(var, out);
}

std::optional<TypedVar> Session::typedVar(const ComponentValue& var, const std::string& family){
    memoVarReads++;
    ValueList fallback;
    bool hasFallback = false;
    const std::string name = CustomPropertyIndex::varName(var, &fallback, &hasFallback);
    const VarFamily* f = findVarFamily(family);
    if (name.empty() || !f)
        return std::nullopt;
    std::vector<ValueList> values;
    if (!vars.declared(name)){
        if (!hasFallback)
            return std::nullopt;
        values.push_back(fallback);
    }else if (!vars.expandedValues(name, values)){
        return std::nullopt;
    }
    std::string kind;
    for (const ValueList& value : values){
        const std::string k = f->classify(*this, value);
        if (k.empty())
            return std::nullopt;
        if (!kind.empty() && k != kind)
            return std::nullopt;
        kind = k;
    }
    // a declaration that itself references a variable cannot be written in another kind; background layers without
    // a url() are written as declared (only their urls are rewritten), whatever variables they reference
    std::function<bool(const ValueList&)> hasUrl = [&hasUrl](const ValueList& value){
        for (const ComponentValue& v : value){
            if (v.isToken(TokenType::Url) || v.isFunction("url") || v.isFunction("src") || v.isFunction("image-set") ||
                v.isFunction("-webkit-image-set") || hasUrl(v.children))
                return true;
        }
        return false;
    };
    const bool asDeclared = (kind == "layers" && std::none_of(values.begin(), values.end(), hasUrl));
    if (const std::vector<CustomPropertyIndex::Entry>* entries = asDeclared ? nullptr : vars.find(name)){
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

    // text.compare(0, prefix.size(), prefix) == 0
    inline bool startsWith(std::string_view text, std::string_view prefix){
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

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
        const SelectorList* parents = nullptr;
        std::vector<uint32_t> importChain;
        bool startingStyle = false;             // inside @starting-style (AnimationLonghands)
    };

    int registerLayer(Session& session, const std::string& name){
        for (size_t i = 0; i < session.layerOrder.size(); i++){
            if (session.layerOrder[i] == name)
                return (int)i;
        }
        // "a.b" declares "a" first (CSS Cascade 5)
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0)
            registerLayer(session, name.substr(0, dot));
        session.layerOrder.push_back(name);
        return (int)session.layerOrder.size() - 1;
    }

    // the cascade rank of each layer, lowest first: sibling layers in declaration order, a
    // layer's sublayers before the layer's own rules (post-order over the layer tree)
    std::vector<int> layerRanks(const Session& session){
        const std::vector<std::string>& names = session.layerOrder;
        std::vector<std::vector<size_t>> children(names.size());
        std::vector<size_t> roots;
        for (size_t i = 0; i < names.size(); i++){
            const size_t dot = names[i].rfind('.');
            size_t parent = names.size();
            if (dot != std::string::npos && dot > 0){
                const std::string parentName = names[i].substr(0, dot);
                for (size_t j = 0; j < names.size(); j++){
                    if (names[j] == parentName)
                        parent = j;
                }
            }
            (parent < names.size() ? children[parent] : roots).push_back(i);
        }
        std::vector<int> rank(names.size(), 0);
        int next = 0;
        std::function<void(size_t)> visit = [&](size_t i){
            for (size_t child : children[i])
                visit(child);
            rank[i] = next++;
        };
        for (size_t root : roots)
            visit(root);
        return rank;
    }

    void addRule(Session& session, const FlattenScope& scope, SelectorList selectors,
                 DeclarationList declarations, SourceRef ref, SourceRef preludeRef){
        if (declarations.empty() || scope.media.never)
            return;
        const uint32_t index = (uint32_t)session.rules.size();
        FlatRule& rule = session.rules.emplace_back();
        rule.index = index;
        rule.sheet = scope.sheet;
        rule.file = scope.file;
        rule.origin = scope.origin;
        rule.parsedSelectors = std::move(selectors);
        rule.selectorsParsed = true;
        rule.ref = ref;
        rule.preludeRef = preludeRef;
        rule.media = scope.media;
        rule.declarations = std::move(declarations);
        rule.layer = scope.layer;
        rule.startingStyle = scope.startingStyle;
        rule.order = session.nextOrder++;
        session.sheets[scope.sheet].push_back(SheetItem{false, rule.index});
    }

    // a rule whose selector list was scanned (SelectorRewriter::scanList): its rewritten
    // selectors are final, its selectors are parsed from the prelude when something reads them
    void addScannedRule(Session& session, const FlattenScope& scope, Rule& source, ScannedSelectors& scanned){
        if (source.declarations.empty() || scope.media.never)
            return;
        const uint32_t index = (uint32_t)session.rules.size();
        FlatRule& rule = session.rules.emplace_back();
        rule.index = index;
        rule.sheet = scope.sheet;
        rule.file = scope.file;
        rule.origin = scope.origin;
        rule.source = &source;
        rule.scanned = true;
        rule.selectorTexts = std::move(scanned.texts);
        rule.rewritten = std::move(scanned.rewritten);
        rule.ref = source.ref;
        rule.preludeRef = source.preludeRef;
        rule.media = scope.media;
        rule.declarations = std::move(source.declarations);
        rule.layer = scope.layer;
        rule.startingStyle = scope.startingStyle;
        rule.order = session.nextOrder++;
        session.sheets[scope.sheet].push_back(SheetItem{false, rule.index});
    }

    // MediaQuery::normalize of an @media rule's prelude, kept by the prelude's text when it
    // reported nothing (the result depends on the text and the options alone)
    MediaResult mediaOf(Session& session, const Rule& rule){
        if (rule.preludeText.empty())
            return MediaQuery::normalize(rule.prelude, rule.preludeRef, session.options, session.sink);
        bool made = false;
        MediaResult fresh;
        std::optional<MediaResult>& kept = session.mediaMemo.get(rule.preludeText, [&](std::string_view){
            const uint64_t reports = session.sink->reports();
            fresh = MediaQuery::normalize(rule.prelude, rule.preludeRef, session.options, session.sink);
            made = true;
            return session.sink->reports() == reports ? std::optional<MediaResult>(fresh) : std::optional<MediaResult>();
        });
        if (made)
            return fresh;
        if (kept)
            return *kept;
        return MediaQuery::normalize(rule.prelude, rule.preludeRef, session.options, session.sink);
    }

    void flatten(Session& session, RuleList& rules, const FlattenScope& scope, bool topLevel);

    // a style rule: its declarations and nested rules in source order (CSS Nesting)
    void flattenStyleRule(Session& session, Rule& rule, const FlattenScope& scope){
        // a selector list that maps to itself is written from its text (no nested rules: they
        // need the parsed selectors)
        if (!scope.parents && rule.rules.empty() && !rule.preludeText.empty() && scope.origin != RuleOrigin::StyleAttribute){
            const SelectorOptions& options = scope.origin == RuleOrigin::UserAgent ? session.userAgentSelectorOptions : session.selectorOptions;
            ScannedSelectors scanned;
            if (SelectorRewriter::scanList(rule.preludeText, options, scanned)){
                addScannedRule(session, scope, rule, scanned);
                return;
            }
        }
        std::string error;
        SourceRef errorRef;
        const ValueList prelude = preludeOf(rule);
        std::optional<SelectorList> parsed = SelectorRewriter::parseList(prelude, scope.file, &error, &errorRef);
        if (!parsed){
            session.sink->report(codes::InvalidSelector, rule.preludeRef.valid() ? rule.preludeRef : rule.ref,
                                 "invalid selector '" + serialize(trimWhitespace(prelude)) + "' (" + error + "); the rule is dropped",
                                 "fix the selector syntax", std::string(), "selector");
            return;
        }
        SelectorList selectors = std::move(*parsed);
        if (scope.parents){
            std::optional<SelectorList> combined = Nesting::combine(*scope.parents, selectors, session.options.fork);
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

        // no nested rules (the common case): the declarations, in source order already, are the rule
        if (rule.rules.empty()){
            addRule(session, scope, std::move(selectors), std::move(rule.declarations), rule.ref, rule.preludeRef);
            return;
        }

        // declarations and nested rules in source order
        struct Item {
            uint32_t offset;
            Declaration* declaration;
            Rule* rule;
        };
        std::vector<Item> items;
        items.reserve(rule.declarations.size() + rule.rules.size());
        for (Declaration& d : rule.declarations)
            items.push_back(Item{d.ref.offset, &d, nullptr});
        for (Rule& r : rule.rules)
            items.push_back(Item{r.ref.offset, nullptr, &r});
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b){ return a.offset < b.offset; });

        // the parsed sheet is not used after flattening: declarations and rules are moved out
        DeclarationList group;
        FlattenScope nested = scope;
        nested.parents = &selectors;
        for (const Item& item : items){
            if (item.declaration){
                group.push_back(std::move(*item.declaration));
                continue;
            }
            addRule(session, scope, selectors, std::move(group), rule.ref, rule.preludeRef);
            group.clear();
            RuleList one;
            one.push_back(std::move(*item.rule));
            flatten(session, one, nested, false);
        }
        // the last use of the selectors (nested rules were flattened above)
        addRule(session, scope, std::move(selectors), std::move(group), rule.ref, rule.preludeRef);
    }

    void flatten(Session& session, RuleList& rules, const FlattenScope& scope, bool topLevel){
        bool importsAllowed = topLevel;
        for (Rule& rule : rules){
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
                    ValueList condition = trimWhitespace(imported->supports);
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
                for (const ValueList& group : splitAtCommas(rule.prelude)){
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
                const MediaResult media = mediaOf(session, rule);
                inner.media = MediaResult::conjoin(scope.media, media);
                if (inner.media.never)
                    continue;
                if (scope.parents && !rule.declarations.empty())
                    addRule(session, inner, *scope.parents, rule.declarations, rule.ref, rule.preludeRef);
                flatten(session, rule.rules, inner, false);
                continue;
            }
            if (name == "starting-style" && session.options.fork.has(ForkFeature::AnimationLonghands)){
                // the fork reads @starting-style (phase 3): its rules are written inside one
                FlattenScope inner = scope;
                inner.startingStyle = true;
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
                at.rule = std::move(rule);
                at.media = scope.media;
                at.layer = scope.layer;
                at.order = session.nextOrder++;
                session.sheets[scope.sheet].push_back(SheetItem{true, (uint32_t)session.atRules.size()});
                session.atRules.push_back(std::move(at));
                continue;
            }
            if (name == "property"){
                // registered custom properties with the fork (phase 5): registered when the document
                // loads, or dropped; a syntax the fork does not register falls back to phase 2's root rule
                if (session.options.fork.has(ForkFeature::RegisteredProperties) &&
                    CompatAtProperty::take(session, rule, scope.parents != nullptr, scope.media) != CompatAtProperty::Outcome::Fallback)
                    continue;
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
                std::optional<SelectorList> root = SelectorRewriter::parseList(":root");
                if (root)
                    addRule(session, scope, *root, {declaration}, rule.ref, rule.preludeRef);
                continue;
            }
            // an at-rule a later phase handles (phase 5: @effect); flattening runs before any
            // declaration is written, so it applies to the whole document wherever it is
            if (const AtRuleHandler* handler = findAtRuleHandler(name)){
                if (scope.parents){
                    session.sink->reportAs(Severity::Warning, codes::UnsupportedAtRule, rule.ref, "@" + name + " inside a style rule is dropped",
                                           "move it to the top level");
                    continue;
                }
                (*handler)(rule, session, *session.sink);
                continue;
            }
            AtRules::reportDropped(session, rule);
        }
    }

    // ---- pre-pass helpers

    bool hasIdent(const ValueList& values, std::string_view lowercase){
        for (const ComponentValue& v : values){
            if (v.isToken(TokenType::Ident) && equalsNoCase(v.token.value, lowercase))
                return true;
            if (v.kind != ComponentValue::Kind::Token && hasIdent(v.children, lowercase))
                return true;
        }
        return false;
    }

    // animation names of an `animation` shorthand or `animation-name` value
    std::vector<std::string> animationNames(const std::string& property, const ValueList& value){
        static const std::set<std::string> keywords = {
            "none", "infinite", "normal", "reverse", "alternate", "alternate-reverse", "forwards", "backwards", "both", "running",
            "paused", "linear", "ease", "ease-in", "ease-out", "ease-in-out", "step-start", "step-end", "initial", "inherit", "unset"};
        std::vector<std::string> names;
        for (const ValueList& group : splitAtCommas(value)){
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

    // `body` alone (the HTML body, whose background CSS propagates to the canvas)
    bool isBodyOnly(const ComplexSelector& selector){
        return selector.compounds.size() == 1 && selector.compounds.front().simples.size() == 1 &&
               selector.compounds.front().simples.front().kind == SimpleSelector::Kind::Type &&
               selector.compounds.front().simples.front().name == "body";
    }

    bool isBodyRule(const FlatRule& rule){
        // (a scanned list has no `body` type selector)
        if (rule.scanned || rule.selectors().empty())
            return false;
        for (const ComplexSelector& s : rule.selectors()){
            if (!isBodyOnly(s))
                return false;
        }
        return true;
    }

    bool isBackgroundProperty(std::string_view name){
        return name == "background" || name.compare(0, 11, "background-") == 0;
    }

    bool isOverflowProperty(std::string_view name){
        return name == "overflow" || name == "overflow-x" || name == "overflow-y";
    }

    void prePass(Session& session, const std::vector<DeclarationList>& styleAttributes){
        // selectors (a scanned list was rewritten when it was read)
        for (FlatRule& rule : session.rules){
            if (rule.scanned)
                continue;
            const SelectorOptions& options = rule.origin == RuleOrigin::UserAgent ? session.userAgentSelectorOptions : session.selectorOptions;
            SelectorRewriteResult result = SelectorRewriter::rewrite(rule.selectors(), options, session.sink, rule.preludeRef.valid() ? rule.preludeRef : rule.ref);
            rule.rewritten = std::move(result.selectors);
            if (result.invalid || rule.rewritten.empty())
                rule.dropped = true;
        }
        BaseRules::index(session);

        // the custom property index (values normalized quietly; phase B reports)
        auto normalizeQuietly = [&](const ValueList& value, const std::string& name){
            NormalizeContext context;
            context.customProperty = true;
            context.options = &session.options;
            NormalizeResult result = ValueNormalizer::normalize(value, context);
            if (!result.ok)
                return trimWhitespace(value);
            trimInPlace(result.value);
            return std::move(result.value);
        };
        auto indexDeclarations = [&](const DeclarationList& declarations, uint32_t ruleId){
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
        for (const DeclarationList& declarations : styleAttributes)
            indexDeclarations(declarations, 0);

        // uses, flags, conflicts index
        auto scan = [&](const DeclarationList& declarations, const FlatRule* rule){
            for (const Declaration& d : declarations){
                std::vector<std::string> referenced;
                CustomPropertyIndex::referencedVariables(d.value, referenced);
                if (d.isCustomProperty()){
                    for (const std::string& name : referenced)
                        session.vars.addRawUse(name);
                    continue;
                }
                std::string name = d.name;
                if (!session.facts(name).row){
                    for (const char* prefix : {"-webkit-", "-moz-", "-ms-", "-o-"}){
                        const std::string p(prefix);
                        if (name.compare(0, p.size(), p) == 0 && session.facts(std::string_view(name).substr(p.size())).row)
                            name = name.substr(p.size());
                    }
                }
                // (a native text-shadow reads the variable as it is: no font-effect copy; the fork's background and mask
                // layers read it as the CSS value, its url()s resolved: phase 3 p3-paint)
                const bool nativeLayers = session.options.fork.has(ForkFeature::BackgroundLayers) &&
                                          (name == "background" || name == "background-image" || name == "mask" || name == "mask-image");
                const std::string_view family = (name == "text-shadow" && session.options.fork.has(ForkFeature::TextShadow))
                                                    ? std::string_view()
                                                    : (nativeLayers ? std::string_view("native-layers") : varFamilyOf(name));
                const ComponentValue* sole = soleValue(d.value);
                const bool whole = sole && sole->isFunction("var");
                const bool timing = name == "transition" || name == "animation" || name == "transition-timing-function" ||
                                    name == "animation-timing-function";
                for (const std::string& variable : referenced){
                    if (!family.empty() && whole && family != "easing"){
                        session.vars.addUse(variable, std::string(family));
                        continue;
                    }
                    if (timing){
                        // a variable in an easing position is written as a tween name
                        std::vector<ValueList> values;
                        bool easing = session.vars.expandedValues(variable, values) && !values.empty();
                        for (const ValueList& value : values)
                            easing = easing && !kEasingFamily.classify(session, value).empty();
                        if (easing){
                            session.vars.addUse(variable, "easing");
                            continue;
                        }
                    }
                    session.vars.addRawUse(variable);
                }
                if (startsWith(name, "border-image") && !hasIdent(d.value, "none"))
                    session.usesBorderImage = true;
                // (a native text-shadow and a text stroke's font effect do not share the font-effect list)
                if (name == "text-shadow" && !hasIdent(d.value, "none") && !session.options.fork.has(ForkFeature::TextShadow))
                    session.usesTextShadow = true;
                if (startsWith(name, "-webkit-text-stroke"))
                    session.usesTextStroke = true;
                // (keywordOf(d.value) == "inherit")
                if (sole && sole->isToken(TokenType::Ident) && equalsNoCase(sole->token.value, "inherit") &&
                    !session.options.fork.has(ForkFeature::CssWideKeywords)){
                    for (const std::string& p : CascadeKeywords::outputProperties(name, session.options.fork)){
                        const PropertySupport* row = session.facts(name).row;
                        const bool inherited = row ? row->inherited : CascadeKeywords::inherited(p);
                        if (!inherited && !CascadeKeywords::inherited(p))
                            session.inheritCompanions.insert(p);
                    }
                }
                // mirrored keyframes for the reverse directions, unless the fork plays them (AnimationLonghands)
                if ((name == "animation" || name == "animation-direction") && !session.options.fork.has(ForkFeature::AnimationLonghands) &&
                    (hasIdent(d.value, "reverse") || hasIdent(d.value, "alternate-reverse"))){
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
                if (rule && rule->origin == RuleOrigin::Author)
                    session.propertyUses.push_back(Session::PendingUse{&d, (uint32_t)(d.name.size() - name.size()), rule->index});
            }
        };
        for (const FlatRule& rule : session.rules){
            if (!rule.dropped)
                scan(rule.declarations, &rule);
        }
        for (const DeclarationList& declarations : styleAttributes)
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

        // background propagation (§3.10 item 1)
        {
            bool rootBackground = false, bodyBackground = false;
            for (const FlatRule& rule : session.rules){
                if (rule.dropped || rule.origin != RuleOrigin::Author)
                    continue;
                bool background = false;
                for (const Declaration& d : rule.declarations)
                    background = background || isBackgroundProperty(d.name);
                // (a scanned list is never a root or body rule)
                if (!background || rule.scanned)
                    continue;
                for (const ComplexSelector& s : rule.selectors()){
                    if (SelectorRewriter::isRootRule(s, session.selectorOptions))
                        rootBackground = true;
                }
                if (isBodyRule(rule))
                    bodyBackground = true;
            }
            // (the fork propagates it itself, at run time: ElementDocument::SetCanvasBackground)
            session.propagateBodyBackground = bodyBackground && !rootBackground && !session.options.fork.has(ForkFeature::BackgroundLayers);
        }

        // overflow propagation (CSS Overflow 3 §3.3, phase 3 §3.11.3): with the root's overflow visible, the body's
        // is the viewport's (our root's, the document being the viewport's scroll container) and the body's is visible
        {
            bool rootOverflow = false, bodyOverflow = false;
            for (const FlatRule& rule : session.rules){
                if (rule.dropped || rule.origin != RuleOrigin::Author || rule.scanned)
                    continue;
                bool overflow = false;
                for (const Declaration& d : rule.declarations)
                    overflow = overflow || isOverflowProperty(d.name);
                if (!overflow)
                    continue;
                for (const ComplexSelector& s : rule.selectors()){
                    if (SelectorRewriter::isRootRule(s, session.selectorOptions))
                        rootOverflow = true;
                }
                if (isBodyRule(rule))
                    bodyOverflow = true;
            }
            session.propagateBodyOverflow = bodyOverflow && !rootOverflow;
        }

        // the root font size (rem, root em) from static root rules
        for (const FlatRule& rule : session.rules){
            if (rule.dropped || rule.origin != RuleOrigin::Author || rule.scanned)
                continue;
            bool root = false;
            for (const ComplexSelector& s : rule.selectors()){
                if (SelectorRewriter::isRootRule(s, session.selectorOptions))
                    root = true;
            }
            if (!root)
                continue;
            for (const Declaration& d : rule.declarations){
                if (d.name != "font-size")
                    continue;
                const ComponentValue* v = soleValue(d.value);
                if (!v)
                    continue;
                if (v->isToken(TokenType::Percentage))
                    session.rootFontSize = 16.0 * v->token.number / 100.0;
                else if (v->isToken(TokenType::Dimension)){
                    const std::string unit = lowerAscii(v->token.unit);
                    if (unit == "em" || unit == "rem")
                        session.rootFontSize = 16.0 * v->token.number;
                    else if (std::optional<double> factor = canonicalFactor(unit); factor && unitCategory(unit) == UnitCategory::AbsoluteLength)
                        session.rootFontSize = v->token.number * *factor;
                }
            }
        }

        // @layer: a layered rule that outranks an unlayered one by specificity (RmlUi) loses in CSS
        if (!session.layerOrder.empty()){
            for (const auto& entry : session.propertyRulesIndex()){
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
                                if (sl.specificity > su.specificity && SelectorRewriter::subjectsOverlap(l.selectors()[sl.source], u.selectors()[su.source]))
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

    bool mayHaveViewportOrientation(std::string_view value){
        // "vm" in any case with two more characters after it (vmin, vmax), found by the 'm'
        if (value.size() < 4)
            return false;
        const size_t last = value.size() - 3;   // an 'm' at 1 .. last
        // ((c | 0x20) == 'm' holds for 'm' and 'M' alone, and 'v' likewise); eight bytes are
        // tested for an 'm' at once, a word that holds one byte by byte
        const unsigned char* const p = (const unsigned char*)value.data();
        size_t i = 1;
        for (; i + 8 <= last + 1; i += 8){
            uint64_t word;
            std::memcpy(&word, p + i, 8);
            const uint64_t m = (word | 0x2020202020202020ull) ^ 0x6D6D6D6D6D6D6D6Dull;
            if (((m - 0x0101010101010101ull) & ~m & 0x8080808080808080ull) == 0)
                continue;
            for (size_t j = i; j < i + 8; j++){
                if ((p[j] | 0x20) == 'm' && (p[j - 1] | 0x20) == 'v')
                    return true;
            }
        }
        for (; i <= last; i++){
            if ((p[i] | 0x20) == 'm' && (p[i - 1] | 0x20) == 'v')
                return true;
        }
        return false;
    }

    std::string replaceViewportOrientation(const std::string& value, bool landscape){
        if (!mayHaveViewportOrientation(value))
            return value;
        ValueList values = parseComponentValueList(value);
        std::function<void(ValueList&)> walk = [&](ValueList& list){
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

    bool hasViewportOrientation(std::string_view value){
        if (!mayHaveViewportOrientation(value))
            return false;
        ValueList values = parseComponentValueList(value);
        std::function<bool(const ValueList&)> walk = [&](const ValueList& list){
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

    // a declaration written with the fork's own !important (ForkFeature::Important: the flag is
    // part of the value text)
    inline bool writtenImportant(const OutDecl& declaration){
        constexpr std::string_view suffix = " !important";
        const std::string_view value = declaration.value.view();
        return value.size() > suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // does `later` (after `earlier` in the same rule) replace it? The last declaration of a
    // property wins, but a normal one never replaces an important one
    inline bool replaces(const OutDecl& later, const OutDecl& earlier){
        return later.name == earlier.name && (!writtenImportant(earlier) || writtenImportant(later));
    }

    // the declarations that take effect: in one rule the last declaration of a property wins,
    // so earlier ones are not written (an important one stays before a later normal one; the
    // fork keeps it)
    std::vector<const OutDecl*> effective(const ArenaList<OutDecl>& declarations){
        std::vector<const OutDecl*> out;
        out.reserve(declarations.size());
        // a rule's declarations: a later one of the same name is found by a scan
        for (size_t i = 0; i < declarations.size(); i++){
            bool later = false;
            for (size_t j = i + 1; j < declarations.size() && !later; j++)
                later = replaces(declarations[j], declarations[i]);
            if (!later)
                out.push_back(&declarations[i]);
        }
        return out;
    }

    void writeBlock(RcssWriter& writer, std::string_view selectors, const ArenaList<OutDecl>& declarations, SourceRef ref){
        if (declarations.empty() || selectors.empty())
            return;
        writer.beginRule(selectors, ref);
        if (declarations.size() <= 32){
            // effective() without its list: a declaration a later one of the same name replaces is skipped
            for (size_t i = 0; i < declarations.size(); i++){
                bool later = false;
                for (size_t j = i + 1; j < declarations.size() && !later; j++)
                    later = replaces(declarations[j], declarations[i]);
                if (!later)
                    writer.declaration(declarations[i].name, declarations[i].value, declarations[i].ref);
            }
        }else{
            for (const OutDecl* d : effective(declarations))
                writer.declaration(d->name, d->value, d->ref);
        }
        writer.endRule();
    }

    std::string joinSelectors(const std::vector<std::string>& selectors, const std::string& suffix = std::string()){
        std::string text;
        for (const std::string& s : selectors)
            text += (text.empty() ? "" : ", ") + s + suffix;
        return text;
    }

    // a rule (and its generated rules) under each block of a media result
    template<typename Body>
    void writeUnderMedia(RcssWriter& writer, const MediaResult& media, SourceRef ref, const Body& body){
        if (media.never)
            return;
        if (media.always){
            body();
            return;
        }
        for (const MediaConjunction& block : media.blocks){
            writer.beginMedia(block.toRcss(), ref);
            body();
            writer.endMedia();
        }
    }

    bool placeholderProperty(std::string_view name){
        return name == "color" || name == "opacity" || name.compare(0, 5, "font-") == 0 || name == "--html-current-color" ||
               name == "font-effect" || name == "letter-spacing";
    }

    void writeRule(Session& session, const FlatRule& rule, RcssWriter& writer){
        RuleOutput& output = session.scratchOutput;
        output.declarations.clear();
        output.generated.clear();
        processDeclarations(session, &rule, rule.origin, false, rule.declarations, output);

        // !important and vmin/vmax declarations are written apart: without any (the usual
        // case) the output is the rule's block as it is (vmin/vmax the fork computes stay)
        bool apart = false;
        for (const OutDecl& d : output.declarations)
            apart = apart || d.important || (rule.origin != RuleOrigin::StyleAttribute && !d.nativeUnits && hasViewportOrientation(d.value));
        ArenaList<OutDecl> rest, vmin;
        if (apart){
            Session::ImportantRule important;
            important.rule = &rule;
            rest.reserve(output.declarations.size());
            for (OutDecl& d : output.declarations){
                if (d.important){
                    important.declarations.push_back(std::move(d));
                    continue;
                }
                if (rule.origin != RuleOrigin::StyleAttribute && !d.nativeUnits && hasViewportOrientation(d.value))
                    vmin.push_back(std::move(d));
                else
                    rest.push_back(std::move(d));
            }
            if (!important.declarations.empty())
                session.important.push_back(std::move(important));
        }
        ArenaList<OutDecl>& normal = apart ? rest : output.declarations;

        // ::placeholder selectors take only text properties (selector lists joined as joinSelectors does)
        std::string joinedPlain, placeholderText;
        bool anyPlaceholder = false;
        const bool single = rule.rewritten.size() == 1 && !rule.rewritten[0].placeholder;
        if (!single){
            for (const RewrittenSelector& s : rule.rewritten){
                std::string& text = s.placeholder ? placeholderText : joinedPlain;
                anyPlaceholder = anyPlaceholder || s.placeholder;
                if (!text.empty())
                    text += ", ";
                text += s.text;
            }
        }
        // (one selector: its text itself)
        const std::string_view plainText = single ? rule.rewritten[0].text.view() : std::string_view(joinedPlain);
        auto plainWith = [&](const std::string& suffix){
            std::string text;
            for (const RewrittenSelector& s : rule.rewritten){
                if (s.placeholder)
                    continue;
                if (!text.empty())
                    text += ", ";
                text += s.text;
                text += suffix;
            }
            return text;
        };
        ArenaList<OutDecl> placeholderDeclarations;
        if (anyPlaceholder){
            for (const OutDecl& d : normal){
                if (placeholderProperty(d.name))
                    placeholderDeclarations.push_back(d);
                else
                    session.sink->reportAs(Severity::Info, codes::UnsupportedProperty, d.ref, "'" + d.name +
                                           "' has no effect in ::placeholder (only color, opacity and font properties apply); dropped there");
            }
        }

        // the body's background on the root, the body itself transparent (§3.10 item 1)
        ArenaList<OutDecl> rootBackground;
        if (session.propagateBodyBackground && rule.origin == RuleOrigin::Author && isBodyRule(rule)){
            for (OutDecl& d : normal){
                if (d.name != "background-color" && d.name != "decorator" && d.name != "--html-bg-layers")
                    continue;
                rootBackground.push_back(d);
                d.value = d.name == "background-color" ? "transparent" : d.name == "decorator" ? "none" : "linear-gradient(#0000, #0000)";
            }
        }
        // the body's overflow on the root (the viewport), the body itself visible (phase 3 §3.11.3)
        if (session.propagateBodyOverflow && rule.origin == RuleOrigin::Author && isBodyRule(rule)){
            for (OutDecl& d : normal){
                if (!isOverflowProperty(d.name))
                    continue;
                rootBackground.push_back(d);
                d.value = "visible";
            }
        }

        bool any = !normal.empty() || !placeholderDeclarations.empty();
        for (const GeneratedRule& generated : output.generated)
            any = any || !generated.declarations.empty();
        if (any)
        writeUnderMedia(writer, rule.media, rule.ref, [&](){
            // (a rule of @starting-style is written inside one: AnimationLonghands)
            if (rule.startingStyle)
                writer.atRule("@starting-style {", rule.ref);
            if (!rootBackground.empty())
                writeBlock(writer, "body#htmlui-root", rootBackground, rule.ref);
            writeBlock(writer, plainText, normal, rule.ref);
            writeBlock(writer, placeholderText, placeholderDeclarations, rule.ref);
            for (const GeneratedRule& generated : output.generated)
                writeBlock(writer, plainWith(generated.suffix), generated.declarations, rule.ref);
            if (rule.startingStyle)
                writer.atRule("}", rule.ref);
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
                ArenaList<OutDecl> converted = vmin;
                for (OutDecl& d : converted)
                    d.value = replaceViewportOrientation(d.value, landscape);
                writeUnderMedia(writer, media, rule.ref, [&](){
                    writeBlock(writer, plainText, converted, rule.ref);
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
        const std::vector<int> ranks = layerRanks(session);
        const auto before = [&](size_t a, size_t b){
            const int la = layerOf(items[a]);
            const int lb = layerOf(items[b]);
            // layers first in cascade order, unlayered last
            const int ka = la < 0 ? 1 << 30 : ranks[(size_t)la];
            const int kb = lb < 0 ? 1 << 30 : ranks[(size_t)lb];
            if (ka != kb)
                return ka < kb;
            return orderOf(items[a]) < orderOf(items[b]);
        };
        // (a sheet without @layer is in order already)
        if (!std::is_sorted(order.begin(), order.end(), before))
            std::stable_sort(order.begin(), order.end(), before);
        return order;
    }

    void importantConflicts(Session& session, const FlatRule& rule, const OutDecl&, const std::string& property, SourceRef ref){
        const auto it = session.propertyRulesIndex().find(property);
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
                    if (theirs.specificity > mine.specificity && SelectorRewriter::subjectsOverlap(rule.selectors()[mine.source], other.selectors()[theirs.source])){
                        session.sink->report(codes::ImportantMayLose, ref, "'" + property + " !important' may lose to the more specific rule '" +
                                             SelectorRewriter::serialize(other.selectors()[theirs.source]) + "' at " + BaseRules::location(session, other) +
                                             " (the engine has no !important)", "raise this selector's specificity or lower the other one");
                        return;
                    }
                }
            }
        }
    }

    // the property name a declaration is read as (vendor prefixes resolved); "" + reported when dropped
    std::string effectiveName(Session& session, const Declaration& d, const DeclarationList& block, bool& silentlyDropped,
                              const PropertyFacts** declaredFacts = nullptr){
        silentlyDropped = false;
        const PropertyFacts& facts = session.facts(d.name);
        if (facts.row){
            if (declaredFacts)
                *declaredFacts = &facts;
            return d.name;
        }
        session.memoTaint++;   // (the block's other declarations decide a prefixed property)
        for (const char* prefix : {"-webkit-", "-moz-", "-ms-", "-o-"}){
            const std::string p(prefix);
            if (d.name.compare(0, p.size(), p) != 0)
                continue;
            const std::string base = d.name.substr(p.size());
            if (!session.facts(base).row)
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

    // The rewrites that write the declared value as it is (keywords mapped, the value checked
    // against RmlUi's grammar), so a calc()/min()/max()/clamp() the fork evaluates can stay in
    // it (ForkFeature::Calc). The others read their value token by token or write it into
    // compat decorator arguments: math functions are folded for them in every fork state.
    // The rewrites that write a color value as it is into an RmlUi property whose colour parser
    // takes 'currentcolor' (ForkFeature::CurrentColor): the keyword stays for the fork to resolve.
    // The others write colors into compat decorator arguments, font effects, filters or
    // companions read elsewhere: the current-color companion stays there in every fork state.
    bool writesCurrentColorNatively(RewriteId rewrite){
        return rewrite == RewriteId::ColorValue || rewrite == RewriteId::Border;
    }

    bool writesCalcNatively(RewriteId rewrite){
        switch (rewrite){
            case RewriteId::None:
            case RewriteId::Sizing:
            case RewriteId::Gap:
            case RewriteId::FontSize:
            case RewriteId::LineHeight:
            case RewriteId::ZIndex:
            case RewriteId::Opacity:
            // (p3-values: the fork's parsers take them there too; a logical property's value goes to
            // its physical properties as written)
            case RewriteId::LogicalBox:
            case RewriteId::FontWeight:
                return true;
            default:
                return false;
        }
    }

    // The fork reads currentcolor itself in these values (p3-text: text-shadow, text-decoration-color):
    // the keyword is kept instead of the rule's static color.
    bool keepsCurrentColorKeyword(const Session& session, const PropertySupport& row){
        if (row.rewrite == RewriteId::TextShadow)
            return session.options.fork.has(ForkFeature::TextShadow);
        if (row.rewrite == RewriteId::TextDecoration)
            return session.options.fork.has(ForkFeature::TextProperties);
        return false;
    }

    // what the normalizer may read of the rule for a value (see processOne)
    struct ValueNeeds {
        bool staticColor = false;   // a currentcolor ident or a var()
        bool fontSize = false;      // a function (the math functions resolve em against it)
    };

    void scanNeeds(const ValueList& values, ValueNeeds& needs){
        for (const ComponentValue& v : values){
            if (v.kind == ComponentValue::Kind::Token){
                if (v.token.type == TokenType::Ident && equalsNoCase(v.token.value, "currentcolor"))
                    needs.staticColor = true;
                continue;
            }
            if (v.kind == ComponentValue::Kind::Function){
                needs.fontSize = true;
                if (equalsNoCase(v.token.value, "var"))
                    needs.staticColor = true;
            }
            scanNeeds(v.children, needs);
        }
    }

    // one declaration through the pipeline of §3.9.6
    // what processDeclared made of a declaration
    struct Processed {
        bool completed = false;   // the pipeline ran to its end (not dropped on the way)
        std::string name;         // the effective property name
    };

    // The pipeline of a declared property (not a custom property): its declarations are
    // appended to output.declarations from `first` on.
    Processed processDeclared(Session& session, const FlatRule* rule, RuleOrigin origin, bool keyframe, const Declaration& d,
                              const DeclarationList& block, RuleOutput& output, const size_t first){
        Processed done;
        bool silentlyDropped = false;
        const PropertyFacts* declaredFacts = nullptr;
        std::string name = effectiveName(session, d, block, silentlyDropped, &declaredFacts);
        if (silentlyDropped)
            return done;
        if (name.empty()){
            const std::string closest = closestProperty(d.name);
            session.sink->report(codes::UnknownProperty, d.nameRef.valid() ? d.nameRef : d.ref, "'" + d.name + "' is not a CSS property; dropped",
                                 closest.empty() ? std::string() : "did you mean '" + closest + "'?", closest, d.name);
            return done;
        }
        // (the declared name's facts when it is a property itself, the usual case)
        const PropertyFacts& facts = declaredFacts ? *declaredFacts : session.facts(name);
        const PropertySupport* row = facts.row;
        const Support support = effectiveSupport(*row, session.options.fork);
        // phase 5: effect() outside the properties that take it (HTMLUI-FX-0002)
        if (rejectsHandledFunction(session, d, name))
            return done;
        // phase 5: background-blend-mode is the blend of the rule's effect layers (EffectCompat)
        if (name == "background-blend-mode" && rule && ruleHasEffectLayer(session, *rule))
            return done;
        if (support == Support::Unsupported){
            Severity severity = isInfoNote(row->note) ? Severity::Info : Severity::Warning;
            std::string suggestion = row->suggestion;
            if (name == "aspect-ratio" && rule){
                for (const ComplexSelector& s : rule->selectors()){
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
                // the user-agent sheet's list-item counter (for the fork's markers): the li
                // emulation numbers the items without it
                if (onlyListItem && origin == RuleOrigin::UserAgent)
                    return done;
            }
            // (Chromium 153 drops the standard 'line-clamp' as well: beside the '-webkit-line-clamp' it implements, it is for
            // other browsers, Info)
            if (name == "line-clamp"){
                for (const Declaration& other : block){
                    if (equalsNoCase(other.name, "-webkit-line-clamp"))
                        severity = Severity::Info;
                }
            }
            session.sink->reportAs(severity, codes::UnsupportedProperty, d.ref, "'" + name + "' is not supported; dropped", suggestion,
                                   std::string(), name);
            return done;
        }
        if (support == Support::Ignored){
            session.sink->report(codes::IgnoredProperty, d.ref, "'" + name + "' has no effect in the engine; dropped", row->suggestion,
                                 std::string(), name);
            return done;
        }

        DeclContext context{session, RuleRef{rule, &session.memoTaint}, origin, keyframe, d, name, row, {}, false, output, false};
        context.important = d.important && !keyframe;
        // the normalized value is built in a vector of the session's pool, returned when done
        struct ValueStorage {
            DeclContext& context;
            ~ValueStorage(){
                context.value.clear();
                if (context.value.capacity() > 0 && context.value.capacity() <= 64)
                    context.session.valuePool.push_back(std::move(context.value));
            }
        } valueStorage{context};

        // the value kept the units the fork computes (ForkFeature::ExtraUnits)
        bool nativeUnits = false;
        // the color property's own currentColor is `inherit`
        const ComponentValue* sole = soleValue(d.value);
        const std::string keyword = name != "all" ? CascadeKeywords::keywordOf(d.value) : std::string();
        if (name == "color" && sole && sole->isToken(TokenType::Ident) && equalsNoCase(sole->token.value, "currentcolor")){
            // the parent's color: the fork's own meaning of it (ForkFeature::CurrentColor), else `inherit`
            if (session.options.fork.has(ForkFeature::CurrentColor))
                context.emit("color", "currentcolor");
            else
                context.emit("color", std::string("var(") + companions::kUnset + ")");
        }else if (!keyword.empty()){
            // 5. CSS-wide keywords
            CascadeKeywords::apply(context, keyword);
        }else{
            // 4. value normalization
            NormalizeContext normalize;
            normalize.colors = facts.colorKeywords;
            // (the fork's outline-color takes currentcolor too: ForkFeature::Outline, p3-paint)
            // (and the fork's layers resolve currentcolor in gradient stops where they are drawn: ForkFeature::BackgroundLayers)
            // (box-shadow reaches the fork's box-shadow through compat's companions, substituted on the element:
            // its currentcolor is the element's)
            const bool nativeColorRewrite = writesCurrentColorNatively(row->rewrite) || row->rewrite == RewriteId::BoxShadow ||
                                            (row->rewrite == RewriteId::Outline && session.options.fork.has(ForkFeature::Outline)) ||
                                            ((row->rewrite == RewriteId::Background || row->rewrite == RewriteId::BackgroundLonghand ||
                                              row->rewrite == RewriteId::MaskImage) &&
                                             session.options.fork.has(ForkFeature::BackgroundLayers));
            normalize.keepCurrentColorKeyword = row->rewrite == RewriteId::SvgPaint || keepsCurrentColorKeyword(session, *row) ||
                                                (session.options.fork.has(ForkFeature::CurrentColor) && nativeColorRewrite);
            normalize.currentColorMixNative = session.options.fork.has(ForkFeature::CurrentColor) &&
                                              (nativeColorRewrite || keepsCurrentColorKeyword(session, *row));
            // (box-shadow reaches the fork through compat's companions, which it substitutes; filter as written)
            normalize.colorFunctionsNative = session.options.fork.has(ForkFeature::ColorFunctions) &&
                                             (nativeColorRewrite || keepsCurrentColorKeyword(session, *row) ||
                                              row->rewrite == RewriteId::BoxShadow || row->rewrite == RewriteId::Filter);
            // a style attribute or a keyframe cannot be duplicated per orientation: vmin/vmax evaluated
            normalize.inlineStyle = origin == RuleOrigin::StyleAttribute || keyframe;
            // math functions the fork evaluates stay in the properties written as they are read
            // (not in the families a rewrite reads token by token or into decorator arguments)
            normalize.calcNative = writesCalcNatively(row->rewrite) ||
                (row->rewrite == RewriteId::Grid && session.options.fork.has(ForkFeature::Grid)) ||
                // translate / rotate / scale written as they are: the fork reads them with its transform parser
                (row->rewrite == RewriteId::IndividualTransform && session.options.fork.has(ForkFeature::IndividualTransforms));
            normalize.unitsNative = normalize.calcNative;
            // 'translate' combined into transform (no IndividualTransforms): a sum stays for the
            // rewrite to write as one translation per term
            normalize.lengthSums = row->rewrite == RewriteId::IndividualTransform && name == "translate" &&
                                   !session.options.fork.has(ForkFeature::IndividualTransforms);
            nativeUnits = normalize.unitsNative && session.options.fork.has(ForkFeature::ExtraUnits);
            normalize.attrNative = row->rewrite == RewriteId::Content && session.options.fork.has(ForkFeature::PseudoElements);
            normalize.percentBasis = facts.percentBasis;
            normalize.vertical = facts.vertical;
            normalize.options = &session.options;
            if (rule && !keyframe){
                // the rule's static color is read for currentColor (written, or behind a var())
                // and its font size by the math functions: looked up only for such values
                ValueNeeds needs;
                scanNeeds(d.value, needs);
                // (both read the rule's other declarations)
                if (needs.staticColor && name != "color"){
                    // (read, and counted for the memo, when the value holds a currentColor)
                    normalize.ruleStaticColor = [&session, rule](){
                        session.memoTaint++;
                        return std::string(props::ruleStaticColor(session, *rule));
                    };
                }
                if (needs.fontSize && name != "font-size"){
                    // (read, and counted for the memo, when the value resolves a font-relative unit)
                    normalize.ruleFontSize = [&session, rule](){
                        session.memoTaint++;
                        return props::ruleFontSizePx(session, *rule);
                    };
                }
            }
            normalize.remPx = session.rootFontSize.value_or(16.0);
            // (a runtime variable has no static value here: math functions keep it, or fold it themselves)
            normalize.resolveVar = [&session](const ComponentValue& var, ValueList& out){
                if (CustomPropertyIndex::isDynamic(CustomPropertyIndex::varName(var)))
                    return false;
                return session.staticVar(var, out);
            };
            // (one captured pointer: the std::function keeps it without an allocation)
            normalize.report = [ctx = &context](DiagCode code, Severity severity, const std::string& message, const std::string& suggestion){
                const Declaration& declaration = ctx->declaration;
                ctx->session.sink->reportAs(severity, code, declaration.valueRef.valid() ? declaration.valueRef : declaration.ref, message,
                                            suggestion, std::string(), ctx->name);
            };
            ValueList storage;
            if (!session.valuePool.empty()){
                storage = std::move(session.valuePool.back());
                session.valuePool.pop_back();
            }
            NormalizeResult normalized = ValueNormalizer::normalize(d.value, normalize, std::move(storage));
            context.value = std::move(normalized.value);
            if (!normalized.ok)
                return done;
            if (normalized.dependsOnViewport)
                session.result->dependsOnViewport = true;
            trimInPlace(context.value);
            if (context.value.empty())
                return done;

            // 6. the property rewrite
            const uint32_t foldedBefore = session.dynamicVarsFolded;
            if (name == "transform" && rule)
                session.memoTaint++;
            if (name == "transform" && rule && ruleHasIndividualTransform(*rule) && !session.options.fork.has(ForkFeature::IndividualTransforms)){
                // written with translate / rotate / scale (Transform.cpp)
            }else if (row->rewrite != RewriteId::None){
                if (RewriteFunction function = findRewrite(row->rewrite))
                    function(context);
            }else{
                if (name == "transform-origin" || name == "perspective-origin"){
                    // RmlUi reads the horizontal keyword first ("top left" -> "left top")
                    ValueList w;
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
                            return done;
                        }
                    }
                }
                if (!facts.rcssKnown){
                    context.reportAs(Severity::Warning, codes::UnsupportedProperty, "'" + name + "' is not supported by the engine; dropped", row->suggestion);
                    return done;
                }
                if (!RcssGrammar::accepts(name, text, session.options.fork)){
                    std::string suggestion;
                    const std::string keywords = RcssGrammar::keywords(name);
                    if (!keywords.empty())
                        suggestion = "use one of: " + keywords;
                    context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + name + ": " + text + "' is not supported; declaration dropped",
                                     suggestion);
                    return done;
                }
                context.emit(name, text);
            }
            if (session.dynamicVarsFolded != foldedBefore && !context.dropped){
                context.reportAs(Severity::Info, codes::ApproximatedMinor,
                                 "env(safe-area-inset-*) is 0 here (static value): '" + name + "' is rewritten for RmlUi",
                                 "write the inset where the property is read as written (padding, margin, top, width...)");
            }
        }
        if (context.dropped)
            output.declarations.resize(first);

        // 7. !important, companions
        ArenaList<OutDecl> extra;
        for (size_t i = first; i < output.declarations.size(); i++){
            OutDecl& out = output.declarations[i];
            if (out.final)
                continue;
            out.final = true;
            out.nativeUnits = nativeUnits;
            if (d.important){
                if (keyframe){
                    out.important = false;
                }else if (session.options.fork.has(ForkFeature::Important)){
                    out.important = false;
                    out.value = Text(out.value + " !important");
                }else if (origin == RuleOrigin::StyleAttribute){
                    out.important = false;
                }else{
                    out.important = true;
                }
            }
            if (keyframe)
                continue;
            const bool keyword = CascadeKeywords::isKeywordText(out.value);
            // (a `color: currentcolor` the fork resolves is the parent's: the inherited companion stays right)
            if (out.name == "color" && !startsWith(out.value, "var(") && !keyword && out.value.view() != "currentcolor")
                extra.push_back(OutDecl{companions::kCurrentColor, out.value, out.ref, out.important, false, true});
            // (nothing to add when no rule reads a property with `inherit`)
            if (!session.inheritCompanions.empty() && !keyword && out.value.find("--html-inh-") == std::string::npos){
                if (session.inheritCompanions.count(out.name))
                    extra.push_back(OutDecl{companions::inheritName(out.name), out.value, out.ref, out.important, false, true});
                else
                    boxShorthandCompanions(session, out, extra);
            }
        }
        for (OutDecl& e : extra)
            output.declarations.push_back(std::move(e));

        done.completed = true;
        done.name = std::move(name);
        return done;
    }

    // ---- the declaration memo (Session::MemoEntry)

    constexpr uint32_t kNoMemo = 0xFFFFFFFFu;

    uint32_t memoFind(const Session& session, uint64_t hash, const Declaration& d, uint8_t bits){
        if (session.memoSlots.empty())
            return kNoMemo;
        const size_t mask = session.memoSlots.size() - 1;
        for (size_t i = (size_t)hash & mask; session.memoSlots[i] != kNoMemo; i = (i + 1) & mask){
            const Session::MemoEntry& entry = session.memoEntries[session.memoSlots[i]];
            if (entry.hash == hash && entry.bits == bits && entry.file == d.ref.file && entry.text == d.text)
                return session.memoSlots[i];
        }
        return kNoMemo;
    }

    void memoInsert(Session& session, const Session::MemoEntry& entry){
        if ((session.memoEntries.size() + 1) * 2 > session.memoSlots.size()){
            session.memoSlots.assign(session.memoSlots.empty() ? 64 : session.memoSlots.size() * 2, kNoMemo);
            const size_t mask = session.memoSlots.size() - 1;
            for (uint32_t index = 0; index < (uint32_t)session.memoEntries.size(); index++){
                size_t i = (size_t)session.memoEntries[index].hash & mask;
                while (session.memoSlots[i] != kNoMemo)
                    i = (i + 1) & mask;
                session.memoSlots[i] = index;
            }
        }
        const size_t mask = session.memoSlots.size() - 1;
        size_t i = (size_t)entry.hash & mask;
        while (session.memoSlots[i] != kNoMemo)
            i = (i + 1) & mask;
        session.memoSlots[i] = (uint32_t)session.memoEntries.size();
        session.memoEntries.push_back(entry);
    }

    bool sameRef(const SourceRef& a, const SourceRef& b){
        return a.file == b.file && a.offset == b.offset && a.length == b.length;
    }

    // JASPER_HTMLUI_CSS_MEMO=check: the pipeline's output for the declaration is the entry's
    [[maybe_unused]] void memoVerify(Session& session, const FlatRule* rule, RuleOrigin origin, bool keyframe, const Declaration& d,
                                     const DeclarationList& block, uint32_t index){
        const uint32_t taint = session.memoTaint;
        const uint64_t reports = session.sink->reports();
        const size_t fonts = session.result->fontFamilies.size();
        RuleOutput check;
        const Processed done = processDeclared(session, rule, origin, keyframe, d, block, check, 0);
        const Session::MemoEntry& entry = session.memoEntries[index];
        bool same = session.memoTaint == taint && session.sink->reports() == reports && check.generated.empty() &&
                    done.completed == entry.completed && entry.name == done.name && check.declarations.size() == entry.count &&
                    session.result->fontFamilies.size() - fonts == entry.fontCount;
        for (size_t i = 0; same && i < entry.count; i++){
            const OutDecl& a = check.declarations[i];
            const OutDecl& b = session.memoOutput[entry.first + i];
            same = a.name == b.name && a.value == b.value && a.important == b.important && a.vmin == b.vmin && a.final == b.final &&
                   a.nativeUnits == b.nativeUnits &&
                   sameRef(a.ref, d.ref);
        }
        for (size_t i = 0; same && i < entry.fontCount; i++)
            same = session.result->fontFamilies[fonts + i] == session.memoFonts[entry.fontFirst + i];
        if (!same){
            std::fprintf(stderr, "[HTML UI] declaration memo mismatch for '%s'\n", std::string(d.text).c_str());
            gMemoMismatches.fetch_add(1, std::memory_order_relaxed);
        }
        session.result->fontFamilies.resize(fonts);
    }

    void processOne(Session& session, const FlatRule* rule, RuleOrigin origin, bool keyframe, const Declaration& d,
                    const DeclarationList& block, RuleOutput& output){
        if (session.options.dropTransitionsForTesting &&
            (d.name == "transition" || d.name.compare(0, 11, "transition-") == 0 || d.name.compare(0, 18, "-webkit-transition") == 0))
            return;
        const size_t first = output.declarations.size();

        // 1. custom properties
        if (d.isCustomProperty()){
            NormalizeContext context;
            context.customProperty = true;
            context.options = &session.options;
            // RmlUi substitutes the written value where it is used (the index keeps a folded copy)
            context.calcNative = true;
            // a registered custom property (phase 5) is a property of the fork: units it computes stay, a
            // <color>'s currentcolor stays the keyword; a value that does not match is reported (it is invalid at
            // computed-value time, as in CSS) and written as declared
            const RegisteredProperty* registered = CompatAtProperty::find(session, d.name.view());
            if (registered){
                context.unitsNative = session.options.fork.has(ForkFeature::ExtraUnits);
                context.keepCurrentColorKeyword = registered->type == AtPropertySyntax::Color;
                if (!CompatAtProperty::matches(registered->type, d.value))
                    session.sink->reportAs(Severity::Warning, codes::AtPropertyInvalid, d.ref,
                                           "'" + serialize(trimWhitespace(d.value)) + "' is not a " + registered->syntax + ", the syntax @property gives " +
                                           d.name + ": the declaration is invalid at computed-value time and " + d.name + " takes " +
                                           (registered->inherits ? "its parent's value" : "its initial value") +
                                           (registered->hasInitialValue ? " (initial-value " + registered->initialValue + ")" : std::string()),
                                           std::string("write a ") + registered->syntax + " value", std::string(), d.name);
            }
            context.inlineStyle = origin == RuleOrigin::StyleAttribute;
            context.report = [&](DiagCode code, Severity severity, const std::string& message, const std::string& suggestion){
                session.sink->reportAs(severity, code, d.ref, message, suggestion, std::string(), d.name);
            };
            NormalizeResult normalized = ValueNormalizer::normalize(d.value, context);
            if (!normalized.ok)
                return;
            if (normalized.dependsOnViewport)
                session.result->dependsOnViewport = true;
            ValueList value = std::move(normalized.value);
            trimInPlace(value);
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
            // (a typed registered property is read by the fork as it is: no typed copies)
            for (const std::string& familyName : (registered && registered->type != AtPropertySyntax::Universal) ? std::set<std::string>()
                                                                                                                : session.vars.families(d.name)){
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
            // the fork's own !important (ForkFeature::Important) also weighs custom properties;
            // without it an important custom property is a normal one, as in phase 2
            const bool important = d.important && !keyframe && session.options.fork.has(ForkFeature::Important);
            if (important)
                text += " !important";
            OutDecl out{d.name, text, d.ref, false, false};
            output.declarations.push_back(out);
            for (const auto& copy : copies)
                output.declarations.push_back(OutDecl{copy.first, important ? copy.second + " !important" : copy.second, d.ref, false, false});
            return;
        }

        // 2. vendor prefixes, 3. lookup
        // ---- the pipeline, or what it made of the same declaration before (the memo)
        Processed done;
        const bool memo = session.memoOn && !d.text.empty() && session.sink == session.mainSink;
        if (!memo){
            done = processDeclared(session, rule, origin, keyframe, d, block, output, first);
        }else{
            const uint8_t bits = (uint8_t)((d.important ? 1 : 0) | (keyframe ? 2 : 0) | (rule ? 4 : 0) | ((unsigned)origin << 3));
            const uint64_t hash = StringIndex::hash(d.text) ^ ((uint64_t)bits * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)d.ref.file << 40);
            const uint32_t found = memoFind(session, hash, d, bits);
            // (an entry that read the custom properties holds for the index it read)
            const bool usable = found != kNoMemo && session.memoEntries[found].pure &&
                                (!session.memoEntries[found].readVars || session.memoEntries[found].varRevision == session.vars.revision());
            if (usable){
                if (session.memoCheck){
                    gMemoHits.fetch_add(1, std::memory_order_relaxed);
                    memoVerify(session, rule, origin, keyframe, d, block, found);
                }
                const Session::MemoEntry& entry = session.memoEntries[found];
                for (uint32_t i = 0; i < entry.count; i++){
                    OutDecl& out = output.declarations.emplace_back();
                    out = session.memoOutput[entry.first + i];
                    out.ref = d.ref;
                }
                for (uint32_t i = 0; i < entry.fontCount; i++)
                    session.result->fontFamilies.push_back(session.memoFonts[entry.fontFirst + i]);
                done.completed = entry.completed;
                done.name = entry.name;
            }else if (found != kNoMemo){
                done = processDeclared(session, rule, origin, keyframe, d, block, output, first);
            }else{
                const uint32_t taint = session.memoTaint;
                const uint32_t varReads = session.memoVarReads;
                const uint64_t reports = session.sink->reports();
                const size_t generated = output.generated.size();
                const size_t fonts = session.result->fontFamilies.size();
                done = processDeclared(session, rule, origin, keyframe, d, block, output, first);
                Session::MemoEntry entry;
                entry.readVars = session.memoVarReads != varReads;
                entry.varRevision = session.vars.revision();
                entry.hash = hash;
                entry.text = d.text;
                entry.file = d.ref.file;
                entry.bits = bits;
                entry.pure = session.memoTaint == taint && session.sink->reports() == reports && output.generated.size() == generated;
                for (size_t i = first; entry.pure && i < output.declarations.size(); i++)
                    entry.pure = sameRef(output.declarations[i].ref, d.ref);
                if (entry.pure){
                    entry.completed = done.completed;
                    entry.name = Text(done.name);
                    entry.first = (uint32_t)session.memoOutput.size();
                    entry.count = (uint32_t)(output.declarations.size() - first);
                    for (size_t i = first; i < output.declarations.size(); i++)
                        session.memoOutput.push_back(output.declarations[i]);
                    entry.fontFirst = (uint32_t)session.memoFonts.size();
                    entry.fontCount = (uint32_t)(session.result->fontFamilies.size() - fonts);
                    for (size_t i = fonts; i < session.result->fontFamilies.size(); i++)
                        session.memoFonts.push_back(session.result->fontFamilies[i]);
                }
                memoInsert(session, entry);
            }
        }
        if (!done.completed)
            return;
        const std::string& name = done.name;

        if (d.important && rule && !keyframe && origin == RuleOrigin::Author && !session.options.fork.has(ForkFeature::Important) &&
            output.declarations.size() > first)
            importantConflicts(session, *rule, output.declarations[first], name, d.ref);
        // (with the fork's Important the cascade is CSS's: nothing to report)
        if (d.important && origin == RuleOrigin::StyleAttribute && !session.options.fork.has(ForkFeature::Important)){
            const auto it = session.propertyRulesIndex().find(name);
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

namespace {

    // A box shorthand RCSS keeps as a shorthand (border-radius: 16px) whose longhands some rule
    // reads with `inherit`: the companions of the longhands (`--html-inh-border-top-left-radius`),
    // one value per side or corner by the CSS 1-4 value rule
    void boxShorthandCompanions(Session& session, const OutDecl& out, ArenaList<OutDecl>& extra){
        static const std::map<std::string, std::array<const char*, 4>> shorthands = {
            {"border-radius", {"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius"}},
            {"margin", {"margin-top", "margin-right", "margin-bottom", "margin-left"}},
            {"padding", {"padding-top", "padding-right", "padding-bottom", "padding-left"}},
            {"inset", {"top", "right", "bottom", "left"}},
        };
        const auto found = shorthands.find(out.name);
        if (found == shorthands.end())
            return;
        bool wanted = false;
        for (const char* longhand : found->second)
            wanted = wanted || session.inheritCompanions.count(std::string_view(longhand)) > 0;
        if (!wanted || out.value.find('/') != std::string::npos)
            return;
        // the values, split at top-level spaces (var() and functions keep theirs)
        std::vector<std::string> values;
        std::string current;
        int depth = 0;
        for (char c : out.value){
            if (c == '(')
                depth++;
            else if (c == ')')
                depth--;
            if (c == ' ' && depth == 0){
                if (!current.empty())
                    values.push_back(current);
                current.clear();
                continue;
            }
            current += c;
        }
        if (!current.empty())
            values.push_back(current);
        if (values.empty() || values.size() > 4)
            return;
        const size_t n = values.size();
        const std::string sides[4] = {values[0], values[n > 1 ? 1 : 0], values[n > 2 ? 2 : 0], values[n > 3 ? 3 : (n > 1 ? 1 : 0)]};
        for (int i = 0; i < 4; i++){
            if (session.inheritCompanions.count(std::string_view(found->second[i])))
                extra.push_back(OutDecl{companions::inheritName(found->second[i]), sides[i], out.ref, out.important, false, true});
        }
    }

}

void jasperengine::htmlui::compat::processDeclarations(Session& session, const FlatRule* rule, RuleOrigin origin, bool keyframe,
                                                       const DeclarationList& declarations, RuleOutput& output){
    for (const Declaration& d : declarations)
        processOne(session, rule, origin, keyframe, d, declarations, output);
}

void jasperengine::htmlui::compat::processSynthetic(DeclContext& context, const std::string& property, const std::string& value){
    DeclarationList block;
    block.reserve(1);
    Declaration& synthetic = block.emplace_back();
    synthetic.name = property;
    synthetic.value = context.session.syntheticValues.get(value, [](std::string_view text){
        ValueList parsed = parseComponentValueList(text);
        trimInPlace(parsed);
        return parsed;
    });
    synthetic.ref = context.declaration.ref;
    synthetic.nameRef = context.declaration.nameRef;
    synthetic.valueRef = context.declaration.valueRef;
    synthetic.important = context.declaration.important;
    if (context.session.memoOn){
        // (the text the memo knows it by: no source text holds "property: value")
        std::string& text = context.session.scratchText;
        text.assign(property);
        text += '\x01';
        text += value;
        synthetic.text = Text(text);
    }
    RuleOutput output;
    processOne(context.session, context.rule.untracked(), context.origin, context.keyframe, synthetic, block, output);
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
    std::vector<DeclarationList> styleAttributes(inputs.size());
    std::vector<DeclarationList> styleAttributeList;
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
        Stylesheet sheet = parseStylesheet(input.file, input.text, impl->sink, input.baseOffset, true);
        // the flat rules: about one a style rule (one more for the declarations after each
        // nested rule), allocated once
        std::function<size_t(const RuleList&, bool)> count = [&](const RuleList& rules, bool nested){
            size_t n = 0;
            for (const Rule& r : rules)
                n += (r.kind == Rule::Kind::Style ? (nested ? 2 : 1) : 0) + count(r.rules, r.kind == Rule::Kind::Style || nested);
            return n;
        };
        session.rules.reserve(session.rules.size() + count(sheet.rules, false));
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
    // (the registered custom properties of every sheet are known before any declaration is rewritten: hoisted)
    CompatAtProperty::finish(session);
    prePass(session, styleAttributeList);
    // (the session's facts about the document are complete: declarations can be memoized)
    session.memoOn = !session.memoDisabled;

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
            writeUnderMedia(writer, rule.media, rule.ref, [&](){
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
    // S51 R15: a rule naming a bitmap font family with a glow, a blur or a blurred text-shadow: the
    // engine drops the effect (pixel fonts draw pixel outlines and sharp shadows only)
    if (!result.bitmapFontFamilies.empty()){
        auto lower = [](std::string text){
            for (char& c : text) c = (char)std::tolower((unsigned char)c);
            return text;
        };
        auto trimmed = [](const std::string& text){
            const size_t a = text.find_first_not_of(" \t\n");
            const size_t b = text.find_last_not_of(" \t\n");
            return a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
        };
        auto split = [](const std::string& text, char by){
            std::vector<std::string> parts;
            size_t start = 0;
            while (true){
                const size_t end = text.find(by, start);
                parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            return parts;
        };
        auto namesBitmapFamily = [&](const std::string& value){
            std::string first = lower(value.substr(0, value.find(',')));
            first.erase(std::remove(first.begin(), first.end(), '"'), first.end());
            first.erase(std::remove(first.begin(), first.end(), '\''), first.end());
            for (const std::string& family : result.bitmapFontFamilies){
                const std::string name = lower(family);
                // `font: 18px Hud` ends with the family; `font-family: Hud` is it
                if (first == name || (first.size() > name.size() && first.compare(first.size() - name.size(), name.size(), name) == 0 &&
                                      first[first.size() - name.size() - 1] == ' '))
                    return true;
            }
            return false;
        };
        for (const FlatRule& rule : session.rules){
            if (rule.dropped)
                continue;
            bool bitmap = false;
            for (const Declaration& d : rule.declarations)
                if ((d.name == "font-family" || d.name == "font") && namesBitmapFamily(trimmed(serialize(d.value))))
                    bitmap = true;
            if (!bitmap)
                continue;
            for (const Declaration& d : rule.declarations){
                const std::string value = lower(serialize(d.value));
                bool blurred = false;
                if (d.name == "font-effect")
                    blurred = value.find("glow") != std::string::npos || value.find("blur") != std::string::npos;
                if (d.name == "text-shadow" && value != "none"){
                    // a third length that is not zero is a blur radius
                    for (const std::string& shadow : split(value, ',')){
                        int lengths = 0;
                        for (const std::string& part : split(trimmed(shadow), ' ')){
                            if (part.empty() || !(std::isdigit((unsigned char)part[0]) || part[0] == '-' || part[0] == '.'))
                                continue;
                            if (++lengths == 3 && std::atof(part.c_str()) != 0.0)
                                blurred = true;
                        }
                    }
                }
                if (!blurred)
                    continue;
                std::string buffer;
                const std::string selector = rule.selectorCount() > 0 ? std::string(rule.selectorText(0, buffer)) : std::string();
                session.sink->report(codes::BitmapFontEffect, d.ref,
                                     "'" + selector + "' blurs a bitmap font (" + d.name + ": " + serialize(d.value) + "): dropped",
                                     "use font-effect: outline(1px <colour>) or a text-shadow without blur", std::string(), selector);
            }
        }
    }
    unique(result.fontFamilies);
    unique(result.declaredFontFamilies);
    unique(result.bitmapFontFamilies);
    unique(result.animationFillNames);
    return result;
}

std::string CssRewriter::rewriteSelector(std::string_view selector, DiagnosticSink* sink, const CompatOptions& options){
    // (no session: the parsed selector lives in this arena)
    Arena arena;
    ArenaScope scope(arena);
    SourceFiles files;
    const uint32_t file = files.add("[selector]", std::string(selector));
    DiagnosticSink local(files);
    std::string error;
    std::optional<SelectorList> list = SelectorRewriter::parseList(parseComponentValueList(files.text(file)), file, &error);
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
