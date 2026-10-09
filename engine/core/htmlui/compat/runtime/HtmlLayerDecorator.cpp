// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlLayerDecorator.h"

#include "htmlui/compat/DiagnosticCodes.h"

#include "Log.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::decorators;

// ---------------------------------------------------------------- layer math

bool LayerRect::operator==(const LayerRect& other) const{
    const auto near = [](float a, float b){ return std::fabs(a - b) < 0.001f; };
    return near(x, other.x) && near(y, other.y) && near(width, other.width) && near(height, other.height);
}

namespace {

    // the tile starts along one axis whose tiles meet [clipStart, clipEnd)
    std::vector<float> tileStarts(float areaStart, float areaLength, float tile, const LayerPosition& position, LayerRepeat repeat,
                                  float clipStart, float clipEnd){
        std::vector<float> starts;
        if (!(tile > 0.0f) || !(clipEnd > clipStart))
            return starts;

        float period = tile;
        float origin = areaStart + position.percent / 100.0f * (areaLength - tile) + position.offset;
        bool repeating = repeat == LayerRepeat::Repeat || repeat == LayerRepeat::Round;
        if (repeat == LayerRepeat::Space){
            const float count = std::floor(areaLength / tile + 1e-4f);
            if (count >= 2.0f){
                // the first and last tile touch the area's edges; the position is ignored
                period = tile + (areaLength - count * tile) / (count - 1.0f);
                origin = areaStart;
                repeating = true;
            }
        }

        if (!repeating){
            if (origin < clipEnd && origin + tile > clipStart)
                starts.push_back(origin);
            return starts;
        }

        const float first = origin - std::ceil((origin - clipStart) / period - 1e-4f) * period;
        for (float start = first; start < clipEnd - 1e-4f; start += period){
            if (start + tile > clipStart + 1e-4f)
                starts.push_back(start);
        }
        return starts;
    }

}

LayerGeometry jasperengine::htmlui::compat::computeLayerGeometry(const LayerInput& in){
    LayerGeometry out;
    const LayerRect& area = in.positioningArea;
    const float iw = in.intrinsicWidth, ih = in.intrinsicHeight;
    const bool hasRatio = iw > 0.0f && ih > 0.0f;

    // background-size (§3.9)
    float tw = 0.0f, th = 0.0f;
    if (in.sizeMode == LayerSizeMode::Cover || in.sizeMode == LayerSizeMode::Contain){
        if (hasRatio){
            const float sx = area.width / iw, sy = area.height / ih;
            const float s = in.sizeMode == LayerSizeMode::Cover ? std::max(sx, sy) : std::min(sx, sy);
            tw = iw * s;
            th = ih * s;
        }else{
            tw = area.width;
            th = area.height;
        }
    }else if (!in.sizeX.automatic && !in.sizeY.automatic){
        tw = in.sizeX.value;
        th = in.sizeY.value;
    }else if (!in.sizeX.automatic){
        tw = in.sizeX.value;
        th = hasRatio ? tw * ih / iw : (ih > 0.0f ? ih : area.height);
    }else if (!in.sizeY.automatic){
        th = in.sizeY.value;
        tw = hasRatio ? th * iw / ih : (iw > 0.0f ? iw : area.width);
    }else{
        tw = iw > 0.0f ? iw : area.width;
        th = ih > 0.0f ? ih : area.height;
    }

    // round (§3.4): a whole number of tiles; an auto other dimension keeps the ratio
    const bool roundX = in.repeatX == LayerRepeat::Round && tw > 0.0f && area.width > 0.0f;
    const bool roundY = in.repeatY == LayerRepeat::Round && th > 0.0f && area.height > 0.0f;
    const float w0 = tw, h0 = th;
    if (roundX)
        tw = area.width / std::max(1.0f, std::round(area.width / tw));
    if (roundY)
        th = area.height / std::max(1.0f, std::round(area.height / th));
    const bool otherAuto = in.sizeMode == LayerSizeMode::Lengths;
    if (roundX && !roundY && otherAuto && in.sizeY.automatic && w0 > 0.0f)
        th = h0 * tw / w0;
    if (roundY && !roundX && otherAuto && in.sizeX.automatic && h0 > 0.0f)
        tw = w0 * th / h0;

    if (!(tw > 0.0f) || !(th > 0.0f)){
        return out;
    }

    // repeated tiles below the minimum are drawn at the minimum (bounds the quad count)
    const bool repeatsX = in.repeatX != LayerRepeat::NoRepeat;
    const bool repeatsY = in.repeatY != LayerRepeat::NoRepeat;
    if (repeatsX && tw < in.minTile){
        tw = in.minTile;
        out.tileClamped = true;
    }
    if (repeatsY && th < in.minTile){
        th = in.minTile;
        out.tileClamped = true;
    }

    const LayerRect& clip = in.clipArea;
    std::vector<float> xs = tileStarts(area.x, area.width, tw, in.positionX, in.repeatX, clip.x, clip.right());
    std::vector<float> ys = tileStarts(area.y, area.height, th, in.positionY, in.repeatY, clip.y, clip.bottom());
    if (in.maxQuads > 0 && xs.size() * ys.size() > in.maxQuads){
        const float factor = std::ceil(std::sqrt((float)(xs.size() * ys.size()) / (float)in.maxQuads) * 100.0f) / 100.0f;
        tw *= factor;
        th *= factor;
        out.countClamped = true;
        xs = tileStarts(area.x, area.width, tw, in.positionX, in.repeatX, clip.x, clip.right());
        ys = tileStarts(area.y, area.height, th, in.positionY, in.repeatY, clip.y, clip.bottom());
    }
    out.tileWidth = tw;
    out.tileHeight = th;

    out.quads.reserve(xs.size() * ys.size());
    for (float y : ys){
        for (float x : xs){
            const float left = std::max(x, clip.x), top = std::max(y, clip.y);
            const float right = std::min(x + tw, clip.right()), bottom = std::min(y + th, clip.bottom());
            if (right - left <= 1e-4f || bottom - top <= 1e-4f)
                continue;
            LayerQuad quad;
            quad.rect = LayerRect{left, top, right - left, bottom - top};
            quad.u0 = (left - x) / tw;
            quad.v0 = (top - y) / th;
            quad.u1 = (right - x) / tw;
            quad.v1 = (bottom - y) / th;
            out.quads.push_back(quad);
        }
    }
    return out;
}

