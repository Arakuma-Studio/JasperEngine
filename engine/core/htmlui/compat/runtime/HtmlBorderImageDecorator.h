// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLBORDERIMAGEDECORATOR_H
#define COMPAT_RUNTIME_HTMLBORDERIMAGEDECORATOR_H

#include "Export.h"
#include "htmlui/compat/runtime/DecoratorCommon.h"
#include "htmlui/compat/runtime/HtmlLayerDecorator.h"

#include <RmlUi/Core/Decorator.h>

#include <array>
#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    // border-image-repeat
    enum class BorderImageRepeat : uint8_t { Stretch, Repeat, Round, Space };

    // one border-image-width value: a length-percentage, a number (x border width) or auto
    struct BorderImageWidth {
        bool automatic = false;
        decorators::Length length;   // NUMBER = a multiple of the border width
    };

    // The sixteen arguments of html-border-image, as p2-css writes them (phase-2 §3.9.7):
    //   html-border-image("<path>" <slice t r b l> <fill> <width t r b l> <outset t r b l> <repeat-x> <repeat-y>)
    // slice: <number> (image px) | <percentage> (of the image size); fill: fill | nofill;
    // width: <length-percentage> (of the border image area) | <number> (x the border width) |
    // auto (the slice); outset: <length> | <number> (x the border width); repeat: stretch |
    // repeat | round | space.
    struct JASPERENGINE_INTERNAL BorderImageSpec {
        std::string path;
        std::array<decorators::Length, 4> slice;       // NUMBER or PERCENT, top right bottom left
        bool fill = false;
        std::array<BorderImageWidth, 4> width;
        std::array<decorators::Length, 4> outset;      // length or NUMBER
        BorderImageRepeat repeatX = BorderImageRepeat::Stretch, repeatY = BorderImageRepeat::Stretch;

        static bool parse(const std::vector<std::string>& arguments, BorderImageSpec& spec, std::string* error);
    };

    // the resolved geometry inputs, px
    struct BorderImageInput {
        LayerRect borderBox;                  // the element's border box
        std::array<float, 4> borderWidths{};  // computed border widths (top right bottom left)
        float imageWidth = 0, imageHeight = 0;   // intrinsic image size
        std::array<float, 4> slice{};         // image px
        bool fill = false;
        std::array<float, 4> width{};         // drawn border image widths, px (before scaling down)
        std::array<float, 4> outset{};        // px
        BorderImageRepeat repeatX = BorderImageRepeat::Stretch, repeatY = BorderImageRepeat::Stretch;
    };

    // The nine-slice of CSS Backgrounds 3 §6: the area (border box + outset), widths scaled
    // down when opposite ones overlap (§6.6), corners scaled to their width/height, edges and
    // the middle (fill) scaled to the adjacent widths and stretched, repeated (centred), rounded
    // or spaced (§6.5). Quads carry their texture coordinates into the whole image.
    JASPERENGINE_API std::vector<LayerQuad> computeBorderImageGeometry(const BorderImageInput& input);

    // html-border-image(...): the nine-slice of an image (raster or SVG) per CSS Backgrounds 3 §6.
    class JASPERENGINE_INTERNAL HtmlBorderImageDecorator : public Rml::Decorator {
    public:
        explicit HtmlBorderImageDecorator(BorderImageSpec spec);
        ~HtmlBorderImageDecorator() override;

        Rml::DecoratorDataHandle GenerateElementData(Rml::Element* element, Rml::BoxArea paint_area) const override;
        void ReleaseElementData(Rml::DecoratorDataHandle element_data) const override;
        void RenderElement(Rml::Element* element, Rml::DecoratorDataHandle element_data) const override;

        // tests
        static const std::vector<LayerQuad>* quadsOf(Rml::DecoratorDataHandle element_data);

    private:
        struct Data;
        BorderImageSpec spec;
    };

    class JASPERENGINE_INTERNAL HtmlBorderImageDecoratorInstancer : public Rml::DecoratorInstancer {
    public:
        HtmlBorderImageDecoratorInstancer();
        ~HtmlBorderImageDecoratorInstancer() override;

        Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String& name, const Rml::PropertyDictionary& properties,
                                                         const Rml::DecoratorInstancerInterface& instancer_interface) override;

    private:
        std::array<Rml::PropertyId, 16> ids{};
    };

}

#endif //COMPAT_RUNTIME_HTMLBORDERIMAGEDECORATOR_H
