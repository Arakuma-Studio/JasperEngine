// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlBorderImageDecorator.h"

#include "Log.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::decorators;

namespace {

    struct Span {
        float start, length;
    };

    // tiles of `tile` along [start, start + length) for a border-image-repeat (§6.5)
    std::vector<Span> tileAxis(float start, float length, float tile, BorderImageRepeat repeat){
        std::vector<Span> spans;
        if (!(length > 0.0f))
            return spans;
        if (repeat == BorderImageRepeat::Stretch || !(tile > 0.0f)){
            spans.push_back({start, length});
            return spans;
        }
        if (repeat == BorderImageRepeat::Round){
            const float count = std::max(1.0f, std::round(length / tile));
            const float size = length / count;
            for (int i = 0; i < (int)count; i++)
                spans.push_back({start + i * size, size});
            return spans;
        }
        if (repeat == BorderImageRepeat::Space){
            const float count = std::floor(length / tile + 1e-4f);
            if (count < 1.0f)
                return spans;
            const float gap = (length - count * tile) / (count + 1.0f);
            for (int i = 0; i < (int)count; i++)
                spans.push_back({start + gap + i * (tile + gap), tile});
            return spans;
        }
        // repeat: centred on the area, cut at its ends
        const float centre = start + (length - tile) * 0.5f;
        const float first = centre - std::ceil((centre - start) / tile - 1e-4f) * tile;
        for (float at = first; at < start + length - 1e-4f; at += tile)
            spans.push_back({at, tile});
        return spans;
    }

    // a source rectangle drawn over a destination rectangle with the given repeats
    void addRegion(std::vector<LayerQuad>& quads, const LayerRect& destination, float sx, float sy, float sw, float sh,
                   float tileWidth, float tileHeight, BorderImageRepeat repeatX, BorderImageRepeat repeatY,
                   float imageWidth, float imageHeight){
        if (!(destination.width > 0.0f) || !(destination.height > 0.0f) || !(sw > 0.0f) || !(sh > 0.0f))
            return;
        const std::vector<Span> xs = tileAxis(destination.x, destination.width, tileWidth, repeatX);
        const std::vector<Span> ys = tileAxis(destination.y, destination.height, tileHeight, repeatY);
        for (const Span& y : ys){
            for (const Span& x : xs){
                const float left = std::max(x.start, destination.x), right = std::min(x.start + x.length, destination.right());
                const float top = std::max(y.start, destination.y), bottom = std::min(y.start + y.length, destination.bottom());
                if (right - left <= 1e-4f || bottom - top <= 1e-4f)
                    continue;
                LayerQuad quad;
                quad.rect = LayerRect{left, top, right - left, bottom - top};
                quad.u0 = (sx + (left - x.start) / x.length * sw) / imageWidth;
                quad.u1 = (sx + (right - x.start) / x.length * sw) / imageWidth;
                quad.v0 = (sy + (top - y.start) / y.length * sh) / imageHeight;
                quad.v1 = (sy + (bottom - y.start) / y.length * sh) / imageHeight;
                quads.push_back(quad);
            }
        }
    }

    BorderImageRepeat parseRepeat(const std::string& token, bool& ok){
        const std::string value = lower(trim(token));
        ok = true;
        if (value == "stretch") return BorderImageRepeat::Stretch;
        if (value == "repeat") return BorderImageRepeat::Repeat;
        if (value == "round") return BorderImageRepeat::Round;
        if (value == "space") return BorderImageRepeat::Space;
        ok = false;
        return BorderImageRepeat::Stretch;
    }

}