// ---------------------------------------------------------------- LayerSpec

bool LayerSpec::parseSize(const std::string& token, LayerSpec& spec, bool x, std::string* error){
    const std::string value = lower(trim(token));
    if (value == "cover" || value == "contain"){
        spec.sizeMode = value == "cover" ? LayerSizeMode::Cover : LayerSizeMode::Contain;
        return true;
    }
    bool& automatic = x ? spec.autoX : spec.autoY;
    Length& length = x ? spec.sizeX : spec.sizeY;
    if (value == "auto" || value.empty()){
        automatic = true;
        return true;
    }
    if (!parseLength(value, length) || length.value < 0.0f){
        if (error)
            *error = "size '" + token + "' is not auto, cover, contain or a non-negative length or percentage";
        return false;
    }
    automatic = false;
    return true;
}

bool LayerSpec::parsePosition(const std::string& token, bool x, Length& percent, Length& offset){
    const std::string value = lower(trim(token));
    percent = Length{0.0f, Rml::Unit::PERCENT};
    offset = Length{0.0f, Rml::Unit::PX};
    if (value == "center"){
        percent.value = 50.0f;
        return true;
    }
    if (value == (x ? "left" : "top"))
        return true;
    if (value == (x ? "right" : "bottom")){
        percent.value = 100.0f;
        return true;
    }
    // one <length-percentage>
    Length single;
    if (parseLength(value, single)){
        if (single.isPercent())
            percent = single;
        else
            offset = single;
        return true;
    }
    // calc(<a> +|- <b>) with one percentage and one length (in either order)
    if (value.compare(0, 5, "calc(") == 0 && value.back() == ')'){
        const std::string inner = trim(value.substr(5, value.size() - 6));
        const std::vector<std::string> parts = tokenize(inner);
        if (parts.size() != 3 || (parts[1] != "+" && parts[1] != "-"))
            return false;
        Length a, b;
        if (!parseLength(parts[0], a) || !parseLength(parts[2], b))
            return false;
        if (parts[1] == "-")
            b.value = -b.value;
        bool havePercent = false, haveLength = false;
        for (const Length& term : {a, b}){
            if (term.isPercent()){
                if (havePercent)
                    return false;
                percent = term;
                havePercent = true;
            }else{
                if (haveLength)
                    return false;
                offset = term;
                haveLength = true;
            }
        }
        return true;
    }
    return false;
}

