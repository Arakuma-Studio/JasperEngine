// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "foliage/FoliageOverlays.h"

#include "component/TerrainComponent.h"
#include "subsystem/FoliageSystem.h"
#include "terrain/FoliageRules.h"
#include "terrain/TerrainOverlay.h"
#include "terrain/TreeStore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using terrain::MapId;
using terrain::MapKind;

namespace {

    // trees are counted per cell of this grid, so a big image does not read as single dots
    constexpr int TREE_OVERLAY_GRID = 96;

    // blue, through green and yellow, to red; nothing at all stays faint
    Vector4 foliageHeat(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        const float r = std::clamp(t * 2.0f - 0.5f, 0.0f, 1.0f);
        const float g = t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
        const float b = std::clamp(1.0f - t * 2.0f, 0.0f, 1.0f);
        return Vector4(r, g, b, t > 0.0f ? 1.0f : 0.15f);
    }

    int foliageOverlayLayer(const TerrainOverlayBake& context) {
        return std::max(context.settings->layer, 0);
    }

    // Each texel's value per square unit, in the image's order
    std::vector<float> densityValues(const TerrainOverlayBake& context) {
        std::vector<float> values(static_cast<size_t>(context.width) * context.height, 0.0f);
        const int layer = foliageOverlayLayer(context);
        if (layer >= static_cast<int>(context.terrain->foliageLayers.size())) return values;
        const MapId map{MapKind::Density, layer};
        const terrain::MapFormat format = context.store->format(map);
        if (!format.exists || format.bounds.empty()) return values;
        std::vector<float> densities(static_cast<size_t>(format.bounds.area()));
        if (!context.store->read(map, format.bounds, densities)) return values;
        const float scale = context.terrain->foliageLayers[layer].density;
        for (int y = 0; y < context.height; y++) {
            for (int x = 0; x < context.width; x++) {
                const Vector2 local = context.localOf(x, y);
                const Vector2 at = context.store->texelOfWorld(map, local.x, local.y);
                const int tx = std::clamp(static_cast<int>(std::lround(at.x)), 0, format.bounds.w - 1);
                const int ty = std::clamp(static_cast<int>(std::lround(at.y)), 0, format.bounds.h - 1);
                values[static_cast<size_t>(y) * context.width + x] = densities[static_cast<size_t>(ty) * format.bounds.w + tx] * scale;
            }
        }
        return values;
    }

    std::vector<float> treeValues(const TerrainOverlayBake& context) {
        std::vector<float> values(static_cast<size_t>(context.width) * context.height, 0.0f);
        const float size = context.terrain->terrainSize;
        const FoliageSystem* foliage = context.scene ? context.scene->getSystem<FoliageSystem>().get() : nullptr;
        const std::shared_ptr<const terrain::TreeStore> trees = foliage ? foliage->getTreeStore(context.entity) : nullptr;
        if (!trees || size <= 0.0f) return values;
        const float half = size * 0.5f;
        const float cell = size / static_cast<float>(TREE_OVERLAY_GRID);
        std::vector<float> cells(static_cast<size_t>(TREE_OVERLAY_GRID) * TREE_OVERLAY_GRID, 0.0f);
        for (const terrain::TreeRecord& record : trees->records()) {
            const int cx = std::clamp(static_cast<int>((terrain::foliageTreeLocalX(record, size) + half) / cell), 0, TREE_OVERLAY_GRID - 1);
            const int cz = std::clamp(static_cast<int>((terrain::foliageTreeLocalZ(record, size) + half) / cell), 0, TREE_OVERLAY_GRID - 1);
            cells[static_cast<size_t>(cz) * TREE_OVERLAY_GRID + cx] += 1.0f / (cell * cell);
        }
        for (int y = 0; y < context.height; y++) {
            for (int x = 0; x < context.width; x++) {
                const Vector2 local = context.localOf(x, y);
                const int cx = std::clamp(static_cast<int>((local.x + half) / cell), 0, TREE_OVERLAY_GRID - 1);
                const int cz = std::clamp(static_cast<int>((local.y + half) / cell), 0, TREE_OVERLAY_GRID - 1);
                values[static_cast<size_t>(y) * context.width + x] = cells[static_cast<size_t>(cz) * TREE_OVERLAY_GRID + cx];
            }
        }
        return values;
    }

    float largestValue(const std::vector<float>& values) {
        float top = 0.0f;
        for (float value : values) top = std::max(top, value);
        return top;
    }

    void bakeHeat(const std::vector<float>& values, std::vector<uint8_t>& rgba) {
        const float top = largestValue(values);
        const float scale = top > 0.0f ? 1.0f / top : 0.0f;
        for (size_t i = 0; i < values.size(); i++) {
            const Vector4 color = foliageHeat(values[i] * scale);
            rgba[i * 4 + 0] = static_cast<uint8_t>(std::lround(color.x * 255.0f));
            rgba[i * 4 + 1] = static_cast<uint8_t>(std::lround(color.y * 255.0f));
            rgba[i * 4 + 2] = static_cast<uint8_t>(std::lround(color.z * 255.0f));
            rgba[i * 4 + 3] = static_cast<uint8_t>(std::lround(color.w * 255.0f));
        }
    }

    std::vector<TerrainOverlayLegendStop> heatLegend(const std::vector<float>& values) {
        const float top = largestValue(values);
        const auto label = [](float value) {
            char text[48];
            std::snprintf(text, sizeof(text), "%.3g per m2", value);
            return std::string(text);
        };
        return {{"none", foliageHeat(0.0f)}, {label(top * 0.5f), foliageHeat(0.5f)}, {label(top), foliageHeat(1.0f)}};
    }

}

void editor::registerFoliageOverlays() {
    TerrainOverlay::registerSource("density",
        [](const TerrainOverlayBake& context, std::vector<uint8_t>& rgba) { bakeHeat(densityValues(context), rgba); },
        [](const TerrainOverlayBake& context) { return heatLegend(densityValues(context)); });
    TerrainOverlay::registerSource("tree_density",
        [](const TerrainOverlayBake& context, std::vector<uint8_t>& rgba) { bakeHeat(treeValues(context), rgba); },
        [](const TerrainOverlayBake& context) { return heatLegend(treeValues(context)); });
}
