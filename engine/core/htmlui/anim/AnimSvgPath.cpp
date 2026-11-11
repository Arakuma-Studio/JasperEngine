// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimSvgPath.h"

#include "../../../libs/lunasvg/plutovg/include/plutovg.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <sstream>

using namespace jasperengine;

namespace {

    constexpr double svgPi = 3.14159265358979323846;

    double svgDistance(AnimSvgPoint a, AnimSvgPoint b){ return std::hypot(a.x - b.x, a.y - b.y); }
    AnimSvgPoint svgMidpoint(AnimSvgPoint a, AnimSvgPoint b){ return {(a.x + b.x) / 2, (a.y + b.y) / 2}; }

    std::string svgAttribute(const std::map<std::string, std::string>& attributes, const char* name){
        auto it = attributes.find(name);
        return it == attributes.end() ? std::string() : it->second;
    }

    double svgNumber(const std::map<std::string, std::string>& attributes, const char* name){
        const std::string text = svgAttribute(attributes, name);
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        return end != text.c_str() && std::isfinite(value) ? value : 0;
    }

}

void AnimSvgPath::moveTo(AnimSvgPoint point){ cursor = first = point; }

void AnimSvgPath::lineTo(AnimSvgPoint point){
    const double distance = svgDistance(cursor, point);
    if (distance > 0){
        totalLength += distance;
        segments.push_back({cursor, point, totalLength});
    }
    cursor = point;
}

void AnimSvgPath::cubicTo(AnimSvgPoint a, AnimSvgPoint b, AnimSvgPoint end, int depth){
    const double polygon = svgDistance(cursor, a) + svgDistance(a, b) + svgDistance(b, end);
    const double chord = svgDistance(cursor, end);
    // An absolute arclength error bound, rather than a fixed sample count, keeps small paths
    // and highly curved paths accurate. The depth cap also bounds hostile SVG input.
    if (depth == 18 || polygon - chord <= 0.00001){
        lineTo(end);
        return;
    }
    const AnimSvgPoint start = cursor;
    const AnimSvgPoint ab = svgMidpoint(start, a), bc = svgMidpoint(a, b), cd = svgMidpoint(b, end);
    const AnimSvgPoint abc = svgMidpoint(ab, bc), bcd = svgMidpoint(bc, cd), middle = svgMidpoint(abc, bcd);
    cubicTo(ab, abc, middle, depth + 1);
    cubicTo(bcd, cd, end, depth + 1);
}

AnimSvgPath AnimSvgPath::fromAttributes(const std::string& tag, const std::map<std::string, std::string>& attributes){
    AnimSvgPath result;
    std::unique_ptr<plutovg_path_t, decltype(&plutovg_path_destroy)> path(plutovg_path_create(), plutovg_path_destroy);
    if (tag == "path"){
        const std::string data = svgAttribute(attributes, "d");
        if (!plutovg_path_parse(path.get(), data.c_str(), (int)data.size()))
            return result;
    } else if (tag == "line"){
        plutovg_path_move_to(path.get(), (float)svgNumber(attributes, "x1"), (float)svgNumber(attributes, "y1"));
        plutovg_path_line_to(path.get(), (float)svgNumber(attributes, "x2"), (float)svgNumber(attributes, "y2"));
    } else if (tag == "circle"){
        result.circleX = svgNumber(attributes, "cx");
        result.circleY = svgNumber(attributes, "cy");
        result.circleRadius = std::max(0.0, svgNumber(attributes, "r"));
        result.totalLength = 2 * svgPi * result.circleRadius;
        result.valid = true;
        return result;
    } else if (tag == "ellipse"){
        const float rx = (float)std::max(0.0, svgNumber(attributes, "rx"));
        const float ry = (float)std::max(0.0, svgNumber(attributes, "ry"));
        if (rx == 0 || ry == 0){ result.valid = true; return result; }
        plutovg_path_add_ellipse(path.get(), (float)svgNumber(attributes, "cx"), (float)svgNumber(attributes, "cy"), rx, ry);
    } else if (tag == "rect"){
        const float x = (float)svgNumber(attributes, "x"), y = (float)svgNumber(attributes, "y");
        const float width = (float)std::max(0.0, svgNumber(attributes, "width"));
        const float height = (float)std::max(0.0, svgNumber(attributes, "height"));
        if (width == 0 || height == 0){ result.valid = true; return result; }
        const float rx = (float)std::max(0.0, attributes.count("rx") ? svgNumber(attributes, "rx") : svgNumber(attributes, "ry"));
        const float ry = (float)std::max(0.0, attributes.count("ry") ? svgNumber(attributes, "ry") : svgNumber(attributes, "rx"));
        plutovg_path_add_round_rect(path.get(), x, y, width, height, std::min(rx, width / 2), std::min(ry, height / 2));
    } else if (tag == "polyline" || tag == "polygon"){
        const std::string data = svgAttribute(attributes, "points");
        const char* cursor = data.c_str();
        bool first = true;
        while (*cursor){
            while (*cursor == ',' || std::isspace((unsigned char)*cursor)) ++cursor;
            if (!*cursor) break;
            char* end = nullptr;
            const double x = std::strtod(cursor, &end);
            if (end == cursor || !std::isfinite(x)) return result;
            cursor = end;
            while (*cursor == ',' || std::isspace((unsigned char)*cursor)) ++cursor;
            const double y = std::strtod(cursor, &end);
            if (end == cursor || !std::isfinite(y)) return result;
            cursor = end;
            if (first) plutovg_path_move_to(path.get(), (float)x, (float)y);
            else plutovg_path_line_to(path.get(), (float)x, (float)y);
            first = false;
        }
        if (tag == "polygon") plutovg_path_close(path.get());
    } else {
        return result;
    }
    plutovg_path_traverse(path.get(), [](void* closure, plutovg_path_command_t command, const plutovg_point_t* points, int){
        auto& output = *static_cast<AnimSvgPath*>(closure);
        const auto point = [&](int i) -> AnimSvgPoint { return {points[i].x, points[i].y}; };
        switch (command){
            case PLUTOVG_PATH_COMMAND_MOVE_TO: output.moveTo(point(0)); break;
            case PLUTOVG_PATH_COMMAND_LINE_TO: output.lineTo(point(0)); break;
            case PLUTOVG_PATH_COMMAND_CUBIC_TO: output.cubicTo(point(0), point(1), point(2)); break;
            case PLUTOVG_PATH_COMMAND_CLOSE: output.lineTo(output.first); break;
        }
    }, &result);
    result.valid = true;
    return result;
}

