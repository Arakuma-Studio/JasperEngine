// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R33, R40, R43: Convert to Terrain World. A Terrain becomes a new World at the same place, as one
// undo step: its heights come through T1's surface query (sampleTerrainSurface), so the world's lattice
// lies on the drawn surface although a map's texels sit at (i + 0.5) / W; its blend, hole and density
// maps resample to the tiles' lattices; its trees split into each tile's .jetrees; its layers,
// material and foliage settings copy; the source GameObject is deactivated, not deleted. The
// resampling is a job (convertTerrainLater) or runs in the call when it is small (convertTerrainNow).

#include "ecs/Entity.h"
#include "terrainworld/TerrainWorldEditor.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {

    class Project;

    // What a call may choose; 0 or empty: from the source
    struct TerrainConvertOptions {
        std::string name;           // "<Terrain> World"
        std::string assetPath;      // "<name>.terrainworld", the first free one
        int tileQuads = 0;
        float spacing = 0.0f;
    };

    struct TerrainConvertPlan {
        TerrainWorldSettings world;     // lattice, grid round the source, its max height
        int64_t samples = 0;            // height samples the job resamples
        bool trees = false;
    };

    // The world a Terrain would become, or why it cannot: a World already, no heightmap yet, a rotated or
    // scaled source (a World translates only, R4), or options past the world's limits. Spacing keeps
    // the heightmap's sample density (the largest power of two at or under a texel); tile size is the
    // largest the platform takes, down to what the source needs, so a small Terrain is 2 x 2 tiles
    bool planTerrainConversion(Scene* scene, Entity source, const TerrainConvertOptions& options, TerrainConvertPlan& plan, std::string& error);

    // "<name>.terrainworld", or "<name> 1.terrainworld" and on when that is taken: an undone conversion
    // leaves its folder, so the next one picks another
    std::string freeTerrainWorldAssetPath(Project* project, const std::string& name);

    // Conversions above this many samples run as a job
    inline constexpr int64_t TERRAIN_CONVERT_JOB_SAMPLES = 2049ll * 2049ll;

    struct TerrainConvertResult {
        Entity entity = NULL_ENTITY;
        std::string assetPath;
        int tiles = 0;
        std::string error;
        bool cancelled = false;
    };

    // In the call: the folder, then the GameObject and the source's deactivation as one undo step
    // "Convert to Terrain World (Name)"
    TerrainConvertResult convertTerrainNow(Project* project, uint32_t sceneId, Entity source, const TerrainConvertOptions& options);

    // The same with the resampling on a job; done runs on the main thread (EditorHost's queue). The
    // returned cancel stops it before anything is written.
    std::function<void()> convertTerrainLater(Project* project, uint32_t sceneId, Entity source, const TerrainConvertOptions& options,
                                              std::function<void(const TerrainConvertResult&)> done);

}
