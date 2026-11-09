// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainShape.h"
#include "terrain/TerrainLayers.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::terrain;

MapLattice jasperengine::terrain::latticeOf(const TerrainMapStore& store, MapId map, const MapFormat& format) {
    MapLattice lattice;
    const TexelRect& b = format.bounds;
    lattice.baseX = b.x;
    lattice.baseY = b.y;
    const Vector2 origin = store.worldOfTexel(map, b.x, b.y);
    const Vector2 beyond = store.worldOfTexel(map, b.x + std::max(b.w, 1), b.y + std::max(b.h, 1));
    lattice.originX = origin.x;
    lattice.originZ = origin.y;
    lattice.stepX = (beyond.x - origin.x) / static_cast<float>(std::max(b.w, 1));
    lattice.stepZ = (beyond.y - origin.y) / static_cast<float>(std::max(b.h, 1));
    return lattice;
}

LocalBounds jasperengine::terrain::localBoundsOf(const MapLattice& lattice, const MapFormat& format) {
    LocalBounds result;
    const float halfX = std::fabs(lattice.stepX) * 0.5f;
    const float halfZ = std::fabs(lattice.stepZ) * 0.5f;
    const float x0 = lattice.x(format.bounds.x), x1 = lattice.x(format.bounds.right() - 1);
    const float z0 = lattice.z(format.bounds.y), z1 = lattice.z(format.bounds.bottom() - 1);
    result.min = Vector2(std::min(x0, x1) - halfX, std::min(z0, z1) - halfZ);
    result.max = Vector2(std::max(x0, x1) + halfX, std::max(z0, z1) + halfZ);
    return result;
}

ShapeEvaluator::ShapeEvaluator(const Shape& shape): shape(&shape) {
    const bool roundByDefault = shape.type == ShapeType::Circle || shape.type == ShapeType::Stroke;
    feather = shape.feather < 0.0f ? (roundByDefault ? 1.0f : 0.0f) : detClamp01(shape.feather);
    const bool polylineMask = shape.type == ShapeType::Polyline && !shape.maskPixels.empty();
    if (shape.rotation != 0.0f && (shape.type == ShapeType::Rect || shape.type == ShapeType::Mask || polylineMask)) {
        const float angle = detRadians(shape.rotation);
        cosine = detCos(angle);
        sine = detSin(angle);
    }
    if (shape.type == ShapeType::Rect) {
        middle = Vector2((shape.min.x + shape.max.x) * 0.5f, (shape.min.y + shape.max.y) * 0.5f);
        half = Vector2(std::fabs(shape.max.x - shape.min.x) * 0.5f, std::fabs(shape.max.y - shape.min.y) * 0.5f);
    } else if (shape.type == ShapeType::Mask) {
        middle = shape.center;
        half = Vector2(std::fabs(shape.size.x) * 0.5f, std::fabs(shape.size.y) * 0.5f);
    } else if (shape.type == ShapeType::Circle) {
        middle = shape.center;
    }
}

float ShapeEvaluator::falloffOf(float distance01) const {
    // the share of the radius past the solid core
    if (shape->falloff == Falloff::Constant || feather <= 0.0f) return 1.0f;
    const float t = detClamp01((1.0f - distance01) / feather);
    return shape->falloff == Falloff::Linear ? t : detSmooth(t);
}

float ShapeEvaluator::maskAt(float u, float v) const {
    if (shape->maskPixels.empty() || shape->maskWidth <= 0 || shape->maskHeight <= 0) return 0.0f;
    const float texelX = detClamp01(u) * static_cast<float>(shape->maskWidth - 1);
    const float texelY = detClamp01(v) * static_cast<float>(shape->maskHeight - 1);
    const int lowerX = std::clamp(static_cast<int>(std::floor(texelX)), 0, shape->maskWidth - 1);
    const int lowerY = std::clamp(static_cast<int>(std::floor(texelY)), 0, shape->maskHeight - 1);
    const int upperX = std::min(lowerX + 1, shape->maskWidth - 1);
    const int upperY = std::min(lowerY + 1, shape->maskHeight - 1);
    const float blendX = texelX - static_cast<float>(lowerX);
    const float blendY = texelY - static_cast<float>(lowerY);
    auto at = [&](int x, int y) {
        return shape->maskPixels[static_cast<size_t>(y) * shape->maskWidth + x];
    };
    const float lower = at(lowerX, lowerY) + (at(upperX, lowerY) - at(lowerX, lowerY)) * blendX;
    const float upper = at(lowerX, upperY) + (at(upperX, upperY) - at(lowerX, upperY)) * blendX;
    return lower + (upper - lower) * blendY;
}

