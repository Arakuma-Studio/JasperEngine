// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_PROPS_REWRITERS_H
#define COMPAT_CSS_PROPS_REWRITERS_H

// The property rewrites of phase-2 §3.9.7, one function per RewriteId (css/props/*.cpp), and
// the helpers they share. The registry is an explicit table (Rewriters.cpp): the engine is a
// static library, so self-registering objects in otherwise unreferenced files would be
// dropped by the linker.

#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/css/RewriteSession.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    using RewriteFunction = void (*)(DeclContext& context);

    struct RewriteRegistration {
        RewriteId id;
        RewriteFunction function;
    };

    // every registered rewrite (one per RewriteId other than None)
    Span<const RewriteRegistration> rewriteRegistry();
    RewriteFunction findRewrite(RewriteId id);

    // A var() family (§3.9.8): how a structured rewrite types a whole-value var() and writes
    // the variable's declarations in that kind.
    struct VarFamily {
        const char* name;   // "decorator", "font-effect", ...
        // the kind of one declared value ("" = not a value of this family)
        std::string (*classify)(Session& session, const std::vector<ComponentValue>& value);
        // the RCSS text of a declared value of that kind (nullopt: cannot be written)
        std::optional<std::string> (*transform)(Session& session, const std::vector<ComponentValue>& value, const std::string& kind,
                                                SourceRef ref);
    };
    const VarFamily* findVarFamily(const std::string& name);
    // the family the declarations of a property type their var() uses in ("" = written as declared)
    std::string varFamilyOf(const std::string& property);

    // ---- the rewrites (css/props/*.cpp)
    void rewriteBackground(DeclContext&);
    void rewriteBackgroundLonghand(DeclContext&);
    void rewriteMaskImage(DeclContext&);
    void rewriteBorder(DeclContext&);
    void rewriteBorderStyle(DeclContext&);
    void rewriteBorderRadius(DeclContext&);
    void rewriteBorderImage(DeclContext&);
    void rewriteBorderSpacing(DeclContext&);
    void rewriteOutline(DeclContext&);
    void rewriteBoxShadow(DeclContext&);
    void rewriteFont(DeclContext&);
    void rewriteFontFamily(DeclContext&);
    void rewriteFontSize(DeclContext&);
    void rewriteFontWeight(DeclContext&);
    void rewriteFontStyle(DeclContext&);
    void rewriteLineHeight(DeclContext&);
    void rewriteTextShadow(DeclContext&);
    void rewriteTextStroke(DeclContext&);
    void rewriteTextFillColor(DeclContext&);
    void rewriteTextDecoration(DeclContext&);
    void rewriteTextAlign(DeclContext&);
    void rewriteWhiteSpace(DeclContext&);
    void rewriteOverflowWrap(DeclContext&);
    void rewriteLineClamp(DeclContext&);
    void rewriteDirection(DeclContext&);
    void rewriteTransition(DeclContext&);
    void rewriteTransitionLonghand(DeclContext&);
    void rewriteAnimation(DeclContext&);
    void rewriteAnimationLonghand(DeclContext&);
    void rewriteDisplay(DeclContext&);
    void rewritePosition(DeclContext&);
    void rewriteLogicalBox(DeclContext&);
    void rewriteSizing(DeclContext&);
    void rewriteOverflow(DeclContext&);
    void rewriteVisibility(DeclContext&);
    void rewriteZIndex(DeclContext&);
    void rewriteOpacity(DeclContext&);
    void rewriteGap(DeclContext&);
    void rewriteGrid(DeclContext&);
    void rewriteFlex(DeclContext&);
    void rewritePlaceAlign(DeclContext&);
    void rewriteAlignKeyword(DeclContext&);
    void rewriteIndividualTransform(DeclContext&);
    void rewriteFilter(DeclContext&);
    void rewriteClipPath(DeclContext&);
    void rewriteListStyle(DeclContext&);
    void rewriteObjectFit(DeclContext&);
    void rewriteImageRendering(DeclContext&);
    void rewriteAccentColor(DeclContext&);
    void rewriteAppearance(DeclContext&);
    void rewriteColorValue(DeclContext&);
    void rewriteCursor(DeclContext&);
    void rewritePointerEvents(DeclContext&);
    void rewriteTabIndexLike(DeclContext&);
    void rewriteSvgPaint(DeclContext&);
    void rewriteScrollbarStyle(DeclContext&);
    void rewriteContent(DeclContext&);
    void rewriteAll(DeclContext&);

    // var() families (css/props/*.cpp)
    extern const VarFamily kDecoratorFamily;
    extern const VarFamily kMaskFamily;
    extern const VarFamily kFontEffectFamily;
    extern const VarFamily kTransitionFamily;
    extern const VarFamily kAnimationFamily;
    extern const VarFamily kFontFamilyFamily;
    extern const VarFamily kFilterFamily;
    extern const VarFamily kEasingFamily;

    // ---- helpers shared by the rewrites (css/props/Rewriters.cpp)
    namespace props {

        bool isWs(const ComponentValue& value);
        // top-level groups split at whitespace / commas (whitespace trimmed)
        std::vector<ComponentValue> words(const std::vector<ComponentValue>& value);
        std::vector<std::vector<ComponentValue>> commaGroups(const std::vector<ComponentValue>& value);
        std::string text(const ComponentValue& value);
        std::string text(const std::vector<ComponentValue>& value);
        // "a b c" from words
        std::string join(const std::vector<std::string>& parts, const char* separator = " ");

        bool isIdent(const ComponentValue& value, const char* lowercase);
        std::string identOf(const ComponentValue& value);        // lowercase ident, "" otherwise
        bool isVar(const ComponentValue& value);
        // the kind of a var() over every declared value: "color", "length", "number",
        // "percentage", "time", "angle", "string", "other", "mixed", "undeclared"
        std::string varKind(Session& session, const ComponentValue& var);
        // a color after normalization (#rrggbbaa, the current-color companion, or a var() of colors)
        bool isColor(Session& session, const ComponentValue& value);
        // a length (dimension with a length unit, 0, or a var() of lengths); percent allowed when asked
        bool isLength(Session& session, const ComponentValue& value, bool allowPercent = false);
        bool isNumber(const ComponentValue& value);
        bool isTime(Session& session, const ComponentValue& value);
        std::optional<double> lengthPx(const ComponentValue& value);   // px of an absolute length, 0 for 0
        // "0" written as "0px" (RmlUi's length parsers accept both; keeps output uniform)
        std::string lengthText(const ComponentValue& value);

        // resolves a url() / string for a resource: the path RCSS references ("/ui/a.png",
        // "/@htmlui/data/....png"); registers data URIs, remote references and dependencies.
        // "" when dropped (reported).
        std::string resolveUrl(DeclContext& context, const ComponentValue& value, const char* kind);
        std::string resolveUrlIn(Session& session, const std::string& url, uint32_t file, SourceRef ref, const char* kind);
        // the url text of a url() token / function / string ("" otherwise)
        std::optional<std::string> urlOf(const ComponentValue& value);

        // a whole-value var() typed for a family; reports HTMLUI-CSS-0012 when it cannot be typed
        std::optional<TypedVar> wholeVar(DeclContext& context, const std::string& family);

        // the static value a rule declares for a property (normalized), last one wins
        std::optional<std::vector<ComponentValue>> ruleValue(Session& session, const FlatRule& rule, const std::string& property);
        // the rule's static color (#rrggbbaa) or ""
        std::string ruleStaticColor(Session& session, const FlatRule& rule);
        // the rule's static font-size in px (absolute lengths only)
        std::optional<double> ruleFontSizePx(Session& session, const FlatRule& rule);

        // logical side -> physical (LTR, or RTL when the document root is rtl)
        std::string physicalSide(Session& session, const std::string& logical);   // "inline-start" -> "left"

        // the RCSS grammar check of a value for an RmlUi property (RcssGrammar)
        bool accepts(const std::string& property, const std::string& value);

        // A CSS easing (keyword or function) as written for RmlUi: the nearest tween until the
        // fork has TimingFunctions (Transition.cpp). note/code/severity: what to report ("" =
        // exact). nullopt when the value is not an easing.
        struct EasingResult {
            std::string text;
            DiagCode code{};
            Severity severity = Severity::Info;
            std::string note;
        };
        std::optional<EasingResult> easingOf(Session& session, const ComponentValue& value);
    }

    // the html-shape mask decorator of a clip-path declaration (Effects.cpp); nullopt when none
    // or unsupported (reported)
    std::optional<std::string> clipPathShape(DeclContext& context, const Declaration& declaration);
    // does the rule set translate / rotate / scale (Transform.cpp: transform is merged with them)
    bool ruleHasIndividualTransform(const FlatRule& rule);
    // does a declaration of the rule reach its property rewrite (known or vendor-prefixed
    // property, not Unsupported / Ignored, not a CSS-wide keyword)? The families merged at
    // their last declaration (backgrounds, masks, transitions, animations) only count these.
    bool reachesRewrite(Session& session, const Declaration& declaration, const std::vector<Declaration>& block);

}

#endif //COMPAT_CSS_PROPS_REWRITERS_H
