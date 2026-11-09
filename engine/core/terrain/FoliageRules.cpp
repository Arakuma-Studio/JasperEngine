// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/FoliageRules.h"

#include "terrain/FoliageScatter.h"
#include "terrain/TerrainDeterministic.h"
#include "terrain/TerrainHoles.h"
#include "terrain/TerrainLayers.h"
#include "terrain/TerrainSurface.h"
#include "util/Fnv.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::terraincore;

namespace {

    constexpr MapId FOLIAGE_HEIGHT_MAP{MapKind::Height, 0};
    constexpr MapId FOLIAGE_HOLE_MAP{MapKind::Hole, 0};

    // A grid over the Terrain, nearest texel, row 0 at -Z
    float foliageMaskAt(const FoliageRuleMask& mask, float terrainSize, float localX, float localZ) {
        if (mask.width <= 0 || mask.height <= 0 || mask.values.size() < static_cast<size_t>(mask.width) * mask.height) return 0.0f;
        const float half = terrainSize * 0.5f;
        const int x = std::clamp(static_cast<int>(std::floor((localX + half) / terrainSize * static_cast<float>(mask.width))), 0, mask.width - 1);
        const int y = std::clamp(static_cast<int>(std::floor((localZ + half) / terrainSize * static_cast<float>(mask.height))), 0, mask.height - 1);
        return mask.values[static_cast<size_t>(y) * mask.width + x];
    }

    inline int64_t foliageCellKey(int x, int z) {
        return (static_cast<int64_t>(x) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(z));
    }

}

float jasperengine::terraincore::foliageDistanceToPolyline(std::span<const Vector2> points, float x, float z) {
    float best = 1e30f;
    if (points.empty()) return best;
    if (points.size() == 1) {
        const float dx = x - points[0].x;
        const float dz = z - points[0].y;
        return std::sqrt(dx * dx + dz * dz);
    }
    for (size_t i = 0; i + 1 < points.size(); i++) {
        const float ax = points[i].x, az = points[i].y;
        const float bx = points[i + 1].x - ax, bz = points[i + 1].y - az;
        const float lengthSquared = bx * bx + bz * bz;
        const float t = lengthSquared > 0.0f ? std::clamp(((x - ax) * bx + (z - az) * bz) / lengthSquared, 0.0f, 1.0f) : 0.0f;
        const float dx = x - (ax + bx * t);
        const float dz = z - (az + bz * t);
        best = std::min(best, dx * dx + dz * dz);
    }
    return std::sqrt(best);
}

FoliageRuleSurface::FoliageRuleSurface(const TerrainMapStore& store): store(&store) {
    frameHeight = store.frame().maxHeight;
    const MapFormat format = store.format(FOLIAGE_HEIGHT_MAP);
    if (format.exists && !format.bounds.empty()) {
        heightWidth = format.bounds.w;
        heightHeight = format.bounds.h;
        heightChannels = format.channels;
        heightBytesPerChannel = bytesPerChannelOf(format.bitsPerChannel);
        heightBytes.resize(static_cast<size_t>(format.bounds.area()) * bytesPerTexel(format));
        if (!store.readBytes(FOLIAGE_HEIGHT_MAP, format.bounds, heightBytes)) heightBytes.clear();
        // the size the map's lattice stands for: its texels' outer edges
        const Vector2 first = store.worldOfTexel(FOLIAGE_HEIGHT_MAP, format.bounds.x, format.bounds.y);
        const Vector2 second = store.worldOfTexel(FOLIAGE_HEIGHT_MAP, format.bounds.x + 1, format.bounds.y);
        terrainSize = (second.x - first.x) * static_cast<float>(format.bounds.w);
    }
    const MapFormat holes = store.format(FOLIAGE_HOLE_MAP);
    if (holes.exists && !holes.bounds.empty()) {
        holeWidth = holes.bounds.w;
        holeHeight = holes.bounds.h;
        holeBytes.resize(static_cast<size_t>(holes.bounds.area()) * bytesPerTexel(holes));
        if (!store.readBytes(FOLIAGE_HOLE_MAP, holes.bounds, holeBytes)) holeBytes.clear();
    }
}

float FoliageRuleSurface::height(float localX, float localZ) const {
    if (heightBytes.empty()) return 0.0f;
    return frameHeight * sampleHeightLocal(heightBytes.data(), heightWidth, heightHeight, heightChannels, heightBytesPerChannel, terrainSize, localX, localZ);
}

