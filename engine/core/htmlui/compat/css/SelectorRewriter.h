// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_SELECTORREWRITER_H
#define COMPAT_CSS_SELECTORREWRITER_H

// Selectors Level 4 to what RmlUi's ImportProperties reads (phase-2 §3.8, §3.9.5, Appendix
// B): the HTML root and body names, the #htmlui-root band that ranks author rules above the
// user-agent sheet, focus pseudo-classes, :is()/:where() expansion, pseudo-element mappings,
// and the drops (with diagnostics) of what RmlUi cannot match.

#include "Export.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/css/CssAst.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    class DiagnosticSink;
    struct ComplexSelector;

    struct SimpleSelector {
        enum class Kind : uint8_t { Type, Universal, Id, Class, Attribute, PseudoClass, PseudoElement, Nesting, RootMarker };
        // (members ordered by size)
        Kind kind = Kind::Universal;
        bool hasNamespace = false;     // ns|E, *|E, |E, [ns|a]
        char flag = 0;                 // attribute: 'i' or 's'
        bool function = false;         // :name(...)
        bool legacyColon = false;      // :before / :after written with one colon
        SourceRef ref;
        std::string name;              // type (lowercase), id, class, attribute name, pseudo name (lowercase, no colons)
        std::string op;                // attribute: "", "=", "~=", "|=", "^=", "$=", "*="
        std::string value;             // attribute value (unescaped)
        std::vector<ComponentValue> arguments;      // raw arguments of a functional pseudo
        std::vector<ComplexSelector> selectors;     // parsed selector-list arguments (:is, :where, :not, :has, ...)
    };

    struct Compound {
        char combinator = 0;           // to the previous compound: ' ', '>', '+', '~', '|' (column ||); 0 for the first
        std::vector<SimpleSelector> simples;
    };

    struct ComplexSelector {
        std::vector<Compound> compounds;
        SourceRef ref;
        bool relative = false;         // starts with a combinator (nested rules: "> .x")
    };

    struct SelectorOptions {
        enum class Origin : uint8_t { Author, UserAgent, Query };
        Origin origin = Origin::Author;   // Query: Lua querySelector (no band, no specificity)
        ForkFeatures fork = ForkFeatures::present();
        // the author's attributes of the HTML <html> element (RewriteContext::rootAttributes)
        std::string rootId;
        std::vector<std::string> rootClasses;
        std::vector<std::string> rootAttributes;   // attribute names
    };

    struct RewrittenSelector {
        std::string text;              // RCSS
        uint32_t source = 0;           // the CSS selector it came from: its index in the list given to rewrite()
        uint32_t specificity = 0;      // RmlUi weights (id 1 000 000, class/attribute/pseudo-class 100 000, tag 10 000)
        bool placeholder = false;      // from ::placeholder: only color, opacity and font-* apply
        bool pseudoElement = false;    // ::before/::after kept for the fork
    };

    struct SelectorRewriteResult {
        std::vector<RewrittenSelector> selectors;
        bool invalid = false;          // a syntax error: the whole rule is dropped (as browsers do)
    };

    class JASPERENGINE_API SelectorRewriter {
    public:
        // Parses a selector list (a style rule prelude); nested-rule forms (&, leading
        // combinators) are accepted. nullopt on a syntax error; errorRef/error say where/why.
        static std::optional<std::vector<ComplexSelector>> parseList(const std::vector<ComponentValue>& prelude, uint32_t file,
                                                                     std::string* error = nullptr, SourceRef* errorRef = nullptr);
        static std::optional<std::vector<ComplexSelector>> parseList(std::string_view text, std::string* error = nullptr);

        // Rewrites a parsed list; diagnostics at ruleRef (sink may be null).
        static SelectorRewriteResult rewrite(const std::vector<ComplexSelector>& list, const SelectorOptions& options,
                                             DiagnosticSink* sink, SourceRef ruleRef);

        // CSS / RCSS text of a selector
        static std::string serialize(const ComplexSelector& selector);
        static std::string serialize(const Compound& compound);
        static std::string serialize(const SimpleSelector& simple);
        // RmlUi's specificity of a selector
        static uint32_t specificity(const ComplexSelector& selector);

        // Would the rewriter keep (part of) the selector? @supports selector() asks this.
        static bool supported(const std::vector<ComplexSelector>& list, const SelectorOptions& options);

        // The subject compound of a selector (its last compound)
        static const Compound* subject(const ComplexSelector& selector);
        // Do the subject compounds of two selectors overlap: a shared tag, id or class, or one
        // of them universal (no tag, id or class)? (!important and @layer conflict checks)
        static bool subjectsOverlap(const ComplexSelector& a, const ComplexSelector& b);
        // The base-rule key (§3.9.8): the CSS text with pseudo-classes, attribute and
        // structural selectors removed from the subject compound
        static std::string baseKey(const ComplexSelector& selector);
        // the subject compound's type selector ("" when none)
        static std::string subjectTag(const ComplexSelector& selector);
        // does the selector's subject chain start at the root (html, :root, the root id)?
        static bool isRootRule(const ComplexSelector& selector, const SelectorOptions& options);
    };

}

#endif //COMPAT_CSS_SELECTORREWRITER_H
