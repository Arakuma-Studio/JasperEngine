// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_SUPPORTTABLES_H
#define COMPAT_SUPPORTTABLES_H

// What the engine does with every CSS property, value, selector, at-rule, media feature,
// HTML element, attribute, input type, unit and function (phase-2 §3.5, Appendices A-E).
// The tables are the single source of truth: the rewriter dispatches through them,
// diagnostics name their subject after them, phase 4 renders them to the support matrix
// (jasperengine-editor ui-support). Data in compat/tables/*.cpp.

#include "Export.h"
#include "CompatTypes.h"
#include "DiagnosticCodes.h"
#include "ForkFeatures.h"

#include <cstdint>
#include <string_view>

namespace jasperengine::htmlui::compat {

    enum class Support : uint8_t {
        Native,        // RmlUi understands the CSS as written (values may be normalized)
        Rewritten,     // rewritten to RCSS with the same result
        Approximated,  // rewritten with a visible but bounded difference (diagnostic)
        Emulated,      // implemented by compat runtime code (custom property, element, decorator)
        Fallback,      // waits for a phase-3 fork feature; a fallback runs until the flag is on
        Unsupported,   // dropped (diagnostic with a suggestion)
        Ignored,       // accepted and dropped because it has no effect in a game UI (Info)
    };

    // The rewrite function a property row dispatches to (css/props/*.cpp, p2-css). None =
    // passed through after value normalization (and the value table). Append only.
    enum class RewriteId : uint16_t {
        None, Background, BackgroundLonghand, Border, BorderStyle, BorderRadius, BorderImage,
        Outline, BoxShadow, Font, FontFamily, FontSize, FontWeight, FontStyle, LineHeight,
        TextShadow, TextStroke, TextFillColor, TextDecoration, TextAlign, Transition,
        TransitionLonghand, Animation, AnimationLonghand, Display, Position, LogicalBox,
        PlaceAlign, AlignKeyword, Overflow, OverflowWrap, WhiteSpace, Filter, MaskImage,
        ClipPath, ListStyle, ObjectFit, ImageRendering, AccentColor, Appearance,
        IndividualTransform, ColorValue, Cursor, PointerEvents, TabIndexLike, Direction,
        SvgPaint, ScrollbarStyle, Gap, BorderSpacing, Visibility, ZIndex, Opacity,
        // added by p2-core where Appendix A rewrites a property no id above covers
        Sizing,      // width/height/min-*/max-*: sizing keywords, min-*: auto
        Flex,        // flex: none/auto/<number>; legacy -webkit-box-* / -ms-flex* properties
        Grid,        // grid-* properties read by the display: grid fallback
        Content,     // content (pseudo-elements)
        All,         // all: unset/initial/inherit/revert
        LineClamp,   // -webkit-line-clamp, line-clamp
        // added by phase 3
        UserSelect,  // user-select (p3-text: text/all/contain select nothing outside text inputs)
        Count
    };

    // Appendix B rows: Selector = type/universal/class/id/attribute selectors, combinators,
    // lists, nesting, namespaces; then pseudo-classes and pseudo-elements (phase 4's kinds
    // selector / pseudo_class / pseudo_element).
    enum class SelectorKind : uint8_t { Selector, PseudoClass, PseudoElement };

    // Every row: support without fork features; fork = the ForkFeature that changes it (None
    // if none) and supportWithFork = the support when it is present. Every row with a status
    // other than Native has a note; Unsupported rows have a suggestion.

    struct PropertySupport {
        const char* name;            // "background", "-webkit-text-stroke"; sorted, unique
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        bool inherited;              // CSS inheritance
        const char* initial;         // CSS initial value as CSS text ("0", "currentColor", "normal")
        bool rmluiDefaultDiffers;    // the fork's default is not the CSS initial value (initial/unset write `initial` literally);
                                     // phase 3 made most of them equal (docs/html-ui/FORK.md P3-4)
        RewriteId rewrite;
        const char* output;          // RCSS it produces: "decorator; background-color"
        const char* note;            // one line for the matrix
        const char* suggestion;      // used in the drop diagnostic
        const char* spec;            // "CSS2.1", "css-backgrounds-3", "webkit", "rmlui"
    };

    struct ValueSupport {
        const char* property;        // sorted by property, then table order
        const char* value;           // keyword, or "<percentage>", "<length>", "<string>", "<function>()"
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        const char* output;          // RCSS value written instead ("" = unchanged / dropped)
        const char* note;
        const char* suggestion;
    };

    struct SelectorSupport {
        SelectorKind kind;
        const char* name;            // ":hover", ":nth-child()", "::before", "[attr=value]", ">", "&"
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        const char* output;
        const char* note;
        const char* suggestion;
    };

