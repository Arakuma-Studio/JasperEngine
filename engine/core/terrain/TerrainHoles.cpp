// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainHoles.h"

#include <algorithm>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // The texels [first, last] of a map of size texels that lattice span [index, index + 1] of a
    // spans-long lattice overlaps; touching at an edge is not overlapping. Integer arithmetic, so
    // that lattice and map of the same size agree exactly
    void texelSpan(int index, int spans, int size, int& first, int& last) {
        const int64_t from = static_cast<int64_t>(index) * size;
        const int64_t to = static_cast<int64_t>(index + 1) * size;
        first = static_cast<int>(from / spans);
        last = static_cast<int>((to + spans - 1) / spans) - 1;
        if (last >= size) last = size - 1;
    }

    inline bool holeTexel(const HoleMapView& map, int x, int y) {
        return map.texels[static_cast<size_t>(y) * map.width + x] < 128;
    }

    // The rectangle in pieces no larger than a store's block (S53 R87)
    template<typename Visit>
    void forEachBlock(const TerrainMapStore& store, MapId map, TexelRect rect, Visit&& visit) {
        const TexelRect preferred = store.preferredBlock(map);
        const int stepX = preferred.w > 0 ? preferred.w : rect.w;
        const int stepY = preferred.h > 0 ? preferred.h : rect.h;
        for (int y = rect.y; y < rect.bottom(); y += stepY) {
            for (int x = rect.x; x < rect.right(); x += stepX) {
                const TexelRect block = intersect(TexelRect{x, y, stepX, stepY}, rect);
                if (!block.empty()) visit(block);
            }
        }
    }

}

void jasperengine::terrain::openSquares(const HoleMapView& map, int samples, float mirrorX, float mirrorZ, std::vector<uint8_t>& open) {
    const int spans = samples - 1;
    open.assign(spans > 0 ? static_cast<size_t>(spans) * spans : 0, 0);
    if (!map.texels || map.width <= 0 || map.height <= 0 || spans <= 0) return;
    for (int j = 0; j < spans; j++) {
        int firstY = 0, lastY = 0;
        texelSpan(mirrorZ < 0.0f ? spans - 1 - j : j, spans, map.height, firstY, lastY);
        for (int i = 0; i < spans; i++) {
            int firstX = 0, lastX = 0;
            texelSpan(mirrorX < 0.0f ? spans - 1 - i : i, spans, map.width, firstX, lastX);
            bool allHoles = true;
            for (int y = firstY; allHoles && y <= lastY; y++) {
                for (int x = firstX; x <= lastX; x++) {
                    if (!holeTexel(map, x, y)) {
                        allHoles = false;
                        break;
                    }
                }
            }
            open[static_cast<size_t>(j) * spans + i] = allHoles ? 1 : 0;
        }
    }
}

int jasperengine::terrain::noCollisionSamples(const std::vector<uint8_t>& open, int samples, std::vector<uint8_t>& out) {
    out.assign(samples > 0 ? static_cast<size_t>(samples) * samples : 0, 0);
    const int spans = samples - 1;
    if (spans <= 0 || open.size() != static_cast<size_t>(spans) * spans) return 0;
    int count = 0;
    for (int y = 0; y < samples; y++) {
        for (int x = 0; x < samples; x++) {
            bool allOpen = true;
            for (int j = std::max(y - 1, 0); allOpen && j <= std::min(y, spans - 1); j++) {
                for (int i = std::max(x - 1, 0); i <= std::min(x, spans - 1); i++) {
                    if (!open[static_cast<size_t>(j) * spans + i]) {
                        allOpen = false;
                        break;
                    }
                }
            }
            if (allOpen) {
                out[static_cast<size_t>(y) * samples + x] = 1;
                count++;
            }
        }
    }
    return count;
}

int jasperengine::terrain::navHoleCells(const HoleMapView& map, int samples, std::vector<uint8_t>& cells) {
    const int spans = samples - 1;
    cells.assign(spans > 0 ? static_cast<size_t>(spans) * spans : 0, 0);
    if (!map.texels || map.width <= 0 || map.height <= 0 || spans <= 0) return 0;
    int count = 0;
    for (int j = 0; j < spans; j++) {
        int firstY = 0, lastY = 0;
        texelSpan(j, spans, map.height, firstY, lastY);
        for (int i = 0; i < spans; i++) {
            int firstX = 0, lastX = 0;
            texelSpan(i, spans, map.width, firstX, lastX);
            bool anyHole = false;
            for (int y = firstY; !anyHole && y <= lastY; y++) {
                for (int x = firstX; x <= lastX; x++) {
                    if (holeTexel(map, x, y)) {
                        anyHole = true;
                        break;
                    }
                }
            }
            if (anyHole) {
                cells[static_cast<size_t>(j) * spans + i] = 1;
                count++;
            }
        }
    }
    return count;
}

