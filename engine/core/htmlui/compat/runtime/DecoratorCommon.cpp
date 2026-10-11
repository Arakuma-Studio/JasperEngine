// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/DecoratorCommon.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/Hash.h"
#include "htmlui/compat/runtime/CompatProperties.h"
#include "htmlui/compat/runtime/SvgElementHook.h"

#include "HtmlUI.h"
#include "Log.h"
#include "RmlFontEngine.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <list>
#include <mutex>
#include <set>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::decorators;

namespace {

    struct UnitName {
        const char* name;
        Rml::Unit unit;
    };

    const UnitName kUnits[] = {
        {"px", Rml::Unit::PX}, {"em", Rml::Unit::EM}, {"rem", Rml::Unit::REM}, {"vw", Rml::Unit::VW}, {"vh", Rml::Unit::VH},
        {"dp", Rml::Unit::DP}, {"in", Rml::Unit::INCH}, {"cm", Rml::Unit::CM}, {"mm", Rml::Unit::MM}, {"pt", Rml::Unit::PT},
        {"pc", Rml::Unit::PC}, {"%", Rml::Unit::PERCENT},
    };

    // ---------------------------------------------------------------- raster cache

    constexpr size_t kRasterCacheBytes = 32u * 1024u * 1024u;

    struct RasterCache {
        std::mutex mutex;
        // most recent first
        std::list<std::pair<std::string, std::shared_ptr<const std::vector<uint8_t>>>> entries;
        size_t bytes = 0;
    };

    RasterCache& rasterCache(){
        static RasterCache cache;
        return cache;
    }

    std::mutex& reportMutex(){
        static std::mutex mutex;
        return mutex;
    }

    std::set<std::string>& reported(){
        static std::set<std::string> keys;
        return keys;
    }

    std::string documentPath(Rml::Element* element){
        Rml::ElementDocument* document = element ? element->GetOwnerDocument() : nullptr;
        return document ? Rml::StringUtilities::Replace(document->GetSourceURL(), '|', ':') : std::string();
    }

}

std::string decorators::lower(std::string_view text){
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return out;
}

std::string decorators::trim(std::string_view text){
    const size_t first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos)
        return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n\f");
    return std::string(text.substr(first, last - first + 1));
}

std::string decorators::unquote(std::string_view text){
    const std::string value = trim(text);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
        return value.substr(1, value.size() - 2);
    return value;
}

bool decorators::parseNumber(std::string_view token, float& value){
    const std::string text = trim(token);
    if (text.empty())
        return false;
    // locale-free: digits, one point, an exponent
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(parsed))
        return false;
    value = (float)parsed;
    return true;
}

bool decorators::parseLength(std::string_view token, Length& out, bool allowNumber){
    const std::string text = lower(trim(token));
    if (text.empty())
        return false;
    size_t split = text.size();
    while (split > 0 && (std::isalpha((unsigned char)text[split - 1]) || text[split - 1] == '%'))
        split--;
    float number = 0.0f;
    if (!parseNumber(text.substr(0, split), number))
        return false;
    const std::string unit = text.substr(split);
    if (unit.empty()){
        if (number == 0.0f){
            out = Length{0.0f, Rml::Unit::PX};
            return true;
        }
        if (!allowNumber)
            return false;
        out = Length{number, Rml::Unit::NUMBER};
        return true;
    }
    for (const UnitName& entry : kUnits){
        if (unit == entry.name){
            out = Length{number, entry.unit};
            return true;
        }
    }
    return false;
}

float decorators::resolve(Rml::Element* element, const Length& length, float percentBase){
    switch (length.unit){
        case Rml::Unit::PERCENT: return length.value * percentBase / 100.0f;
        case Rml::Unit::NUMBER:
        case Rml::Unit::PX: return length.value;
        default: return element ? element->ResolveLength(Rml::NumericValue(length.value, length.unit)) : length.value;
    }
}

std::vector<std::string> decorators::tokenize(std::string_view text){
    std::vector<std::string> tokens;
    std::string current;
    int depth = 0;
    char quote = 0;
    auto flush = [&](){
        if (!current.empty()){
            tokens.push_back(current);
            current.clear();
        }
    };
    for (char c : text){
        if (quote){
            current += c;
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\''){
            quote = c;
            current += c;
        }else if (c == '('){
            depth++;
            current += c;
        }else if (c == ')'){
            depth = std::max(0, depth - 1);
            current += c;
        }else if (depth == 0 && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')){
            flush();
        }else if (depth == 0 && c == ','){
            flush();
            tokens.emplace_back(",");
        }else{
            current += c;
        }
    }
    flush();
    return tokens;
}