void FoliageRuleSurface::normal(float localX, float localZ, float centre, float out[3]) const {
    const float delta = heightWidth > 0 ? terrainSize / static_cast<float>(heightWidth) : 1.0f;
    const float nx = centre - height(localX + delta, localZ);
    const float nz = centre - height(localX, localZ + delta);
    const float length = std::sqrt(nx * nx + delta * delta + nz * nz);
    out[0] = length > 0.0f ? nx / length : 0.0f;
    out[1] = length > 0.0f ? delta / length : 1.0f;
    out[2] = length > 0.0f ? nz / length : 0.0f;
}

bool FoliageRuleSurface::hole(float localX, float localZ) const {
    if (holeBytes.empty() || !(terrainSize > 0.0f)) return false;
    const HoleMapView view{holeBytes.data(), holeWidth, holeHeight};
    const float half = terrainSize * 0.5f;
    return isHole(view, (localX + half) / terrainSize, (localZ + half) / terrainSize);
}

bool FoliageRuleSurface::prepareLayers(const FoliageRuleSet& rules) {
    blends.clear();
    for (const FoliageRuleLayer& rule : rules.layers) {
        const int map = terrainLayerChannel(rule.index).map;
        bool found = false;
        for (const BlendWindow& window : blends) found = found || window.map == map;
        if (found) continue;
        const MapId id{MapKind::Blend, map};
        const MapFormat format = store->format(id);
        BlendWindow window;
        window.map = map;
        if (format.exists && !format.bounds.empty()) {
            window.width = format.bounds.w;
            window.height = format.bounds.h;
            window.channels = floatChannels(id, format);
            window.values.resize(static_cast<size_t>(format.bounds.area()) * window.channels);
            if (!store->read(id, format.bounds, window.values)) window.values.clear();
        }
        blends.push_back(std::move(window));
    }
    return true;
}

float FoliageRuleSurface::layerWeight(int layer, float localX, float localZ) const {
    // T3 R15: layers 9-11 weigh by a blend map's alpha, inverted; the one mapping says which
    const LayerChannel route = terrainLayerChannel(layer);
    const int map = route.map;
    const int channel = route.channel;
    const float half = terrainSize * 0.5f;
    for (const BlendWindow& window : blends) {
        if (window.map != map) continue;
        if (window.values.empty() || channel >= window.channels || !(terrainSize > 0.0f)) return 0.0f;
        const int x = std::clamp(static_cast<int>(std::floor((localX + half) / terrainSize * static_cast<float>(window.width))), 0, window.width - 1);
        const int y = std::clamp(static_cast<int>(std::floor((localZ + half) / terrainSize * static_cast<float>(window.height))), 0, window.height - 1);
        const float stored = window.values[(static_cast<size_t>(y) * window.width + x) * window.channels + channel];
        return route.inverted ? 1.0f - stored : stored;
    }
    return 0.0f;
}

bool FoliageRuleSurface::passes(const FoliageRuleSet& rules, float localX, float localZ, FoliageRejections* rejected, float* heightOut, float normalOut[3], const float* maskShare) const {
    const auto reject = [&](int64_t FoliageRejections::*counter) {
        if (rejected) (rejected->*counter)++;
        return false;
    };

    if (rules.excludeHoles && hole(localX, localZ)) return reject(&FoliageRejections::hole);

    for (const FoliageSplineZone& zone : rules.excludeSplines) {
        if (foliageDistanceToPolyline(zone.points, localX, localZ) <= zone.margin) return reject(&FoliageRejections::spline);
    }

    const float surface = height(localX, localZ);
    if (heightOut) *heightOut = surface;
    if (rules.height && (surface < rules.height->min || surface > rules.height->max)) return reject(&FoliageRejections::height);

    float up[3] = {0.0f, 1.0f, 0.0f};
    normal(localX, localZ, surface, up);
    if (normalOut) std::copy(up, up + 3, normalOut);
    if (rules.slope) {
        const float normalY = std::clamp(up[1], -1.0f, 1.0f);
        if ((rules.slope->min > 0.0f && normalY > detCos(detRadians(rules.slope->min))) || normalY < detCos(detRadians(rules.slope->max))) {
            return reject(&FoliageRejections::slope);
        }
    }

    for (const FoliageRuleLayer& rule : rules.layers) {
        const float weight = layerWeight(rule.index, localX, localZ);
        if (weight < rule.min || weight > rule.max) return reject(&FoliageRejections::layer);
    }

    if (rules.noiseMask && foliageMaskAt(*rules.noiseMask, terrainSize, localX, localZ) < rules.noiseMask->threshold) {
        return reject(&FoliageRejections::noise);
    }
    if (rules.densityMask) {
        // with a draw, a soft mask thins: a hard one is all or nothing
        const float value = foliageMaskAt(*rules.densityMask, terrainSize, localX, localZ);
        const float share = maskShare ? *maskShare : 0.5f;
        if (share >= value) return reject(&FoliageRejections::noise);
    }
    return true;
}

