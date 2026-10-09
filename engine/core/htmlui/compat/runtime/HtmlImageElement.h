// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLIMAGEELEMENT_H
#define COMPAT_RUNTIME_HTMLIMAGEELEMENT_H

#include "Export.h"
#include "htmlui/compat/runtime/DecoratorCommon.h"
#include "htmlui/compat/runtime/HtmlLayerDecorator.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>

#include <string>

namespace jasperengine::htmlui::compat {

    enum class ObjectFit : uint8_t { Fill, Contain, Cover, None, ScaleDown };

    // Where an object of `objectSize` is drawn in a box of `boxSize` for object-fit and
    // object-position (CSS Images 3 §5), cropped to the box: the destination rectangle and its
    // texture coordinates. False when nothing is visible.
    JASPERENGINE_API bool computeObjectFit(ObjectFit fit, Rml::Vector2f objectSize, Rml::Vector2f boxSize,
                                           const decorators::CssPosition& position, LayerQuad& out);
    JASPERENGINE_API ObjectFit parseObjectFit(const std::string& value);   // fill for unknown values

    // <img> replacing RmlUi's (phase-2 §3.11.2): `src` raster images through RmlUi's texture
    // database (nearest sampling for image-rendering: pixelated / crisp-edges) or SVG rasterized
    // at the drawn size; the intrinsic size from the image, scaled by the width/height
    // attributes (one keeps the ratio), as RmlUi's img does; drawn with object-fit and
    // object-position, tinted by image-color x opacity. RmlUi's `sprite` and `rect` attributes
    // keep working for RML documents. (With the phase-3 ObjectFit feature the fork's
    // ComputeObjectFitRect replaces computeObjectFit.)
    class JASPERENGINE_INTERNAL HtmlImageElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlImageElement, Rml::Element)

        explicit HtmlImageElement(const Rml::String& tag);
        ~HtmlImageElement() override;

        bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;

        // the image's own size in CSS px (0 x 0 before a successful load)
        Rml::Vector2f getNaturalSize();
        bool isLoaded();

    protected:
        void OnRender() override;
        void OnResize() override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
        void OnPropertyChange(const Rml::PropertyIdSet& changed_properties) override;
        void OnDpRatioChange() override;
        void OnStyleSheetChange() override;

    private:
        void ensureLoaded();
        void generateGeometry();

        decorators::Image image;
        Rml::Texture spriteTexture;
        Rml::Rectanglef sourceRect;          // sprite or rect="x y w h", in texture px
        bool hasSourceRect = false;
        bool isSprite = false;
        Rml::Vector2f natural{0, 0};
        bool loaded = false;
        bool sourceDirty = true;
        bool geometryDirty = true;
        Rml::Geometry geometry;
        Rml::Texture texture;
    };

}

#endif //COMPAT_RUNTIME_HTMLIMAGEELEMENT_H
