// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLLAYERDECORATOR_H
#define COMPAT_RUNTIME_HTMLLAYERDECORATOR_H

#include "Export.h"
#include "htmlui/compat/runtime/DecoratorCommon.h"

#include <RmlUi/Core/Decorator.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    // ---------------------------------------------------------------- layer math (CSS Backgrounds 3)

    enum class LayerRepeat : uint8_t { Repeat, NoRepeat, Space, Round };
    enum class LayerSizeMode : uint8_t { Lengths, Cover, Contain };
    enum class LayerAttachment : uint8_t { Scroll, Fixed, Local };

    struct LayerRect {
        float x = 0, y = 0, width = 0, height = 0;

        float right() const { return x + width; }
        float bottom() const { return y + height; }
        bool operator==(const LayerRect& other) const;
    };

    // a background-position component: percent of (area - tile) plus an offset in px
    struct LayerPosition {
        float percent = 0.0f;
        float offset = 0.0f;
    };

    // one size component resolved to px, or auto
    struct LayerSizeValue {
        bool automatic = true;
        float value = 0.0f;
    };

    struct LayerInput {
        LayerRect positioningArea;          // the origin box (fixed: the viewport)
        LayerRect clipArea;                 // the painted box
        float intrinsicWidth = 0.0f;        // 0 = the image has no intrinsic width
        float intrinsicHeight = 0.0f;
        LayerSizeMode sizeMode = LayerSizeMode::Lengths;
        LayerSizeValue sizeX, sizeY;        // Lengths mode, px (percentages already resolved)
        LayerPosition positionX, positionY;
        LayerRepeat repeatX = LayerRepeat::Repeat, repeatY = LayerRepeat::Repeat;
        float minTile = 2.0f;               // repeated tiles smaller than this are drawn at this size
        size_t maxQuads = 16384;            // beyond, tiles are enlarged to bound the quad count
    };

    struct LayerQuad {
        LayerRect rect;                     // clipped to the clip area
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    };

    struct LayerGeometry {
        float tileWidth = 0.0f, tileHeight = 0.0f;
        std::vector<LayerQuad> quads;
        bool tileClamped = false;           // tiles below minTile drawn larger (HTMLUI-CSS-0006)
        bool countClamped = false;          // tiles enlarged to stay under maxQuads
    };

    // The tiles of one background layer, per CSS Backgrounds 3 §3.6-3.9: background-size
    // (auto, cover, contain, lengths with the intrinsic ratio), round (§3.4: the tile scaled to
    // a whole count, the other auto dimension keeping the ratio), the position, then repeat,
    // space (n >= 2 tiles spread from edge to edge, else one tile at the position), no-repeat;
    // every tile intersecting the clip area, cropped to it with its texture coordinates.
    JASPERENGINE_API LayerGeometry computeLayerGeometry(const LayerInput& input);

    // ---------------------------------------------------------------- html-layer(...)

    // The ten arguments of html-layer, as p2-css writes them (phase-2 §3.9.7, §3.11.3):
    //   html-layer("<path>" <size-x> <size-y> <pos-x> <pos-y> <repeat-x> <repeat-y> <origin> <clip> <attachment>)
    // size: auto | cover | contain | <length-percentage> (cover/contain in either size argument
    //   apply to both); pos: left | center | right (x), top | center | bottom (y) |
    //   <length-percentage> | calc(<percentage> +|- <length>) (an edge offset: `right 10px` is
    //   calc(100% - 10px)); repeat: repeat | no-repeat | space | round; origin/clip:
    //   border-box | padding-box | content-box; attachment: scroll | fixed | local (local is
    //   drawn as scroll).
    struct JASPERENGINE_INTERNAL LayerSpec {
        std::string path;
        LayerSizeMode sizeMode = LayerSizeMode::Lengths;
        bool autoX = true, autoY = true;
        decorators::Length sizeX, sizeY;
        decorators::Length positionX, positionY;          // the percentage part (PERCENT) ...
        decorators::Length positionOffsetX, positionOffsetY;   // ... and the length part
        LayerRepeat repeatX = LayerRepeat::Repeat, repeatY = LayerRepeat::Repeat;
        Rml::BoxArea origin = Rml::BoxArea::Padding, clip = Rml::BoxArea::Border;
        LayerAttachment attachment = LayerAttachment::Scroll;

        // false with the reason when an argument is invalid
        static bool parse(const std::array<std::string, 10>& arguments, LayerSpec& spec, std::string* error);
        static bool parseSize(const std::string& token, LayerSpec& spec, bool x, std::string* error);
        static bool parsePosition(const std::string& token, bool x, decorators::Length& percent, decorators::Length& offset);
        static bool parseRepeat(const std::string& token, LayerRepeat& repeat);
        static bool parseBox(const std::string& token, Rml::BoxArea& box);
    };

    class JASPERENGINE_INTERNAL HtmlLayerDecorator : public Rml::Decorator {
    public:
        explicit HtmlLayerDecorator(LayerSpec spec);
        ~HtmlLayerDecorator() override;

        Rml::DecoratorDataHandle GenerateElementData(Rml::Element* element, Rml::BoxArea paint_area) const override;
        void ReleaseElementData(Rml::DecoratorDataHandle element_data) const override;
        void RenderElement(Rml::Element* element, Rml::DecoratorDataHandle element_data) const override;

        const LayerSpec& getSpec() const { return spec; }

        // tests: the geometry the last GenerateElementData made for an element data handle
        static const LayerGeometry* geometryOf(Rml::DecoratorDataHandle element_data);

    private:
        struct Data;
        void build(Rml::Element* element, Data& data) const;

        LayerSpec spec;
    };

    class JASPERENGINE_INTERNAL HtmlLayerDecoratorInstancer : public Rml::DecoratorInstancer {
    public:
        HtmlLayerDecoratorInstancer();
        ~HtmlLayerDecoratorInstancer() override;

        Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String& name, const Rml::PropertyDictionary& properties,
                                                         const Rml::DecoratorInstancerInterface& instancer_interface) override;

    private:
        std::array<Rml::PropertyId, 10> ids{};
    };

}

#endif //COMPAT_RUNTIME_HTMLLAYERDECORATOR_H