void jasperengine::terraincore::foliageSetTreeSurface(TreeRecord& record, float terrainSize, float terrainMaxHeight, float localX, float localZ, float surfaceHeight) {
    record.u = localX / terrainSize + 0.5f;
    record.v = localZ / terrainSize + 0.5f;
    record.height = terrainMaxHeight != 0.0f ? surfaceHeight / terrainMaxHeight : 0.0f;
}

void jasperengine::terraincore::foliageInstanceFromTree(const TreeRecord& record, float terrainSize, float terrainMaxHeight, uint8_t windBend, const float* surfaceNormal, FoliageInstance& out) {
    out.position[0] = foliageTreeLocalX(record, terrainSize);
    out.position[1] = record.height * terrainMaxHeight + record.yOffset;
    out.position[2] = foliageTreeLocalZ(record, terrainSize);
    out.scale = record.scale;
    const float share = treeAlignShare(record.flags);
    if (surfaceNormal && share > 0.0f) {
        // the stored yaw under a tilt towards the surface, the product foliageOrientation makes
        const float length = std::sqrt(surfaceNormal[2] * surfaceNormal[2] + surfaceNormal[0] * surfaceNormal[0]);
        if (length > 1.1920929e-07f) {
            const float halfTilt = foliageAcos(surfaceNormal[1]) * 0.5f * share;
            const float sine = detSin(halfTilt);
            const float tx = surfaceNormal[2] / length * sine;
            const float tz = -surfaceNormal[0] / length * sine;
            const float tw = detCos(halfTilt);
            const float y = static_cast<float>(record.rotation[1]) / 32767.0f;
            const float w = static_cast<float>(record.rotation[3]) / 32767.0f;
            const float x0 = static_cast<float>(record.rotation[0]) / 32767.0f;
            const float z0 = static_cast<float>(record.rotation[2]) / 32767.0f;
            const float rotation[4] = {
                tw * x0 + tx * w - tz * y,
                tw * y + tz * x0 - tx * z0,
                tw * z0 + tz * w + tx * y,
                tw * w - tz * z0 - tx * x0
            };
            foliageQuantizeRotation(rotation, out.rotation);
        } else {
            std::memcpy(out.rotation, record.rotation, sizeof(out.rotation));
        }
    } else {
        std::memcpy(out.rotation, record.rotation, sizeof(out.rotation));
    }
    std::memcpy(out.color, record.color, sizeof(out.color));
    out.heightRatio = record.heightRatio;
    // what tells one tree from another, stable across resizes and edits
    const unsigned int hash = mixFoliage(record.id);
    out.windPhase = static_cast<uint8_t>(hash >> 24);
    out.windBend = windBend;
    out.densityRank = static_cast<uint8_t>((hash >> 16) & 0xFF);
}

uint64_t jasperengine::terraincore::foliageTreesHash(std::span<const TreeRecord> trees, uint64_t previous) {
    uint64_t hash = previous;
    for (TreeRecord record : trees) {
        record.id = 0;
        hash = fnv1a64(&record, sizeof(TreeRecord), hash);
    }
    return hash;
}

