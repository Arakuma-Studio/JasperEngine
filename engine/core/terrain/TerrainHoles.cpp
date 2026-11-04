// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainHoles.h"

#include <algorithm>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // The rectangle in pieces no larger than a store's block (T1 R87)
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
