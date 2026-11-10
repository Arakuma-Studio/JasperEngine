// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef GRID_COMPONENT_H
#define GRID_COMPONENT_H

#include "Export.h"
#include "asset/AssetRef.h"
#include "math/Vector2.h"
#include "tilemap/TileGridLayout.h"

namespace jasperengine{

    // Unity's Grid (docs/specs/S50-grid-tilemap.md R1, R47): the cell layout of the Tilemap Layers
    // on its GameObject and below it. The default hexagon is pointy-top odd-r, as S19's NavGrid.
    struct JASPERENGINE_API GridComponent{
        TileGridLayout layout = TileGridLayout::Rectangle;
        Vector2 cellSize = Vector2(16.0f, 16.0f);
        Vector2 cellGap = Vector2(0.0f, 0.0f);
        TileGridPlane plane = TileGridPlane::XY;
        StaggerAxis staggerAxis = StaggerAxis::Y;
        StaggerIndex staggerIndex = StaggerIndex::Odd;
        // hexagon: the side's length; 0: a regular hexagon in the cell
        float hexSideLength = 0.0f;
        // the Tiled or LDtk map this Grid was imported from; linked: its layers are read-only and
        // the import runs again when the file changes (R47)
        AssetRef importSource;
        bool linked = false;

        // runtime: moves on with every edit of the layout fields (the layers' chunks rebuild)
        uint64_t revision = 1;
    };

    inline TileGridSpec gridSpecOf(const GridComponent& grid){
        TileGridSpec spec;
        spec.layout = grid.layout;
        spec.cellWidth = grid.cellSize.x;
        spec.cellHeight = grid.cellSize.y;
        spec.gapX = grid.cellGap.x;
        spec.gapY = grid.cellGap.y;
        spec.staggerAxis = grid.staggerAxis;
        spec.staggerIndex = grid.staggerIndex;
        spec.hexSideLength = grid.hexSideLength;
        spec.plane = grid.plane;
        return spec;
    }

}

#endif //GRID_COMPONENT_H