FoliageTreeScatterResult jasperengine::terraincore::scatterTreesByRules(const TerrainMapStore& store, const FoliageTreeScatter& scatter, std::span<const Vector2> existing, FoliageStopFn stop, void* stopUser) {
    FoliageTreeScatterResult result;
    result.hash = foliageTreesHash({});

    FoliageRuleSurface surface(store);
    const float size = surface.size();
    if (!surface.ready() || !(size > 0.0f) || scatter.targets.empty() || !(scatter.density > 0.0f)) {
        return result;
    }
    surface.prepareLayers(scatter.rules);

    float totalWeight = 0.0f;
    for (const FoliageTreeTarget& target : scatter.targets) totalWeight += std::max(target.weight, 0.0f);
    if (!(totalWeight > 0.0f)) return result;

    const float half = size * 0.5f;
    const ShapeEvaluator shape(scatter.region);
    LocalBounds bounds;
    if (!shape.bounds(bounds)) {
        bounds.min = Vector2(-half, -half);
        bounds.max = Vector2(half, half);
    }
    const float minX = std::max(bounds.min.x, -half);
    const float minZ = std::max(bounds.min.y, -half);
    const float maxX = std::min(bounds.max.x, half);
    const float maxZ = std::min(bounds.max.y, half);
    if (maxX < minX || maxZ < minZ) return result;

    const int firstX = static_cast<int>(std::floor(minX / FOLIAGE_TREE_CELL_SIZE));
    const int firstZ = static_cast<int>(std::floor(minZ / FOLIAGE_TREE_CELL_SIZE));
    const int lastX = static_cast<int>(std::floor(maxX / FOLIAGE_TREE_CELL_SIZE));
    const int lastZ = static_cast<int>(std::floor(maxZ / FOLIAGE_TREE_CELL_SIZE));

    const float spacing = scatter.rules.minSpacing;
    const float spacingSquared = spacing * spacing;
    // spacing's reach, in cells of at least the spacing: only earlier trees and neighbouring cells (R24)
    const float gridSize = std::max(spacing, FOLIAGE_TREE_CELL_SIZE);
    std::unordered_map<int64_t, std::vector<Vector2>> grid;
    const auto gridOf = [&](float v) { return static_cast<int>(std::floor(v / gridSize)); };
    if (spacing > 0.0f) {
        for (const Vector2& point : existing) {
            grid[foliageCellKey(gridOf(point.x), gridOf(point.y))].push_back(point);
        }
    }

    const float expected = scatter.density * FOLIAGE_TREE_CELL_SIZE * FOLIAGE_TREE_CELL_SIZE;
    const unsigned int attempts = static_cast<unsigned int>(std::ceil(std::clamp(expected, 0.0f, 1048576.0f)));
    const uint8_t white[4] = {255, 255, 255, 255};
    const float scaleRange = scatter.jitter.maxScale - scatter.jitter.minScale;
    const float ratioRange = scatter.jitter.maxHeightRatio - scatter.jitter.minHeightRatio;

    uint64_t hash = foliageTreesHash({});
    for (int cellZ = firstZ; cellZ <= lastZ; cellZ++) {
        for (int cellX = firstX; cellX <= lastX; cellX++) {
            if (stop && stop(stopUser)) {
                result.stopped = true;
                return result;
            }
            const float originX = static_cast<float>(cellX) * FOLIAGE_TREE_CELL_SIZE;
            const float originZ = static_cast<float>(cellZ) * FOLIAGE_TREE_CELL_SIZE;
            const size_t firstTree = result.trees.size();
            for (unsigned int i = 0; i < attempts; i++) {
                const unsigned int hashX = hashFoliage(scatter.seed, static_cast<unsigned int>(cellX), static_cast<unsigned int>(cellZ), i);
                const unsigned int hashZ = mixFoliage(hashX);
                const unsigned int hashAccept = mixFoliage(hashZ);
                const unsigned int hashMask = mixFoliage(hashAccept);
                const unsigned int hashPick = mixFoliage(hashMask);
                const unsigned int hashScale = mixFoliage(hashPick);
                const unsigned int hashRatio = mixFoliage(hashScale);
                const unsigned int hashYaw = mixFoliage(hashRatio);

                result.attempts++;
                // a fractional last attempt keeps sparse density from rounding away
                if (i + 1 == attempts && randomFoliage(hashAccept) >= expected - static_cast<float>(i)) {
                    continue;
                }

                const float x = originX + randomFoliage(hashX) * FOLIAGE_TREE_CELL_SIZE;
                const float z = originZ + randomFoliage(hashZ) * FOLIAGE_TREE_CELL_SIZE;
                if (x < -half || x > half || z < -half || z > half || !shape.sample(x, z).inside) {
                    result.rejected.outside++;
                    continue;
                }

                float height = 0.0f;
                float normal[3] = {0.0f, 1.0f, 0.0f};
                const float maskShare = randomFoliage(hashMask);
                if (!surface.passes(scatter.rules, x, z, &result.rejected, &height, normal, &maskShare)) {
                    continue;
                }

                if (spacing > 0.0f) {
                    const int gx = gridOf(x);
                    const int gz = gridOf(z);
                    bool tooClose = false;
                    for (int oz = -1; oz <= 1 && !tooClose; oz++) {
                        for (int ox = -1; ox <= 1 && !tooClose; ox++) {
                            const auto it = grid.find(foliageCellKey(gx + ox, gz + oz));
                            if (it == grid.end()) continue;
                            for (const Vector2& other : it->second) {
                                const float dx = other.x - x;
                                const float dz = other.y - z;
                                if (dx * dx + dz * dz < spacingSquared) {
                                    tooClose = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (tooClose) {
                        result.rejected.spacing++;
                        continue;
                    }
                    grid[foliageCellKey(gx, gz)].push_back(Vector2(x, z));
                }

                float pick = randomFoliage(hashPick) * totalWeight;
                uint16_t prototype = scatter.targets.back().prototypeId;
                for (const FoliageTreeTarget& target : scatter.targets) {
                    const float weight = std::max(target.weight, 0.0f);
                    if (pick < weight) {
                        prototype = target.prototypeId;
                        break;
                    }
                    pick -= weight;
                }

                TreeRecord record{};
                record.id = 0;
                foliageSetTreeSurface(record, size, surface.maxHeight(), x, z, height);
                record.yOffset = 0.0f;
                record.scale = scatter.jitter.minScale + randomFoliage(hashScale) * scaleRange;
                float rotation[4];
                foliageOrientation(randomFoliage(hashYaw) * scatter.jitter.yaw, 0.0f, normal, rotation);
                foliageQuantizeRotation(rotation, record.rotation);
                record.prototypeId = prototype;
                const float ratio = scatter.jitter.minHeightRatio + randomFoliage(hashRatio) * ratioRange;
                record.heightRatio = static_cast<uint8_t>(std::clamp(std::floor(ratio * 128.0f + 0.5f), 1.0f, 255.0f));
                record.flags = TREE_SNAP_TO_TERRAIN | treeAlignFlags(scatter.jitter.alignToNormal);
                std::memcpy(record.color, white, sizeof(record.color));
                result.trees.push_back(record);
            }
            hash = foliageTreesHash(std::span<const TreeRecord>(result.trees).subspan(firstTree), hash);
        }
    }
    result.hash = hash;
    return result;
}

FoliageDensityScatterResult jasperengine::terraincore::scatterDensityByRules(const TerrainMapStore& store, const FoliageDensityScatter& scatter, FoliageStopFn stop, void* stopUser) {
    FoliageDensityScatterResult result;

    const MapId map{MapKind::Density, scatter.layerIndex};
    const MapFormat format = store.format(map);
    if (!format.exists || format.bounds.empty()) {
        result.mapMissing = true;
        return result;
    }

    FoliageRuleSurface surface(store);
    if (!surface.ready() && (scatter.rules.height || scatter.rules.slope)) {
        return result;
    }
    surface.prepareLayers(scatter.rules);

    const ShapeEvaluator shape(scatter.region);
    const TexelRect rect = shapeTexels(store, map, format, shape);
    if (rect.empty()) return result;

    result.rect = rect;
    result.values.resize(static_cast<size_t>(rect.area()));
    if (!store.read(map, rect, result.values)) {
        result.values.clear();
        result.rect = TexelRect{};
        return result;
    }

    // on the map's own steps, so a remove of what an add wrote comes back to exactly 0
    const float wanted = scatter.layerDensity > 0.0f ? detClamp01(scatter.density / scatter.layerDensity) : 0.0f;
    const float top = format.bitsPerChannel >= 16 ? 65535.0f : 255.0f;
    const float absolute = static_cast<float>(quantizeChannel(wanted, format.bitsPerChannel)) / top;
    const MapLattice lattice = latticeOf(store, map, format);
    for (int y = 0; y < rect.h; y++) {
        if (stop && stop(stopUser)) {
            result.values.clear();
            result.rect = TexelRect{};
            return result;
        }
        const float z = lattice.z(rect.y + y);
        for (int x = 0; x < rect.w; x++) {
            const float px = lattice.x(rect.x + x);
            if (!shape.sample(px, z).inside) continue;

            float& value = result.values[static_cast<size_t>(y) * rect.w + x];
            const float noMask = 0.0f;
            const bool pass = surface.ready() ? surface.passes(scatter.rules, px, z, &result.rejected, nullptr, nullptr, &noMask) : true;
            switch (scatter.mode) {
                case FoliageScatterMode::Replace:
                    value = pass ? absolute : 0.0f;
                    break;
                case FoliageScatterMode::Add:
                    if (pass) value = detClamp01(value + absolute);
                    break;
                case FoliageScatterMode::Remove:
                    if (pass) value = detClamp01(value - absolute);
                    break;
            }
            if (pass) result.texelsSet++;
        }
    }
    return result;
}
