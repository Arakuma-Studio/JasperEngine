// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_GRID_H
#define NAV_GRID_H

#include "object/EntityHandle.h"
#include "component/NavGridComponent.h"
#include "math/AABB.h"

#include <vector>

namespace jasperengine{

    class NavigationSystem;

    // A GameObject's NavGrid (docs/navigation.md): Lua NavGrid(scene, entity). Cells are GridCell
    // {x, y} (columns, rows); edits repath the agents whose path crosses them.
    class JASPERENGINE_API NavGrid: public EntityHandle{
    private:
        NavGridComponent& component() const;
        NavigationSystem* system() const;

    public:
        NavGrid(Scene* scene, Entity entity);
        virtual ~NavGrid();

        NavGrid(const NavGrid& rhs);
        NavGrid& operator=(const NavGrid& rhs);
        NavGrid(NavGrid&& rhs) noexcept;
        NavGrid& operator=(NavGrid&& rhs) noexcept;

        bool isValid() const;

        nav::GridPath findPath(nav::GridCell from, nav::GridCell to, const nav::GridQuery& query = {}) const;
        // the path's waypoints in the world
        std::vector<Vector3> getPathPoints(const nav::GridPath& path) const;
        nav::GridCell worldToCell(const Vector3& world) const;
        Vector3 cellToWorld(nav::GridCell cell) const;
        bool setCellArea(nav::GridCell cell, int area);
        int getCellArea(nav::GridCell cell) const;
        int fillRect(nav::GridCell from, nav::GridCell to, int area);
        int stampObstacle(const AABB& worldBounds);
        bool isReachable(nav::GridCell from, nav::GridCell to) const;
        bool contains(nav::GridCell cell) const;
        bool rebuild();

        void setEnabled(bool enabled);
        bool isEnabled() const;
        int getWidth() const;
        int getHeight() const;
        float getCellSize() const;
        nav::GridLayout getLayout() const;
    };

}

#endif //NAV_GRID_H
