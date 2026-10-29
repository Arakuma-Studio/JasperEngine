// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_GRID_COMPONENT_H
#define NAV_GRID_COMPONENT_H

#include "Export.h"
#include "asset/AssetRef.h"
#include "ecs/Entity.h"
#include "navigation/GridPathfinder.h"
#include "navigation/NavGridMap.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    enum class NavGridSource{
        NONE,       // painted only
        TILEMAP,    // rasterized from a Tilemap (the rect -> area table)
        TERRAIN     // heights and slopes from a Terrain
    };

    // The area a tilemap rect gives the cells under it
    struct NavGridTileArea{
        std::string rect;
        int area = nav::AREA_WALKABLE;

        bool operator==(const NavGridTileArea& other) const { return rect == other.rect && area == other.area; }
        bool operator!=(const NavGridTileArea& other) const { return !(*this == other); }
    };

    // A navigation grid for 2D, tilemap and strategy games (spec S19 R15-R20, R35, R36): square
    // (4 or 8 neighbours) or hex (pointy-top, odd-r) cells of one area each, A* with area costs and
    // reservations for the NavMeshAgents on it. The cells are in a .navgrid asset (painted with
    // the nav-grid brush, or rebuilt from a Tilemap or a Terrain); the grid's corner is at the
    // GameObject, its cells along local X and Z (plane XZ) or X and Y (plane XY).
    struct JASPERENGINE_API NavGridComponent{
        // component checkbox: a disabled grid has no agents and answers no query
        bool enabled = true;

        nav::GridLayout layout = nav::GridLayout::Square;
        int neighbours = 8;
        float cellSize = 1.0f;
        int width = 32;
        int height = 32;
        nav::NavPlane plane = nav::NavPlane::XZ;
        float maxSlope = 45.0f;
        float maxStep = 0.5f;
        nav::GridMovement movement = nav::GridMovement::Free;
        // seconds of fixed ticks the agents reserve ahead, and what others pay on reserved cells
        float reservationLookahead = 1.5f;
        float reservationPenalty = 3.0f;

        NavGridSource source = NavGridSource::NONE;
        // the Tilemap or Terrain; none: this GameObject's
        Entity sourceEntity = NULL_ENTITY;
        std::vector<NavGridTileArea> tileAreas;
        int emptyCellArea = nav::AREA_NOT_WALKABLE;
        // static Collider2D footprints block the cells under them (Tilemap source)
        bool collidersBlock = false;
        bool autoRebuild = true;

        // the cells (.navgrid)
        AssetRef gridData;

        // ---- runtime (not saved): the cells, loaded from gridData or rebuilt
        std::shared_ptr<nav::NavGridMap> map;
        // the map has edits not in gridData yet
        bool mapDirty = false;
    };

}

#endif //NAV_GRID_COMPONENT_H
