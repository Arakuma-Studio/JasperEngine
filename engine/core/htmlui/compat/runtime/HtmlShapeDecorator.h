// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLSHAPEDECORATOR_H
#define COMPAT_RUNTIME_HTMLSHAPEDECORATOR_H

#include "Export.h"
#include "htmlui/compat/runtime/DecoratorCommon.h"

#include <RmlUi/Core/Decorator.h>

#include <string>
#include <vector>

namespace Rml {
    class Element;
}

namespace jasperengine::htmlui::compat {

    // A CSS <basic-shape> (CSS Shapes 1 §3) for clip-path, kept as written and turned into SVG
    // for a reference box: inset() with round (elliptical radii with '/'), circle() and
    // ellipse() (radii as lengths, percentages, closest-side, farthest-side; `at` positions with
    // keywords, lengths, percentages and edge offsets), polygon() (fill rule, length-percentage
    // pairs), path() (fill rule, SVG path data in reference-box px), xywh() and rect() (with
    // round). An optional trailing box keyword (border-box, padding-box, content-box,
    // margin-box, fill-box, stroke-box, view-box) picks the reference box (default border-box).
    struct JASPERENGINE_INTERNAL BasicShape {
        std::string text;            // the shape as given
        Rml::BoxArea box = Rml::BoxArea::Border;

        // false with the reason when the shape is not understood
        static bool parse(const std::string& text, BasicShape& shape, std::string* error);

        // An SVG document of width x height (the reference box in px) with the shape filled
        // white, black elsewhere transparent; `resolveLength` resolves lengths other than px and
        // percentages (em, rem, vw ...) for the element. "" when the shape is invalid.
        std::string toSvg(float width, float height, Rml::Element* element = nullptr, std::string* error = nullptr) const;

        // The shape as a rectangle with elliptical corners in the reference box (inset(), xywh(), rect(), circle(),
        // ellipse(): an analytic clip, S92 R8); false for polygon() and path(), and when invalid.
        bool toRoundedRect(float width, float height, Rml::Element* element, Rml::Rectanglef& rect, Rml::Array<Rml::Vector2f, 4>& radii) const;
        // polygon() as a convex polygon of at most Rml::ClipShape::MaxPolygonPoints points in the reference box, in
        // counter-clockwise order (positive area in its coordinates): an analytic clip too (S92); false for another
        // shape, a concave or self-intersecting polygon, more points, or none of area
        bool toConvexPolygon(float width, float height, Rml::Element* element, std::vector<Rml::Vector2f>& points) const;
    };

    // html-shape("<basic-shape>"): the shape rasterized (LunaSVG) at the reference box's size x
    // the context's pixel ratio into a callback texture drawn over that box. p2-css appends it to
    // mask-image for clip-path, so the element is masked to the exact geometry (hit testing is
    // not clipped).
    class JASPERENGINE_INTERNAL HtmlShapeDecorator : public Rml::Decorator {
    public:
        explicit HtmlShapeDecorator(BasicShape shape);
        ~HtmlShapeDecorator() override;

        Rml::DecoratorDataHandle GenerateElementData(Rml::Element* element, Rml::BoxArea paint_area) const override;
        void ReleaseElementData(Rml::DecoratorDataHandle element_data) const override;
        void RenderElement(Rml::Element* element, Rml::DecoratorDataHandle element_data) const override;
        // S92 R8: the border box's inset(), circle(), ellipse(), xywh(), rect() and a convex polygon() clip analytically
        bool GetMaskClipShape(Rml::Element* element, Rml::DecoratorDataHandle element_data, Rml::ClipShape& shape,
                              Rml::Geometry*& geometry) const override;

        const BasicShape& getShape() const { return shape; }
        // tests: the SVG and pixel size the element data was rasterized with
        static std::string svgOf(Rml::DecoratorDataHandle element_data);
        static Rml::Vector2i pixelSizeOf(Rml::DecoratorDataHandle element_data);

    private:
        struct Data;
        BasicShape shape;
    };

    class JASPERENGINE_INTERNAL HtmlShapeDecoratorInstancer : public Rml::DecoratorInstancer {
    public:
        HtmlShapeDecoratorInstancer();
        ~HtmlShapeDecoratorInstancer() override;

        Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String& name, const Rml::PropertyDictionary& properties,
                                                         const Rml::DecoratorInstancerInterface& instancer_interface) override;

    private:
        Rml::PropertyId shapeId = Rml::PropertyId::Invalid;
    };

}

#endif //COMPAT_RUNTIME_HTMLSHAPEDECORATOR_H