ShapeSample ShapeEvaluator::sample(float x, float z) const {
    ShapeSample result;
    switch (shape->type) {
        case ShapeType::All:
            result.inside = true;
            result.weight = 1.0f;
            result.falloff = 1.0f;
            return result;

        case ShapeType::Stroke:
        case ShapeType::Circle: {
            if (shape->radius <= 0.0f) return result;
            const float dx = x - shape->center.x;
            const float dz = z - shape->center.y;
            const float distance = std::sqrt(dx * dx + dz * dz) / shape->radius;
            if (distance > 1.0f) return result;
            result.inside = true;
            result.falloff = falloffOf(distance);
            result.weight = result.falloff;
            return result;
        }

        case ShapeType::Rect: {
            if (half.x <= 0.0f || half.y <= 0.0f) return result;
            const float dx = x - middle.x;
            const float dz = z - middle.y;
            const float localX = (dx * cosine - dz * sine) / half.x;
            const float localZ = (dx * sine + dz * cosine) / half.y;
            const float distance = std::max(std::fabs(localX), std::fabs(localZ));
            if (distance > 1.0f) return result;
            result.inside = true;
            result.falloff = falloffOf(distance);
            result.weight = result.falloff;
            return result;
        }

        case ShapeType::Mask: {
            if (half.x <= 0.0f || half.y <= 0.0f) return result;
            const float dx = x - middle.x;
            const float dz = z - middle.y;
            const float localX = dx / half.x;
            const float localZ = dz / half.y;
            const float distance = shape->maskRound ? std::sqrt(localX * localX + localZ * localZ)
                                                    : std::max(std::fabs(localX), std::fabs(localZ));
            if (distance > 1.0f) return result;
            // the image turns inside its frame, as a stamp brush's does
            const float rotatedX = localX * cosine - localZ * sine;
            const float rotatedZ = localX * sine + localZ * cosine;
            result.mask = maskAt(rotatedX * 0.5f + 0.5f, rotatedZ * 0.5f + 0.5f);
            result.falloff = falloffOf(distance);
            result.weight = result.falloff * result.mask;
            result.inside = result.mask >= 0.5f;
            return result;
        }

        case ShapeType::Polyline: {
            const std::vector<Vector2>& points = shape->points;
            if (points.empty()) return result;

            if (shape->closed && shape->radius <= 0.0f) {
                // the inside of a polygon: even-odd, with a texel on an edge decided by which side
                // of it the sample's row falls, the same on both polygons sharing that edge
                bool odd = false;
                for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
                    const bool crosses = (points[i].y > z) != (points[j].y > z);
                    if (crosses && x < (points[j].x - points[i].x) * (z - points[i].y) / (points[j].y - points[i].y) + points[i].x) {
                        odd = !odd;
                    }
                }
                result.inside = odd;
                result.falloff = odd ? 1.0f : 0.0f;
                result.weight = result.falloff;
                return result;
            }

            if (shape->radius <= 0.0f) return result;
            const size_t segments = points.size() < 2 ? 1 : (shape->closed ? points.size() : points.size() - 1);
            float best = -1.0f;
            float bestAlong = 0.0f;
            float bestX = 0.0f;
            float bestZ = 0.0f;
            for (size_t i = 0; i < segments; i++) {
                const Vector2& a = points[i];
                const Vector2& b = points.size() < 2 ? points[i] : points[(i + 1) % points.size()];
                const float abX = b.x - a.x;
                const float abZ = b.y - a.y;
                const float lengthSquared = abX * abX + abZ * abZ;
                const float t = lengthSquared > 0.0f ? detClamp01(((x - a.x) * abX + (z - a.y) * abZ) / lengthSquared) : 0.0f;
                const float dx = x - (a.x + abX * t);
                const float dz = z - (a.y + abZ * t);
                const float distanceSquared = dx * dx + dz * dz;
                if (best < 0.0f || distanceSquared < best) {
                    best = distanceSquared;
                    bestAlong = t;
                    bestX = dx;
                    bestZ = dz;
                }
            }
            const float distance = shape->squareEnds ? std::max(std::fabs(bestX), std::fabs(bestZ)) / shape->radius
                                                     : std::sqrt(best) / shape->radius;
            if (distance > 1.0f) return result;
            result.inside = true;
            result.falloff = falloffOf(distance);
            result.weight = result.falloff;
            result.along = bestAlong;
            if (!shape->maskPixels.empty()) {
                const float localX = bestX / shape->radius;
                const float localZ = bestZ / shape->radius;
                const float rotatedX = localX * cosine - localZ * sine;
                const float rotatedZ = localX * sine + localZ * cosine;
                result.mask = maskAt(rotatedX * 0.5f + 0.5f, rotatedZ * 0.5f + 0.5f);
                result.weight = result.falloff * result.mask;
            }
            return result;
        }
    }
    return result;
}