std::vector<LayerQuad> jasperengine::htmlui::compat::computeBorderImageGeometry(const BorderImageInput& in){
    std::vector<LayerQuad> quads;
    const float iw = in.imageWidth, ih = in.imageHeight;
    if (!(iw > 0.0f) || !(ih > 0.0f))
        return quads;

    // the border image area: the border box grown by the outsets
    const LayerRect area{in.borderBox.x - in.outset[3], in.borderBox.y - in.outset[0],
                         in.borderBox.width + in.outset[1] + in.outset[3], in.borderBox.height + in.outset[0] + in.outset[2]};
    if (!(area.width > 0.0f) || !(area.height > 0.0f))
        return quads;

    // overlapping widths scale down together (§6.6)
    float top = in.width[0], right = in.width[1], bottom = in.width[2], left = in.width[3];
    float factor = 1.0f;
    if (left + right > area.width)
        factor = std::min(factor, area.width / (left + right));
    if (top + bottom > area.height)
        factor = std::min(factor, area.height / (top + bottom));
    top *= factor;
    right *= factor;
    bottom *= factor;
    left *= factor;

    // slices within the image
    const float st = std::min(in.slice[0], ih), sr = std::min(in.slice[1], iw);
    const float sb = std::min(in.slice[2], ih), sl = std::min(in.slice[3], iw);
    const float middleWidth = std::max(0.0f, iw - sl - sr), middleHeight = std::max(0.0f, ih - st - sb);
    const float innerWidth = area.width - left - right, innerHeight = area.height - top - bottom;

    const BorderImageRepeat stretch = BorderImageRepeat::Stretch;
    // corners: scaled to their widths
    addRegion(quads, {area.x, area.y, left, top}, 0, 0, sl, st, 0, 0, stretch, stretch, iw, ih);
    addRegion(quads, {area.right() - right, area.y, right, top}, iw - sr, 0, sr, st, 0, 0, stretch, stretch, iw, ih);
    addRegion(quads, {area.right() - right, area.bottom() - bottom, right, bottom}, iw - sr, ih - sb, sr, sb, 0, 0, stretch, stretch, iw, ih);
    addRegion(quads, {area.x, area.bottom() - bottom, left, bottom}, 0, ih - sb, sl, sb, 0, 0, stretch, stretch, iw, ih);

    // edges: scaled to their width, then repeated along their length
    const float topScale = st > 0.0f ? top / st : 0.0f;
    const float bottomScale = sb > 0.0f ? bottom / sb : 0.0f;
    const float leftScale = sl > 0.0f ? left / sl : 0.0f;
    const float rightScale = sr > 0.0f ? right / sr : 0.0f;
    addRegion(quads, {area.x + left, area.y, innerWidth, top}, sl, 0, middleWidth, st, middleWidth * topScale, top, in.repeatX, stretch, iw, ih);
    addRegion(quads, {area.x + left, area.bottom() - bottom, innerWidth, bottom}, sl, ih - sb, middleWidth, sb, middleWidth * bottomScale, bottom,
              in.repeatX, stretch, iw, ih);
    addRegion(quads, {area.x, area.y + top, left, innerHeight}, 0, st, sl, middleHeight, left, middleHeight * leftScale, stretch, in.repeatY, iw, ih);
    addRegion(quads, {area.right() - right, area.y + top, right, innerHeight}, iw - sr, st, sr, middleHeight, right, middleHeight * rightScale,
              stretch, in.repeatY, iw, ih);

    // the middle (fill): the width scaled like the top (else the bottom), the height like the left (else the right)
    if (in.fill){
        float horizontal = topScale > 0.0f ? topScale : bottomScale;
        float vertical = leftScale > 0.0f ? leftScale : rightScale;
        if (!(horizontal > 0.0f))
            horizontal = 1.0f;
        if (!(vertical > 0.0f))
            vertical = 1.0f;
        addRegion(quads, {area.x + left, area.y + top, innerWidth, innerHeight}, sl, st, middleWidth, middleHeight, middleWidth * horizontal,
                  middleHeight * vertical, in.repeatX, in.repeatY, iw, ih);
    }
    return quads;
}

// ---------------------------------------------------------------- spec

