// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/SvgElementHook.h"

#include "htmlui/compat/Hash.h"
#include "htmlui/compat/runtime/CompatProperties.h"

#include "Log.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/PropertyIdSet.h>

#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    // results by (content, overrides); cleared whole when it grows past the limit
    constexpr size_t kCacheLimit = 512;

    struct Cache {
        std::mutex mutex;
        std::unordered_map<std::string, std::shared_ptr<const SvgResult>> results;
        std::set<std::pair<std::string, std::string>> logged;   // (file, code): logged once
        size_t processed = 0;
    };

    Cache& cache(){
        static Cache value;
        return value;
    }

    std::string paintText(Rml::Element& element, CompatProperty property){
        std::string text = CompatProperties::text(element, property);
        if (text.empty())
            return text;
        Rml::Colourb colour;
        if (CompatProperties::color(element, property, colour))
            return SvgPreprocess::colorText(colour.red, colour.green, colour.blue, colour.alpha);
        // keywords: LunaSVG spells it currentColor
        return text == "currentcolor" ? std::string("currentColor") : text;
    }

    // runtime findings of <svg src> files, once per file (inline SVG is diagnosed at compile)
    void logOnce(const std::string& path, const std::vector<Diagnostic>& diagnostics){
        Cache& c = cache();
        for (Diagnostic diagnostic : diagnostics){
            {
                std::lock_guard<std::mutex> lock(c.mutex);
                if (!c.logged.insert({path, diagnostic.code.id()}).second)
                    continue;
            }
            diagnostic.file = path;
            const std::string line = "[HTML UI] " + toText(diagnostic);
            if (diagnostic.severity == Severity::Error)
                Log::error("%s", line.c_str());
            else if (diagnostic.severity == Severity::Warning)
                Log::warn("%s", line.c_str());
            else
                Log::verbose("%s", line.c_str());
        }
    }

    // The wrapper ElementSVG builds for inline SVG copies the element's attributes onto the
    // root: the per-element ones (rmlui-svgdata-id, data-htmlui-loc, data-*, aria-*, role)
    // never change the drawing, so they are dropped and equal icons share one cache entry.
    // ElementSVG writes `name="value" ` pairs with the values RML-encoded (no raw quotes).
    std::string stripHostAttributes(const std::string& source){
        const size_t open = source.find("<svg");
        const size_t close = source.find('>', open == std::string::npos ? 0 : open);
        if (open != 0 || close == std::string::npos)
            return source;
        std::string out = "<svg";
        size_t i = 4;
        while (i < close){
            while (i < close && (source[i] == ' ' || source[i] == '\t' || source[i] == '\n' || source[i] == '\r'))
                i++;
            const size_t nameStart = i;
            while (i < close && source[i] != '=' && source[i] != ' ' && source[i] != '/')
                i++;
            const std::string name = source.substr(nameStart, i - nameStart);
            size_t end = i;
            if (i < close && source[i] == '=' && i + 1 < close && (source[i + 1] == '"' || source[i + 1] == '\'')){
                const size_t quote = source.find(source[i + 1], i + 2);
                if (quote == std::string::npos || quote > close)
                    return source;
                end = quote + 1;
            }
            if (name.empty()){
                if (end == i)
                    i++;   // a stray character ('/')
                else
                    i = end;
                continue;
            }
            const bool host = name.compare(0, 6, "rmlui-") == 0 || name.compare(0, 5, "data-") == 0 ||
                              name.compare(0, 5, "aria-") == 0 || name == "role";
            if (!host)
                out += " " + source.substr(nameStart, end - nameStart);
            i = end;
        }
        out += source.substr(close);
        return out;
    }

}

SvgElementHook& SvgElementHook::instance(){
    static SvgElementHook hook;
    return hook;
}

void SvgElementHook::install(){
    Rml::SVG::SetSourceHook(&instance());
}

void SvgElementHook::uninstall(){
    if (Rml::SVG::GetSourceHook() == &instance())
        Rml::SVG::SetSourceHook(nullptr);
    clearCache();
}

SvgPaintOverrides SvgElementHook::overridesOf(Rml::Element& element){
    SvgPaintOverrides overrides;
    if (!CompatProperties::isRegistered())
        return overrides;
    overrides.fill = paintText(element, CompatProperty::Fill);
    overrides.stroke = paintText(element, CompatProperty::Stroke);
    overrides.strokeWidth = CompatProperties::text(element, CompatProperty::StrokeWidth);
    overrides.strokeOpacity = CompatProperties::text(element, CompatProperty::StrokeOpacity);
    overrides.fillOpacity = CompatProperties::text(element, CompatProperty::FillOpacity);
    overrides.strokeLinecap = CompatProperties::text(element, CompatProperty::StrokeLinecap);
    overrides.strokeLinejoin = CompatProperties::text(element, CompatProperty::StrokeLinejoin);
    overrides.strokeDasharray = CompatProperties::text(element, CompatProperty::StrokeDasharray);
    return overrides;
}

