// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOLIAGE_SCATTER_H
#define FOLIAGE_SCATTER_H

// Places one cell of a detail layer (T5 R1, R2, F2) from a self-contained input: the same input gives
// the same FoliageInstance bytes on every target, whatever the worker count, the frame timing or the
// camera path that brought the cell in. No ECS, no render types, no globals, no libm beyond sqrt and
// floor, and the file builds with no floating-point contraction (TerrainSources.cmake).

#include "Export.h"
#include "terrain/FoliageTypes.h"
#include "terrain/TerrainHoles.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine::terrain {

    // The layer's fields a placement depends on. Colours are bytes, so the input hashes exactly.
    struct FoliageLayerParams {
        uint32_t seed = 0;
        float density = 1.0f;           // instances per square unit where the map is full
        float minScale = 0.8f;
        float maxScale = 1.2f;
        float rotationJitter = 1.0f;    // share of a full turn
        float alignToNormal = 0.0f;
        float minSlope = 0.0f;          // degrees
        float maxSlope = 35.0f;
        float minHeight = 0.0f;         // normalized against the terrain's maxHeight
        float maxHeight = 1.0f;
        float minHeightRatio = 1.0f;    // Min/Max Height, over the width scale
        float maxHeightRatio = 1.0f;
        uint8_t healthyColor[4] = {255, 255, 255, 255};
        uint8_t dryColor[4] = {255, 255, 255, 255};
        float noiseSpread = 0.0f;       // noise cells per unit between dry and healthy; 0: per instance
        uint8_t windBend = 0;
        uint32_t algorithmVersion = FOLIAGE_ALGORITHM_VERSION;
    };

    // A rectangle of a map, in the bytes the map stores, copied for a job. Sampling computes on the
    // whole map's coordinates and only then reads from here, so a value equals the whole map's to the
    // last bit: what T1's SurfaceView cannot do, since it has no window (T5 Deviation 1).
    struct FoliageMapPatch {
        const uint8_t* texels = nullptr;    // row by row; one byte per channel, or two for 16-bit
        int mapWidth = 0, mapHeight = 0;
        int originX = 0, originY = 0;
        int width = 0, height = 0;
        int channels = 1;
        int bytesPerChannel = 1;
    };

    // What a whole cell needs of a map, in texels; empty for no map. margin: extra texels around
    struct FoliagePatchRect {
        int x = 0, y = 0, width = 0, height = 0;
    };
    JASPERENGINE_API FoliagePatchRect foliageHeightPatchRect(int cellX, int cellZ, float terrainSize, int mapWidth, int mapHeight);
    JASPERENGINE_API FoliagePatchRect foliageDensityPatchRect(int cellX, int cellZ, float terrainSize, int mapWidth, int mapHeight);

    struct FoliageCellInput {
        FoliageLayerParams layer;
        int cellX = 0, cellZ = 0;
        int regionOffsetX = 0, regionOffsetZ = 0;   // whole cells, 0 for one Terrain (T5 F2, F11)
        float terrainSize = 0.0f;
        float terrainMaxHeight = 0.0f;
        FoliageMapPatch heightMap;
        FoliageMapPatch densityMap;
        HoleMapView holes;
    };

    // Counts of what one call turned down, by the rule that did (MCP's rejected)
    struct FoliageScatterStats {
        uint32_t attempts = 0;
        uint32_t density = 0;
        uint32_t hole = 0;
        uint32_t slope = 0;
        uint32_t height = 0;
    };

    JASPERENGINE_API unsigned int foliageCellAttempts(const FoliageLayerParams& layer);

    // Appends the cell's instances in attempt order
    JASPERENGINE_API void scatterFoliageCell(const FoliageCellInput& input, std::vector<FoliageInstance>& out, FoliageScatterStats* stats = nullptr);

    // The surface a patch answers: as querySurface, within what a float holds
    JASPERENGINE_API float foliagePatchHeight(const FoliageMapPatch& patch, float terrainSize, float terrainMaxHeight, float localX, float localZ);
    JASPERENGINE_API void foliagePatchNormal(const FoliageMapPatch& patch, float terrainSize, float terrainMaxHeight, float localX, float localZ,
                                             float height, float normal[3]);

    // Instances in canonical order (layer, cell Z, cell X, attempt) hash to one constant everywhere.
    // Feed cells in that order; the previous call's result chains.
    JASPERENGINE_API uint64_t foliageHash(std::span<const FoliageInstance> instances, uint64_t previous = 0xcbf29ce484222325ull);

    // F2's kernels, exposed for what shares them (re-snap, rules). No libm, within 1e-5 rad.
    JASPERENGINE_API float foliageAcos(float cosine);
    // A quaternion, x y z w: the yaw of a share of a full turn, tilted towards normal by alignToNormal
    JASPERENGINE_API void foliageOrientation(float yawTurns, float alignToNormal, const float normal[3], float rotation[4]);
    JASPERENGINE_API void foliageQuantizeRotation(const float rotation[4], int16_t out[4]);
    // Value noise in [0, 1] from a hash lattice, the same everywhere
    JASPERENGINE_API float foliageValueNoise(float x, float z, uint32_t seed);

    // F2's hash, moved verbatim
    JASPERENGINE_API unsigned int hashFoliage(unsigned int seed, unsigned int chunkX, unsigned int chunkZ, unsigned int index);
    JASPERENGINE_API unsigned int mixFoliage(unsigned int hash);
    JASPERENGINE_API float randomFoliage(unsigned int hash);

}

#endif // FOLIAGE_SCATTER_H