bool BorderImageSpec::parse(const std::vector<std::string>& arguments, BorderImageSpec& spec, std::string* error){
    spec = BorderImageSpec();
    const auto fail = [error](const std::string& message){
        if (error)
            *error = message;
        return false;
    };
    if (arguments.size() != 16)
        return fail("html-border-image needs 16 arguments");
    spec.path = unquote(arguments[0]);
    if (spec.path.empty())
        return fail("no image path");
    for (int i = 0; i < 4; i++){
        Length slice;
        if (!parseLength(arguments[1 + i], slice, true) || (!slice.isNumber() && !slice.isPercent() && slice.value != 0.0f) || slice.value < 0.0f)
            return fail("slice '" + arguments[1 + i] + "' is not a non-negative number or percentage");
        if (!slice.isPercent())
            slice.unit = Rml::Unit::NUMBER;
        spec.slice[i] = slice;
    }
    const std::string fill = lower(trim(arguments[5]));
    if (fill != "fill" && fill != "nofill" && fill != "none")
        return fail("fill '" + arguments[5] + "' is not fill or nofill");
    spec.fill = fill == "fill";
    for (int i = 0; i < 4; i++){
        const std::string token = lower(trim(arguments[6 + i]));
        if (token == "auto"){
            spec.width[i].automatic = true;
            continue;
        }
        if (!parseLength(token, spec.width[i].length, true) || spec.width[i].length.value < 0.0f)
            return fail("width '" + arguments[6 + i] + "' is not auto, a number, a length or a percentage");
    }
    for (int i = 0; i < 4; i++){
        if (!parseLength(arguments[10 + i], spec.outset[i], true) || spec.outset[i].isPercent() || spec.outset[i].value < 0.0f)
            return fail("outset '" + arguments[10 + i] + "' is not a non-negative number or length");
    }
    bool ok = false;
    spec.repeatX = parseRepeat(arguments[14], ok);
    if (!ok)
        return fail("repeat '" + arguments[14] + "' is not stretch, repeat, round or space");
    spec.repeatY = parseRepeat(arguments[15], ok);
    if (!ok)
        return fail("repeat '" + arguments[15] + "' is not stretch, repeat, round or space");
    return true;
}

// ---------------------------------------------------------------- decorator

struct HtmlBorderImageDecorator::Data {
    Image image;
    std::vector<LayerQuad> quads;
    Rml::Geometry geometry;
    Rml::Texture texture;
};

HtmlBorderImageDecorator::HtmlBorderImageDecorator(BorderImageSpec spec): spec(std::move(spec)) {}

HtmlBorderImageDecorator::~HtmlBorderImageDecorator() = default;

Rml::DecoratorDataHandle HtmlBorderImageDecorator::GenerateElementData(Rml::Element* element, Rml::BoxArea /*paint_area*/) const{
    Data* data = new Data();
    Rml::RenderManager* renderManager = element->GetRenderManager();
    if (!renderManager || !data->image.load(element, spec.path) || !data->image.hasIntrinsicSize())
        return reinterpret_cast<Rml::DecoratorDataHandle>(data);

    const Rml::Box& box = element->GetBox();
    const Rml::Vector2f borderSize = box.GetSize(Rml::BoxArea::Border);
    const Rml::BoxEdge edges[4] = {Rml::BoxEdge::Top, Rml::BoxEdge::Right, Rml::BoxEdge::Bottom, Rml::BoxEdge::Left};

    BorderImageInput input;
    input.borderBox = LayerRect{0, 0, borderSize.x, borderSize.y};
    for (int i = 0; i < 4; i++)
        input.borderWidths[i] = box.GetEdge(Rml::BoxArea::Border, edges[i]);
    // slice coordinates: image pixels for raster images, SVG units (CSS px) for SVG
    const Rml::Vector2f intrinsic = data->image.intrinsicSize();
    input.imageWidth = intrinsic.x;
    input.imageHeight = intrinsic.y;
    input.fill = spec.fill;
    input.repeatX = spec.repeatX;
    input.repeatY = spec.repeatY;
    for (int i = 0; i < 4; i++){
        const bool vertical = (i % 2) == 0;   // top, bottom
        input.slice[i] = spec.slice[i].isPercent() ? spec.slice[i].value * (vertical ? intrinsic.y : intrinsic.x) / 100.0f : spec.slice[i].value;
        input.outset[i] = spec.outset[i].isNumber() ? spec.outset[i].value * input.borderWidths[i] : resolve(element, spec.outset[i], 0.0f);
    }
    const float areaWidth = borderSize.x + input.outset[1] + input.outset[3];
    const float areaHeight = borderSize.y + input.outset[0] + input.outset[2];
    for (int i = 0; i < 4; i++){
        const bool vertical = (i % 2) == 0;
        const BorderImageWidth& width = spec.width[i];
        if (width.automatic)
            input.width[i] = input.slice[i];
        else if (width.length.isNumber())
            input.width[i] = width.length.value * input.borderWidths[i];
        else
            input.width[i] = resolve(element, width.length, vertical ? areaHeight : areaWidth);
    }

    data->quads = computeBorderImageGeometry(input);
    if (data->quads.empty())
        return reinterpret_cast<Rml::DecoratorDataHandle>(data);

    // an SVG is rasterized at the largest scale a region is drawn with
    float scale = 1.0f;
    for (int i = 0; i < 4; i++){
        if (input.slice[i] > 0.0f)
            scale = std::max(scale, input.width[i] / input.slice[i]);
    }
    data->texture = data->image.texture(element, intrinsic * std::min(scale, 16.0f));
    const Rml::ColourbPremultiplied colour = data->image.colour(element);
    Rml::Mesh mesh;
    for (const LayerQuad& quad : data->quads){
        Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(quad.rect.x, quad.rect.y), Rml::Vector2f(quad.rect.width, quad.rect.height), colour,
                                         Rml::Vector2f(quad.u0, quad.v0), Rml::Vector2f(quad.u1, quad.v1));
    }
    data->geometry = renderManager->MakeGeometry(std::move(mesh));
    return reinterpret_cast<Rml::DecoratorDataHandle>(data);
}

