// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/SvgPreprocess.h"

#include "htmlui/compat/Arena.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"

#include "Log.h"
#include "util/LunaSVGRuntime.h"

#include "tinyxml2.h"
#include <lunasvg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    // the root color attribute of a Substitute result, replaced by withColor
    constexpr const char* kColorPlaceholder = "#htmlui-current-color";
    constexpr const char* kWhite = "#ffffff";

    const char* const kPaintProperties[] = {"fill", "stroke", "stop-color", "flood-color", "lighting-color"};

    // shapes whose fill defaults to black when nothing sets one
    const char* const kFilledShapes[] = {"path", "rect", "circle", "ellipse", "polygon", "polyline", "text"};

    // subtrees that are not drawn where they stand (referenced, or clip/mask geometry)
    const char* const kNotRendered[] = {"defs", "symbol", "clipPath", "mask", "marker", "pattern", "linearGradient",
                                         "radialGradient", "filter"};

    std::string lower(std::string_view text){
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return out;
    }

    std::string trim(std::string_view text){
        const size_t first = text.find_first_not_of(" \t\r\n\f");
        if (first == std::string_view::npos)
            return std::string();
        const size_t last = text.find_last_not_of(" \t\r\n\f");
        return std::string(text.substr(first, last - first + 1));
    }

    bool isPaintProperty(std::string_view name){
        for (const char* property : kPaintProperties){
            if (name == property)
                return true;
        }
        return false;
    }

    bool inList(const char* name, const char* const* list, size_t count){
        for (size_t i = 0; i < count; i++){
            if (std::strcmp(name, list[i]) == 0)
                return true;
        }
        return false;
    }

    std::string localName(const char* name){
        const char* colon = std::strchr(name, ':');
        return colon ? std::string(colon + 1) : std::string(name);
    }

    // replaces every currentColor spelling (case-insensitive) with `replacement`
    std::string replaceCurrentColor(std::string_view text, std::string_view replacement){
        static const char* needle = "currentcolor";
        const size_t length = std::strlen(needle);
        std::string out;
        out.reserve(text.size());
        size_t i = 0;
        while (i < text.size()){
            if (i + length <= text.size()){
                bool match = true;
                for (size_t k = 0; k < length; k++){
                    if (std::tolower((unsigned char)text[i + k]) != needle[k]){
                        match = false;
                        break;
                    }
                }
                if (match){
                    out += replacement;
                    i += length;
                    continue;
                }
            }
            out += text[i++];
        }
        return out;
    }

    struct Scan {
        bool current = false;        // some paint is currentColor
        bool fixed = false;          // some paint is a fixed color (or SVG's default black)
        bool innerColor = false;     // an inner element (or a sheet) redefines `color`
        size_t foreignObjects = 0;
    };

    void classify(const std::string& value, Scan& scan){
        const std::string v = lower(trim(value));
        if (v == "currentcolor"){
            scan.current = true;
            return;
        }
        if (v.empty() || v == "none" || v == "transparent" || v == "inherit" || v == "context-fill" || v == "context-stroke")
            return;
        if (v.compare(0, 4, "url(") == 0){
            // a paint server; its fallback (after the url) is a paint of its own
            const size_t close = v.find(')');
            if (close != std::string::npos)
                classify(v.substr(close + 1), scan);
            return;
        }
        scan.fixed = true;
    }

    struct Declaration {
        std::string name;
        std::string value;
    };

    std::vector<Declaration> parseStyle(const char* style){
        std::vector<Declaration> declarations;
        if (!style)
            return declarations;
        std::string_view text(style);
        size_t start = 0;
        while (start <= text.size()){
            size_t end = text.find(';', start);
            if (end == std::string_view::npos)
                end = text.size();
            const std::string_view part = text.substr(start, end - start);
            const size_t colon = part.find(':');
            if (colon != std::string_view::npos){
                const std::string name = lower(trim(part.substr(0, colon)));
                if (!name.empty())
                    declarations.push_back({name, trim(part.substr(colon + 1))});
            }
            start = end + 1;
        }
        return declarations;
    }

    std::string writeStyle(const std::vector<Declaration>& declarations){
        std::string out;
        for (const Declaration& declaration : declarations){
            if (!out.empty())
                out += "; ";
            out += declaration.name + ": " + declaration.value;
        }
        return out;
    }

    // paint declarations of a <style> sheet (comments removed, selectors ignored)
    void scanSheet(std::string_view sheet, Scan& scan){
        std::string text;
        for (size_t i = 0; i < sheet.size(); i++){
            if (sheet[i] == '/' && i + 1 < sheet.size() && sheet[i + 1] == '*'){
                const size_t end = sheet.find("*/", i + 2);
                if (end == std::string_view::npos)
                    break;
                i = end + 1;
                continue;
            }
            text += sheet[i];
        }
        size_t start = 0;
        while (start < text.size()){
            const size_t end = text.find_first_of("{};", start);
            const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const size_t colon = part.find(':');
            if (colon != std::string::npos){
                const std::string name = lower(trim(part.substr(0, colon)));
                if (isPaintProperty(name))
                    classify(part.substr(colon + 1), scan);
                else if (name == "color")
                    scan.innerColor = true;
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }

    void rewriteSvgDrawableStroke(tinyxml2::XMLElement& element){
        const char* draw = element.Attribute("draw");
        const char* dashes = element.Attribute("stroke-dasharray");
        const char* offset = element.Attribute("stroke-dashoffset");
        if (!draw || !dashes || !offset) return;
        std::istringstream range(draw);
        double start = 0, end = 0;
        if (!(range >> start >> end) || !std::isfinite(start) || !std::isfinite(end)) return;
        const bool collapsed = start == end;
        const char* authored = element.Attribute("style");
        if (!authored || !*authored) return;
        const std::string text(authored);
        Arena arena;
        ArenaScope scope(arena);
        const auto parsed = parseBlockContents(0, text, 0, nullptr);
        std::string style, cap;
        bool capSeen = false, capImportant = false;
        size_t cursor = 0;
        for (const auto& declaration : parsed.declarations){
            const bool linecap = declaration.name == "stroke-linecap";
            if (!linecap && declaration.name != "stroke-dasharray" && declaration.name != "stroke-dashoffset") continue;
            if (linecap && !collapsed && (!capSeen || declaration.important || !capImportant)){
                cap = serialize(declaration.value);
                capSeen = true;
                capImportant = declaration.important;
            }
            const size_t begin = std::min((size_t)declaration.ref.offset, text.size());
            size_t finish = std::min(begin + (size_t)declaration.ref.length, text.size());
            while (finish < text.size() && std::isspace((unsigned char)text[finish])) ++finish;
            if (finish < text.size() && text[finish] == ';') ++finish;
            if (begin >= cursor){
                style.append(text, cursor, begin - cursor);
                cursor = finish;
            }
        }
        style.append(text, cursor, std::string::npos);
        // Draw controls the rendered stroke even when an exported inline declaration wins
        // over its presentation attributes. Rewrite only the rasterizer's temporary copy:
        // authored styles and concurrent Rml property animations stay intact, with no growth.
        std::string stroke = "stroke-dasharray:" + std::string(dashes) + ";stroke-dashoffset:" + offset + ";";
        if (collapsed) stroke += "stroke-linecap:butt;";
        else if (capSeen) stroke += "stroke-linecap:" + cap + ";";
        // LunaSVG stops its inline parser at a quoted semicolon in an unrelated custom
        // property. Put these declarations first so those preserved values cannot hide them.
        style.insert(0, stroke);
        element.SetAttribute("style", style.c_str());
    }

    void walk(tinyxml2::XMLElement* element, bool fillSet, bool rendered, bool root, Scan& scan){
        if (element->Attribute("data-anime-drawable")) rewriteSvgDrawableStroke(*element);
        // paints set on this element: attributes, then style="" (which wins)
        const std::vector<Declaration> style = parseStyle(element->Attribute("style"));
        bool hasFill = false;
        bool hasStopColor = false;
        for (const char* property : kPaintProperties){
            const char* attribute = element->Attribute(property);
            std::string value = attribute ? attribute : "";
            bool set = attribute != nullptr;
            for (const Declaration& declaration : style){
                if (declaration.name == property){
                    value = declaration.value;
                    set = true;
                }
            }
            if (!set)
                continue;
            classify(value, scan);
            if (std::strcmp(property, "fill") == 0)
                hasFill = true;
            if (std::strcmp(property, "stop-color") == 0)
                hasStopColor = true;
        }
        if (!root){
            bool hasColor = element->Attribute("color") != nullptr;
            for (const Declaration& declaration : style)
                hasColor = hasColor || declaration.name == "color";
            if (hasColor)
                scan.innerColor = true;
        }

        const std::string name = localName(element->Name());
        fillSet = fillSet || hasFill;
        if (rendered && !fillSet && inList(name.c_str(), kFilledShapes, sizeof(kFilledShapes) / sizeof(kFilledShapes[0])))
            scan.fixed = true;   // SVG's initial fill is black
        if (name == "stop" && !hasStopColor)
            scan.fixed = true;   // and a stop's initial color too
        if (name == "style" && element->GetText())
            scanSheet(element->GetText(), scan);
        const bool childrenRendered = rendered && !inList(name.c_str(), kNotRendered, sizeof(kNotRendered) / sizeof(kNotRendered[0]));

        tinyxml2::XMLElement* child = element->FirstChildElement();
        while (child){
            tinyxml2::XMLElement* next = child->NextSiblingElement();
            if (localName(child->Name()) == "foreignObject"){
                element->DeleteChild(child);
                scan.foreignObjects++;
            }else{
                walk(child, fillSet, childrenRendered, false, scan);
            }
            child = next;
        }
    }

    // every paint value (attributes, style="", <style> text) with its currentColor replaced
    void rewritePaints(tinyxml2::XMLElement* element, std::string_view replacement){
        for (const char* property : kPaintProperties){
            if (const char* value = element->Attribute(property))
                element->SetAttribute(property, replaceCurrentColor(value, replacement).c_str());
        }
        if (const char* value = element->Attribute("color"))
            element->SetAttribute("color", replaceCurrentColor(value, replacement).c_str());
        if (const char* styleText = element->Attribute("style")){
            std::vector<Declaration> style = parseStyle(styleText);
            bool changed = false;
            for (Declaration& declaration : style){
                if (isPaintProperty(declaration.name) || declaration.name == "color"){
                    const std::string replaced = replaceCurrentColor(declaration.value, replacement);
                    changed = changed || replaced != declaration.value;
                    declaration.value = replaced;
                }
            }
            if (changed)
                element->SetAttribute("style", writeStyle(style).c_str());
        }
        if (localName(element->Name()) == "style"){
            for (tinyxml2::XMLNode* node = element->FirstChild(); node; node = node->NextSibling()){
                if (tinyxml2::XMLText* text = node->ToText())
                    text->SetValue(replaceCurrentColor(text->Value(), replacement).c_str());
            }
        }
        for (tinyxml2::XMLElement* child = element->FirstChildElement(); child; child = child->NextSiblingElement())
            rewritePaints(child, replacement);
    }

    const char* useName(SvgUse use){
        switch (use){
            case SvgUse::Inline: return "inline <svg>";
            case SvgUse::File: return "<svg src>";
            case SvgUse::Layer: return "background image";
        }
        return "SVG";
    }

    Diagnostic makeDiagnostic(DiagCode code, std::string message, std::string suggestion){
        Diagnostic diagnostic;
        diagnostic.code = code;
        const DiagCodeInfo* info = findDiagnosticCode(code);
        diagnostic.severity = info ? info->severity : Severity::Warning;
        diagnostic.message = std::move(message);
        diagnostic.suggestion = std::move(suggestion);
        diagnostic.subject = "svg";
        return diagnostic;
    }

}

bool SvgPaintOverrides::empty() const{
    return fill.empty() && stroke.empty() && strokeWidth.empty() && strokeOpacity.empty() && fillOpacity.empty() &&
           strokeLinecap.empty() && strokeLinejoin.empty() && strokeDasharray.empty();
}

std::string SvgPaintOverrides::key() const{
    if (empty())
        return std::string();
    return fill + "|" + stroke + "|" + strokeWidth + "|" + strokeOpacity + "|" + fillOpacity + "|" + strokeLinecap + "|" +
           strokeLinejoin + "|" + strokeDasharray;
}

std::string SvgResult::withColor(std::string_view cssColor) const{
    if (tint != SvgTint::Substitute)
        return xml;
    std::string out = xml;
    const std::string placeholder = kColorPlaceholder;
    const size_t at = out.find(placeholder);
    if (at != std::string::npos)
        out.replace(at, placeholder.size(), cssColor);
    return out;
}

SvgResult SvgPreprocess::process(std::string_view xml, SvgUse use, const SvgPaintOverrides& overrides){
    SvgResult result;
    result.xml = std::string(xml);

    tinyxml2::XMLDocument document(true, tinyxml2::PRESERVE_WHITESPACE);
    if (document.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS){
        result.error = std::string("not well-formed XML: ") + (document.ErrorStr() ? document.ErrorStr() : "parse error");
        return result;
    }
    tinyxml2::XMLElement* root = document.RootElement();
    if (!root || localName(root->Name()) != "svg"){
        result.error = "the root element is not <svg>";
        return result;
    }

    // CSS on the <svg> element beats its presentation attributes and is inherited by the
    // shapes unless they set their own: exactly a root attribute (its style="" still wins)
    const std::pair<const char*, const std::string*> paints[] = {
        {"fill", &overrides.fill}, {"stroke", &overrides.stroke}, {"stroke-width", &overrides.strokeWidth},
        {"stroke-opacity", &overrides.strokeOpacity}, {"fill-opacity", &overrides.fillOpacity},
        {"stroke-linecap", &overrides.strokeLinecap}, {"stroke-linejoin", &overrides.strokeLinejoin},
        {"stroke-dasharray", &overrides.strokeDasharray}};
    for (const auto& paint : paints){
        if (!paint.second->empty())
            root->SetAttribute(paint.first, paint.second->c_str());
    }

    Scan scan;
    walk(root, false, true, true, scan);
    result.foreignObjects = scan.foreignObjects;
    if (scan.foreignObjects > 0){
        result.diagnostics.push_back(makeDiagnostic(codes::SvgUnsupportedContent,
            "<foreignObject> in an " + std::string(useName(use)) + " is dropped: the SVG renderer cannot draw HTML inside SVG",
            "draw the content with HTML elements over the SVG"));
    }

    if (scan.current && !scan.fixed && !scan.innerColor){
        result.tint = SvgTint::Color;
        rewritePaints(root, kWhite);
    }else if (scan.current){
        result.tint = SvgTint::Substitute;
        rewritePaints(root, "currentColor");
        root->SetAttribute("color", kColorPlaceholder);
        result.diagnostics.push_back(makeDiagnostic(codes::SvgRecolor,
            "the " + std::string(useName(use)) + " mixes currentColor with fixed colors; it is rasterized again whenever the CSS color changes",
            "use currentColor for every paint (or none) to recolor it without rasterizing"));
    }

    tinyxml2::XMLPrinter printer(nullptr, true);
    document.Print(&printer);
    result.xml.assign(printer.CStr(), printer.CStrSize() > 0 ? (size_t)printer.CStrSize() - 1 : 0);
    result.ok = true;
    return result;
}

std::string SvgPreprocess::colorText(uint8_t r, uint8_t g, uint8_t b, uint8_t a){
    static const char* hex = "0123456789abcdef";
    std::string out = "#";
    for (uint8_t channel : {r, g, b, a}){
        out += hex[channel >> 4];
        out += hex[channel & 0xF];
    }
    return out;
}

std::vector<uint8_t> jasperengine::htmlui::rasterizeSvg(std::string_view xml, int width, int height){
    return rasterizeSvg(xml, width, height, 0.0f, 0.0f);
}

std::vector<uint8_t> jasperengine::htmlui::rasterizeSvg(std::string_view xml, int width, int height, float logicalWidth, float logicalHeight){
    std::vector<uint8_t> rgba;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384){
        Log::error("[HTML UI] SVG rasterization: %i x %i is outside 1..16384", width, height);
        return rgba;
    }
    LunaSVGRuntime::ensureFonts();
    std::unique_ptr<lunasvg::Document> document = lunasvg::Document::loadFromData(xml.data(), xml.size());
    if (!document){
        Log::error("[HTML UI] SVG rasterization: the SVG could not be parsed");
        return rgba;
    }
    // Drawn at that size as browsers draw an SVG image: the root's width and height become the
    // target size, so its viewBox and preserveAspectRatio place the content (an SVG without a
    // viewBox gets the one CSS images synthesize from its intrinsic size)
    const float intrinsicWidth = document->width();
    const float intrinsicHeight = document->height();
    lunasvg::Element root = document->documentElement();
    if (!root.hasAttribute("viewBox") && intrinsicWidth > 0.0f && intrinsicHeight > 0.0f)
        root.setAttribute("viewBox", "0 0 " + compat::formatNumber(intrinsicWidth) + " " + compat::formatNumber(intrinsicHeight));
    const bool logical = logicalWidth > 0.0f && logicalHeight > 0.0f && (logicalWidth != (float)width || logicalHeight != (float)height);
    root.setAttribute("width", logical ? compat::formatNumber(logicalWidth) : std::to_string(width));
    root.setAttribute("height", logical ? compat::formatNumber(logicalHeight) : std::to_string(height));
    document->forceLayout();
    lunasvg::Bitmap bitmap;
    if (logical){
        bitmap = lunasvg::Bitmap(width, height);
        if (bitmap.valid()){
            bitmap.clear(0x00000000);
            document->render(bitmap, lunasvg::Matrix::scaled((float)width / logicalWidth, (float)height / logicalHeight));
        }
    }else{
        bitmap = document->renderToBitmap(width, height);
    }
    if (!bitmap.valid() || !bitmap.data() || bitmap.width() != width || bitmap.height() != height){
        Log::error("[HTML UI] SVG rasterization: the SVG could not be rendered at %i x %i", width, height);
        return rgba;
    }
    // LunaSVG gives premultiplied ARGB32 (B, G, R, A bytes on little endian)
    const size_t rowBytes = (size_t)width * 4;
    rgba.resize(rowBytes * (size_t)height);
    for (int y = 0; y < height; y++){
        const uint8_t* row = bitmap.data() + (size_t)y * (size_t)bitmap.stride();
        uint8_t* out = rgba.data() + (size_t)y * rowBytes;
        for (size_t x = 0; x < rowBytes; x += 4){
            out[x + 0] = row[x + 2];
            out[x + 1] = row[x + 1];
            out[x + 2] = row[x + 0];
            out[x + 3] = row[x + 3];
        }
    }
    return rgba;
}

bool jasperengine::htmlui::svgIntrinsicSize(std::string_view xml, float& width, float& height){
    width = height = 0.0f;
    LunaSVGRuntime::ensureFonts();
    std::unique_ptr<lunasvg::Document> document = lunasvg::Document::loadFromData(xml.data(), xml.size());
    if (!document)
        return false;
    width = document->width();
    height = document->height();
    return width > 0.0f && height > 0.0f;
}
