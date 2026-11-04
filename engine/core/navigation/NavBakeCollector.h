// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The collection policy (world triangles gathered once on the main thread into an immutable
// input; entities that move, agents and their descendants left out; terrains as height lattices)
// follows OpenEngine's NavigationSourceCollector (Engine/Modules/PathfindingECS/Source/
// NavigationSourceCollector.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors,
// MIT License, as a reference only; the code is our own. See NOTICE.md.

#ifndef NAV_BAKE_COLLECTOR_H
#define NAV_BAKE_COLLECTOR_H

// Scene -> NavBakeInput for one NavMeshSurface (spec R2-R5, R7, R21, D3). Main thread: it reads
// the ECS. No physics world is needed: colliders are read from their components' parameters and
// meshes from their buffers (util/MeshTriangles), so a bake works with both physics options off.

#include "Export.h"
#include "ecs/Entity.h"
#include "navigation/NavBakeInput.h"
#include "navigation/NavDiagnostics.h"
#include "navigation/NavFrame.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class Scene;
    struct NavMeshSurfaceComponent;

}

namespace jasperengine::nav {

    struct NavCollectStats {
        int meshes = 0;
        int instances = 0;
        int terrains = 0;
        int colliders = 0;
        int colliders2D = 0;
        int volumes = 0;
        int links = 0;
        // left out by the policy (moving bodies, agents, obstacles, Exclude modifiers)
        int excluded = 0;
        size_t triangles = 0;
        std::vector<std::string> warnings;
        // S54, with NavCollectOptions::recordSources only: each GameObject left out and why
        // ("layer", "inactive", "moving body", "agent", "obstacle", "exclude modifier")
        std::vector<std::pair<Entity, std::string>> excludedEntities;
    };

    // the largest tile a web player bakes at runtime (NavBakeCollector::configOf)
    constexpr int WEB_MAX_TILE_CELLS = 32;

    class JASPERENGINE_API NavBakeCollector {
    public:
        // The input of the surface's bake, null (with *error, and *code) when the surface cannot be
        // baked (NoSurface, NothingCollected, TooLarge)
        static std::shared_ptr<NavBakeInput> collect(Scene* scene, Entity surface, std::string& error, NavCollectStats* stats = nullptr,
                                                     const NavCollectOptions& options = {}, NavBakeError* code = nullptr);

        // The surface's nav space now (its world position and rotation, its plane)
        static NavFrame frameOf(Scene* scene, Entity surface);
        // its Recast configuration from the project's agent type and its overrides; 2D surfaces
        // are in scene points (pointsToMeterScale2D)
        static NavBuildConfig configOf(Scene* scene, const NavMeshSurfaceComponent& surface);
        // scene points per metre of a 2D surface (1 for 3D)
        static float unitScaleOf(Scene* scene, const NavMeshSurfaceComponent& surface);

        // The off-mesh connections of one link in a surface's nav space (width > 0: parallel
        // connections); costAreas maps cost overrides to link cost areas (null: area costs)
        static void linkConnections(Scene* scene, Entity link, const NavFrame& frame, float agentRadius,
                                    std::vector<float>* costAreas, std::vector<NavLinkInput>& out);

        // A box (center and size in the space worldOf maps to the world) as the frame's nav-space
        // prism: the hull of its corners' footprint and their height range (unbounded on a 2D
        // surface), with its bounds. False when it has no footprint. (Modifier volumes, S55's
        // runtime volumes)
        static bool boxVolume(const NavFrame& frame, const Matrix4& worldOf, const Vector3& center, const Vector3& size, NavConvexVolume& out);
    };

}

#endif // NAV_BAKE_COLLECTOR_H
