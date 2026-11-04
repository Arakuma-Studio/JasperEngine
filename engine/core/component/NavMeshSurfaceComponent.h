// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_SURFACE_COMPONENT_H
#define NAV_MESH_SURFACE_COMPONENT_H

#include "Export.h"
#include "asset/AssetRef.h"
#include "math/Vector3.h"
#include "navigation/NavTypes.h"
#include "util/FunctionSubscribe.h"

#include <cstdint>

namespace jasperengine{

    class NavMeshSurface;

    // Unity's NavMeshSurface CollectObjects
    enum class NavCollectObjects{
        ALL,        // every GameObject of the scene
        VOLUME,     // what overlaps the volume box
        CHILDREN    // the surface's GameObject and its descendants
    };

    // Unity's NavMeshSurface UseGeometry; terrains are collected under every choice
    enum class NavUseGeometry{
        RENDER_MESHES,
        PHYSICS_COLLIDERS,
        BOTH
    };

    // A navmesh for one agent type over the surface's bounds (docs/navigation.md, spec S19 R2):
    // baked in tiles from terrains, static meshes and colliders (3D) or from a ground plane and
    // static 2D collider footprints (plane XY), saved as a .navmesh asset next to the scene. The
    // navmesh lives in the surface's frame: moving the GameObject moves it.
    struct JASPERENGINE_API NavMeshSurfaceComponent{
        // component checkbox: a disabled surface has no navmesh at runtime
        bool enabled = true;

        int agentType = 0;
        NavCollectObjects collectObjects = NavCollectObjects::ALL;
        NavUseGeometry useGeometry = NavUseGeometry::RENDER_MESHES;
        // GameObject layers collected (Unity's Include Layers)
        uint32_t includeLayers = 0xFFFFFFFF;
        int defaultArea = nav::AREA_WALKABLE;
        // Volume: the box collected, in the surface's space
        Vector3 volumeCenter = Vector3(0.0f, 2.0f, 0.0f);
        Vector3 volumeSize = Vector3(10.0f, 10.0f, 10.0f);
        // Advanced: cell size (default agent radius / 3), tile size in cells, min region area
        bool overrideCellSize = false;
        float cellSize = 0.1667f;
        bool overrideTileSize = false;
        int tileSize = 64;
        float minRegionArea = 2.0f;
        nav::NavPlane plane = nav::NavPlane::XZ;
        // keep tile-cache layers, so carving NavMeshObstacles cut the navmesh at runtime
        bool runtimeCarving = false;
        // the baked .navmesh
        AssetRef navMeshData;

        // fired after bakeAsync/updateTiles land (Lua surf.onBakeCompleted)
        FunctionSubscribe<void(NavMeshSurface)> onBakeCompleted;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onBakeCompleted); }
    };

}

#endif //NAV_MESH_SURFACE_COMPONENT_H
