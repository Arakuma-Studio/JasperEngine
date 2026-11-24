// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_WORLD_COLLISION_H
#define TERRAIN_WORLD_COLLISION_H

// T4 R20-R23: a Terrain World's collision, paged. Height field chunks built from the tiles' own
// samples on Background jobs round every subject (the focuses, every awake dynamic body and
// Character Controller), and a coarse collider from the overview under any subject standing over a
// tile that is not in memory yet, so nothing falls through. Everything is one static compound on the
// World's own Rigidbody (its layer, mask, friction and listeners are the World's), swapped in the
// pump before the next step: coarse and fine change in one shape (spec Deviations: one compound
// instead of a body per chunk).

#include "Export.h"
#include "ecs/Entity.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace jasperengine {

    class Scene;
    class TerrainWorldRuntime;

    // R42's Collision Chunks overlay: one collider of the World's compound, entity-local
    struct TerrainWorldColliderRect {
        float minX = 0.0f, minZ = 0.0f, size = 0.0f;
        bool coarse = false;
    };

    class JASPERENGINE_API TerrainWorldCollision {
    public:
        // R21
        static constexpr int MAX_TRANSIENT_SUBJECTS = 64;
        // R22
        static constexpr int MAX_COARSE_PER_FRAME = 8;
        // R20
        static constexpr int MAX_CHUNKS_PER_FRAME = 16;
        // R23: a chunk's samples at 8 bits, Jolt's most. Not Jolt's estimate for an error
        // (CalculateBitsPerSampleForError): its second pass over a block lacks the first pass's
        // clamp, so on a side that is not a multiple of the block size, as a chunk's 2^n + 1 is, it
        // read wrapped and out-of-range samples and answered 8 for every chunk that is not flat.
        // With the bits it means, two chunks' shared edge quantizes apart and a crate at a
        // four-chunk corner never settles (TerrainWorldPlacement.CratesRestOnTheSurfaceAcrossBorders)
        static constexpr uint32_t CHUNK_BITS_PER_SAMPLE = 8;

        // R23: one fine chunk's samples as the collider builds them (worldChunkSamples), its tile and
        // neighbours read now if they must be (Physics Placement reads synchronously); false when the
        // tile is absent
        static bool chunkSamplesNow(TerrainWorldRuntime& runtime, terraincore::TileCoord tile, int chunkQuads, int chunkX, int chunkZ,
                                    std::vector<float>& out);

        explicit TerrainWorldCollision(TerrainWorldRuntime& runtime);
        ~TerrainWorldCollision();

        TerrainWorldCollision(const TerrainWorldCollision&) = delete;
        TerrainWorldCollision& operator=(const TerrainWorldCollision&) = delete;

        // In the pump, while the scene simulates: subjects, coarse tiles, chunk jobs, the shape swap
        void pump(float collisionRadius, int chunkQuads);
        // R18: a coarse collider under this point now, for a teleport; false off the world
        bool ensureAt(float localX, float localZ);
        // R18's collision level: the tile's fine chunks round its subjects are in place
        bool fineActive(terraincore::TileCoord tile) const;
        // Drops every collider (Stop, the World removed)
        void clear();
        // The colliders in the compound now, coarse tiles first
        std::vector<TerrainWorldColliderRect> colliders() const;

        int fineBodies() const { return fineCount; }
        int coarseBodies() const { return coarseCount; }
        int frozenBodies() const { return frozenCount; }
        // the compound swaps so far, for AC24's no-per-chunk-body check
        int shapeSwaps() const { return swaps; }

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        TerrainWorldRuntime& runtime;
        int fineCount = 0;
        int coarseCount = 0;
        int frozenCount = 0;
        int swaps = 0;
    };

}

#endif // TERRAIN_WORLD_COLLISION_H