bool decorators::parseCssPosition(const std::vector<std::string>& input, Rml::Element* element, CssPosition& out){
    out = CssPosition();
    std::vector<std::string> t;
    for (const std::string& token : input)
        t.push_back(lower(token));
    if (t.empty() || t.size() > 4)
        return false;
    const auto horizontalKeyword = [](const std::string& v){ return v == "left" || v == "right"; };
    const auto verticalKeyword = [](const std::string& v){ return v == "top" || v == "bottom"; };
    const auto keyword = [&](const std::string& v){ return horizontalKeyword(v) || verticalKeyword(v) || v == "center"; };
    const auto keywordPercent = [](const std::string& v){ return (v == "left" || v == "top") ? 0.0f : v == "center" ? 50.0f : 100.0f; };
    // a length-percentage into one axis (percent + px)
    const auto axisValue = [&](const std::string& v, float& percent, float& offset) -> bool {
        Length length;
        if (!parseLength(v, length))
            return false;
        percent = length.isPercent() ? length.value : 0.0f;
        offset = length.isPercent() ? 0.0f : resolve(element, length, 0.0f);
        return true;
    };

    if (t.size() <= 2){
        std::string x = t[0], y = t.size() == 2 ? t[1] : std::string("center");
        if (t.size() == 1 && verticalKeyword(t[0])){
            x = "center";
            y = t[0];
        }
        if (verticalKeyword(x) || horizontalKeyword(y))
            std::swap(x, y);
        if (verticalKeyword(x) || horizontalKeyword(y))
            return false;
        if (keyword(x))
            out.percentX = keywordPercent(x);
        else if (!axisValue(x, out.percentX, out.offsetX))
            return false;
        if (keyword(y))
            out.percentY = keywordPercent(y);
        else if (!axisValue(y, out.percentY, out.offsetY))
            return false;
        return true;
    }

    // 3 or 4 values: edge keywords, each with an optional offset from that edge
    bool haveX = false, haveY = false;
    size_t i = 0;
    while (i < t.size()){
        const std::string edge = t[i++];
        if (!keyword(edge))
            return false;
        float percent = 0.0f, offset = 0.0f;
        const bool hasOffset = edge != "center" && i < t.size() && !keyword(t[i]);
        if (hasOffset && !axisValue(t[i++], percent, offset))
            return false;
        bool horizontal = horizontalKeyword(edge);
        if (edge == "center")
            horizontal = !haveX;
        float& targetPercent = horizontal ? out.percentX : out.percentY;
        float& targetOffset = horizontal ? out.offsetX : out.offsetY;
        if ((horizontal && haveX) || (!horizontal && haveY))
            return false;
        if (edge == "right" || edge == "bottom"){
            targetPercent = 100.0f - percent;
            targetOffset = -offset;
        }else{
            targetPercent = keywordPercent(edge) + percent;
            targetOffset = offset;
        }
        (horizontal ? haveX : haveY) = true;
    }
    return true;
}

std::string decorators::stringArgument(const Rml::PropertyDictionary& properties, Rml::PropertyId id){
    const Rml::Property* property = properties.GetProperty(id);
    return property ? property->Get<Rml::String>() : std::string();
}

float decorators::rasterScale(Rml::Element* element){
    Rml::RenderManager* renderManager = element ? element->GetRenderManager() : nullptr;
    if (!renderManager || !HtmlUI::isInitialized())
        return 1.0f;
    const float scale = HtmlUI::getFontEngine().getRasterScale(*renderManager);
    return scale > 0.0f ? scale : 1.0f;
}

Rml::Rectanglef decorators::boxRectangle(Rml::Element* element, Rml::BoxArea area){
    const Rml::Box& box = element->GetBox();
    Rml::Vector2f offset(0, 0);
    for (int a = (int)Rml::BoxArea::Border; a < (int)area; a++){
        offset.x += box.GetEdge(Rml::BoxArea(a), Rml::BoxEdge::Left);
        offset.y += box.GetEdge(Rml::BoxArea(a), Rml::BoxEdge::Top);
    }
    return Rml::Rectanglef::FromPositionSize(offset, box.GetSize(area));
}

