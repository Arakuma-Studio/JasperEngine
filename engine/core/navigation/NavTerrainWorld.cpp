// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavTerrainWorld.h"

#include "Log.h"
#include "Scene.h"
#include "component/NavMeshSurfaceComponent.h"
#include "subsystem/NavigationSystem.h"

#if JASPERENGINE_TERRAIN_WORLD
#include "component/Transform.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#endif

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::nav;

#if JASPERENGINE_TERRAIN_WORLD

namespace {

    // the distance from a point to a tile's square, entity-local
    float navDistanceToTile(float x, float z, terrain::TileCoord tile, float size) {
        const float minX = (float)tile.x * size, minZ = (float)tile.z * size;
        const float dx = x < minX ? minX - x : (x > minX + size ? x - minX - size : 0.0f);
        const float dz = z < minZ ? minZ - z : (z > minZ + size ? z - minZ - size : 0.0f);
        return std::sqrt(dx * dx + dz * dz);
    }

    TerrainWorldRuntime* navFollowedRuntime(Scene* scene, Entity world) {
        std::shared_ptr<TerrainWorldSystem> worlds = scene->findSystem<TerrainWorldSystem>();
        TerrainWorldRuntime* runtime = worlds && world != NULL_ENTITY ? worlds->ensureRuntime(world) : nullptr;
        return runtime && runtime->started() ? runtime : nullptr;
    }

}

bool nav::terrainWorldFocus(const TerrainWorldRuntime& runtime, float& x, float& z) {
    // TerrainWorldSystem's ids: -2 the component's Focus GameObject, -1 the camera; scripts' are positive
    const std::vector<terrain::StreamFocus>& focuses = runtime.tiles().focuses();
    for (int wanted : {-2, -1}) {
        for (const terrain::StreamFocus& focus : focuses) {
            if (focus.id == wanted) {
                x = focus.x;
                z = focus.z;
                return true;
            }
        }
    }
    if (focuses.empty())
        return false;
    x = focuses.front().x;
    z = focuses.front().z;
    return true;
}

std::set<uint32_t> nav::followTiles(const TerrainWorldRuntime& runtime, float x, float z, float radius, const std::set<uint32_t>& previous) {
    std::set<uint32_t> tiles;
    const float size = runtime.lattice().tileSize();
    const float reach = radius + size * 0.5f;
    const int lowX = std::max(terrain::WORLD_TILE_MIN, (int)std::floor((x - reach) / size));
    const int highX = std::min(terrain::WORLD_TILE_MAX, (int)std::floor((x + reach) / size));
    const int lowZ = std::max(terrain::WORLD_TILE_MIN, (int)std::floor((z - reach) / size));
    const int highZ = std::min(terrain::WORLD_TILE_MAX, (int)std::floor((z + reach) / size));
    for (int tz = lowZ; tz <= highZ; tz++) {
        for (int tx = lowX; tx <= highX; tx++) {
            const terrain::TileCoord tile(tx, tz);
            if (!runtime.occupancy().present(tile))
                continue;
            const float distance = navDistanceToTile(x, z, tile, size);
            if (distance <= radius || (distance <= reach && previous.count(tile.key())))
                tiles.insert(tile.key());
        }
    }
    return tiles;
}

void nav::followTerrainWorldFocus(NavigationSystem& system, Scene* scene, Entity surface, NavWorldContext& context) {
    const NavMeshSurfaceComponent* component = scene->findComponent<NavMeshSurfaceComponent>(surface);
    if (!component || !component->enabled || !component->followTerrainWorldFocus || component->plane != NavPlane::XZ || system.isBaking(surface))
        return;
    const Entity world = followedTerrainWorld(scene);
    TerrainWorldRuntime* runtime = navFollowedRuntime(scene, world);
    float x = 0.0f, z = 0.0f;
    if (!runtime || !terrainWorldFocus(*runtime, x, z))
        return;
    const float size = runtime->lattice().tileSize();
    const float dx = x - context.focusX, dz = z - context.focusZ;
    const bool moved = dx * dx + dz * dz >= (size * 0.25f) * (size * 0.25f);
    std::string error;

    // the first bake round the focus: only the tiles whose inputs differ from the navmesh it has
    if (!context.followed || context.world != world) {
        if (context.tried && context.world == world && !moved)
            return;
        context.world = world;
        context.tried = true;
        context.focusX = x;
        context.focusZ = z;
        context.hasTarget = false;
        if (!system.bake(surface, NavBakeMode::DIRTY, error))
            Log::warn("NavMeshSurface '%s' cannot follow its Terrain World: %s", scene->getEntityName(surface).c_str(), error.c_str());
        return;
    }
    if (!moved)
        return;
    const std::set<uint32_t> target = followTiles(*runtime, x, z, component->followRadius, context.region);
    if (target == context.region) {
        context.focusX = x;
        context.focusZ = z;
        return;
    }
    // the box of the tiles that come and go: updateTiles rebuilds the navmesh tiles it meets, and
    // drops those left with nothing
    Vector3 origin;
    if (const Transform* transform = scene->findComponent<Transform>(world))
        origin = transform->worldPosition;
    AABB bounds;
    auto add = [&](uint32_t key) {
        const terrain::TileCoord tile = terrain::TileCoord::fromKey(key);
        const float minX = origin.x + (float)tile.x * size, minZ = origin.z + (float)tile.z * size;
        bounds.merge(Vector3(minX, origin.y, minZ));
        bounds.merge(Vector3(minX + size, origin.y + runtime->maxHeight(), minZ + size));
    };
    for (uint32_t key : target) {
        if (!context.region.count(key))
            add(key);
    }
    for (uint32_t key : context.region) {
        if (!target.count(key))
            add(key);
    }
    context.target = target;
    context.targetX = x;
    context.targetZ = z;
    context.hasTarget = true;
    context.lastBounds = bounds;
    context.steps++;
    if (!system.updateTiles(surface, bounds, error)) {
        context.hasTarget = false;
        context.focusX = x;
        context.focusZ = z;
        Log::warn("NavMeshSurface '%s' cannot follow its Terrain World: %s", scene->getEntityName(surface).c_str(), error.c_str());
    }
}

#else

bool nav::terrainWorldFocus(const TerrainWorldRuntime&, float&, float&) {
    return false;
}

std::set<uint32_t> nav::followTiles(const TerrainWorldRuntime&, float, float, float, const std::set<uint32_t>&) {
    return {};
}

void nav::followTerrainWorldFocus(NavigationSystem&, Scene*, Entity, NavWorldContext&) {
}

#endif // JASPERENGINE_TERRAIN_WORLD
