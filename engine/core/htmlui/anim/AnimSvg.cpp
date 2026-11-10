// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Anime.js 4.5.0 SVG modules (MIT): drawable.js, morphto.js and motionpath.js. The
// geometry API a browser provides is backed by the engine's existing SVG path parser.

#include "AnimSvg.h"

#include "AnimCommon.h"
#include "AnimSvgPath.h"
#include "AnimTargets.h"
#include "Log.h"
#include "object/ui/UIElementInternal.h"

#include "../../../libs/lunasvg/plutovg/include/plutovg.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <sstream>

using namespace jasperengine;

namespace {

    constexpr double motionPi = 3.14159265358979323846;

    std::string svgTag(const UIElement& element){
        const Rml::Element* native = UIElementAccess::get(element);
        return native ? native->GetTagName() : std::string();
    }

    double svgLengthInUserUnits(const std::string& text, double reference, double fontSize){
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || !std::isfinite(value)) return std::nan("");
        while (std::isspace((unsigned char)*end)) ++end;
        std::string unit(end);
        while (!unit.empty() && std::isspace((unsigned char)unit.back())) unit.pop_back();
        if (unit == "%") return value * reference / 100;
        if (unit == "em") return value * fontSize;
        if (unit == "ex") return value * fontSize / 2;
        if (unit == "in") return value * 96;
        if (unit == "cm") return value * 96 / 2.54;
        if (unit == "mm") return value * 96 / 25.4;
        if (unit == "pt") return value * 96 / 72;
        if (unit == "pc") return value * 16;
        return unit.empty() || unit == "px" ? value : std::nan("");
    }

    double svgGeometryFontSize(Rml::Element* element){
        std::vector<Rml::Element*> lineage;
        size_t root = 0;
        for (Rml::Element* current = element; current; current = current->GetParentNode()){
            lineage.push_back(current);
            if (current->GetTagName() == "svg") root = lineage.size() - 1;
        }
        // The rasterizer's SVG initial font size and ex factor (LunaSVG SVGLayoutState and
        // LengthContext); SVG presentation attributes and inline CSS inherit down the tree.
        double size = 12;
        for (size_t i = root + 1; i-- > 0;){
            Rml::Element* current = lineage[i];
            std::string value = current->GetAttribute<Rml::String>("font-size", "");
            std::istringstream styles(current->GetAttribute<Rml::String>("style", ""));
            std::string declaration;
            while (std::getline(styles, declaration, ';')){
                const size_t colon = declaration.find(':');
                if (colon == std::string::npos) continue;
                std::string name = declaration.substr(0, colon);
                name.erase(0, name.find_first_not_of(" \t\r\n"));
                const size_t last = name.find_last_not_of(" \t\r\n");
                if (last != std::string::npos) name.resize(last + 1);
                if (name == "font-size") value = declaration.substr(colon + 1);
            }
            const auto& local = current->GetLocalStyleProperties();
            auto property = local.find(Rml::PropertyId::FontSize);
            if (property != local.end()) value = property->second.ToString();
            if (!value.empty()){
                const double resolved = svgLengthInUserUnits(value, size, size);
                size = std::isfinite(resolved) && resolved >= 0 ? resolved : 12;
            }
        }
        return size;
    }

    AnimSvgPath svgGeometry(const UIElement& element){
        if (!element.isValid()) return AnimSvgPath();
        const std::string tag = svgTag(element);
        auto attributes = element.getAttributes();
        if (tag == "path" || tag == "polygon" || tag == "polyline")
            return AnimSvgPath::fromAttributes(tag, attributes);
        Rml::Element* native = UIElementAccess::get(element);
        Rml::Element* viewport = native;
        while (viewport && viewport->GetTagName() != "svg") viewport = viewport->GetParentNode();
        double width = 0, height = 0;
        if (viewport){
            std::string viewBox = viewport->GetAttribute<Rml::String>("viewBox", "");
            if (viewBox.empty()) viewBox = viewport->GetAttribute<Rml::String>("viewbox", "");
            std::replace(viewBox.begin(), viewBox.end(), ',', ' ');
            std::istringstream values(viewBox);
            double x = 0, y = 0;
            if (!(values >> x >> y >> width >> height) || width <= 0 || height <= 0){
                const auto size = viewport->GetBox().GetSize(Rml::BoxArea::Content);
                width = size.x;
                height = size.y;
            }
        }
        const double font = svgGeometryFontSize(native);
        const char* horizontal[] = {"x", "x1", "x2", "cx", "rx", "width"};
        const char* vertical[] = {"y", "y1", "y2", "cy", "ry", "height"};
        const auto resolve = [&](const char* name, double reference){
            auto found = attributes.find(name);
            if (found != attributes.end()){
                const double resolved = svgLengthInUserUnits(found->second, reference, font);
                found->second = AnimMath::toString(std::isfinite(resolved) ? resolved : 0);
            }
        };
        for (const char* name : horizontal) resolve(name, width);
        for (const char* name : vertical) resolve(name, height);
        resolve("r", std::hypot(width, height) / std::sqrt(2.0));
        return AnimSvgPath::fromAttributes(tag, attributes);
    }

    bool svgMorphTag(const std::string& tag){ return tag == "path" || tag == "polygon" || tag == "polyline"; }

    std::string svgFloat(double value){ return AnimMath::toString(value); }

    UIElement firstSvgElement(const AnimTargetList& targets){
        return targets.empty() ? UIElement() : AnimTargets::getElement(targets.front());
    }

    bool insideSvg(Rml::Element* element){
        for (; element; element = element->GetParentNode())
            if (element->GetTagName() == "svg") return true;
        return false;
    }

    // getCTM maps user units to the SVG viewport, independent of its position on the page.
    plutovg_matrix_t svgViewportMatrix(const UIElement& element){
        plutovg_matrix_t matrix;
        plutovg_matrix_init_identity(&matrix);
        Rml::Element* current = UIElementAccess::get(element);
        while (current && current->GetTagName() != "svg"){
            const std::string transform = current->GetAttribute<Rml::String>("transform", "");
            plutovg_matrix_t local;
            if (plutovg_matrix_parse(&local, transform.c_str(), (int)transform.size())){
                plutovg_matrix_t combined;
                plutovg_matrix_multiply(&combined, &matrix, &local);
                matrix = combined;
            }
            current = current->GetParentNode();
        }
        if (!current) return matrix;
        std::string viewBox = current->GetAttribute<Rml::String>("viewBox", "");
        if (viewBox.empty()) viewBox = current->GetAttribute<Rml::String>("viewbox", "");
        std::replace(viewBox.begin(), viewBox.end(), ',', ' ');
        std::istringstream values(viewBox);
        double x = 0, y = 0, width = 0, height = 0;
        if (!(values >> x >> y >> width >> height) || width <= 0 || height <= 0) return matrix;
        const auto size = current->GetBox().GetSize(Rml::BoxArea::Content);
        if (size.x <= 0 || size.y <= 0) return matrix;
        double sx = size.x / width, sy = size.y / height;
        double tx = -x * sx, ty = -y * sy;
        const std::string aspect = current->GetAttribute<Rml::String>("preserveAspectRatio", "xMidYMid meet");
        if (aspect.find("none") == std::string::npos){
            const double scale = aspect.find("slice") != std::string::npos ? std::max(sx, sy) : std::min(sx, sy);
            const double ax = aspect.find("xMin") != std::string::npos ? 0 : aspect.find("xMax") != std::string::npos ? 1 : 0.5;
            const double ay = aspect.find("YMin") != std::string::npos ? 0 : aspect.find("YMax") != std::string::npos ? 1 : 0.5;
            tx = -x * scale + ax * (size.x - width * scale);
            ty = -y * scale + ay * (size.y - height * scale);
            sx = sy = scale;
        }
        const plutovg_matrix_t viewport{(float)sx, 0, 0, (float)sy, (float)tx, (float)ty};
        plutovg_matrix_t combined;
        plutovg_matrix_multiply(&combined, &matrix, &viewport);
        return combined;
    }

}