bool ShapeEvaluator::bounds(LocalBounds& out) const {
    switch (shape->type) {
        case ShapeType::All:
            return false;
        case ShapeType::Stroke:
        case ShapeType::Polyline: {
            if (shape->points.empty()) return false;
            float minX = shape->points[0].x, maxX = minX, minZ = shape->points[0].y, maxZ = minZ;
            for (const Vector2& point : shape->points) {
                minX = std::min(minX, point.x);
                maxX = std::max(maxX, point.x);
                minZ = std::min(minZ, point.y);
                maxZ = std::max(maxZ, point.y);
            }
            out.min = Vector2(minX - shape->radius, minZ - shape->radius);
            out.max = Vector2(maxX + shape->radius, maxZ + shape->radius);
            return true;
        }
        case ShapeType::Circle:
            out.min = Vector2(shape->center.x - shape->radius, shape->center.y - shape->radius);
            out.max = Vector2(shape->center.x + shape->radius, shape->center.y + shape->radius);
            return true;
        case ShapeType::Rect: {
            // a rotated rectangle's corners, turned back into the terrain's axes
            const float extentX = std::fabs(cosine) * half.x + std::fabs(sine) * half.y;
            const float extentZ = std::fabs(sine) * half.x + std::fabs(cosine) * half.y;
            out.min = Vector2(middle.x - extentX, middle.y - extentZ);
            out.max = Vector2(middle.x + extentX, middle.y + extentZ);
            return true;
        }
        case ShapeType::Mask:
            out.min = Vector2(middle.x - half.x, middle.y - half.y);
            out.max = Vector2(middle.x + half.x, middle.y + half.y);
            return true;
    }
    return false;
}

float ShapeEvaluator::characteristicRadius() const {
    switch (shape->type) {
        case ShapeType::All:
            return 0.0f;
        case ShapeType::Stroke:
        case ShapeType::Polyline:
        case ShapeType::Circle:
            return shape->radius;
        case ShapeType::Rect:
        case ShapeType::Mask:
            return std::min(half.x, half.y);
    }
    return 0.0f;
}

