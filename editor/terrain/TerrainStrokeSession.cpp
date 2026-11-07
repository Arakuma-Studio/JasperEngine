// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainStrokeSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // R34: full-strength deposit in normalized map units per second of painting. Blend and density
    // switch a channel that is often already full, so they run faster.
    constexpr float HEIGHT_FLOW_PER_SECOND = 1.5f;
    constexpr float PAINT_FLOW_PER_SECOND = 10.0f;
    // Droplets released per square texel of brush area, per second at full strength
    constexpr float ERODE_DROPLETS_PER_SECOND = 0.4f;
    // Enough for a broken surface, not so many it reads as grain; a fixed seed, so going over the same
    // ground again deepens the field instead of replacing it
    constexpr int NOISE_OCTAVES = 4;
    constexpr uint32_t NOISE_SEED = 0;

    // The most stamps one move lays. The spacing floor bounds a segment inside the terrain well
    // below it, so it only guards degenerate input.
    constexpr int MAX_STAMPS_PER_MOVE = 8192;

    // The smallest brush reaches a hundredth of a texel, or nothing would be left to round
    constexpr float MIN_RADIUS_TEXELS = 0.01f;

    OpType opTypeOfMode(editor::TerrainBrushMode mode){
        using editor::TerrainBrushMode;
        switch (mode){
            case TerrainBrushMode::Raise: return OpType::Raise;
            case TerrainBrushMode::Lower: return OpType::Lower;
            case TerrainBrushMode::Smooth: return OpType::Smooth;
            case TerrainBrushMode::Flatten: return OpType::SetHeight;
            case TerrainBrushMode::Sharpen: return OpType::Sharpen;
            case TerrainBrushMode::Noise: return OpType::AddNoise;
            case TerrainBrushMode::Terrace: return OpType::Terrace;
            case TerrainBrushMode::Stamp: return OpType::Stamp;
            case TerrainBrushMode::Erode: return OpType::Erode;
            case TerrainBrushMode::Ramp: return OpType::Ramp;
            case TerrainBrushMode::PaintBase: return OpType::PaintBase;
            case TerrainBrushMode::PaintLayer: return OpType::PaintLayer;
            case TerrainBrushMode::PaintDensity: return OpType::PaintDensity;
            case TerrainBrushMode::EraseDensity: return OpType::EraseDensity;
            case TerrainBrushMode::PlaceObject:
            case TerrainBrushMode::EraseObject:
                break;
        }
        return OpType::Raise;
    }

}

editor::TerrainStrokeSession::TerrainStrokeSession(TerrainMapStore& base, MapId map, float terrainSize, TerrainStrokeBrush brush)
    : transaction(base), map(map), format(base.format(map)), terrainSize(terrainSize), maxHeight(base.frame().maxHeight), brush(std::move(brush)) {
    const float texel = format.bounds.w > 0 ? terrainSize / static_cast<float>(format.bounds.w) : 0.0f;
    radius = std::max(this->brush.size, MIN_RADIUS_TEXELS * texel);
    flow = map.kind == MapKind::Height ? HEIGHT_FLOW_PER_SECOND : PAINT_FLOW_PER_SECOND;
    buildOp();
}

void editor::TerrainStrokeSession::buildOp(){
    op.type = opTypeOfMode(brush.mode);
    // The heights run in map units through terrain-local ops: maxHeight comes in here and goes out in
    // the kernel, which is one rounding away from the map units a brush always wrote
    switch (brush.mode){
        case TerrainBrushMode::Raise:
        case TerrainBrushMode::Lower:
        case TerrainBrushMode::Stamp:
            op.delta = maxHeight;
            break;
        case TerrainBrushMode::Flatten:
            op.height = std::clamp(brush.flattenTarget, 0.0f, 1.0f) * maxHeight;
            break;
        case TerrainBrushMode::Noise:
            op.amplitude = maxHeight;
            op.noiseSize = std::max(0.01f, brush.noiseSize);
            op.octaves = NOISE_OCTAVES;
            op.seed = NOISE_SEED;
            break;
        case TerrainBrushMode::Terrace:
            op.steps = brush.terraceSteps;
            break;
        case TerrainBrushMode::PaintLayer:
        case TerrainBrushMode::PaintBase:
            op.layer = brush.textureLayer;
            op.normalize = brush.normalizeBlend;
            break;
        case TerrainBrushMode::PaintDensity:
        case TerrainBrushMode::EraseDensity:
            op.layer = brush.foliageLayer;
            break;
        default:
            break;
    }
    if (map.kind == MapKind::Blend || brush.filterAllMaps){
        op.filter.slope = brush.slope;
        op.filter.height = brush.height;
        op.filter.invertSlope = brush.invertSlope;
        op.filter.invertHeight = brush.invertHeight;
    }

    Shape& shape = op.shape;
    shape.falloff = brush.falloff == TerrainBrushFalloff::Smooth ? Falloff::Smooth :
                    (brush.falloff == TerrainBrushFalloff::Linear ? Falloff::Linear : Falloff::Constant);
    shape.feather = 1.0f;
    shape.nearestTexel = true;
    const bool masked = !brush.mask.empty();
    if (masked){
        shape.maskPixels = std::move(brush.mask);
        shape.maskWidth = brush.maskWidth;
        shape.maskHeight = brush.maskHeight;
        shape.rotation = brush.rotation;
    }
    if (brush.mode == TerrainBrushMode::Ramp){
        shape.type = ShapeType::Polyline;
        shape.radius = radius;
        shape.squareEnds = brush.shape == TerrainBrushShape::Square;
        shape.points.assign(2, Vector2(0.0f, 0.0f));
    } else if (masked || brush.mode == TerrainBrushMode::Stamp){
        shape.type = ShapeType::Mask;
        shape.size = Vector2(radius * 2.0f, radius * 2.0f);
        shape.maskRound = brush.shape == TerrainBrushShape::Circle;
    } else if (brush.shape == TerrainBrushShape::Circle){
        shape.type = ShapeType::Circle;
        shape.radius = radius;
    } else {
        shape.type = ShapeType::Rect;
    }
}