void HtmlBorderImageDecorator::ReleaseElementData(Rml::DecoratorDataHandle element_data) const{
    delete reinterpret_cast<Data*>(element_data);
}

void HtmlBorderImageDecorator::RenderElement(Rml::Element* element, Rml::DecoratorDataHandle element_data) const{
    Data* data = reinterpret_cast<Data*>(element_data);
    if (!data || !data->geometry)
        return;
    data->geometry.Render(element->GetAbsoluteOffset(Rml::BoxArea::Border), data->texture);
}

const std::vector<LayerQuad>* HtmlBorderImageDecorator::quadsOf(Rml::DecoratorDataHandle element_data){
    const Data* data = reinterpret_cast<const Data*>(element_data);
    return data ? &data->quads : nullptr;
}

// ---------------------------------------------------------------- instancer

HtmlBorderImageDecoratorInstancer::HtmlBorderImageDecoratorInstancer(){
    static const char* const names[16] = {"source", "slice-top", "slice-right", "slice-bottom", "slice-left", "fill",
                                          "width-top", "width-right", "width-bottom", "width-left",
                                          "outset-top", "outset-right", "outset-bottom", "outset-left", "repeat-x", "repeat-y"};
    static const char* const defaults[16] = {"", "100%", "100%", "100%", "100%", "nofill", "1", "1", "1", "1", "0", "0", "0", "0",
                                             "stretch", "stretch"};
    std::string list;
    for (size_t i = 0; i < 16; i++){
        ids[i] = RegisterProperty(names[i], defaults[i]).AddParser("string").GetId();
        list += (i ? ", " : "") + std::string(names[i]);
    }
    RegisterShorthand("decorator", list, Rml::ShorthandType::FallThrough);
}

HtmlBorderImageDecoratorInstancer::~HtmlBorderImageDecoratorInstancer() = default;

Rml::SharedPtr<Rml::Decorator> HtmlBorderImageDecoratorInstancer::InstanceDecorator(const Rml::String& /*name*/,
                                                                                    const Rml::PropertyDictionary& properties,
                                                                                    const Rml::DecoratorInstancerInterface& /*instancer_interface*/){
    std::vector<std::string> arguments;
    for (size_t i = 0; i < 16; i++)
        arguments.push_back(stringArgument(properties, ids[i]));
    BorderImageSpec spec;
    std::string error;
    if (!BorderImageSpec::parse(arguments, spec, &error)){
        Log::warn("[HTML UI] html-border-image(): %s", error.c_str());
        return nullptr;
    }
    return std::make_shared<HtmlBorderImageDecorator>(std::move(spec));
}