TexelRect jasperengine::terrain::shapeTexels(const TerrainMapStore& store, MapId map, const MapFormat& format, const ShapeEvaluator& evaluator, int64_t* rawArea) {
    LocalBounds local;
    if (!evaluator.bounds(local)) {
        if (rawArea) *rawArea = format.bounds.area();
        return format.bounds;
    }
    const Vector2 a = store.texelOfWorld(map, local.min.x, local.min.y);
    const Vector2 b = store.texelOfWorld(map, local.max.x, local.max.y);
    // Doubles: a shape far outside a map must clip to nothing, not overflow an int
    const double minX = std::floor(static_cast<double>(std::min(a.x, b.x)));
    const double maxX = std::ceil(static_cast<double>(std::max(a.x, b.x)));
    const double minY = std::floor(static_cast<double>(std::min(a.y, b.y)));
    const double maxY = std::ceil(static_cast<double>(std::max(a.y, b.y)));
    if (rawArea) {
        const double area = (maxX - minX + 1.0) * (maxY - minY + 1.0);
        *rawArea = area > 9.0e18 ? INT64_MAX : static_cast<int64_t>(area);
    }
    const double boundsLeft = format.bounds.x, boundsTop = format.bounds.y;
    const double boundsRight = format.bounds.right(), boundsBottom = format.bounds.bottom();
    const double left = std::max(minX, boundsLeft), top = std::max(minY, boundsTop);
    const double right = std::min(maxX + 1.0, boundsRight), bottom = std::min(maxY + 1.0, boundsBottom);
    if (right <= left || bottom <= top) return TexelRect{};
    return TexelRect{static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left), static_cast<int>(bottom - top)};
}

float jasperengine::terrain::softRange(float value, float low, float high, float softness) {
    if (!(value >= low && value <= high)) return 0.0f;
    if (softness <= 0.0f) return 1.0f;
    const float band = (high - low) * 0.5f * detClamp01(softness);
    if (band <= 0.0f) return 1.0f;
    return detSmoothstep(low, low + band, value) * detSmoothstep(high, high - band, value);
}

float jasperengine::terrain::softSlope(float normalY, float minDegrees, float maxDegrees, float softness) {
    // Steeper is a smaller cosine, so the range turns over
    const float cosLow = minDegrees <= 0.0f ? 1.0f : detCos(detRadians(minDegrees));
    const float cosHigh = maxDegrees >= 90.0f ? 0.0f : detCos(detRadians(maxDegrees));
    if (!(normalY <= cosLow && normalY >= cosHigh)) return 0.0f;
    if (softness <= 0.0f) return 1.0f;
    const float band = (maxDegrees - minDegrees) * 0.5f * detClamp01(softness);
    if (band <= 0.0f) return 1.0f;
    const float cosLowInner = detCos(detRadians(minDegrees + band));
    const float cosHighInner = detCos(detRadians(maxDegrees - band));
    return detSmoothstep(cosLow, cosLowInner, normalY) * detSmoothstep(cosHigh, cosHighInner, normalY);
}

float jasperengine::terrain::sampleWindow(const HeightWindow& window, float x, float z) {
    float texelX = static_cast<float>(window.lattice.baseX) + (x - window.lattice.originX) * window.invStepX;
    float texelY = static_cast<float>(window.lattice.baseY) + (z - window.lattice.originZ) * window.invStepZ;
    // clamp to the map's edge, the sampler's rule
    texelX = std::clamp(texelX, static_cast<float>(window.bounds.x), static_cast<float>(window.bounds.right() - 1));
    texelY = std::clamp(texelY, static_cast<float>(window.bounds.y), static_cast<float>(window.bounds.bottom() - 1));
    const int lowerX = static_cast<int>(std::floor(texelX));
    const int lowerY = static_cast<int>(std::floor(texelY));
    const int upperX = std::min(lowerX + 1, window.bounds.right() - 1);
    const int upperY = std::min(lowerY + 1, window.bounds.bottom() - 1);
    const float blendX = texelX - static_cast<float>(lowerX);
    const float blendY = texelY - static_cast<float>(lowerY);
    auto at = [&](int tx, int ty) {
        const int wx = std::clamp(tx, window.rect.x, window.rect.right() - 1) - window.rect.x;
        const int wy = std::clamp(ty, window.rect.y, window.rect.bottom() - 1) - window.rect.y;
        return window.values[static_cast<size_t>(wy) * window.rect.w + wx];
    };
    const float lower = at(lowerX, lowerY) + (at(upperX, lowerY) - at(lowerX, lowerY)) * blendX;
    const float upper = at(lowerX, upperY) + (at(upperX, upperY) - at(lowerX, upperY)) * blendX;
    return lower + (upper - lower) * blendY;
}