    struct AtRuleSupport {
        const char* name;            // "@media"
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        const char* output;
        const char* note;
        const char* suggestion;
    };

    struct MediaFeatureSupport {
        const char* name;            // "width", "prefers-color-scheme", "print" (media types too)
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        const char* output;
        const char* note;
        const char* suggestion;
    };

    struct ElementSupport {
        const char* name;            // "div"; sorted, unique
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        const char* rmlTag;          // RML tag written ("" = the same name; "-" = dropped)
        const char* display;         // the user-agent stylesheet's display ("" = inline / none set)
        const char* note;
        const char* suggestion;
    };

    struct AttributeSupport {
        const char* element;         // "*" = global; sorted by element, then name
        const char* name;            // "data-*", "aria-*", "on*" are patterns
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        const char* output;
        const char* note;
        const char* suggestion;
    };

    struct InputTypeSupport {
        const char* type;
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        const char* rmlType;         // the RmlUi input type HtmlInputElement maps it to
        bool reported;               // HTMLUI-HTML-0008 is reported
        Severity severity;           // its severity (when reported)
        const char* note;
        const char* suggestion;
    };

    struct UnitSupport {
        const char* unit;            // lowercase except "Q"
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        const char* output;
        const char* note;
        const char* suggestion;
    };

    struct FunctionSupport {
        const char* name;            // "calc" (without parentheses); sorted, unique
        Support support;
        ForkFeature fork;
        Support supportWithFork;
        const char* output;
        const char* note;
        const char* suggestion;
    };

    JASPERENGINE_API Span<const PropertySupport> propertyTable();
    // exact (lowercase) name; custom properties ("--x") find the "--*" row
    JASPERENGINE_API const PropertySupport* findProperty(std::string_view name);

    JASPERENGINE_API Span<const ValueSupport> valueTable();
    JASPERENGINE_API Span<const ValueSupport> valuesOf(std::string_view property);
    JASPERENGINE_API const ValueSupport* findValue(std::string_view property, std::string_view value);

    JASPERENGINE_API Span<const SelectorSupport> selectorTable();
    // ":hover" / "hover" finds ":hover"; "nth-child" finds ":nth-child()"
    JASPERENGINE_API const SelectorSupport* findPseudoClass(std::string_view name);
    // "::before" / "before" finds "::before"
    JASPERENGINE_API const SelectorSupport* findPseudoElement(std::string_view name);
    JASPERENGINE_API const SelectorSupport* findSelector(SelectorKind kind, std::string_view name);

    JASPERENGINE_API Span<const AtRuleSupport> atRuleTable();
    JASPERENGINE_API const AtRuleSupport* findAtRule(std::string_view name);   // "media" or "@media"

    JASPERENGINE_API Span<const MediaFeatureSupport> mediaFeatureTable();
    JASPERENGINE_API const MediaFeatureSupport* findMediaFeature(std::string_view name);

    JASPERENGINE_API Span<const ElementSupport> elementTable();
    JASPERENGINE_API const ElementSupport* findElement(std::string_view name);

    JASPERENGINE_API Span<const AttributeSupport> attributeTable();
    // the element's own row first, then the global ("*") one; patterns "data-*", "aria-*", "on*"
    JASPERENGINE_API const AttributeSupport* findAttribute(std::string_view element, std::string_view name);

    JASPERENGINE_API Span<const InputTypeSupport> inputTypeTable();
    JASPERENGINE_API const InputTypeSupport* findInputType(std::string_view type);

    JASPERENGINE_API Span<const UnitSupport> unitTable();
    JASPERENGINE_API const UnitSupport* findUnit(std::string_view unit);   // case-insensitive ("q" finds "Q")

    JASPERENGINE_API Span<const FunctionSupport> functionTable();
    JASPERENGINE_API const FunctionSupport* findFunction(std::string_view name);   // case-insensitive

    // The support with the given fork features present.
    JASPERENGINE_API Support effectiveSupport(Support base, ForkFeature fork, Support withFork, const ForkFeatures& features);
    template<typename Row>
    Support effectiveSupport(const Row& row, const ForkFeatures& features){
        return effectiveSupport(row.support, row.fork, row.supportWithFork, features);
    }

    JASPERENGINE_API const char* supportName(Support support);    // "native", "rewritten", ...
    JASPERENGINE_API const char* rewriteIdName(RewriteId id);     // "Background"
    JASPERENGINE_API const char* selectorKindName(SelectorKind kind);   // "selector", "pseudo_class", "pseudo_element"

}

#endif //COMPAT_SUPPORTTABLES_H
