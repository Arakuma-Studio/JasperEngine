// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlImageElement.h"

#include "htmlui/compat/runtime/CompatProperties.h"

#include "Log.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/RenderBox.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/Spritesheet.h>
#include <RmlUi/Core/StyleSheet.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::decorators;

ObjectFit jasperengine::htmlui::compat::parseObjectFit(const std::string& value){
    if (value == "contain") return ObjectFit::Contain;
    if (value == "cover") return ObjectFit::Cover;
    if (value == "none") return ObjectFit::None;
    if (value == "scale-down") return ObjectFit::ScaleDown;
    return ObjectFit::Fill;
}

bool jasperengine::htmlui::compat::computeObjectFit(ObjectFit fit, Rml::Vector2f object, Rml::Vector2f box, const CssPosition& position,
                                                    LayerQuad& out){
    if (box.x <= 0.0f || box.y <= 0.0f)
        return false;
    Rml::Vector2f size = box;
    if (fit != ObjectFit::Fill && object.x > 0.0f && object.y > 0.0f){
        const float contain = std::min(box.x / object.x, box.y / object.y);
        const float cover = std::max(box.x / object.x, box.y / object.y);
        switch (fit){
            case ObjectFit::Contain: size = object * contain; break;
            case ObjectFit::Cover: size = object * cover; break;
            case ObjectFit::None: size = object; break;
            case ObjectFit::ScaleDown: size = contain < 1.0f ? object * contain : object; break;
            case ObjectFit::Fill: break;
        }
    }
    const float x = position.percentX / 100.0f * (box.x - size.x) + position.offsetX;
    const float y = position.percentY / 100.0f * (box.y - size.y) + position.offsetY;
    // cropped to the content box
    const float left = std::max(0.0f, x), top = std::max(0.0f, y);
    const float right = std::min(box.x, x + size.x), bottom = std::min(box.y, y + size.y);
    if (right - left <= 1e-4f || bottom - top <= 1e-4f || size.x <= 0.0f || size.y <= 0.0f)
        return false;
    out.rect = LayerRect{left, top, right - left, bottom - top};
    out.u0 = (left - x) / size.x;
    out.v0 = (top - y) / size.y;
    out.u1 = (right - x) / size.x;
    out.v1 = (bottom - y) / size.y;
    return true;
}

RMLUI_RTTI_Define(HtmlImageElement)

HtmlImageElement::HtmlImageElement(const Rml::String& tag): Rml::Element(tag) {}

HtmlImageElement::~HtmlImageElement() = default;

void HtmlImageElement::ensureLoaded(){
    if (!sourceDirty)
        return;
    Rml::RenderManager* renderManager = GetRenderManager();
    if (!renderManager)
        return;   // no context yet: the next layout tries again
    sourceDirty = false;
    geometryDirty = true;
    loaded = false;
    natural = Rml::Vector2f(0, 0);
    isSprite = false;
    spriteTexture = Rml::Texture();

    // RmlUi's sprites first (RML documents)
    const std::string spriteName = GetAttribute<Rml::String>("sprite", "");
    if (!spriteName.empty()){
        const Rml::StyleSheet* sheet = GetOwnerDocument() ? GetOwnerDocument()->GetStyleSheet() : nullptr;
        const Rml::Sprite* sprite = sheet ? sheet->GetSprite(spriteName) : nullptr;
        if (!sprite){
            Log::warn("[HTML UI] <img sprite=\"%s\">: no such sprite", spriteName.c_str());
            return;
        }
        spriteTexture = sprite->sprite_sheet->texture_source.GetTexture(*renderManager);
        sourceRect = sprite->rectangle;
        hasSourceRect = true;
        isSprite = true;
        natural = sourceRect.Size() * sprite->sprite_sheet->display_scale * Rml::ElementUtilities::GetDensityIndependentPixelRatio(this);
        loaded = spriteTexture.GetDimensions().x > 0;
        return;
    }

    const std::string source = GetAttribute<Rml::String>("src", "");
    if (source.empty())
        return;
    loaded = image.load(this, source);
    if (!loaded)
        return;
    natural = image.intrinsicSize();

    // rect="x y width height": a part of the image, in image pixels
    hasSourceRect = false;
    const std::string rect = GetAttribute<Rml::String>("rect", "");
    if (!rect.empty() && !image.isSvg()){
        std::istringstream stream(rect);
        stream.imbue(std::locale::classic());
        float values[4] = {0, 0, 0, 0};
        if (stream >> values[0] >> values[1] >> values[2] >> values[3]){
            sourceRect = Rml::Rectanglef::FromPositionSize({values[0], values[1]}, {values[2], values[3]});
            hasSourceRect = true;
            natural = sourceRect.Size() * Rml::ElementUtilities::GetDensityIndependentPixelRatio(this);
        }
    }
}