void jasperengine::terrain::holeOverlayClasses(const HoleMapView& map, int collisionSamples, int navSamples, std::vector<uint8_t>& out) {
    out.assign(map.texels && map.width > 0 && map.height > 0 ? static_cast<size_t>(map.width) * map.height : 0, SOLID_VALUE);
    if (out.empty()) return;
    std::vector<uint8_t> open, gaps, cells;
    openSquares(map, collisionSamples, 1.0f, 1.0f, open);
    noCollisionSamples(open, collisionSamples, gaps);
    navHoleCells(map, navSamples, cells);
    const int gapSpans = collisionSamples - 1;
    const int navSpans = navSamples - 1;
    const auto sampleGap = [&](int x, int y) {
        return gaps[static_cast<size_t>(y) * collisionSamples + x] != 0;
    };
    for (int y = 0; y < map.height; y++) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(map.height);
        for (int x = 0; x < map.width; x++) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(map.width);
            const size_t at = static_cast<size_t>(y) * map.width + x;
            if (holeTexel(map, x, y)) {
                // a texel over a square whose triangles all lost a corner has no collider left under it
                const int i = std::min(static_cast<int>(u * static_cast<float>(gapSpans)), gapSpans - 1);
                const int j = std::min(static_cast<int>(v * static_cast<float>(gapSpans)), gapSpans - 1);
                const bool open0 = sampleGap(i, j) || sampleGap(i + 1, j + 1) || (sampleGap(i + 1, j) && sampleGap(i, j + 1));
                out[at] = open0 ? HOLE_OVERLAY_DEEP : HOLE_OVERLAY_RIM;
            } else {
                const int i = std::min(static_cast<int>(u * static_cast<float>(navSpans)), navSpans - 1);
                const int j = std::min(static_cast<int>(v * static_cast<float>(navSpans)), navSpans - 1);
                if (cells[static_cast<size_t>(j) * navSpans + i]) out[at] = HOLE_OVERLAY_NAV;
            }
        }
    }
}

int64_t jasperengine::terrain::countHoleTexels(const HoleMapView& map, int x0, int y0, int x1, int y1) {
    if (!map.texels || map.width <= 0 || map.height <= 0) return 0;
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, map.width);
    y1 = std::min(y1, map.height);
    int64_t count = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (holeTexel(map, x, y)) count++;
        }
    }
    return count;
}

int jasperengine::terrain::rasterizeRegion(TerrainMapStore& store, const Shape& region, uint8_t value, TexelRect* changed,
                                           const Filter* filter, HoleRegionResult* detail) {
    const MapId holeId{MapKind::Hole, 0};
    const MapId heightId{MapKind::Height, 0};
    const bool cutting = value == HOLE_VALUE;
    const float target = cutting ? 0.0f : 1.0f;

    HoleRegionResult result;
    TexelRect changedRect;
    MapFormat format = store.format(holeId);

    // Before any map exists, the region is measured on the lattice the map will have
    const MapFormat latticeFormat = format.exists ? format : store.format(heightId);
    if (!latticeFormat.exists) {
        if (detail) *detail = result;
        if (changed) *changed = changedRect;
        return 0;
    }
    if (!format.exists && !cutting) {
        // nothing to fill
        if (detail) *detail = result;
        if (changed) *changed = changedRect;
        return 0;
    }

    const MapId latticeId = format.exists ? holeId : heightId;
    const MapLattice lattice = latticeOf(store, latticeId, latticeFormat);
    const ShapeEvaluator evaluator(region);
    const TexelRect rect = shapeTexels(store, latticeId, latticeFormat, evaluator);

    std::vector<float> weights;
    std::vector<float> values;
    bool created = false;

    forEachBlock(store, latticeId, rect, [&](const TexelRect& block) {
        weights.resize(static_cast<size_t>(block.area()));
        if (filter) {
            evaluateFilter(store, *filter, lattice, block, weights);
        } else {
            std::fill(weights.begin(), weights.end(), 1.0f);
        }
        values.resize(static_cast<size_t>(block.area()));
        if (format.exists) {
            if (!store.read(holeId, block, values)) return;
        } else {
            std::fill(values.begin(), values.end(), 1.0f);
        }

        int blockChanged = 0;
        for (int y = block.y; y < block.bottom(); y++) {
            const float z = lattice.z(y);
            for (int x = block.x; x < block.right(); x++) {
                const ShapeSample sample = evaluator.sample(lattice.x(x), z);
                if (!sample.inside) continue;
                result.inRegion++;
                const size_t index = static_cast<size_t>(y - block.y) * block.w + (x - block.x);
                // a filter's soft band is decided at one half, as a mask's threshold is
                if (weights[index] < 0.5f) {
                    result.filteredOut++;
                    continue;
                }
                const bool isHoleNow = values[index] < 0.5f;
                if (isHoleNow == cutting) continue;
                values[index] = target;
                blockChanged++;
                changedRect = unite(changedRect, TexelRect{x, y, 1, 1});
            }
        }
        if (blockChanged == 0) return;
        result.changed += blockChanged;

        if (!format.exists) {
            MapFormat newFormat = latticeFormat;
            newFormat.channels = 1;
            newFormat.bitsPerChannel = 8;
            newFormat.exists = true;
            if (!store.createMap(holeId, newFormat, 1.0f).ok) {
                result.changed = 0;
                return;
            }
            created = true;
            format = store.format(holeId);
        }
        store.write(holeId, block, values);
    });

    result.mapCreated = created;

    // The map's own count, so a repeated or partial fill knows when the last hole went
    if (format.exists) {
        int64_t holes = 0;
        forEachBlock(store, holeId, format.bounds, [&](const TexelRect& block) {
            values.resize(static_cast<size_t>(block.area()));
            if (!store.read(holeId, block, values)) return;
            for (float stored : values) {
                if (stored < 0.5f) holes++;
            }
        });
        result.holeTexels = holes;
        const int64_t total = format.bounds.area();
        result.holeFraction = total > 0 ? static_cast<float>(static_cast<double>(holes) / static_cast<double>(total)) : 0.0f;

        if (!cutting && holes == 0 && store.canRemoveMap(holeId)) {
            result.mapRemoved = store.removeMap(holeId).ok;
        }
    }

    if (detail) *detail = result;
    if (changed) *changed = changedRect;
    return result.changed;
}