std::shared_ptr<const SvgResult> SvgElementHook::preprocess(const std::string& source, SvgUse use, const SvgPaintOverrides& overrides){
    const std::string key = toHex(fnv1a64(source)) + ":" + std::to_string(source.size()) + ":" + std::to_string((int)use) + ":" + overrides.key();
    Cache& c = cache();
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        const auto it = c.results.find(key);
        if (it != c.results.end())
            return it->second;
    }
    auto result = std::make_shared<const SvgResult>(SvgPreprocess::process(source, use, overrides));
    std::lock_guard<std::mutex> lock(c.mutex);
    c.processed++;
    if (c.results.size() >= kCacheLimit)
        c.results.clear();
    c.results[key] = result;
    return result;
}

bool SvgElementHook::ProcessSource(Rml::Element* element, const Rml::String& path, const Rml::String& source, Result& result){
    if (!element)
        return false;
    const SvgPaintOverrides overrides = overridesOf(*element);
    const std::shared_ptr<const SvgResult> svg = path.empty() ? preprocess(stripHostAttributes(source), SvgUse::Inline, overrides)
                                                              : preprocess(source, SvgUse::File, overrides);
    if (!svg->ok)
        return false;   // the plugin loads it as it is (and reports what LunaSVG thinks of it)
    if (!path.empty())
        logOnce(path, svg->diagnostics);

    // The plugin's cache shares one document (and its geometry and texture) per source id
    // across every element: the render manager is part of the id, so contexts (the editor's
    // views, texture-mode documents) never draw with another context's resources
    const std::string id = "htmlui-svg:" + toHex(fnv1a64(svg->xml)) + ":" + std::to_string(svg->xml.size()) + "@" +
                           toHex((uint64_t)reinterpret_cast<uintptr_t>(element->GetRenderManager()));
    const Rml::ComputedValues& computed = element->GetComputedValues();
    const Rml::Colourb color = computed.color();
    switch (svg->tint){
        case SvgTint::Color:{
            // white x CSS color x image-color, then opacity (premultiplied, as RmlUi tints)
            const Rml::Colourb image = computed.image_color();
            const Rml::Colourb tint((Rml::byte)((color.red * image.red + 127) / 255), (Rml::byte)((color.green * image.green + 127) / 255),
                                    (Rml::byte)((color.blue * image.blue + 127) / 255), (Rml::byte)((color.alpha * image.alpha + 127) / 255));
            result.source_id = id;
            result.data = svg->xml;
            result.use_colour = true;
            result.colour = tint.ToPremultiplied(computed.opacity());
            return true;
        }
        case SvgTint::Substitute:{
            const std::string css = SvgPreprocess::colorText(color.red, color.green, color.blue, color.alpha);
            result.source_id = id + ":" + css;
            result.data = svg->withColor(css);
            result.use_colour = false;
            return true;
        }
        case SvgTint::None:
            result.source_id = id;
            result.data = svg->xml;
            result.use_colour = false;
            return true;
    }
    return false;
}

bool SvgElementHook::IsSourceProperty(const Rml::PropertyIdSet& changed_properties){
    if (changed_properties.Contains(Rml::PropertyId::Color))
        return true;
    for (CompatProperty property : {CompatProperty::Fill, CompatProperty::Stroke, CompatProperty::StrokeWidth, CompatProperty::StrokeOpacity,
                                    CompatProperty::FillOpacity, CompatProperty::StrokeLinecap, CompatProperty::StrokeLinejoin,
                                    CompatProperty::StrokeDasharray}){
        const Rml::PropertyId id = CompatProperties::id(property);
        if (id != Rml::PropertyId::Invalid && changed_properties.Contains(id))
            return true;
    }
    return false;
}

size_t SvgElementHook::cacheSize(){
    std::lock_guard<std::mutex> lock(cache().mutex);
    return cache().results.size();
}

size_t SvgElementHook::processCount(){
    std::lock_guard<std::mutex> lock(cache().mutex);
    return cache().processed;
}

void SvgElementHook::clearCache(){
    std::lock_guard<std::mutex> lock(cache().mutex);
    cache().results.clear();
    cache().logged.clear();
}
