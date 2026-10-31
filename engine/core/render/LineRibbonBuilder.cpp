// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LineRibbonBuilder.h"

#include "math/AnimationCurve.h"
#include "math/Gradient.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // consecutive points closer than this are one point
    const float DUPLICATE_DISTANCE = 1e-6f;
    const float PI = 3.14159265358979f;

    bool isFinite(const Vector3& v){
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    Vector3 direction(const Vector3& from, const Vector3& to){
        Vector3 d = to - from;
        const float length = d.length();
        return (length > 0.0f) ? d / length : Vector3(1.0f, 0.0f, 0.0f);
    }

    // A station of the ribbon: a point of the centre line with what its vertices share
    struct Station {
        Vector3 position;
        Vector3 tangentIn;
        Vector3 tangentOut;
        Vector4 color;
        float halfWidth;
        float u;
        // body vertices ending the segment into it / starting the one out of it, by side
        // (0: -1, 1: +1)
        uint32_t in[2];
        uint32_t out[2];
    };

}

float LineRibbonBuilder::halfWidth(const LineRibbonSettings& settings, float t){
    const float curve = settings.widthCurve ? settings.widthCurve->evaluate(t) : 1.0f;
    const float width = settings.widthMultiplier * curve;
    return (std::isfinite(width) && width > 0.0f) ? width * 0.5f : 0.0f;
}

void LineRibbonBuilder::counts(size_t points, bool loop, unsigned int numCornerVertices, unsigned int numCapVertices,
                               size_t& vertexCount, size_t& indexCount){
    vertexCount = 0;
    indexCount = 0;
    if (points < 2){
        return;
    }
    const size_t corner = std::min(numCornerVertices, LINE_RIBBON_MAX_FAN_VERTICES);
    const size_t cap = std::min(numCapVertices, LINE_RIBBON_MAX_FAN_VERTICES);
    const size_t jointVertices = 2 * (2 + corner);
    const size_t jointIndices = 2 * (corner + 1) * 3;
    if (loop){
        const size_t segments = points;
        vertexCount = 2 + points * jointVertices;
        indexCount = segments * 6 + points * jointIndices;
    }else{
        const size_t segments = points - 1;
        vertexCount = 4 + (points - 2) * jointVertices;
        indexCount = segments * 6 + (points - 2) * jointIndices;
        if (cap > 0){
            vertexCount += 2 * (1 + cap);
            indexCount += 2 * (cap + 1) * 3;
        }
    }
}

