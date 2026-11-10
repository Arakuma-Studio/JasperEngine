// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_TERRAIN_WORLD_H
#define NAV_TERRAIN_WORLD_H

// T4 R25, D15 (docs/specs/T4-terrain-worlds.md): navigation on Terrain Worlds. A surface collects a
// World's tiles, one lattice each, holding their data by shared reference for the bake. A tile not in
// memory is demanded from the World's streamer, which loads it on its jobs, and the collection is
// retried until every tile is in: S19's collection is a main-thread snapshot that hashes every tile's
// inputs before a job runs, so the tiles must be there when it runs, and retrying needs no
// main-thread read, no job holding the runtime and no worker to wait on (single-threaded web). A
// surface with Follow Terrain World Focus bakes the tiles round the World's focus and rebakes through
// NavigationSystem::updateTiles once the focus has moved a quarter tile.

#include "Export.h"
#include "ecs/Entity.h"
#include "math/AABB.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>

namespace jasperengine {

    class NavigationSystem;
    class Scene;
    class TerrainWorldRuntime;
    namespace terraincore {
        struct WorldTile;
    }

}

namespace jasperengine::nav {

    // A surface without a Volume (or Follow) refuses a World of more tiles than this
    constexpr int MAX_UNBOUNDED_WORLD_TILES = 64;

    struct NavWorldHeld {
        std::shared_ptr<const terraincore::WorldTile> data;
        // the runtime's revision of the tile when it was taken: an edit since takes it again
        uint32_t revision = 0;
    };

    // What a surface keeps of its Terrain Worlds between collections
    struct JASPERENGINE_API NavWorldContext {
        // tiles in memory, held while the others load (world entity << 32 | tile key); dropped
        // once a collection takes them all
        std::map<uint64_t, NavWorldHeld> held;

        // Follow: the World followed, the tiles of its last bake and the focus it was made round
        Entity world = NULL_ENTITY;
        std::set<uint32_t> region;
        float focusX = 0.0f;
        float focusZ = 0.0f;
        bool followed = false;
        // a bake round focusX/Z was asked and did not land: asked again once the focus moves S/4
        bool tried = false;
        // what the next collection takes: a step sets it before its updateTiles, so a retried
        // collection takes the region the step's box was made for
        std::set<uint32_t> target;
        float targetX = 0.0f;
        float targetZ = 0.0f;
        bool hasTarget = false;
        // the world box of the last step's updateTiles, and the steps so far
        AABB lastBounds;
        int steps = 0;
    };

    // The World a Follow surface follows: the first active Terrain World (entity order) whose
    // runtime runs; NULL_ENTITY when there is none
    JASPERENGINE_API Entity followedTerrainWorld(Scene* scene);
    // The focus a Follow surface bakes round, entity-local: the World's Focus GameObject, else its
    // camera, else a script's focus; false when it has none
    JASPERENGINE_API bool terrainWorldFocus(const TerrainWorldRuntime& runtime, float& x, float& z);
    // A Follow region's tile keys: the present tiles within radius of the focus, and those of the
    // previous region still within radius + S/2 (a tile leaves half a tile later than it came)
    JASPERENGINE_API std::set<uint32_t> followTiles(const TerrainWorldRuntime& runtime, float x, float z, float radius,
                                                    const std::set<uint32_t>& previous);
    // One Follow step of a surface (the fixed tick): the first bake round the focus, then an
    // updateTiles over the tiles that come and go once the focus has moved S/4
    JASPERENGINE_API void followTerrainWorldFocus(NavigationSystem& system, Scene* scene, Entity surface, NavWorldContext& context);

}

#endif // NAV_TERRAIN_WORLD_H