bool decorators::isSvgPath(std::string_view path){
    const std::string file = lower(filePart(path));
    return file.size() > 4 && file.compare(file.size() - 4, 4, ".svg") == 0;
}

std::string decorators::filePart(std::string_view path){
    const size_t end = path.find_first_of("?#");
    return std::string(path.substr(0, end));
}

// ---------------------------------------------------------------- Image

bool Image::load(Rml::Element* element, const std::string& path){
    Rml::RenderManager* renderManager = element ? element->GetRenderManager() : nullptr;
    source = path;
    intrinsic = Rml::Vector2f(0, 0);
    svg.reset();
    raster = Rml::Texture();
    if (!renderManager || path.empty())
        return false;

    const std::string document = documentPath(element);
    if (isSvgPath(path)){
        std::string resolved;
        Rml::GetSystemInterface()->JoinPath(resolved, document, filePart(path));
        Rml::String content;
        if (!Rml::GetFileInterface()->LoadFile(resolved, content)){
            reportOnce(element, codes::FileNotFound, "image '" + resolved + "' not found", "check the url() path");
            return false;
        }
        svg = SvgElementHook::preprocess(content, SvgUse::Layer, SvgPaintOverrides());
        float width = 0.0f, height = 0.0f;
        if (!htmlui::svgIntrinsicSize(svg->ok ? svg->xml : content, width, height)){
            reportOnce(element, codes::FileNotFound, "image '" + resolved + "' is not an SVG the renderer can read", "");
            svg.reset();
            return false;
        }
        for (const Diagnostic& diagnostic : svg->diagnostics)
            reportOnce(element, diagnostic.code, diagnostic.message + " ('" + resolved + "')", diagnostic.suggestion);
        intrinsic = Rml::Vector2f(width, height);
        return true;
    }

    // raster: RmlUi's texture database (RmlRenderer::LoadTexture, hook H7), nearest sampling
    // for image-rendering: pixelated / crisp-edges
    std::string request = path;
    const std::string rendering = CompatProperties::text(*element, CompatProperty::ImageRendering);
    if (rendering == "pixelated" || rendering == "crisp-edges")
        request += (request.find('?') == std::string::npos ? "?" : "&") + std::string("htmlui-filter=nearest");
    raster = renderManager->LoadTexture(request, document);
    const Rml::Vector2i dimensions = raster.GetDimensions();
    if (dimensions.x <= 0 || dimensions.y <= 0){
        raster = Rml::Texture();
        return false;
    }
    intrinsic = Rml::Vector2f(dimensions) * Rml::ElementUtilities::GetDensityIndependentPixelRatio(element);
    return true;
}

Rml::Texture Image::texture(Rml::Element* element, Rml::Vector2f size){
    if (!svg)
        return raster;
    Rml::RenderManager* renderManager = element ? element->GetRenderManager() : nullptr;
    if (!renderManager)
        return Rml::Texture();
    const float scale = rasterScale(element);
    const Rml::Vector2i pixels(std::max(1, std::min(8192, (int)std::lround(size.x * scale))),
                               std::max(1, std::min(8192, (int)std::lround(size.y * scale))));
    std::string xml = svg->xml;
    if (svg->tint == SvgTint::Substitute){
        const Rml::Colourb color = element->GetComputedValues().color();
        xml = svg->withColor(SvgPreprocess::colorText(color.red, color.green, color.blue, color.alpha));
    }
    if (svgTexture && pixels == svgTextureSize && xml == svgTextureXml)
        return svgTexture;

    std::shared_ptr<const std::vector<uint8_t>> rgba = rasterizeCached(xml, pixels.x, pixels.y);
    if (!rgba || rgba->empty())
        return Rml::Texture();
    svgTextureSize = pixels;
    svgTextureXml = xml;
    svgTexture = renderManager->MakeCallbackTexture([rgba, pixels](const Rml::CallbackTextureInterface& textureInterface) -> bool {
        return textureInterface.GenerateTexture({reinterpret_cast<const Rml::byte*>(rgba->data()), rgba->size()}, pixels);
    });
    return svgTexture;
}