bool LayerSpec::parseRepeat(const std::string& token, LayerRepeat& repeat){
    const std::string value = lower(trim(token));
    if (value == "repeat")
        repeat = LayerRepeat::Repeat;
    else if (value == "no-repeat")
        repeat = LayerRepeat::NoRepeat;
    else if (value == "space")
        repeat = LayerRepeat::Space;
    else if (value == "round")
        repeat = LayerRepeat::Round;
    else
        return false;
    return true;
}

bool LayerSpec::parseBox(const std::string& token, Rml::BoxArea& box){
    const std::string value = lower(trim(token));
    if (value == "border-box")
        box = Rml::BoxArea::Border;
    else if (value == "padding-box")
        box = Rml::BoxArea::Padding;
    else if (value == "content-box")
        box = Rml::BoxArea::Content;
    else
        return false;
    return true;
}

bool LayerSpec::parse(const std::array<std::string, 10>& arguments, LayerSpec& spec, std::string* error){
    spec = LayerSpec();
    const auto fail = [error](const std::string& message){
        if (error)
            *error = message;
        return false;
    };
    spec.path = unquote(arguments[0]);
    if (spec.path.empty())
        return fail("no image path");
    if (!parseSize(arguments[1], spec, true, error) || !parseSize(arguments[2], spec, false, error))
        return false;
    if (!parsePosition(arguments[3], true, spec.positionX, spec.positionOffsetX))
        return fail("x position '" + arguments[3] + "' is not left, center, right, a length or percentage, or calc(<percentage> +|- <length>)");
    if (!parsePosition(arguments[4], false, spec.positionY, spec.positionOffsetY))
        return fail("y position '" + arguments[4] + "' is not top, center, bottom, a length or percentage, or calc(<percentage> +|- <length>)");
    if (!parseRepeat(arguments[5], spec.repeatX) || !parseRepeat(arguments[6], spec.repeatY))
        return fail("repeat '" + arguments[5] + " " + arguments[6] + "' is not repeat, no-repeat, space or round");
    if (!parseBox(arguments[7], spec.origin))
        return fail("origin '" + arguments[7] + "' is not border-box, padding-box or content-box");
    if (!parseBox(arguments[8], spec.clip))
        return fail("clip '" + arguments[8] + "' is not border-box, padding-box or content-box");
    const std::string attachment = lower(trim(arguments[9]));
    if (attachment == "scroll")
        spec.attachment = LayerAttachment::Scroll;
    else if (attachment == "fixed")
        spec.attachment = LayerAttachment::Fixed;
    else if (attachment == "local")
        spec.attachment = LayerAttachment::Local;
    else
        return fail("attachment '" + arguments[9] + "' is not scroll, fixed or local");
    return true;
}

// ---------------------------------------------------------------- decorator

struct HtmlLayerDecorator::Data {
    Image image;
    bool loaded = false;
    LayerGeometry layer;
    Rml::Geometry geometry;
    Rml::Texture texture;
    Rml::Geometry clip;
    Rml::Vector2f builtAt{0, 0};       // fixed: the element's absolute offset the tiles were made for
};

HtmlLayerDecorator::HtmlLayerDecorator(LayerSpec spec): spec(std::move(spec)) {}

HtmlLayerDecorator::~HtmlLayerDecorator() = default;

