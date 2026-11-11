// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Grid of a GameObject (docs/specs/S50-grid-tilemap.md R1, §6.4): Lua `Grid(scene, entity)` and
// C++ scripts. It ADDS a Grid when the GameObject has none. Main thread only (ECS).

#ifndef OBJECT_TILEMAP_GRID_H
#define OBJECT_TILEMAP_GRID_H

#include "object/EntityHandle.h"
#include "component/GridComponent.h"

namespace jasperengine{

    class JASPERENGINE_API Grid: public EntityHandle{
    public:
        Grid(Scene* scene, Entity entity);
        virtual ~Grid();

        Grid(const Grid& rhs);
        Grid& operator=(const Grid& rhs);

        void setLayout(TileGridLayout layout);
        TileGridLayout getLayout() const;
        void setCellSize(Vector2 size);
        Vector2 getCellSize() const;
        void setCellGap(Vector2 gap);
        Vector2 getCellGap() const;

        // world <-> cell through this GameObject's matrix: the cell holding a point, a cell's anchor
        CellCoord worldToCell(const Vector3& world) const;
        Vector3 cellToWorld(int x, int y) const;
    };

}

#endif //OBJECT_TILEMAP_GRID_H