AnimTargetList AnimSvg::createDrawable(const AnimTargetList& targets, double start, double end){
    AnimTargetList result;
    if (!std::isfinite(start) || !std::isfinite(end)) return result;
    for (const auto& target : targets){
        UIElement element = AnimTargets::getElement(target);
        if (!svgGeometry(element).isValid()) continue;
        element.setAttribute("data-anime-drawable", "");
        if (!element.hasAttribute("data-anime-linecap"))
            element.setAttribute("data-anime-linecap", element.getAttribute("stroke-linecap"));
        element.setAttribute("pathLength", "1000");
        setDraw(element, svgFloat(start) + " " + svgFloat(end));
        result.push_back(target);
    }
    return result;
}

void AnimSvg::setDraw(const UIElement& original, const std::string& value){
    UIElement element = original;
    std::istringstream range(value);
    double start = 0, end = 0;
    if (!(range >> start >> end) || !std::isfinite(start) || !std::isfinite(end)) return;
    const AnimSvgPath geometry = svgGeometry(element);
    if (!geometry.isValid()) return;
    const double length = geometry.length();
    const double offset = -start * length;
    const double dash = end * length + offset;
    const bool whole = (start == 0 && end == 1) || (start == 1 && end == 0);
    // LunaSVG does not apply SVG pathLength normalization, so its dash values are written in
    // user units. The public draw range and the linecap behavior match Anime's proxy.
    element.setAttribute("stroke-dashoffset", svgFloat(offset));
    element.setAttribute("stroke-dasharray", svgFloat(dash) + " " + svgFloat(length * (whole ? 1 : 1.01) - dash));
    const std::string cap = element.getAttribute("data-anime-linecap");
    if (start == end) element.setAttribute("stroke-linecap", "butt");
    else if (cap.empty()) element.removeAttribute("stroke-linecap");
    else element.setAttribute("stroke-linecap", cap);
    if (Rml::Element* native = UIElementAccess::get(element)) native->SetAttribute("draw", value);
}

