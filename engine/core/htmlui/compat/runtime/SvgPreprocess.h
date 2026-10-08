// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_SVGPREPROCESS_H
#define COMPAT_RUNTIME_SVGPREPROCESS_H

#include "Export.h"
#include "htmlui/compat/Diagnostics.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    // Where an SVG is used (diagnostics name it).
    enum class SvgUse : uint8_t { Inline, File, Layer };

    // How the element colors an SVG that uses currentColor (phase-2 §3.7.5).
    enum class SvgTint : uint8_t {
        None,        // no currentColor: drawn as is (image-color keeps its RmlUi meaning)
        Color,       // monochrome currentColor: rasterized white, multiplied by the CSS color
        Substitute   // mixed: the CSS color is substituted as text, re-rasterized when it changes
    };

    // Root presentation overrides from the registered SVG paint properties set on the <svg>
    // element (CSS fill, stroke, ... : §3.11.1). Empty = not set. Values are SVG attribute
    // text ("#ff0000ff", "none", "currentColor", "1.5", "50%", "round").
    struct JASPERENGINE_API SvgPaintOverrides {
        std::string fill, stroke, strokeWidth, strokeOpacity, fillOpacity, strokeLinecap, strokeLinejoin, strokeDasharray;

        bool empty() const;
        std::string key() const;   // cache key part ("" when empty)
    };

    struct JASPERENGINE_API SvgResult {
        bool ok = false;              // parsed; false: `xml` is the input unchanged and `error` says why
        std::string xml;              // the processed SVG (Substitute: with the color placeholder, see withColor)
        SvgTint tint = SvgTint::None;
        size_t foreignObjects = 0;    // <foreignObject> elements removed
        std::string error;
        // HTMLUI-SVG-0001 (mixed currentColor, re-rasterized) and HTMLUI-SVG-0002
        // (foreignObject dropped), without a location (the caller locates them)
        std::vector<Diagnostic> diagnostics;

        // The SVG to rasterize for a CSS color ("#rrggbbaa"): Substitute fills the placeholder,
        // the other modes return `xml` as is.
        std::string withColor(std::string_view cssColor) const;
    };

    // Parses an SVG (tinyxml2), applies the paint overrides to the root element, drops
    // <foreignObject> subtrees (LunaSVG cannot draw them) and classifies its paint (fill,
    // stroke, stop-color, flood-color, lighting-color attributes and style="" declarations,
    // <style> sheets):
    // - every explicit paint is currentColor or none, no shape relies on SVG's default black
    //   fill, and no inner element redefines `color`: Color, every currentColor -> #ffffff;
    // - some currentColor next to fixed colors: Substitute, every spelling normalized to
    //   currentColor (LunaSVG's) and the root's color attribute set to a placeholder;
    // - no currentColor: None.
    class JASPERENGINE_API SvgPreprocess {
    public:
        static SvgResult process(std::string_view xml, SvgUse use, const SvgPaintOverrides& overrides = {});
        // "#rrggbbaa" (lowercase) of an 8-bit RGBA color
        static std::string colorText(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    };

}

namespace jasperengine::htmlui {

    // The engine's SVG rasterizer (LunaSVG, the phase-0 benchmark's choice) for compat code
    // (html-layer, html-shape): renders the SVG scaled to exactly width x height pixels (its
    // viewBox and preserveAspectRatio apply) into premultiplied RGBA8, rows top first.
    // Empty on a parse error or a size outside 1..16384 (logged).
    JASPERENGINE_API std::vector<uint8_t> rasterizeSvg(std::string_view xml, int width, int height);
    // The SVG's intrinsic size in CSS px (LunaSVG's: width/height, else the viewBox);
    // false when it cannot be parsed or has no size.
    JASPERENGINE_API bool svgIntrinsicSize(std::string_view xml, float& width, float& height);

}

#endif //COMPAT_RUNTIME_SVGPREPROCESS_H
