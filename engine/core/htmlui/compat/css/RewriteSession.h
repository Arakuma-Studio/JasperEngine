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
        std::vector<ComplexSelector> selectors;    // CSS selectors, nesting resolved
        SourceRef ref, preludeRef;
        MediaResult media = MediaResult::unconditional();
        std::vector<Declaration> declarations;
        int layer = -1;                // index in Session::layerOrder; -1 unlayered
        uint32_t order = 0;            // source order within the document
        // the selector stage
        std::vector<RewrittenSelector> rewritten;
        bool dropped = false;
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

    struct OutDecl {
        std::string name;
        std::string value;
        SourceRef ref;
        bool important = false;
        bool vmin = false;             // holds vmin/vmax: written in orientation copies of the rule
        bool final = false;            // already through the pipeline's last step (synthetic declarations)
    };

    // a rule generated next to an author rule: <rule selector><suffix> { decls }
    struct GeneratedRule {
        std::string suffix;            // " > *", " > scrollbarvertical"
        std::vector<OutDecl> declarations;
    };

    // What the declaration pipeline produced for one rule (or one style attribute)
    struct RuleOutput {
        std::vector<OutDecl> declarations;
        std::vector<GeneratedRule> generated;
    };

    struct Session;

    // Everything a property rewrite sees about the declaration it rewrites.
    struct DeclContext {
        Session& session;
        const FlatRule* rule = nullptr;     // nullptr: a style attribute or a keyframe block
        RuleOrigin origin = RuleOrigin::Author;
        bool keyframe = false;
        const Declaration& declaration;
        std::string name;                   // the CSS property (vendor prefix resolved), lowercase
        const PropertySupport* row = nullptr;
        std::vector<ComponentValue> value;  // normalized (colors hex, times s, units mapped, calc folded)
        bool important = false;
        RuleOutput& output;
        bool dropped = false;               // the rewrite dropped the declaration

        void emit(const std::string& property, const std::string& value);
        void emit(const std::string& property, const std::string& value, SourceRef ref);
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

    struct Session {
        Session(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, const RewriteContext& context);

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
        std::set<std::string> inheritCompanions;        // RCSS properties some rule sets to `inherit` (non-inherited)
        std::set<std::string> reversedKeyframes;        // keyframes an animation plays in reverse
        std::map<std::string, std::string> keyframeNames;   // CSS name -> RCSS name
        std::set<std::string> fillKeyframes;
        std::optional<double> rootFontSize;             // px, from static root rules
        std::string rootDirection;                      // "rtl" or "ltr"
        SelectorOptions selectorOptions;
        uint32_t nextOrder = 0;
        bool nestingReported = false;

        // base rules: selector text -> rules (cascade order)
        std::unordered_map<std::string, std::vector<uint32_t>> baseIndex;
        bool baseIndexBuilt = false;    // built by the first BaseRules::find (most sheets never need it)

        // CSS property -> the rules declaring it (!important and @layer conflict checks)
        struct PropertyUse {
            uint32_t rule = 0;
            bool important = false;
            SourceRef ref;
        };
        std::unordered_map<std::string, std::vector<PropertyUse>> propertyRules;

        // per-rule static facts, computed on demand
        // (by rule index; sized on first use)
        std::vector<std::optional<std::string>> staticColors;
        std::string scratchColor;       // ruleStaticColor of a rule outside `rules`
        std::vector<std::optional<std::optional<double>>> fontSizes;

        // the !important sheet (§3.9.6 step 7)
        struct ImportantRule {
            const FlatRule* rule = nullptr;
            std::vector<OutDecl> declarations;
        };
        std::vector<ImportantRule> important;

        // props::varKind of a var() without fallback, by variable name, for vars.revision()
        NameTable<std::pair<uint32_t, std::string>> varKinds;

        // parseComponentValueList of the values processSynthetic is given (shorthand rewrites
        // give the same few texts again and again), trimmed
        NameTable<std::vector<ComponentValue>> syntheticValues;

        // per property name (session-wide: the tables do not change)
        NameTable<PropertyFacts> propertyFacts;
        const PropertyFacts& facts(std::string_view property);

        // helpers
        bool fork(ForkFeature feature) const { return options.fork.has(feature); }
        const ImportManifest& manifest() const;
        std::string assetPathOf(uint32_t file) const { return files.path(file); }
        std::string keyframeName(const std::string& css);
        // a var() function's static value (every declaration of the variable identical and static)
        bool staticVar(const ComponentValue& var, std::vector<ComponentValue>& out) const;
        // A var() used at a position of a structured rewrite (family: "decorator", "font-effect",
        // ...): its kind and how to write it; nullopt when the variable cannot be typed
        // (undeclared, mixed kinds): the caller reports HTMLUI-CSS-0012.
        std::optional<TypedVar> typedVar(const ComponentValue& var, const std::string& family);
    };

    // ---- declaration pipeline (CssRewriter.cpp)

    // rewrites the declarations of a rule (or a style attribute / keyframe block)
    void processDeclarations(Session& session, const FlatRule* rule, RuleOrigin origin, bool keyframe,
                             const std::vector<Declaration>& declarations, RuleOutput& output);
    // runs `property: value` (CSS text) through the pipeline as part of the declaration of
    // `context` (CSS-wide keywords writing initial values, longhand expansions); the outputs
    // are added to context.output
    void processSynthetic(DeclContext& context, const std::string& property, const std::string& value);

}

#endif //COMPAT_CSS_REWRITESESSION_H
