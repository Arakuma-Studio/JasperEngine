// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_SURFACE_H
#define NAV_MESH_SURFACE_H

#include "object/EntityHandle.h"
#include "component/NavMeshSurfaceComponent.h"
#include "math/AABB.h"

#include <string>

namespace jasperengine{

    class NavigationSystem;

    // Unity's NavMeshSurface (docs/navigation.md): Lua NavMeshSurface(scene, entity). bakeAsync
    // and updateTiles rebuild the navmesh at runtime (procedural levels); agents keep moving on
    // the old tiles until the new ones land, then onBakeCompleted fires.
    class JASPERENGINE_API NavMeshSurface: public EntityHandle{
    private:
        NavMeshSurfaceComponent& component() const;
        NavigationSystem* system() const;

    public:
        NavMeshSurface(Scene* scene, Entity entity);
        virtual ~NavMeshSurface();

        NavMeshSurface(const NavMeshSurface& rhs);
        NavMeshSurface& operator=(const NavMeshSurface& rhs);
        NavMeshSurface(NavMeshSurface&& rhs) noexcept;
        NavMeshSurface& operator=(NavMeshSurface&& rhs) noexcept;

        bool isValid() const;

        // re-collects and rebuilds every tile; false (logged) when it cannot start
        bool bakeAsync();
        // re-collects and rebuilds the tiles touching the box (world)
        bool updateTiles(const AABB& worldBounds);
        // removes the navmesh
        void clear();
        bool isBaking() const;
        float getBakeProgress() const;
        // the last bake's error, "" when it succeeded
        std::string getLastBakeError() const;
        int getTileCount() const;
        int getPolyCount() const;
        bool hasNavMesh() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setAgentTypeID(int agentType);
        int getAgentTypeID() const;
        void setCollectObjects(NavCollectObjects collect);
        NavCollectObjects getCollectObjects() const;
        void setUseGeometry(NavUseGeometry geometry);
        NavUseGeometry getUseGeometry() const;
        void setLayerMask(int mask);
        int getLayerMask() const;
        void setDefaultArea(int area);
        int getDefaultArea() const;
        void setCenter(Vector3 center);
        Vector3 getCenter() const;
        void setSize(Vector3 size);
        Vector3 getSize() const;
        void setRuntimeCarving(bool carving);
        bool isRuntimeCarving() const;
        // T4 R25: bake round the Terrain World's focus and rebake as it moves (a script's choice
        // turns it on on single-threaded web too)
        void setFollowTerrainWorldFocus(bool follow);
        bool isFollowTerrainWorldFocus() const;
        void setFollowRadius(float radius);
        float getFollowRadius() const;

        FunctionSubscribe<void(NavMeshSurface)>& getOnBakeCompleted();
    };

}

#endif //NAV_MESH_SURFACE_H
