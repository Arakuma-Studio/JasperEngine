// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimSvgPath.h"

#include "../../../libs/lunasvg/plutovg/include/plutovg.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <memory>

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
