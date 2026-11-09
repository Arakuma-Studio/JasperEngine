// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOLIAGE_RULES_H
#define FOLIAGE_RULES_H

// scatter_by_rules (T5 R24): where foliage goes by slope, height, surface weights, a mask, holes and
// spline zones. Pure and deterministic: the same inputs give the same bytes on every target. A dry
// run and a real run are the same call, so their counts match exactly.

#include "Export.h"
#include "terrain/FoliageTypes.h"
#include "terrain/TerrainMapStore.h"
#include "terrain/TerrainShape.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace jasperengine::terraincore {

    enum class FoliageScatterMode : uint8_t { Replace, Add, Remove };

    struct FoliageRuleLayer {
        int index = 0;
        float min = 0.0f;
        float max = 1.0f;
    };

    // A normalized grid over the whole Terrain, row 0 at -Z (a mask the caller evaluated or read)
    struct FoliageRuleMask {
        std::span<const float> values;
        int width = 0, height = 0;
        float threshold = 0.0f;
    };

    // One polyline of a spline zone, terrain-local XZ
    struct FoliageSplineZone {
        std::vector<Vector2> points;
        float margin = 0.0f;
    };

    struct FoliageRuleSet {
        std::optional<FilterRange> slope;       // degrees
        std::optional<FilterRange> height;      // terrain-local
        std::vector<FoliageRuleLayer> layers;
        std::optional<FoliageRuleMask> noiseMask;
        std::optional<FoliageRuleMask> densityMask;     // a density map as mask: instances pass with its value
        bool excludeHoles = true;
        std::vector<FoliageSplineZone> excludeSplines;
        float minSpacing = 0.0f;                // trees only
    };

    struct FoliageJitter {
        float minScale = 1.0f, maxScale = 1.0f;
        float minHeightRatio = 1.0f, maxHeightRatio = 1.0f;
        float yaw = 1.0f;                       // share of a full turn
        float alignToNormal = 0.0f;
    };

    struct FoliageRejections {
        int64_t slope = 0;
        int64_t height = 0;
        int64_t layer = 0;
        int64_t noise = 0;
        int64_t hole = 0;
        int64_t spline = 0;
        int64_t spacing = 0;
        int64_t outside = 0;
    };

    struct FoliageTreeTarget {
        uint16_t prototypeId = 0;
        float weight = 1.0f;
    };

    struct FoliageTreeScatter {
        Shape region;
        uint32_t seed = 0;
        float density = 0.0f;                   // trees per square unit
        std::vector<FoliageTreeTarget> targets;
        FoliageRuleSet rules;
        FoliageJitter jitter;
    };

    struct FoliageTreeScatterResult {
        std::vector<TreeRecord> trees;          // id 0, in canonical order (cell Z, cell X, attempt)
        FoliageRejections rejected;
        int64_t attempts = 0;
        uint64_t hash = 0;
        bool stopped = false;                   // shouldStop said so: the result is partial and unusable
    };

    // Reads what it needs through the store (the transaction, inside an op). existing: trees spacing
    // keeps clear of, in terrain-local XZ; a mode that replaces gives none. Stop is polled per cell.
    using FoliageStopFn = bool (*)(void* user);
    JASPERENGINE_API FoliageTreeScatterResult scatterTreesByRules(const TerrainMapStore& store, const FoliageTreeScatter& scatter, std::span<const Vector2> existing,
                                                                  FoliageStopFn stop = nullptr, void* stopUser = nullptr);

    // The maps the rules read, copied out of the store: whether a position passes the rules that do
    // not depend on other trees
    class JASPERENGINE_API FoliageRuleSurface {
    public:
        explicit FoliageRuleSurface(const TerrainMapStore& store);

        bool ready() const { return !heightBytes.empty(); }
        float size() const { return terrainSize; }
        float maxHeight() const { return frameHeight; }
        float height(float localX, float localZ) const;
        void normal(float localX, float localZ, float height, float out[3]) const;
        bool hole(float localX, float localZ) const;
        // A layer's weight at a position, nearest texel; 0 without its blend map
        float layerWeight(int layer, float localX, float localZ) const;
        // Counts the rule that turned it down. maskShare: the draw a soft density mask compares
        bool passes(const FoliageRuleSet& rules, float localX, float localZ, FoliageRejections* rejected, float* heightOut, float normalOut[3], const float* maskShare = nullptr) const;
        bool prepareLayers(const FoliageRuleSet& rules);

    private:
        const TerrainMapStore* store;
        float terrainSize = 0.0f;
        float frameHeight = 0.0f;
        std::vector<uint8_t> heightBytes;
        int heightWidth = 0, heightHeight = 0, heightChannels = 1, heightBytesPerChannel = 1;
        std::vector<uint8_t> holeBytes;
        int holeWidth = 0, holeHeight = 0;
        struct BlendWindow {
            int map = -1;
            int width = 0, height = 0, channels = 0;
            std::vector<float> values;
        };
        std::vector<BlendWindow> blends;
    };

    // Distance to a polyline, the spline zones' test
    JASPERENGINE_API float foliageDistanceToPolyline(std::span<const Vector2> points, float x, float z);

    // A detail layer's density map by rules: the map's normalized value per texel in the region
    struct FoliageDensityScatter {
        Shape region;
        int layerIndex = 0;
        float density = 0.0f;                   // instances per square unit, absolute
        float layerDensity = 1.0f;              // the layer's own full-map density
        FoliageScatterMode mode = FoliageScatterMode::Replace;
        FoliageRuleSet rules;
    };

    struct FoliageDensityScatterResult {
        TexelRect rect;
        std::vector<float> values;              // rect.w * rect.h
        int64_t texelsSet = 0;
        FoliageRejections rejected;
        bool mapMissing = false;
    };

    JASPERENGINE_API FoliageDensityScatterResult scatterDensityByRules(const TerrainMapStore& store, const FoliageDensityScatter& scatter, FoliageStopFn stop = nullptr, void* stopUser = nullptr);

    // Rotation and record of a tree at a surface position
    JASPERENGINE_API void foliageSetTreeSurface(TreeRecord& record, float terrainSize, float terrainMaxHeight, float localX, float localZ, float surfaceHeight);

    inline float foliageTreeLocalX(const TreeRecord& record, float terrainSize) { return (record.u - 0.5f) * terrainSize; }
    inline float foliageTreeLocalZ(const TreeRecord& record, float terrainSize) { return (record.v - 0.5f) * terrainSize; }

    // The record as an instance: drawn in terrain-local space (T5 F3). surfaceNormal: where the
    // record leans towards its surface (null: upright, or a record with no lean)
    JASPERENGINE_API void foliageInstanceFromTree(const TreeRecord& record, float terrainSize, float terrainMaxHeight, uint8_t windBend, const float* surfaceNormal, FoliageInstance& out);

    // Canonical hash of the scattered records' bytes (ids stay out: they depend on what came before)
    JASPERENGINE_API uint64_t foliageTreesHash(std::span<const TreeRecord> trees, uint64_t previous = 0xcbf29ce484222325ull);

}

#endif // FOLIAGE_RULES_H
