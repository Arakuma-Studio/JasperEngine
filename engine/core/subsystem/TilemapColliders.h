// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef SUBSYSTEM_TILEMAP_COLLIDERS_H
#define SUBSYSTEM_TILEMAP_COLLIDERS_H

// The per-chunk colliders of a Tilemap Collider 2D (docs/specs/S50-grid-tilemap.md R38-R43, D8).
// The TilemapSystem marks chunks dirty as cells change and calls update() once per fixed step,
// before physics: the dirty chunks and the neighbours that see their cells rebuild on jobs, and
// the chunks whose colliders changed are written to the Rigidbody2D's generated list (R42).
// Without JASPERENGINE_PHYSICS_2D every function here does nothing.

#include "Export.h"
#include "ecs/Entity.h"
#include "tilemap/TileColliderBuilder.h"

#include <cstdint>
#include <map>
#include <span>
#include <unordered_set>

namespace jasperengine{

    class Scene;
    struct TilemapLayerComponent;
    struct TilemapCollider2DComponent;
    struct TilesetRuntime;

    struct TileColliderState{
        std::unordered_set<ChunkCoord, ChunkCoordHash> dirty;
        bool allDirty = true;
        // what each chunk last gave: an unchanged result leaves its colliders alone (R41)
        std::map<ChunkCoord, TileColliderChunk> built;
        // chunks built and chunks whose colliders were replaced, for the tests and diagnostics
        uint64_t chunksBuilt = 0;
        uint64_t chunksReplaced = 0;
    };

    namespace tilemapcolliders{

        JASPERENGINE_API void markDirty(TilemapCollider2DComponent& collider, std::span<const ChunkCoord> chunks);
        // every chunk rebuilds; only the ones whose shapes differ are replaced (a changed field of
        // the collider itself replaces them all: needUpdate)
        JASPERENGINE_API void markAll(TilemapCollider2DComponent& collider);
        // rebuilds what is dirty and writes the results to the entity's Rigidbody2D; `tileset`
        // may be null while it loads (nothing is generated from a null one)
        JASPERENGINE_API void update(Scene* scene, Entity entity, const TilemapLayerComponent& layer,
                                     TilemapCollider2DComponent& collider, const TileGridSpec& grid,
                                     const TilesetRuntime* tileset);
        // a disabled or removed component: its generated colliders go
        JASPERENGINE_API void release(Scene* scene, Entity entity, TilemapCollider2DComponent& collider);
    }

}

#endif // SUBSYSTEM_TILEMAP_COLLIDERS_H