void HtmlLayerDecorator::build(Rml::Element* element, Data& data) const{
    data.geometry = Rml::Geometry();
    data.layer = LayerGeometry();
    data.texture = Rml::Texture();
    Rml::RenderManager* renderManager = element->GetRenderManager();
    if (!data.loaded || !renderManager)
        return;

    LayerInput input;
    const Rml::Rectanglef origin = boxRectangle(element, spec.origin);
    const Rml::Rectanglef clip = boxRectangle(element, spec.clip);
    input.positioningArea = LayerRect{origin.Left(), origin.Top(), origin.Width(), origin.Height()};
    if (spec.attachment == LayerAttachment::Fixed){
        // the viewport, in the element's border-box coordinates
        const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2i viewport = element->GetContext() ? element->GetContext()->GetDimensions() : Rml::Vector2i(0, 0);
        input.positioningArea = LayerRect{-offset.x, -offset.y, (float)viewport.x, (float)viewport.y};
        data.builtAt = offset;
    }
    input.clipArea = LayerRect{clip.Left(), clip.Top(), clip.Width(), clip.Height()};
    input.intrinsicWidth = data.image.intrinsicSize().x;
    input.intrinsicHeight = data.image.intrinsicSize().y;
    input.sizeMode = spec.sizeMode;
    input.sizeX = LayerSizeValue{spec.autoX, spec.autoX ? 0.0f : resolve(element, spec.sizeX, input.positioningArea.width)};
    input.sizeY = LayerSizeValue{spec.autoY, spec.autoY ? 0.0f : resolve(element, spec.sizeY, input.positioningArea.height)};
    input.positionX = LayerPosition{spec.positionX.value, resolve(element, spec.positionOffsetX, input.positioningArea.width)};
    input.positionY = LayerPosition{spec.positionY.value, resolve(element, spec.positionOffsetY, input.positioningArea.height)};
    input.repeatX = spec.repeatX;
    input.repeatY = spec.repeatY;

    data.layer = computeLayerGeometry(input);
    if (data.layer.tileClamped || data.layer.countClamped){
        reportOnce(element, codes::ApproximatedVisible,
                   "background image '" + spec.path + "' repeats tiles smaller than 2px; they are drawn larger",
                   "use a larger image or background-size");
    }
    if (data.layer.quads.empty())
        return;

    data.texture = data.image.texture(element, Rml::Vector2f(data.layer.tileWidth, data.layer.tileHeight));
    const Rml::ColourbPremultiplied colour = data.image.colour(element);
    Rml::Mesh mesh;
    for (const LayerQuad& quad : data.layer.quads){
        Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(quad.rect.x, quad.rect.y), Rml::Vector2f(quad.rect.width, quad.rect.height),
                                         colour, Rml::Vector2f(quad.u0, quad.v0), Rml::Vector2f(quad.u1, quad.v1));
    }
    data.geometry = renderManager->MakeGeometry(std::move(mesh));
}

Rml::DecoratorDataHandle HtmlLayerDecorator::GenerateElementData(Rml::Element* element, Rml::BoxArea /*paint_area*/) const{
    // the boxes come from the origin/clip arguments, not RmlUi's paint area
    Data* data = new Data();
    data->loaded = data->image.load(element, spec.path);
    data->clip = makeClipGeometry(element, spec.clip);
    build(element, *data);
    return reinterpret_cast<Rml::DecoratorDataHandle>(data);
}

void HtmlLayerDecorator::ReleaseElementData(Rml::DecoratorDataHandle element_data) const{
    delete reinterpret_cast<Data*>(element_data);
}

void HtmlLayerDecorator::RenderElement(Rml::Element* element, Rml::DecoratorDataHandle element_data) const{
    Data* data = reinterpret_cast<Data*>(element_data);
    if (!data)
        return;
    const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    // fixed: the tiles stay on the viewport while the element moves (scrolling)
    if (spec.attachment == LayerAttachment::Fixed && offset != data->builtAt)
        build(element, *data);
    if (!data->geometry)
        return;
    RoundedClip scope(element, data->clip, offset);
    data->geometry.Render(offset, data->texture);
}

const LayerGeometry* HtmlLayerDecorator::geometryOf(Rml::DecoratorDataHandle element_data){
    const Data* data = reinterpret_cast<const Data*>(element_data);
    return data ? &data->layer : nullptr;
}

// ---------------------------------------------------------------- instancer

HtmlLayerDecoratorInstancer::HtmlLayerDecoratorInstancer(){
    static const char* const names[10] = {"source", "size-x", "size-y", "position-x", "position-y", "repeat-x", "repeat-y",
                                          "origin", "clip", "attachment"};
    static const char* const defaults[10] = {"", "auto", "auto", "0%", "0%", "repeat", "repeat", "padding-box", "border-box", "scroll"};
    for (size_t i = 0; i < 10; i++)
        ids[i] = RegisterProperty(names[i], defaults[i]).AddParser("string").GetId();
    RegisterShorthand("decorator", "source, size-x, size-y, position-x, position-y, repeat-x, repeat-y, origin, clip, attachment",
                      Rml::ShorthandType::FallThrough);
}

HtmlLayerDecoratorInstancer::~HtmlLayerDecoratorInstancer() = default;

Rml::SharedPtr<Rml::Decorator> HtmlLayerDecoratorInstancer::InstanceDecorator(const Rml::String& /*name*/, const Rml::PropertyDictionary& properties,
                                                                              const Rml::DecoratorInstancerInterface& /*instancer_interface*/){
    std::array<std::string, 10> arguments;
    for (size_t i = 0; i < 10; i++)
        arguments[i] = stringArgument(properties, ids[i]);
    LayerSpec spec;
    std::string error;
    if (!LayerSpec::parse(arguments, spec, &error)){
        Log::warn("[HTML UI] html-layer(): %s", error.c_str());
        return nullptr;
    }
    return std::make_shared<HtmlLayerDecorator>(std::move(spec));
}
