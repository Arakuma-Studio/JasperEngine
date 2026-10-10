// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_REWRITESESSION_H
#define COMPAT_CSS_REWRITESESSION_H

// The state of one CssRewriter session (phase-2 §3.9), shared by the rewriter's parts:
// flattening (Import, Nesting, AtRules), the declaration pipeline (ValueNormalizer,
// CascadeKeywords, the property rewrites in css/props), companions and base rules. Internal
// to the compat layer; CssRewriter.h is the public interface.

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/StringIndex.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/css/CssAst.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/css/CustomPropertyIndex.h"
#include "htmlui/compat/css/MediaQuery.h"
#include "htmlui/compat/css/SelectorRewriter.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::htmlui::compat {

    class ImportManifest;

    enum class RuleOrigin : uint8_t { UserAgent, Author, StyleAttribute };

    // A style rule after flattening (@import inlined, @supports evaluated, nesting resolved,
    // @layer ordered), in cascade order.
    struct FlatRule {
        uint32_t index = 0;            // position in Session::rules
        uint32_t sheet = 0;            // the input (output sheet) it is written to
        uint32_t file = 0;
        RuleOrigin origin = RuleOrigin::Author;
        // The CSS selectors, nesting resolved. A rule whose selector list was scanned
        // (SelectorRewriter::scanList: `scanned`, its rewritten selectors and their CSS texts set
        // when it was flattened) has them parsed from its prelude at their first use.
        const SelectorList& selectors() const;
        SourceRef ref, preludeRef;
        MediaResult media = MediaResult::unconditional();
        DeclarationList declarations;
        int layer = -1;                // index in Session::layerOrder; -1 unlayered
        uint32_t order = 0;            // source order within the document
        // the selector stage
        ArenaList<RewrittenSelector> rewritten;
        bool dropped = false;

        // what selectors() reads: the list parsed, or the parsed rule whose prelude it is
        // parsed from (preludeOf; the parsed sheet lives in the session's arena)
        mutable SelectorList parsedSelectors;
        mutable bool selectorsParsed = false;
        const Rule* source = nullptr;
        // a scanned list: serialize() of each selector (never a root or `body` rule: scanList
        // reads no html/body type, :root or root id)
        bool scanned = false;
        ArenaList<Text> selectorTexts;
        // the CSS text of selector `i` (written into `buffer` unless the list was scanned)
        std::string_view selectorText(size_t i, std::string& buffer) const;
        size_t selectorCount() const { return scanned ? selectorTexts.size() : selectors().size(); }
    };

    // An at-rule written as a block (@font-face, @keyframes, @decorator, @spritesheet).
    struct FlatAtRule {
        uint32_t sheet = 0;
        uint32_t file = 0;
        RuleOrigin origin = RuleOrigin::Author;
        Rule rule;
        MediaResult media = MediaResult::unconditional();
        int layer = -1;
        uint32_t order = 0;
    };

    // one item of an output sheet, in writing order
    struct SheetItem {
        bool atRule = false;
        uint32_t index = 0;            // into Session::rules or Session::atRules
    };

    // (in the session's arena: nothing to destroy)
    struct OutDecl {
        Text name;
        Text value;
        SourceRef ref;
        bool important = false;
        bool vmin = false;             // holds vmin/vmax: written in orientation copies of the rule
        bool final = false;            // already through the pipeline's last step (synthetic declarations)
    };

    // a rule generated next to an author rule: <rule selector><suffix> { decls }
    struct GeneratedRule {
        Text suffix;                   // " > *", " > scrollbarvertical"
        ArenaList<OutDecl> declarations;
    };

    // What the declaration pipeline produced for one rule (or one style attribute)
    struct RuleOutput {
        ArenaList<OutDecl> declarations;
        ArenaList<GeneratedRule> generated;
    };

    struct Session;

    // DeclContext::rule. Reading the rule (not testing it for null) counts in Session::memoTaint:
    // the declaration's output then depends on its rule and is not kept by the declaration
    // memo (processOne).
    struct RuleRef {
        const FlatRule* pointer = nullptr;
        uint32_t* taint = nullptr;

        explicit operator bool() const { return pointer != nullptr; }
        operator const FlatRule*() const { ++*taint; return pointer; }
        const FlatRule* operator->() const { ++*taint; return pointer; }
        const FlatRule& operator*() const { ++*taint; return *pointer; }
        const FlatRule* untracked() const { return pointer; }
    };

    // Everything a property rewrite sees about the declaration it rewrites.
    struct DeclContext {
        Session& session;
        RuleRef rule;                       // null: a style attribute or a keyframe block
        RuleOrigin origin = RuleOrigin::Author;
        bool keyframe = false;
        const Declaration& declaration;
        std::string name;                   // the CSS property (vendor prefix resolved), lowercase
        const PropertySupport* row = nullptr;
        ValueList value;  // normalized (colors hex, times s, units mapped, calc folded)
        bool important = false;
        RuleOutput& output;
        bool dropped = false;               // the rewrite dropped the declaration

        // (by value: a temporary is moved into the output)
        void emit(Text property, Text value);
        void emit(Text property, Text value, SourceRef ref);
        void generate(const std::string& suffix, const std::string& property, const std::string& value);
        // diagnostics at the declaration (or its value)
        void report(DiagCode code, const std::string& message, const std::string& suggestion = std::string(),
                    const std::string& replacement = std::string());
        void reportAs(Severity severity, DiagCode code, const std::string& message, const std::string& suggestion = std::string(),
                      const std::string& subject = std::string());
        void reportAt(SourceRef ref, Severity severity, DiagCode code, const std::string& message,
                      const std::string& suggestion = std::string());
        SourceRef valueRef() const;
        std::string valueText() const;      // the normalized value as CSS text
        std::string sourceText() const;     // the declaration as written
        bool fork(ForkFeature feature) const;
    };

    // A var() a structured rewrite met: how it is written at the use site
    struct TypedVar {
        std::string kind;                  // the classifier's kind ("color", "layers", ...)
        std::string reference;             // "var(--x)" or "var(--x--decorator)" (with the fallback when given)
    };

    // What the declaration pipeline looks up about a property name, computed once per name
    struct PropertyFacts {
        const PropertySupport* row = nullptr;                    // findProperty
        bool colorKeywords = true;                               // ValueNormalizer::colorKeywordsApply
        PercentBasis percentBasis = PercentBasis::ContainingBlock;   // ValueNormalizer::percentBasisOf
        bool vertical = false;
        bool rcssKnown = false;                                  // RcssGrammar::known
    };

    // The storage of a session's big lists, which a thread keeps from one session to the next
    // (emptied, its pages already mapped): as the arena's blocks, the memory of a rewrite is not
    // mapped again for every style sheet (the page faults of fresh memory are a part of a
    // rewrite's time that no instruction count shows). `Slot` tells lists of one type apart.
    template<typename T, int Slot>
    std::vector<T>& sessionSpare(){
        static thread_local std::vector<T> spare;
        return spare;
    }

    struct Session {
        Session(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, const RewriteContext& context);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // everything the session parses and builds (token texts, value lists, selectors) lives
        // in its arena, current for the session's lifetime and released with it; declared
        // first: the members below are destroyed before it
        Arena arena;
        ArenaScope arenaScope{arena};

        CompatOptions options;
        SourceFiles& files;
        DiagnosticSink* sink;              // a pointer: @supports value checks run against a scratch sink
        RewriteContext context;
        UrlResolver urls;
        RewriteResult* result = nullptr;

        std::vector<FlatRule> rules;
        std::vector<FlatAtRule> atRules;
        std::vector<std::vector<SheetItem>> sheets;     // per input
        std::vector<std::string> layerOrder;            // declared layer names, in order
        std::vector<const StylesheetInput*> inputs;

        CustomPropertyIndex vars;

        // document-wide facts (pre-pass)
        bool usesBorderImage = false;
        bool usesTextShadow = false;
        bool usesTextStroke = false;
        std::set<std::string, std::less<>> inheritCompanions;        // RCSS properties some rule sets to `inherit` (non-inherited)
        std::set<std::string> reversedKeyframes;        // keyframes an animation plays in reverse
        std::map<std::string, std::string> keyframeNames;   // CSS name -> RCSS name
        std::set<std::string> fillKeyframes;
        std::optional<double> rootFontSize;             // px, from static root rules
        std::string rootDirection;                      // "rtl" or "ltr"
        // §3.10 item 1: no root rule sets a background and a `body` rule does: the body's
        // background is written on the root (it covers the canvas, as CSS propagates it)
        bool propagateBodyBackground = false;
        SelectorOptions selectorOptions;            // author rules
        SelectorOptions userAgentSelectorOptions;   // the same for user-agent rules
        uint32_t nextOrder = 0;
        bool nestingReported = false;

        // base rules: selector text -> rules (cascade order)
        // BaseRules: the author rules by the hash of the CSS text of each of their selectors
        // (open addressing; lists in the session's arena). A lookup compares the texts of the
        // rules it finds.
        struct BaseEntry {
            uint64_t hash = 0;
            ArenaList<uint32_t> rules;
        };
        std::vector<BaseEntry> baseEntries;
        std::vector<uint32_t> baseSlots;
        std::string baseKeyText, baseCandidateText;   // BaseRules::find
        bool baseIndexBuilt = false;    // built by the first BaseRules::find (most sheets never need it)

        // CSS property -> the rules declaring it (!important and @layer conflict checks)
        struct PropertyUse {
            uint32_t rule = 0;
            bool important = false;
            SourceRef ref;
        };
        // (built at its first use from propertyUses, in the same order: most sheets need it
        // only for !important and @layer, see propertyRulesIndex())
        std::unordered_map<std::string, std::vector<PropertyUse>> propertyRules;
        struct PendingUse {
            const Declaration* declaration = nullptr;
            uint32_t prefix = 0;       // the vendor prefix length stripped from the declaration's name
            uint32_t rule = 0;
        };
        std::vector<PendingUse> propertyUses;
        bool propertyRulesBuilt = false;
        std::unordered_map<std::string, std::vector<PropertyUse>>& propertyRulesIndex(){
            if (!propertyRulesBuilt){
                propertyRulesBuilt = true;
                for (const PendingUse& pending : propertyUses){
                    PropertyUse use;
                    use.rule = pending.rule;
                    use.important = pending.declaration->important;
                    use.ref = pending.declaration->ref;
                    propertyRules[pending.declaration->name.substr(pending.prefix)].push_back(use);
                }
                propertyUses.clear();
            }
            return propertyRules;
        }

        // per-rule static facts, computed on demand
        // (by rule index; sized on first use)
        std::vector<std::optional<std::string>> staticColors;
        std::string scratchColor;       // ruleStaticColor of a rule outside `rules`
        std::vector<std::optional<std::optional<double>>> fontSizes;

        // the !important sheet (§3.9.6 step 7)
        struct ImportantRule {
            const FlatRule* rule = nullptr;
            ArenaList<OutDecl> declarations;
        };
        std::vector<ImportantRule> important;

        // props::varKind of a var() without fallback, by variable name, for vars.revision()
        NameTable<std::pair<uint32_t, std::string>> varKinds;

        // parseComponentValueList of the values processSynthetic is given (shorthand rewrites
        // give the same few texts again and again), trimmed
        NameTable<ValueList> syntheticValues;

        // buffers reused from one rule / declaration to the next (their contents are cleared,
        // their memory kept)
        RuleOutput scratchOutput;                               // writeRule

        // The declaration memo (processOne): the output of a declaration that depends on its
        // text alone (nothing read from its rule, its block or the custom properties, no URL
        // resolved, no diagnostic, no generated rule) is kept under that text and written
        // again for the same text in the same situation. On from the writing phase on
        // (memoOn), while the diagnostics go to the session's own sink.
        struct MemoEntry {
            uint64_t hash = 0;
            Text text;                     // Declaration::text
            uint32_t file = 0;
            uint8_t bits = 0;              // important, keyframe, in a rule, origin
            bool pure = false;             // false: known to depend on more than its text
            bool completed = false;        // the pipeline ran to its end (the checks after it apply)
            bool readVars = false;         // read custom properties: holds while the index is at varRevision
            uint32_t varRevision = 0;
            Text name;                     // the effective property name
            uint32_t first = 0, count = 0; // in memoOutput
            uint32_t fontFirst = 0, fontCount = 0;   // in memoFonts: the font families it reported
        };
        std::vector<MemoEntry> memoEntries;
        std::vector<uint32_t> memoSlots;
        ArenaList<OutDecl> memoOutput;
        std::vector<std::string> memoFonts;
        mutable uint32_t memoTaint = 0;    // counts the reads that make a declaration depend on more than its text
        // counts the reads of the custom property index: the same for every declaration of the
        // session once the index is complete (a rule has no custom properties of its own here)
        mutable uint32_t memoVarReads = 0;
        bool memoOn = false;
        bool memoDisabled = false;         // JASPER_HTMLUI_CSS_MEMO=off
        bool memoCheck = false;            // JASPER_HTMLUI_CSS_MEMO=check: every memo hit is compared with the pipeline's output
        DiagnosticSink* mainSink = nullptr;
        std::string scratchText;
        // @media preludes that were normalized without a diagnostic, by their text (the same
        // query heads many rules); nullopt: reported something, normalized again each time
        NameTable<std::optional<MediaResult>> mediaMemo;
        std::vector<ValueList> valuePool;     // normalized values of processOne

        // per property name (session-wide: the tables do not change)
        NameTable<PropertyFacts> propertyFacts;
        const PropertyFacts& facts(std::string_view property);

        // helpers
        bool fork(ForkFeature feature) const { return options.fork.has(feature); }
        const ImportManifest& manifest() const;
        std::string assetPathOf(uint32_t file) const { return files.path(file); }
        std::string keyframeName(const std::string& css);
        // a var() function's static value (every declaration of the variable identical and static)
        bool staticVar(const ComponentValue& var, ValueList& out) const;
        // A var() used at a position of a structured rewrite (family: "decorator", "font-effect",
        // ...): its kind and how to write it; nullopt when the variable cannot be typed
        // (undeclared, mixed kinds): the caller reports HTMLUI-CSS-0012.
        std::optional<TypedVar> typedVar(const ComponentValue& var, const std::string& family);
    };

    // ---- declaration pipeline (CssRewriter.cpp)

    // rewrites the declarations of a rule (or a style attribute / keyframe block)
    void processDeclarations(Session& session, const FlatRule* rule, RuleOrigin origin, bool keyframe,
                             const DeclarationList& declarations, RuleOutput& output);
    // runs `property: value` (CSS text) through the pipeline as part of the declaration of
    // `context` (CSS-wide keywords writing initial values, longhand expansions); the outputs
    // are added to context.output
    void processSynthetic(DeclContext& context, const std::string& property, const std::string& value);

}

#endif //COMPAT_CSS_REWRITESESSION_H