AnimSvgPoint AnimSvgPath::pointAtLength(double distance) const{
    if (!std::isfinite(distance)) distance = 0;
    distance = std::clamp(distance, 0.0, totalLength);
    if (circleRadius >= 0){
        const double angle = circleRadius > 0 ? distance / circleRadius : 0;
        return {circleX + circleRadius * std::cos(angle), circleY + circleRadius * std::sin(angle)};
    }
    if (segments.empty()) return cursor;
    auto it = std::lower_bound(segments.begin(), segments.end(), distance,
                              [](const Segment& segment, double value){ return segment.end < value; });
    if (it == segments.end()) return segments.back().to;
    const double start = it == segments.begin() ? 0 : (it - 1)->end;
    const double fraction = (distance - start) / (it->end - start);
    return {it->from.x + (it->to.x - it->from.x) * fraction, it->from.y + (it->to.y - it->from.y) * fraction};
}

double AnimSvgPath::resolveLength(const std::string& text, double reference, double fontSize){
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


AnimSvgViewport AnimSvgViewport::fromAttributes(const std::map<std::string, std::string>& attributes,
                                                double referenceWidth, double referenceHeight, bool root, double fontSize){
    const auto length = [&](const char* name, double reference, double fallback, bool negative){
        auto found = attributes.find(name);
        if (found == attributes.end()) return fallback;
        const double value = AnimSvgPath::resolveLength(found->second, reference, fontSize);
        return std::isfinite(value) && (negative || value >= 0) ? value : fallback;
    };
    const double viewportWidth = root ? referenceWidth : length("width", referenceWidth, referenceWidth, false);
    const double viewportHeight = root ? referenceHeight : length("height", referenceHeight, referenceHeight, false);
    AnimSvgViewport result;
    result.width = viewportWidth;
    result.height = viewportHeight;
    std::string viewBox = svgAttribute(attributes, "viewBox");
    if (viewBox.empty()) viewBox = svgAttribute(attributes, "viewbox");
    std::replace(viewBox.begin(), viewBox.end(), ',', ' ');
    std::istringstream values(viewBox);
    double x = 0, y = 0, width = 0, height = 0;
    const bool hasViewBox = (bool)(values >> x >> y >> width >> height) && width > 0 && height > 0;
    if (hasViewBox){
        result.width = width;
        result.height = height;
    }
    if (hasViewBox && viewportWidth > 0 && viewportHeight > 0){
        double sx = viewportWidth / width, sy = viewportHeight / height;
        double tx = -x * sx, ty = -y * sy;
        const std::string aspect = attributes.count("preserveAspectRatio") ? svgAttribute(attributes, "preserveAspectRatio") : "xMidYMid meet";
        if (aspect.find("none") == std::string::npos){
            const double scale = aspect.find("slice") != std::string::npos ? std::max(sx, sy) : std::min(sx, sy);
            const double ax = aspect.find("xMin") != std::string::npos ? 0 : aspect.find("xMax") != std::string::npos ? 1 : 0.5;
            const double ay = aspect.find("YMin") != std::string::npos ? 0 : aspect.find("YMax") != std::string::npos ? 1 : 0.5;
            tx = -x * scale + ax * (viewportWidth - width * scale);
            ty = -y * scale + ay * (viewportHeight - height * scale);
            sx = sy = scale;
        }
        result.matrix = {sx, 0, 0, sy, tx, ty};
    }
    if (!root){
        const auto& v = result.matrix;
        plutovg_matrix_t matrix{(float)v[0], (float)v[1], (float)v[2], (float)v[3], (float)v[4], (float)v[5]};
        const plutovg_matrix_t position{1, 0, 0, 1, (float)length("x", referenceWidth, 0, true), (float)length("y", referenceHeight, 0, true)};
        plutovg_matrix_multiply(&matrix, &matrix, &position);
        const std::string text = svgAttribute(attributes, "transform");
        plutovg_matrix_t transform;
        if (plutovg_matrix_parse(&transform, text.c_str(), (int)text.size()))
            plutovg_matrix_multiply(&matrix, &matrix, &transform);
        result.matrix = {matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f};
    }
    // LunaSVG ignores transforms on the outer root; nested SVG transforms are applied.
    return result;
}