namespace {

    // T1 R48: a filter over this many texels or more runs its rows on the job system's workers
    constexpr int64_t PARALLEL_FILTER_TEXELS = 16 * 1024;

    bool readMap(const TerrainMapStore& store, MapReader reader, void* user, MapId map, TexelRect rect, std::span<float> out) {
        if (reader) return reader(user, map, rect, out);
        return store.read(map, rect, out);
    }

}

void jasperengine::terrain::evaluateFilter(const TerrainMapStore& store, const Filter& filter, const MapLattice& lattice, TexelRect block,
                                           std::span<float> weights, MapReader reader, void* readerUser) {
    std::fill(weights.begin(), weights.end(), 1.0f);
    if (!filter.active() || block.empty()) return;

    const float softness = filter.softness;

    if (filter.slope || filter.height) {
        const MapId heightId{MapKind::Height, 0};
        const MapFormat heightFormat = store.format(heightId);
        const float maxHeight = store.frame().maxHeight;
        if (!heightFormat.exists) {
            std::fill(weights.begin(), weights.end(), 0.0f);
        } else {
            HeightWindow window;
            window.lattice = latticeOf(store, heightId, heightFormat);
            window.bounds = heightFormat.bounds;
            window.invStepX = window.lattice.stepX != 0.0f ? 1.0f / window.lattice.stepX : 1.0f;
            window.invStepZ = window.lattice.stepZ != 0.0f ? 1.0f / window.lattice.stepZ : 1.0f;
            const float delta = std::fabs(window.lattice.stepX);

            // the block's samples in the height map, one texel beyond for the forward difference
            const float minX = std::min(lattice.x(block.x), lattice.x(block.right() - 1));
            const float maxX = std::max(lattice.x(block.x), lattice.x(block.right() - 1)) + delta;
            const float minZ = std::min(lattice.z(block.y), lattice.z(block.bottom() - 1));
            const float maxZ = std::max(lattice.z(block.y), lattice.z(block.bottom() - 1)) + delta;
            const Vector2 a = store.texelOfWorld(heightId, minX, minZ);
            const Vector2 b = store.texelOfWorld(heightId, maxX, maxZ);
            const TexelRect wanted{static_cast<int>(std::floor(std::min(a.x, b.x))), static_cast<int>(std::floor(std::min(a.y, b.y))), 0, 0};
            TexelRect read = wanted;
            read.w = static_cast<int>(std::ceil(std::max(a.x, b.x))) - wanted.x + 2;
            read.h = static_cast<int>(std::ceil(std::max(a.y, b.y))) - wanted.y + 2;
            read = intersect(read, heightFormat.bounds);
            if (read.empty()) {
                // the whole block maps past the edge: the clamp lands on the edge texels
                read = intersect(TexelRect{std::clamp(wanted.x, heightFormat.bounds.x, heightFormat.bounds.right() - 1),
                                           std::clamp(wanted.y, heightFormat.bounds.y, heightFormat.bounds.bottom() - 1), 2, 2},
                                 heightFormat.bounds);
            }

            std::vector<float> heights(static_cast<size_t>(read.area()));
            if (!readMap(store, reader, readerUser, heightId, read, heights)) {
                std::fill(weights.begin(), weights.end(), 0.0f);
            } else {
                window.values = heights.data();
                window.rect = read;
                // each row on its own: rows run on the job system's workers past a size (T1 R48)
                auto filterRow = [&](size_t rowIndex) {
                    const int y = block.y + static_cast<int>(rowIndex);
                    const float z = lattice.z(y);
                    float* rowWeights = weights.data() + static_cast<size_t>(y - block.y) * block.w;
                    for (int x = block.x; x < block.right(); x++) {
                        const float px = lattice.x(x);
                        const float normalized = sampleWindow(window, px, z);
                        const float height = normalized * maxHeight;
                        float weight = 1.0f;
                        if (filter.height) {
                            const float inRange = softRange(height, filter.height->min, filter.height->max, softness);
                            weight *= filter.invertHeight ? 1.0f - inRange : inRange;
                        }
                        if (filter.slope && weight > 0.0f) {
                            // forward difference at the height map's own spacing, as the query's
                            const float dx = height - sampleWindow(window, px + delta, z) * maxHeight;
                            const float dz = height - sampleWindow(window, px, z + delta) * maxHeight;
                            const float length = std::sqrt(dx * dx + delta * delta + dz * dz);
                            const float normalY = length > 0.0f ? delta / length : 1.0f;
                            const float inRange = softSlope(normalY, filter.slope->min, filter.slope->max, softness);
                            weight *= filter.invertSlope ? 1.0f - inRange : inRange;
                        }
                        rowWeights[x - block.x] *= weight;
                    }
                };
                if (block.area() >= PARALLEL_FILTER_TEXELS) {
                    jobs::parallelFor(static_cast<size_t>(block.h), filterRow);
                } else {
                    for (size_t rowIndex = 0; rowIndex < static_cast<size_t>(block.h); rowIndex++) filterRow(rowIndex);
                }
            }
        }
    }

    if (filter.layer) {
        const LayerChannel channel = terrainLayerChannel(filter.layer->index);
        const MapId blendId{MapKind::Blend, channel.map};
        const MapFormat blendFormat = store.format(blendId);
        if (!blendFormat.exists) {
            std::fill(weights.begin(), weights.end(), 0.0f);
        } else {
            const MapLattice blendLattice = latticeOf(store, blendId, blendFormat);
            const float invX = blendLattice.stepX != 0.0f ? 1.0f / blendLattice.stepX : 1.0f;
            const float invZ = blendLattice.stepZ != 0.0f ? 1.0f / blendLattice.stepZ : 1.0f;
            auto texelOfX = [&](float x) {
                return std::clamp(static_cast<int>(std::round(static_cast<float>(blendLattice.baseX) + (x - blendLattice.originX) * invX)),
                                  blendFormat.bounds.x, blendFormat.bounds.right() - 1);
            };
            auto texelOfZ = [&](float z) {
                return std::clamp(static_cast<int>(std::round(static_cast<float>(blendLattice.baseY) + (z - blendLattice.originZ) * invZ)),
                                  blendFormat.bounds.y, blendFormat.bounds.bottom() - 1);
            };
            const int x0 = texelOfX(lattice.x(block.x)), x1 = texelOfX(lattice.x(block.right() - 1));
            const int y0 = texelOfZ(lattice.z(block.y)), y1 = texelOfZ(lattice.z(block.bottom() - 1));
            const TexelRect read{std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0) + 1, std::abs(y1 - y0) + 1};
            const int count = floatChannels(blendId, blendFormat);
            std::vector<float> blend(static_cast<size_t>(read.area()) * count);
            if (!readMap(store, reader, readerUser, blendId, read, blend)) {
                std::fill(weights.begin(), weights.end(), 0.0f);
            } else {
                for (int y = block.y; y < block.bottom(); y++) {
                    const int texelY = texelOfZ(lattice.z(y));
                    float* rowWeights = weights.data() + static_cast<size_t>(y - block.y) * block.w;
                    for (int x = block.x; x < block.right(); x++) {
                        const int texelX = texelOfX(lattice.x(x));
                        const float stored = blend[(static_cast<size_t>(texelY - read.y) * read.w + (texelX - read.x)) * count + channel.channel];
                        const float weight = channel.inverted ? 1.0f - stored : stored;
                        rowWeights[x - block.x] *= softRange(weight, filter.layer->min, filter.layer->max, softness);
                    }
                }
            }
        }
    }

    if (filter.invert) {
        for (float& weight : weights) {
            weight = 1.0f - weight;
        }
    }
}