bool HtmlImageElement::GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio){
    ensureLoaded();
    dimensions = natural;
    if (dimensions.y > 0.0f){
        ratio = dimensions.x / dimensions.y;
        // width/height attributes: a presentational size below author CSS
        const float width = GetAttribute<float>("width", -1.0f);
        const float height = GetAttribute<float>("height", -1.0f);
        if (width > 0.0f && height > 0.0f){
            ratio = width / height;
            dimensions = Rml::Vector2f(width, height);
        }else if (height > 0.0f){
            dimensions = Rml::Vector2f(height * ratio, height);
        }else if (width > 0.0f){
            dimensions = Rml::Vector2f(width, width / ratio);
        }
    }else{
        // no image (yet): the attributes alone
        const float width = GetAttribute<float>("width", -1.0f);
        const float height = GetAttribute<float>("height", -1.0f);
        dimensions = Rml::Vector2f(std::max(0.0f, width), std::max(0.0f, height));
    }
    return true;
}

Rml::Vector2f HtmlImageElement::getNaturalSize(){
    ensureLoaded();
    return natural;
}

bool HtmlImageElement::isLoaded(){
    ensureLoaded();
    return loaded;
}

void HtmlImageElement::generateGeometry(){
    geometryDirty = false;
    geometry = Rml::Geometry();
    texture = Rml::Texture();
    // replaced content is clipped to the rounded padding box, as in CSS (a round avatar)
    clipGeometry = decorators::makeClipGeometry(this, Rml::BoxArea::Padding);
    Rml::RenderManager* renderManager = GetRenderManager();
    if (!renderManager || !loaded)
        return;

    // the content box as RmlUi's img draws it (rounded so neighbours meet without gaps),
    // relative to the border box
    const Rml::RenderBox renderBox = GetRenderBox(Rml::BoxArea::Content);
    const Rml::Vector2f box = renderBox.GetFillSize();
    const Rml::Vector2f origin = renderBox.GetFillOffset();
    const ObjectFit fit = parseObjectFit(CompatProperties::text(*this, CompatProperty::ObjectFit));
    CssPosition position;
    const std::string positionText = CompatProperties::text(*this, CompatProperty::ObjectPosition);
    if (!positionText.empty() && !parseCssPosition(tokenize(positionText), this, position))
        position = CssPosition();

    LayerQuad quad;
    if (!computeObjectFit(fit, natural, box, position, quad))
        return;

    // the object's drawn size (for SVG rasterization): the quad before cropping
    const Rml::Vector2f drawn(quad.rect.width / std::max(1e-4f, quad.u1 - quad.u0), quad.rect.height / std::max(1e-4f, quad.v1 - quad.v0));
    texture = isSprite ? spriteTexture : image.texture(this, drawn);

    // a sprite or rect: the coordinates inside that part of the texture
    if (hasSourceRect){
        const Rml::Vector2f dimensions(Rml::Math::Max(texture.GetDimensions(), Rml::Vector2i(1)));
        const Rml::Vector2f a = sourceRect.TopLeft() / dimensions, b = sourceRect.BottomRight() / dimensions;
        quad.u0 = a.x + quad.u0 * (b.x - a.x);
        quad.u1 = a.x + quad.u1 * (b.x - a.x);
        quad.v0 = a.y + quad.v0 * (b.y - a.y);
        quad.v1 = a.y + quad.v1 * (b.y - a.y);
    }

    // image-color x opacity, as RmlUi's img; a monochrome currentColor SVG x the CSS color too
    const Rml::ComputedValues& computed = GetComputedValues();
    Rml::Colourb base = computed.image_color();
    if (!isSprite && image.tintsWithColor()){
        const Rml::Colourb color = computed.color();
        base = Rml::Colourb((Rml::byte)((base.red * color.red + 127) / 255), (Rml::byte)((base.green * color.green + 127) / 255),
                            (Rml::byte)((base.blue * color.blue + 127) / 255), (Rml::byte)((base.alpha * color.alpha + 127) / 255));
    }
    const Rml::ColourbPremultiplied colour = base.ToPremultiplied(computed.opacity());
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, origin + Rml::Vector2f(quad.rect.x, quad.rect.y), Rml::Vector2f(quad.rect.width, quad.rect.height), colour,
                                     Rml::Vector2f(quad.u0, quad.v0), Rml::Vector2f(quad.u1, quad.v1));
    geometry = renderManager->MakeGeometry(std::move(mesh));
}