AnimParam AnimSvg::morphTo(const AnimTargetList& targets, double precision){
    UIElement destination = firstSvgElement(targets);
    if (!destination.isValid() || !svgMorphTag(svgTag(destination)) || !std::isfinite(precision) || precision < 0){
        Log::warn("[HTML UI] anime.morphTo: target must be a path, polygon or polyline and precision must be finite and non-negative");
        return AnimParam();
    }
    return AnimParam::function([destination, precision](const AnimFunctionArgs& args){
        UIElement source = AnimTargets::getElement(args.target);
        if (!source.isValid() || !destination.isValid() || !svgMorphTag(svgTag(source))) return AnimParam();
        const bool path = svgTag(source) == "path";
        const char* attribute = path ? "d" : "points";
        if (args.previousValue.isTruthy()) source.setAttribute(attribute, args.previousValue.toString());
        if (precision == 0)
            return AnimParam::array({source.getAttribute(attribute), destination.getAttribute(attribute)});
        const AnimSvgPath a = svgGeometry(source), b = svgGeometry(destination);
        if (!a.isValid() || !b.isValid()) return AnimParam();
        const double requested = std::max(std::ceil(a.length() * precision), std::ceil(b.length() * precision));
        // At least two endpoints for degenerate/tiny paths; bound allocations for hostile input.
        const size_t points = (size_t)std::clamp(requested, 2.0, 100000.0);
        std::string from, to;
        for (size_t i = 0; i < points; ++i){
            const double progress = (double)i / (points - 1);
            const auto first = a.pointAtLength(a.length() * progress), second = b.pointAtLength(b.length() * progress);
            const std::string prefix = path ? (i == 0 ? "M" : "L") : "";
            const char* separator = path ? " " : ",";
            from += prefix + svgFloat(AnimMath::round(first.x, 3)) + separator + svgFloat(first.y) + " ";
            to += prefix + svgFloat(AnimMath::round(second.x, 3)) + separator + svgFloat(second.y) + " ";
        }
        return AnimParam::array({from, to});
    });
}

AnimParam AnimSvg::createMotionPath(const AnimTargetList& targets, double offset){
    UIElement path = firstSvgElement(targets);
    if (!svgGeometry(path).isValid() || !std::isfinite(offset)) return AnimParam();
    AnimParam result = AnimParam::object();
    const char* properties[] = {"translateX", "translateY", "rotate"};
    for (int component = 0; component < 3; ++component){
        result.set(properties[component], AnimParam::function([path, offset, component](const AnimFunctionArgs& args){
            const double length = svgGeometry(path).length();
            const bool inSvg = insideSvg(UIElementAccess::get(AnimTargets::getElement(args.target)));
            const plutovg_matrix_t matrix = svgViewportMatrix(path);
            return AnimParam::object({{"from", 0}, {"to", length}, {"modifier", AnimParam::ease([path, length, offset, component, inSvg, matrix](double distance){
                if (!path.isValid()) return 0.0;
                const AnimSvgPath geometry = svgGeometry(path);
                auto point = [&](double lookup){
                    double value = distance + offset * length + lookup;
                    if (offset == 0) value = std::clamp(value, 0.0, length);
                    else if (length > 0) value = std::fmod(std::fmod(value, length) + length, length);
                    return geometry.pointAtLength(value);
                };
                if (component == 2){
                    const auto a = point(-1), b = point(1);
                    return std::atan2(b.y - a.y, b.x - a.x) * 180 / motionPi;
                }
                const auto p = point(0);
                if (inSvg) return component == 0 ? p.x : p.y;
                return component == 0 ? p.x * matrix.a + p.y * matrix.c + matrix.e : p.x * matrix.b + p.y * matrix.d + matrix.f;
            })}});
        }));
    }
    return result;
}