void editor::TerrainStrokeSession::placeShape(float x, float z){
    Shape& shape = op.shape;
    shape.center = Vector2(x, z);
    if (shape.type == ShapeType::Rect){
        shape.min = Vector2(x - radius, z - radius);
        shape.max = Vector2(x + radius, z + radius);
    } else if (shape.type == ShapeType::Polyline){
        shape.points[0] = Vector2(rampStartX, rampStartZ);
        shape.points[1] = Vector2(x, z);
    }
}

void editor::TerrainStrokeSession::setRampStart(float x, float z){
    rampStartX = x;
    rampStartZ = z;
}

bool editor::TerrainStrokeSession::applyRamp(float x, float z, float strength){
    placeShape(x, z);
    op.heights = FilterRange{heightBefore(rampStartX, rampStartZ) * maxHeight, heightBefore(x, z) * maxHeight};
    op.strength = std::clamp(strength, 0.0f, 1.0f);
    OpOptions options;
    options.inPlace = true;
    return applyOps(transaction, std::span<const Op>(&op, 1), options).ok;
}

bool editor::TerrainStrokeSession::applyStamp(float x, float z, float seconds){
    const float deposit = brush.strength * flow * seconds;
    if (brush.mode == TerrainBrushMode::Ramp){
        return applyRamp(x, z, deposit);
    }

    placeShape(x, z);
    if (brush.mode == TerrainBrushMode::Erode){
        const float radiusX = std::max(MIN_RADIUS_TEXELS, (brush.size / terrainSize) * static_cast<float>(format.bounds.w));
        const float radiusZ = std::max(MIN_RADIUS_TEXELS, (brush.size / terrainSize) * static_cast<float>(format.bounds.h));
        erodeCarry += radiusX * radiusZ * ERODE_DROPLETS_PER_SECOND * brush.strength * seconds;
        const int droplets = static_cast<int>(erodeCarry);
        erodeCarry -= static_cast<float>(droplets);
        op.droplets = droplets;
        op.seed = erodeSeed;
        op.erodeSeconds = seconds;
        op.strength = brush.strength;
        OpOptions options;
        options.inPlace = true;
        const bool ok = applyOps(transaction, std::span<const Op>(&op, 1), options).ok;
        // a stamp whose droplets rounded to none draws no seed, as the kernel draws none for it
        if (ok && droplets > 0) erodeSeed++;
        return ok;
    }

    op.strength = std::clamp(deposit, 0.0f, 1.0f);
    OpOptions options;
    options.inPlace = true;
    return applyOps(transaction, std::span<const Op>(&op, 1), options).ok;
}

bool editor::TerrainStrokeSession::moveTo(float x, float z, float seconds){
    if (brush.mode == TerrainBrushMode::Ramp && !brush.accumulateRamp){
        // R36: only the two ends decide the surface, so each update starts from the before tiles
        if (!transaction.rewind().ok) return false;
        lastX = x;
        lastZ = z;
        hasLastPoint = true;
        return applyRamp(x, z, brush.strength);
    }

    int stamps = 1;
    float fromX = x;
    float fromZ = z;
    if (hasLastPoint){
        fromX = lastX;
        fromZ = lastZ;
        const float dx = x - fromX;
        const float dz = z - fromZ;
        const float distance = std::sqrt(dx * dx + dz * dz);
        const float spacing = std::max(brush.size * 0.25f, terrainSize / 4096.0f);
        stamps = std::clamp(static_cast<int>(std::ceil(distance / spacing)), 1, MAX_STAMPS_PER_MOVE);
    }

    const float stampSeconds = seconds / static_cast<float>(stamps);
    bool applied = false;
    for (int i = 1; i <= stamps; i++){
        const float t = static_cast<float>(i) / static_cast<float>(stamps);
        applied = applyStamp(fromX + (x - fromX) * t, fromZ + (z - fromZ) * t, stampSeconds) || applied;
    }

    lastX = x;
    lastZ = z;
    hasLastPoint = true;
    return applied;
}

