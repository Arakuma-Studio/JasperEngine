// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_SYSTEM_H
#define TILEMAP_SYSTEM_H

// Tilemap Layers at runtime (docs/specs/S50-grid-tilemap.md R29, R31-R37, R41, R62): after the
// frame's world transforms it finds each layer's Grid and Tileset, compares the cell store's chunk
// revisions with what it built, rebuilds the changed chunks that are in view, advances animated
// tiles, and keeps the chunks' GPU buffers resident only where they are seen. Registered after
// NavigationSystem and before PhysicsSystem, so a Tilemap Collider 2D's chunks rebuild before the
// physics step. A scene with no layers stops at the empty component arrays.

#include "SubSystem.h"

#include "component/GridComponent.h"
#include "component/TilemapCollider2DComponent.h"
#include "component/TilemapLayerComponent.h"
#include "render/sprite/SpriteInstanceBatch.h"
#include "tilemap/TilemapChunkBuilder.h"
#include "tilemap/TilesetPool.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    struct TilemapChunkRuntime{
        ChunkCoord coord;
        SpriteInstanceBatch batch;
        TilemapChunkBuild build;
        uint64_t builtRevision = 0;      // the chunk's revision the batch shows
        float unseenSeconds = 0.0f;
        size_t listIndex = 0;
    };

    // a Tilemap Layer's runtime: its chunks and what they were built from (never saved)
    struct JASPERENGINE_API TilemapLayerState{
        SpriteInstanceChunkList list;
        std::unordered_map<ChunkCoord, std::unique_ptr<TilemapChunkRuntime>, ChunkCoordHash> chunks;
        std::shared_ptr<const TilesetRuntime> tileset;
        std::string tilesetPath;
        uint64_t tilesetRevision = 0;
        uint64_t gridRevision = 0;
        Entity grid = NULL_ENTITY;
        TileGridSpec spec;
        double overhangX = 0.0, overhangY = 0.0;
        const TileCells* store = nullptr;
        uint64_t storeRevision = 0;
        float appliedTint[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        double animationMs = 0.0;
        bool attached = false;
        bool snapEdges = false;
        double ppu = 0.0;
        // the chunks rebuilt last update (R33's, for tests)
        uint32_t rebuilt = 0;
    };

    class JASPERENGINE_API TilemapSystem : public SubSystem{
    public:
        explicit TilemapSystem(Scene* scene);

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;
        void fixedUpdate(double dt) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // the nearest Grid on the GameObject or above; NULL_ENTITY: none
        Entity findGrid(Entity entity) const;
        // the layout a layer uses: its Grid's, or a Rectangle of the tileset's tile size over the
        // project's Pixels Per Unit (R2)
        TileGridSpec layerSpec(Entity entity, const TilemapLayerComponent& layer) const;
        // the chunks rebuilt on the last update of this layer (tests, R33)
        uint32_t getRebuiltChunks(const TilemapLayerComponent& layer) const { return layer.state ? layer.state->rebuilt : 0; }

    private:
        void updateLayer(Entity entity, TilemapLayerComponent& layer, double dt);
        void updateColliders();
        void releaseLayer(Entity entity, TilemapLayerComponent& layer);
        void buildChunk(TilemapLayerComponent& layer, TilemapChunkRuntime& chunk) const;
        TilemapChunkParams chunkParams(const TilemapLayerComponent& layer) const;
        void syncChunkList(TilemapLayerComponent& layer);
        void updateAnimations(Entity entity, TilemapLayerComponent& layer, double dt);
    };

}

#endif // TILEMAP_SYSTEM_H
