// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_DECORATORCOMMON_H
#define COMPAT_RUNTIME_DECORATORCOMMON_H

// What the compat decorators (html-layer, html-shape, html-border-image; phase-2 §3.11.3)
// share: argument tokens, lengths resolved against the element, image sources (raster
// through RmlUi's texture database, SVG rasterized at the drawn size with currentColor
// handled as for <svg>), callback textures, the rounded clip of a box, runtime diagnostics.

#include "Export.h"
#include "htmlui/compat/runtime/SvgPreprocess.h"

#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/Texture.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Unit.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
    class Element;
    class PropertyDictionary;
}

namespace jasperengine::htmlui::compat::decorators {

    // a <length-percentage> (or a bare number when allowed) of a decorator argument
    struct Length {
        float value = 0.0f;
        Rml::Unit unit = Rml::Unit::PX;   // PX, EM, REM, VW, VH, DP, PPI units, PERCENT, NUMBER

        bool isPercent() const { return unit == Rml::Unit::PERCENT; }
        bool isNumber() const { return unit == Rml::Unit::NUMBER; }
    };

    std::string lower(std::string_view text);
    std::string trim(std::string_view text);
    // removes one pair of surrounding quotes
    std::string unquote(std::string_view text);
    bool parseNumber(std::string_view token, float& value);
    // "10px", "1.5em", "50%", "0" (px); a bare non-zero number only with allowNumber
    bool parseLength(std::string_view token, Length& out, bool allowNumber = false);
    // px; percentages of percentBase; numbers (where allowed) as given
    float resolve(Rml::Element* element, const Length& length, float percentBase);
    // splits on whitespace and commas outside parentheses and quotes; commas are kept as "," tokens
    std::vector<std::string> tokenize(std::string_view text);

    // A CSS <position> (1 to 4 values: keywords, lengths, percentages, edge offsets) as a
    // percentage plus an offset per axis: x = percentX% of the free space + offsetX px, as
    // background-position and object-position place an object (lengths resolved for `element`;
    // percentages after an edge keyword measured from that edge). False when not a position.
    struct CssPosition {
        float percentX = 50.0f, offsetX = 0.0f;
        float percentY = 50.0f, offsetY = 0.0f;
    };
    bool parseCssPosition(const std::vector<std::string>& tokens, Rml::Element* element, CssPosition& out);

    // the string argument of a decorator property (RmlUi's string parser keeps it as written)
    std::string stringArgument(const Rml::PropertyDictionary& properties, Rml::PropertyId id);

    // physical pixels per document pixel of the element's context (its HtmlUIContext's pixel
    // ratio, as the font engine knows it) x RmlUi's dp ratio
    float rasterScale(Rml::Element* element);

    // the element's box areas in its border-box coordinates
    Rml::Rectanglef boxRectangle(Rml::Element* element, Rml::BoxArea area);

    bool isSvgPath(std::string_view path);
    // without query and fragment
    std::string filePart(std::string_view path);

    // An image of a decorator: a raster file (RmlUi's texture database, "?htmlui-filter=nearest"
    // when pixelated) or an SVG (rasterized by compat at the size it is drawn).
    class Image {
    public:
        // false (logged once per path) when the file cannot be read or is not an image
        bool load(Rml::Element* element, const std::string& path);

        bool isSvg() const { return svg != nullptr; }
        // a monochrome currentColor SVG: drawn white, multiplied by the element's CSS color
        bool tintsWithColor() const { return svg && svg->tint == SvgTint::Color; }
        // CSS px (document px x RmlUi's dp ratio for raster images, the SVG's own size for SVG)
        Rml::Vector2f intrinsicSize() const { return intrinsic; }
        bool hasIntrinsicSize() const { return intrinsic.x > 0.0f && intrinsic.y > 0.0f; }

        // the texture to draw at `size` document px: raster images as loaded, SVG rasterized at
        // size x raster scale (pixels shared through a process cache). The vertex colour: white
        // x opacity, or for a monochrome currentColor SVG the element's color x opacity. An SVG
        // repeated along an axis (a background tile) is sampled with repeat wrap there,
        // rasterized at its size rounded up.
        Rml::Texture texture(Rml::Element* element, Rml::Vector2f size, bool repeatX = false, bool repeatY = false);
        Rml::ColourbPremultiplied colour(Rml::Element* element) const;

    private:
        std::string source;                      // the resolved path
        Rml::Texture raster;
        std::shared_ptr<const SvgResult> svg;
        Rml::Vector2f intrinsic{0, 0};
        Rml::CallbackTexture svgTexture;
        Rml::Vector2i svgTextureSize{0, 0};
        std::string svgTextureXml;
        bool svgTextureRepeatX = false, svgTextureRepeatY = false;
    };

    // the shape of an element box with its rounded corners (border-box coordinates); empty
    // geometry when the element has no border radius
    Rml::Geometry makeClipGeometry(Rml::Element* element, Rml::BoxArea area);

    // Clips the following draws to that shape: the render state is saved, an Intersect clip
    // mask is added to the current list (phase-1 stencil clip masks) and the state is restored
    // when the scope ends. Nothing happens for an empty geometry.
    class RoundedClip {
    public:
        RoundedClip(Rml::Element* element, Rml::Geometry& clip, Rml::Vector2f translation);
        ~RoundedClip();
        RoundedClip(const RoundedClip&) = delete;
        RoundedClip& operator=(const RoundedClip&) = delete;

    private:
        Rml::RenderManager* renderManager = nullptr;
        Rml::RenderState saved;
        Rml::Matrix4f transform;
        bool active = false;
    };

    // premultiplied RGBA of an SVG at a pixel size, shared by every decorator (bounded cache)
    std::shared_ptr<const std::vector<uint8_t>> rasterizeCached(const std::string& xml, int width, int height, float logicalWidth = 0.0f,
                                                                float logicalHeight = 0.0f);
    size_t rasterCacheSize();

    // "[HTML UI] <document>: warning: message (fix: ...) [code]" once per (document, message)
    void reportOnce(Rml::Element* element, DiagCode code, const std::string& message, const std::string& suggestion);

}

#endif //COMPAT_RUNTIME_DECORATORCOMMON_H