void editor::TerrainStrokeSession::addSample(float time, float x, float z, float pressure){
    // a pen reading 0..1; anything else is a mouse, which presses fully
    const float pressed = pressure >= 0.0f && pressure <= 1.0f ? pressure : 1.0f;
    if (brush.mode == TerrainBrushMode::Ramp && !brush.accumulateRamp){
        queue.clear();
        moveTo(x, z, 0.0f);
        return;
    }
    if (!resampler){
        const float texel = format.bounds.w > 0 ? terrainSize / static_cast<float>(format.bounds.w) : 0.0f;
        resampler.emplace(StrokeResampler::defaultSpacing(radius, texel));
    }
    sampled.clear();
    resampler->add(StrokeSample{time, x, z, pressed}, sampled);
    queue.insert(queue.end(), sampled.begin(), sampled.end());
}

bool editor::TerrainStrokeSession::run(double budgetSeconds){
    const auto start = std::chrono::steady_clock::now();
    const float baseRadius = radius;
    while (!queue.empty()){
        const StrokeStamp stamp = queue.front();
        queue.pop_front();
        if (brush.pressureSize){
            radius = std::max(baseRadius * stamp.pressure, MIN_RADIUS_TEXELS * (format.bounds.w > 0 ? terrainSize / static_cast<float>(format.bounds.w) : 0.0f));
            op.shape.radius = op.shape.type == ShapeType::Circle || op.shape.type == ShapeType::Polyline ? radius : op.shape.radius;
            if (op.shape.type == ShapeType::Mask) op.shape.size = Vector2(radius * 2.0f, radius * 2.0f);
        }
        const float strengthScale = brush.pressureStrength ? stamp.pressure : 1.0f;
        const float saved = brush.strength;
        brush.strength = saved * strengthScale;
        applyStamp(stamp.x, stamp.z, StrokeResampler::SECONDS_PER_STAMP);
        brush.strength = saved;
        radius = baseRadius;
        if (op.shape.type == ShapeType::Circle || op.shape.type == ShapeType::Polyline) op.shape.radius = radius;
        if (op.shape.type == ShapeType::Mask) op.shape.size = Vector2(radius * 2.0f, radius * 2.0f);
        if (budgetSeconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= budgetSeconds){
            break;
        }
    }
    return queue.empty();
}

bool editor::TerrainStrokeSession::flush(){
    return transaction.flushToBase().ok;
}

void editor::TerrainStrokeSession::cancel(){
    transaction.rewind();
    transaction.discard();
}

float editor::TerrainStrokeSession::heightBefore(float x, float z) const{
    const MapId heightMap{MapKind::Height, 0};
    const MapFormat heights = transaction.format(heightMap);
    if (!heights.exists || heights.bounds.empty()) return 0.0f;

    const Vector2 texel = transaction.texelOfWorld(heightMap, x, z);
    const TexelRect& bounds = heights.bounds;
    const float tx = std::floor(texel.x);
    const float ty = std::floor(texel.y);
    const float blendX = texel.x - tx;
    const float blendY = texel.y - ty;

    // clamp to edge, as the drawn surface does
    const int x0 = std::clamp(static_cast<int>(tx), bounds.x, bounds.right() - 1);
    const int y0 = std::clamp(static_cast<int>(ty), bounds.y, bounds.bottom() - 1);
    const int x1 = std::clamp(static_cast<int>(tx) + 1, bounds.x, bounds.right() - 1);
    const int y1 = std::clamp(static_cast<int>(ty) + 1, bounds.y, bounds.bottom() - 1);

    const TexelRect corners{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    uint8_t stored[4 * 8] = {};
    if (!transaction.readBeforeBytes(heightMap, corners, stored)) return 0.0f;
    const int stride = bytesPerTexel(heights);
    auto at = [&](int cx, int cy) {
        float value = 0.0f;
        decodeTexel(heightMap, heights, stored + (static_cast<size_t>(cy - y0) * corners.w + (cx - x0)) * stride, &value);
        return value;
    };
    const float lower = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * blendX;
    const float upper = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * blendX;
    return lower + (upper - lower) * blendY;
}
