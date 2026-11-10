// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// At-rule and media feature support tables: Appendix C of docs/html-ui/phase-2-compat.md (and 3.9.4).
// Media types are media feature rows too. Columns: name, support, fork, support with fork, output, note,
// suggestion.

#include "htmlui/compat/SupportTables.h"

using namespace jasperengine::htmlui::compat;

namespace {

    constexpr Support N = Support::Native;
    constexpr Support R = Support::Rewritten;
    constexpr Support A = Support::Approximated;
    constexpr Support U = Support::Unsupported;
    constexpr Support I = Support::Ignored;
    using FF = ForkFeature;

    const AtRuleSupport atRules[] = {
        {"@media", R, FF::None, R, "@media", "normalized (3.9.4); nested blocks combined", ""},
        {"@import", R, FF::None, R, "", "inlined with its media, layer and supports conditions", ""},
        {"@font-face", R, FF::None, R, "@font-face", "TTF/OTF/WOFF/WOFF2 source selection and descriptors", ""},
        {"@keyframes", N, FF::None, N, "@keyframes", "names sanitized; per-keyframe easing dropped until the fork has AnimationLonghands", ""},
        {"@-webkit-keyframes", R, FF::None, R, "@keyframes", "prefix removed; names sanitized; per-keyframe easing dropped until the fork has AnimationLonghands", ""},
        {"@-moz-keyframes", R, FF::None, R, "@keyframes", "prefix removed; names sanitized; per-keyframe easing dropped until the fork has AnimationLonghands", ""},
        {"@supports", R, FF::None, R, "", "evaluated statically against the support tables", ""},
        {"@layer", R, FF::None, R, "", "flattened by layer order (Info on specificity conflicts)", ""},
        {"@property", A, FF::RegisteredProperties, N, "registered with the fork",
         "registered custom properties (phase 5): the syntaxes length, number, integer, percentage, length-percentage, color, angle, "
         "time and * are typed, inherits: false is honoured, transitions and @keyframes interpolate them and var() follows; "
         "a document's registrations are its own (a hot reload applies an edited definition, an unload releases them; two live "
         "documents defining a name differently: the first one's applies, HTMLUI-AT-0013). Other syntaxes, and without the fork: the initial "
         "value written on the root, inherits: false lost (Info)", ""},
        {"@effect", R, FF::None, R, "", "declares a project UI effect (phase 5): { src: url(<file>.frag) }, hoisted; problems are HTMLUI-FX-0012 to 0014", ""},
        {"@container", U, FF::None, U, "", "contents dropped (Warning)", "use @media on the viewport size, or toggle classes from Lua"},
        {"@scope", U, FF::None, U, "", "contents dropped (Warning)", "use descendant selectors under a class"},
        {"@starting-style", U, FF::AnimationLonghands, N, "@starting-style", "contents dropped until the fork has AnimationLonghands (Warning)", "set the start values with a class and remove it from Lua after load, or use @keyframes"},
        {"@document", U, FF::None, U, "", "contents dropped (Warning)", "put the rules in the stylesheet of the document they target"},
        {"@-moz-document", U, FF::None, U, "", "contents dropped (Warning)", "put the rules in the stylesheet of the document they target"},
        {"@page", I, FF::None, I, "", "printing only; dropped (Info)", ""},
        {"@namespace", I, FF::None, I, "", "namespaces are not used; dropped (Info)", ""},
        {"@counter-style", I, FF::None, I, "", "custom counter styles are not supported; dropped (Info)", "use one of: decimal, decimal-leading-zero, lower/upper-alpha/latin, lower/upper-roman, lower-greek, disc, circle, square, none"},
        {"@font-feature-values", I, FF::None, I, "", "font features are not supported; dropped (Info)", ""},
        {"@font-palette-values", I, FF::None, I, "", "font palettes are not supported; dropped (Info)", ""},
        {"@view-transition", I, FF::None, I, "", "view transitions are not supported; dropped (Info)", ""},
        {"@charset", I, FF::None, I, "", "sources are decoded as UTF-8; dropped (Info)", ""},
        {"@decorator", N, FF::None, N, "", "RmlUi extension", ""},
        {"@spritesheet", N, FF::None, N, "", "RmlUi extension", ""},
    };