void HtmlImageElement::OnRender(){
    ensureLoaded();
    if (geometryDirty)
        generateGeometry();
    if (!geometry)
        return;
    const Rml::Vector2f offset = GetAbsoluteOffset(Rml::BoxArea::Border);
    decorators::RoundedClip clip(this, clipGeometry, offset);
    geometry.Render(offset, texture);
}

void HtmlImageElement::OnResize(){
    geometryDirty = true;
}

void HtmlImageElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    Rml::Element::OnAttributeChange(changed_attributes);
    if (changed_attributes.count("src") || changed_attributes.count("sprite") || changed_attributes.count("rect")){
        sourceDirty = true;
        DirtyLayout();
    }
    if (changed_attributes.count("width") || changed_attributes.count("height"))
        DirtyLayout();
}

void HtmlImageElement::OnPropertyChange(const Rml::PropertyIdSet& changed_properties){
    Rml::Element::OnPropertyChange(changed_properties);
    if (changed_properties.Contains(Rml::PropertyId::ImageColor) || changed_properties.Contains(Rml::PropertyId::Opacity) ||
        changed_properties.Contains(Rml::PropertyId::Color) || changed_properties.Contains(Rml::PropertyId::BorderTopLeftRadius) ||
        changed_properties.Contains(Rml::PropertyId::BorderTopRightRadius) || changed_properties.Contains(Rml::PropertyId::BorderBottomRightRadius) ||
        changed_properties.Contains(Rml::PropertyId::BorderBottomLeftRadius))
        geometryDirty = true;
    for (CompatProperty property : {CompatProperty::ObjectFit, CompatProperty::ObjectPosition}){
        const Rml::PropertyId id = CompatProperties::id(property);
        if (id != Rml::PropertyId::Invalid && changed_properties.Contains(id))
            geometryDirty = true;
    }
    const Rml::PropertyId rendering = CompatProperties::id(CompatProperty::ImageRendering);
    if (rendering != Rml::PropertyId::Invalid && changed_properties.Contains(rendering)){
        // nearest or linear sampling is a different texture
        sourceDirty = true;
        geometryDirty = true;
    }
}

void HtmlImageElement::OnDpRatioChange(){
    sourceDirty = true;
    DirtyLayout();
}

void HtmlImageElement::OnStyleSheetChange(){
    if (HasAttribute("sprite")){
        sourceDirty = true;
        DirtyLayout();
    }
}