LineRibbonResult LineRibbonBuilder::build(const Vector3* points, size_t count, const LineRibbonSettings& settings,
                                          std::vector<LineRibbonVertex>& vertices, std::vector<uint32_t>& indices,
                                          const float* pointT, const Vector4* pointColors){
    LineRibbonResult result;
    result.bounds.setNull();

    // the points used, with their index in the input (for pointT and pointColors)
    std::vector<size_t> used;
    used.reserve(count);
    for (size_t i = 0; i < count; i++){
        if (!isFinite(points[i])){
            result.invalidPoints++;
            continue;
        }
        if (!used.empty() && points[used.back()].distance(points[i]) < DUPLICATE_DISTANCE){
            continue;
        }
        used.push_back(i);
    }
    // a loop drawn back onto its first point: the closing segment is the loop's own
    if (settings.loop && used.size() > 2 && points[used.back()].distance(points[used.front()]) < DUPLICATE_DISTANCE){
        used.pop_back();
    }

    const size_t n = used.size();
    if (n < 2){
        return result;
    }

    const bool loop = settings.loop;
    const size_t segments = loop ? n : n - 1;
    // a loop ends where it started: station n is the first point again
    const size_t stationCount = loop ? n + 1 : n;
    const unsigned int corner = std::min(settings.numCornerVertices, LINE_RIBBON_MAX_FAN_VERTICES);
    const unsigned int cap = loop ? 0 : std::min(settings.numCapVertices, LINE_RIBBON_MAX_FAN_VERTICES);

    auto pointOf = [&](size_t station) -> const Vector3& {
        return points[used[station % n]];
    };

    // cumulative lengths
    std::vector<float> lengths(stationCount, 0.0f);
    for (size_t s = 1; s < stationCount; s++){
        lengths[s] = lengths[s - 1] + pointOf(s - 1).distance(pointOf(s));
    }
    const float totalLength = lengths[stationCount - 1];
    result.length = totalLength;

    std::vector<Vector3> segmentDirections(segments);
    for (size_t k = 0; k < segments; k++){
        segmentDirections[k] = direction(pointOf(k), pointOf(k + 1));
    }

    std::vector<Station> stations(stationCount);
    for (size_t s = 0; s < stationCount; s++){
        Station& station = stations[s];
        const size_t source = used[s % n];
        station.position = pointOf(s);

        float t;
        if (pointT){
            t = pointT[source];
        }else{
            t = (totalLength > 0.0f) ? lengths[s] / totalLength : 0.0f;
        }
        if (loop && s == stationCount - 1 && !pointT){
            t = 1.0f;
        }

        if (pointColors){
            station.color = pointColors[source];
        }else if (settings.colorGradient){
            station.color = settings.colorGradient->evaluate(t);
        }else{
            station.color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        station.halfWidth = halfWidth(settings, t);
        result.maxHalfWidth = std::max(result.maxHalfWidth, station.halfWidth);

        switch (settings.textureMode){
            case LineTextureMode::Stretch:
                station.u = ((totalLength > 0.0f) ? lengths[s] / totalLength : 0.0f) * settings.textureScale.x;
                break;
            case LineTextureMode::Tile:
                station.u = lengths[s] * settings.textureScale.x;
                break;
            case LineTextureMode::DistributePerSegment:
                station.u = ((float)s / (float)segments) * settings.textureScale.x;
                break;
            case LineTextureMode::RepeatPerSegment:
                station.u = (float)s * settings.textureScale.x;
                break;
        }

        if (loop){
            station.tangentIn = segmentDirections[(s + segments - 1) % segments];
            station.tangentOut = segmentDirections[s % segments];
        }else{
            station.tangentIn = segmentDirections[(s == 0) ? 0 : s - 1];
            station.tangentOut = segmentDirections[(s == segments) ? segments - 1 : s];
        }

        result.bounds.merge(station.position);
    }

    const uint32_t base = (uint32_t)vertices.size();
    const size_t firstIndex = indices.size();

    // the vertex lists' room for this ribbon at once
    size_t addedVertices = 0;
    size_t addedIndices = 0;
    counts(n, loop, corner, cap, addedVertices, addedIndices);
    vertices.reserve(vertices.size() + addedVertices);
    indices.reserve(indices.size() + addedIndices);

    // (fields written one by one: a trail rebuilds its vertices every frame)
    const float vScale = settings.textureScale.y;
    auto addVertex = [&](const Station& station, float side, float halfWidth, int role, float fan, float v) -> uint32_t {
        LineRibbonVertex& vertex = vertices.emplace_back();
        vertex.position.x = station.position.x; vertex.position.y = station.position.y; vertex.position.z = station.position.z;
        vertex.color.x = station.color.x; vertex.color.y = station.color.y; vertex.color.z = station.color.z; vertex.color.w = station.color.w;
        vertex.uv.x = station.u; vertex.uv.y = v * vScale;
        vertex.tangentIn.x = station.tangentIn.x; vertex.tangentIn.y = station.tangentIn.y; vertex.tangentIn.z = station.tangentIn.z;
        vertex.tangentOut.x = station.tangentOut.x; vertex.tangentOut.y = station.tangentOut.y; vertex.tangentOut.z = station.tangentOut.z;
        vertex.params.x = side; vertex.params.y = halfWidth; vertex.params.z = (float)role; vertex.params.w = fan;
        return (uint32_t)(vertices.size() - 1);
    };
    auto triangle = [&](uint32_t a, uint32_t b, uint32_t c){
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    };
    // the round cap of an end: a fan around its centre from the +1 body vertex to the -1 one
    auto addCap = [&](Station& station, float end){
        const uint32_t centre = addVertex(station, end, 0.0f, LineRibbonRole_In, 0.0f, 0.5f);
        uint32_t previous = station.in[1];
        for (unsigned int k = 1; k <= cap; k++){
            const float w = (float)k / (float)(cap + 1);
            const uint32_t vertex = addVertex(station, end, station.halfWidth, LineRibbonRole_Cap, w, std::cos(PI * w) * 0.5f + 0.5f);
            triangle(centre, previous, vertex);
            previous = vertex;
        }
        triangle(centre, previous, station.in[0]);
    };

    // the in and out vertices of a joint tell the shader it is round (w 1: no miter)
    const float roundJoint = corner > 0 ? 1.0f : 0.0f;
    std::vector<uint32_t> chain[2];
    for (size_t s = 0; s < stationCount; s++){
        Station& station = stations[s];
        const bool end = !loop && (s == 0 || s == stationCount - 1);
        if (end){
            for (int j = 0; j < 2; j++){
                const float side = j == 0 ? -1.0f : 1.0f;
                station.in[j] = station.out[j] = addVertex(station, side, station.halfWidth, LineRibbonRole_In, 0.0f, j == 0 ? 0.0f : 1.0f);
            }
            continue;
        }
        if (loop && s == 0){
            // the loop's start: its out pair placed as the joint at the last station places
            // its own (same point and directions), which draws the joint
            for (int j = 0; j < 2; j++){
                const float side = j == 0 ? -1.0f : 1.0f;
                station.in[j] = station.out[j] = addVertex(station, side, station.halfWidth, LineRibbonRole_Out, roundJoint, j == 0 ? 0.0f : 1.0f);
            }
            continue;
        }
        // a joint: per side the in vertex, the corner fan, the out vertex
        for (int j = 0; j < 2; j++){
            const float side = j == 0 ? -1.0f : 1.0f;
            const float v = j == 0 ? 0.0f : 1.0f;
            chain[j].clear();
            station.in[j] = addVertex(station, side, station.halfWidth, LineRibbonRole_In, roundJoint, v);
            chain[j].push_back(station.in[j]);
            for (unsigned int k = 1; k <= corner; k++){
                chain[j].push_back(addVertex(station, side, station.halfWidth, LineRibbonRole_Corner, (float)k / (float)(corner + 1), v));
            }
            station.out[j] = addVertex(station, side, station.halfWidth, LineRibbonRole_Out, roundJoint, v);
            chain[j].push_back(station.out[j]);
        }
        // the wedge of each side fans around the other side's in vertex: the inner side's
        // vertices all sit on the miter point there, so only the outer side's have area
        for (int j = 0; j < 2; j++){
            const uint32_t centre = station.in[1 - j];
            for (size_t m = 0; m + 1 < chain[j].size(); m++){
                triangle(centre, chain[j][m], chain[j][m + 1]);
            }
        }
    }

    // the body: a quad per segment, from the out pair of a station to the in pair of the next
    for (size_t k = 0; k < segments; k++){
        const Station& a = stations[k];
        const Station& b = stations[k + 1];
        triangle(a.out[0], a.out[1], b.in[1]);
        triangle(a.out[0], b.in[1], b.in[0]);
    }

    if (cap > 0){
        addCap(stations.front(), -1.0f);
        addCap(stations.back(), 1.0f);
    }

    result.vertexCount = vertices.size() - base;
    result.indexCount = indices.size() - firstIndex;
    return result;
}

float LineRibbonBuilder::widthAt(float multiplier, const AnimationCurve& curve, float t){
    return multiplier * curve.evaluate(t);
}

void LineRibbonBuilder::setEndWidths(float& multiplier, AnimationCurve& curve, float start, float end){
    start = std::isfinite(start) ? std::max(0.0f, start) : 0.0f;
    end = std::isfinite(end) ? std::max(0.0f, end) : 0.0f;
    multiplier = 1.0f;
    curve = (start == end) ? AnimationCurve::constant(start) : AnimationCurve::linear(0.0f, start, 1.0f, end);
}

Vector4 LineRibbonBuilder::colorAt(const Gradient& gradient, float t){
    const Vector4 color = gradient.evaluate(t);
    const Vector3 srgb = Color::linearTosRGB(Vector3(color.x, color.y, color.z));
    return Vector4(srgb.x, srgb.y, srgb.z, color.w);
}

void LineRibbonBuilder::setEndColor(Gradient& gradient, float t, const Vector4& srgbColor){
    const Vector3 linear = Color::sRGBToLinear(Vector3(srgbColor.x, srgbColor.y, srgbColor.z));
    const bool atEnd = t > 0.5f;
    gradient.normalize();

    // the key at that end, else a new one (the nearest replaced when the list is full)
    bool found = false;
    for (GradientColorKey& key : gradient.colorKeys){
        if (key.time == t){
            key.color = linear;
            found = true;
        }
    }
    if (!found){
        if (gradient.colorKeys.size() >= Gradient::MAX_KEYS){
            GradientColorKey& key = atEnd ? gradient.colorKeys.back() : gradient.colorKeys.front();
            key = GradientColorKey(t, linear);
        }else{
            gradient.colorKeys.push_back(GradientColorKey(t, linear));
        }
    }

    found = false;
    for (GradientAlphaKey& key : gradient.alphaKeys){
        if (key.time == t){
            key.alpha = srgbColor.w;
            found = true;
        }
    }
    if (!found){
        if (gradient.alphaKeys.size() >= Gradient::MAX_KEYS){
            GradientAlphaKey& key = atEnd ? gradient.alphaKeys.back() : gradient.alphaKeys.front();
            key = GradientAlphaKey(t, srgbColor.w);
        }else{
            gradient.alphaKeys.push_back(GradientAlphaKey(t, srgbColor.w));
        }
    }
    gradient.normalize();
}

namespace {

    Vector3 perpendicular(const Vector3& tangent, const Vector3& n){
        Vector3 s = tangent.crossProduct(n);
        float length = s.length();
        if (length < 1e-5f){
            const Vector3 other = std::fabs(tangent.y) < 0.99f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
            s = tangent.crossProduct(other);
            length = s.length();
        }
        return s / std::max(length, 1e-8f);
    }

    Vector3 normalizedOr(const Vector3& v, const Vector3& fallback){
        const float length = v.length();
        return (length > 0.0f) ? v / length : fallback;
    }

}

Vector3 LineRibbonBuilder::vertexOffset(const LineRibbonVertex& vertex, const Vector3& normal){
    return vertexOffset(vertex, vertex.tangentIn, vertex.tangentOut, normal);
}

Vector3 LineRibbonBuilder::vertexOffset(const LineRibbonVertex& vertex, const Vector3& tin, const Vector3& tout, const Vector3& n){
    const float side = vertex.params.x;
    const float role = vertex.params.z;
    const float w = vertex.params.w;
    const float MITER_LIMIT = 4.0f;
    const Vector3 sin = perpendicular(tin, n);
    const Vector3 sout = perpendicular(tout, n);

    if (role > 1.5f && role < 2.5f){
        const float a = PI * w;
        if (side < 0.0f){
            return sin * std::cos(a) - normalizedOr(n.crossProduct(sin), tin) * std::sin(a);
        }
        return sout * std::cos(a) + normalizedOr(n.crossProduct(sout), tout) * std::sin(a);
    }

    const float turn = tin.crossProduct(tout).dotProduct(n);
    const bool level = std::fabs(turn) < 1e-4f;
    const bool straight = level && tin.dotProduct(tout) > 0.0f;
    const bool uturn = level && !straight;
    const bool outer = uturn || side * turn > 0.0f;

    const Vector3 sum = sin + sout;
    const float sumLength = sum.length();
    const Vector3 miterDirection = sumLength > 1e-4f ? sum / sumLength : sin;
    const float miter = 1.0f / std::max(miterDirection.dotProduct(sin), 1e-4f);

    if (straight || !outer){
        return miterDirection * (std::min(miter, MITER_LIMIT) * side);
    }
    const bool roundJoint = (role > 0.5f && role < 1.5f) || w > 0.5f;
    if (!roundJoint && !uturn && miter <= MITER_LIMIT){
        return miterDirection * (miter * side);
    }
    if (role < 0.5f){
        return sin * side;
    }
    if (role > 2.5f){
        return sout * side;
    }
    const Vector3 from = sin * side;
    const Vector3 to = sout * side;
    const Vector3 front = normalizedOr(n.crossProduct(sin), tin);
    const float angle = std::acos(std::min(1.0f, std::max(-1.0f, from.dotProduct(to)))) * w;
    return from * std::cos(angle) + front * std::sin(angle);
}