    const MediaFeatureSupport mediaFeatures[] = {
        {"width", N, FF::None, N, "", "range syntax -> min-/max- forms; em/rem -> px x 16", ""},
        {"height", N, FF::None, N, "", "range syntax -> min-/max- forms; em/rem -> px x 16", ""},
        {"min-width", N, FF::None, N, "", "range syntax -> min-/max- forms; em/rem -> px x 16", ""},
        {"max-width", N, FF::None, N, "", "range syntax -> min-/max- forms; em/rem -> px x 16", ""},
        {"min-height", N, FF::None, N, "", "range syntax -> min-/max- forms; em/rem -> px x 16", ""},
        {"max-height", N, FF::None, N, "", "range syntax -> min-/max- forms; em/rem -> px x 16", ""},
        {"aspect-ratio", N, FF::None, N, "", "range syntax -> min-/max- forms", ""},
        {"min-aspect-ratio", N, FF::None, N, "", "range syntax -> min-/max- forms", ""},
        {"max-aspect-ratio", N, FF::None, N, "", "range syntax -> min-/max- forms", ""},
        {"resolution", N, FF::None, N, "", "range syntax -> min-/max- forms; dpi, dpcm -> x", ""},
        {"min-resolution", N, FF::None, N, "", "range syntax -> min-/max- forms; dpi, dpcm -> x", ""},
        {"max-resolution", N, FF::None, N, "", "range syntax -> min-/max- forms; dpi, dpcm -> x", ""},
        {"orientation", N, FF::None, N, "", "", ""},
        {"prefers-color-scheme", R, FF::None, R, "theme: dark / theme: light", "UIDocument::setColorScheme switches the theme (light by default)", ""},
        {"prefers-reduced-motion", R, FF::None, R, "theme: reduced-motion", "reduce -> (theme: reduced-motion); no-preference -> its negation", ""},
        {"hover", R, FF::None, R, "static", "evaluated at load: hover unless System::isTouchDevice() (Info)", ""},
        {"any-hover", R, FF::None, R, "static", "evaluated at load: hover unless System::isTouchDevice() (Info)", ""},
        {"pointer", R, FF::None, R, "static", "evaluated at load: fine, or coarse on touch devices (Info)", ""},
        {"any-pointer", R, FF::None, R, "static", "evaluated at load: fine, or coarse on touch devices (Info)", ""},
        {"color", R, FF::None, R, "static", "evaluated at load with the value 8 (Info)", ""},
        {"min-color", R, FF::None, R, "static", "evaluated at load with the value 8 (Info)", ""},
        {"max-color", R, FF::None, R, "static", "evaluated at load with the value 8 (Info)", ""},
        {"color-gamut", R, FF::None, R, "static", "evaluated at load with the value srgb (Info)", ""},
        {"monochrome", R, FF::None, R, "static", "evaluated at load with the value 0 (Info)", ""},
        {"min-monochrome", R, FF::None, R, "static", "evaluated at load with the value 0 (Info)", ""},
        {"max-monochrome", R, FF::None, R, "static", "evaluated at load with the value 0 (Info)", ""},
        {"color-index", R, FF::None, R, "static", "evaluated at load with the value 0 (Info)", ""},
        {"min-color-index", R, FF::None, R, "static", "evaluated at load with the value 0 (Info)", ""},
        {"max-color-index", R, FF::None, R, "static", "evaluated at load with the value 0 (Info)", ""},
        {"prefers-contrast", R, FF::None, R, "static", "evaluated at load with the value no-preference (Info)", ""},
        {"forced-colors", R, FF::None, R, "static", "evaluated at load with the value none (Info)", ""},
        {"inverted-colors", R, FF::None, R, "static", "evaluated at load with the value none (Info)", ""},
        {"scripting", R, FF::None, R, "static", "evaluated at load with the value none (Info)", ""},
        {"update", R, FF::None, R, "static", "evaluated at load with the value fast (Info)", ""},
        {"display-mode", R, FF::None, R, "static", "evaluated at load with the value fullscreen (Info)", ""},
        {"overflow-block", R, FF::None, R, "static", "evaluated at load with the value scroll (Info)", ""},
        {"overflow-inline", R, FF::None, R, "static", "evaluated at load with the value scroll (Info)", ""},
        {"dynamic-range", R, FF::None, R, "static", "evaluated at load with the value standard (Info)", ""},
        {"prefers-reduced-transparency", R, FF::None, R, "static", "evaluated at load with the value no-preference (Info)", ""},
        {"video-dynamic-range", U, FF::None, U, "", "not evaluated: the query never matches (Warning)", "remove the query"},
        {"prefers-reduced-data", U, FF::None, U, "", "not evaluated: the query never matches (Warning)", "remove the query"},
        {"device-width", U, FF::None, U, "", "deprecated device feature: the query never matches (Warning)", "use width, height or aspect-ratio (the viewport is the canvas)"},
        {"device-height", U, FF::None, U, "", "deprecated device feature: the query never matches (Warning)", "use width, height or aspect-ratio (the viewport is the canvas)"},
        {"min-device-width", U, FF::None, U, "", "deprecated device feature: the query never matches (Warning)", "use width, height or aspect-ratio (the viewport is the canvas)"},
        {"max-device-width", U, FF::None, U, "", "deprecated device feature: the query never matches (Warning)", "use width, height or aspect-ratio (the viewport is the canvas)"},
        {"min-device-height", U, FF::None, U, "", "deprecated device feature: the query never matches (Warning)", "use width, height or aspect-ratio (the viewport is the canvas)"},
        {"max-device-height", U, FF::None, U, "", "deprecated device feature: the query never matches (Warning)", "use width, height or aspect-ratio (the viewport is the canvas)"},
        {"device-aspect-ratio", U, FF::None, U, "", "deprecated device feature: the query never matches (Warning)", "use width, height or aspect-ratio (the viewport is the canvas)"},
        {"grid", U, FF::None, U, "", "not evaluated: the query never matches (Warning)", "remove the query"},
        {"all", R, FF::None, R, "", "media type removed (always true)", ""},
        {"screen", R, FF::None, R, "", "media type removed (always true)", ""},
        {"print", R, FF::None, R, "", "media type that never matches; the block is dropped", ""},
        {"speech", R, FF::None, R, "", "media type that never matches; the block is dropped", ""},
        {"tty", R, FF::None, R, "", "media type that never matches; the block is dropped", ""},
        {"tv", R, FF::None, R, "", "media type that never matches; the block is dropped", ""},
        {"projection", R, FF::None, R, "", "media type that never matches; the block is dropped", ""},
        {"handheld", R, FF::None, R, "", "media type that never matches; the block is dropped", ""},
        {"braille", R, FF::None, R, "", "media type that never matches; the block is dropped", ""},
        {"embossed", R, FF::None, R, "", "media type that never matches; the block is dropped", ""},
        {"aural", R, FF::None, R, "", "media type that never matches; the block is dropped", ""},
        {"theme", N, FF::None, N, "", "RmlUi extension", ""},
    };

}

Span<const AtRuleSupport> jasperengine::htmlui::compat::atRuleTable(){ return Span<const AtRuleSupport>(atRules); }
Span<const MediaFeatureSupport> jasperengine::htmlui::compat::mediaFeatureTable(){ return Span<const MediaFeatureSupport>(mediaFeatures); }