Rml::ColourbPremultiplied Image::colour(Rml::Element* element) const{
    const Rml::ComputedValues& computed = element->GetComputedValues();
    if (svg && svg->tint == SvgTint::Color)
        return computed.color().ToPremultiplied(computed.opacity());
    return Rml::Colourb(255, 255, 255, 255).ToPremultiplied(computed.opacity());
}

// ---------------------------------------------------------------- clip

Rml::Geometry decorators::makeClipGeometry(Rml::Element* element, Rml::BoxArea area){
    Rml::RenderManager* renderManager = element ? element->GetRenderManager() : nullptr;
    if (!renderManager)
        return Rml::Geometry();
    if (!element->GetComputedValues().has_border_radius())
        return Rml::Geometry();
    // the radii as the fork resolved them (percentages, overlapping curves scaled)
    const Rml::RenderBox box = element->GetRenderBox(area);
    if (!box.HasBorderRadius())
        return Rml::Geometry();
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateBackground(mesh, box, Rml::ColourbPremultiplied(255, 255, 255, 255));
    if (mesh.indices.empty())
        return Rml::Geometry();
    return renderManager->MakeGeometry(std::move(mesh));
}

RoundedClip::RoundedClip(Rml::Element* element, Rml::Geometry& clip, Rml::Vector2f translation){
    if (!clip || !element)
        return;
    renderManager = element->GetRenderManager();
    if (!renderManager)
        return;
    saved = renderManager->GetState();
    transform = saved.transform;
    Rml::ClipMaskGeometryList list = saved.clip_mask_list;
    const bool identity = transform == Rml::Matrix4f::Identity();
    // the first mask of a list sets the stencil (as RmlUi's own clip lists): an Intersect
    // there would count on a cleared stencil and drew nothing after the first such clip
    const Rml::ClipMaskOperation operation = list.empty() ? Rml::ClipMaskOperation::Set : Rml::ClipMaskOperation::Intersect;
    list.push_back(Rml::ClipMaskGeometry{operation, &clip, translation, identity ? nullptr : &transform});
    renderManager->SetClipMask(std::move(list));
    active = true;
}

RoundedClip::~RoundedClip(){
    if (active)
        renderManager->SetState(saved);
}

// ---------------------------------------------------------------- caches, reports

std::shared_ptr<const std::vector<uint8_t>> decorators::rasterizeCached(const std::string& xml, int width, int height){
    const std::string key = toHex(fnv1a64(xml)) + ":" + std::to_string(xml.size()) + ":" + std::to_string(width) + "x" + std::to_string(height);
    RasterCache& cache = rasterCache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it){
            if (it->first == key){
                cache.entries.splice(cache.entries.begin(), cache.entries, it);
                return cache.entries.front().second;
            }
        }
    }
    auto pixels = std::make_shared<const std::vector<uint8_t>>(htmlui::rasterizeSvg(xml, width, height));
    if (pixels->empty())
        return nullptr;
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.entries.emplace_front(key, pixels);
    cache.bytes += pixels->size();
    while (cache.bytes > kRasterCacheBytes && cache.entries.size() > 1){
        cache.bytes -= cache.entries.back().second->size();
        cache.entries.pop_back();
    }
    return pixels;
}

size_t decorators::rasterCacheSize(){
    std::lock_guard<std::mutex> lock(rasterCache().mutex);
    return rasterCache().entries.size();
}

void decorators::reportOnce(Rml::Element* element, DiagCode code, const std::string& message, const std::string& suggestion){
    Diagnostic diagnostic;
    diagnostic.code = code;
    const DiagCodeInfo* info = findDiagnosticCode(code);
    diagnostic.severity = info ? info->severity : Severity::Warning;
    diagnostic.message = message;
    diagnostic.suggestion = suggestion;
    diagnostic.file = documentPath(element);
    {
        std::lock_guard<std::mutex> lock(reportMutex());
        if (!reported().insert(diagnostic.file + "\n" + code.id() + "\n" + message).second)
            return;
    }
    const std::string line = "[HTML UI] " + toText(diagnostic);
    if (diagnostic.severity == Severity::Error)
        Log::error("%s", line.c_str());
    else if (diagnostic.severity == Severity::Warning)
        Log::warn("%s", line.c_str());
    else
        Log::verbose("%s", line.c_str());
}
