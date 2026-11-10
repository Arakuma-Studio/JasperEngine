// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TILE_COLLIDER_BUILDER_H
#define TILEMAP_TILE_COLLIDER_BUILDER_H

// The colliders of one chunk of a Tilemap Layer, from its cells (docs/specs/S50-grid-tilemap.md
// R38-R43, D8). Pure: no Box2D, no scene; thread-safe build() over a const cell store, so a
// layer's chunks build in parallel.
//
// Space: the Grid plane's coordinates (u right, v up), local units (pixels in 2D). Internally a
// 1/16 pixel integer lattice; the union is Clipper2's, non-zero fill.
//
// Outlines (D8): the union of the solid shapes of the chunk's cells and of a margin ring around it
// (the tiles' overhang included). Every outline is cut along its own edges into runs: a run
// belongs to the chunk holding the cell just inside the solid (localToCell), so it works on every
// layout, the jagged chunk borders of staggered and hexagon grids included. A loop owned by the
// chunk alone is a closed chain; a loop crossing chunks gives open chains, each with the point
// before and after its crossing kept and one more point past it, so neighbouring chunks' chains
// share the three points Box2D's chains need (box2d/types.h, b2ChainDef). Those points come from
// the outline's own vertices or its next cell-boundary crossing, both computed from the same cell
// data on both sides.
//
// Winding: solids counter-clockwise and holes clockwise (Box2D chains collide on their right side:
// outside the solid). Chains have at least 4 points, spaced more than B2_LINEAR_SLOP (0.32 px at 64 px
// per metre) apart.

#include "Export.h"
#include "component/TilemapCollider2DComponent.h"
#include "tilemap/TileCells.h"
#include "tilemap/TileGridLayout.h"
#include "tilemap/Tileset.h"

#include <cstdint>
#include <vector>

namespace jasperengine{

    struct TilesetRuntime;

    struct TileColliderChain{
        std::vector<TilePoint> points;
        bool loop = false;
    };

    // a convex piece, counter-clockwise, at most B2_MAX_POLYGON_VERTICES (8) points
    struct TileColliderPolygon{
        std::vector<TilePoint> points;
    };

    struct TileColliderChunk{
        // closed loops (solids CCW, holes CW)
        std::vector<TileColliderChain> loops;
        // open chains cut at the chunk's border, with the three-point overlap
        std::vector<TileColliderChain> chains;
        // open chains of the one-way shapes' facing edges
        std::vector<TileColliderChain> oneWay;
        // convex pieces of the solids (geometry polygons)
        std::vector<TileColliderPolygon> polygons;
        // convex pieces of the sensor shapes
        std::vector<TileColliderPolygon> sensors;

        size_t chainCount() const { return loops.size() + chains.size() + oneWay.size(); }
        size_t polygonCount() const { return polygons.size() + sensors.size(); }
        bool empty() const { return chainCount() == 0 && polygonCount() == 0; }
        bool operator==(const TileColliderChunk& other) const;
    };

    struct TileColliderSettings{
        const TileCells* cells = nullptr;
        // the tile shapes (source tiles); the terrains' names are already resolved in solidTerrains
        const TilesetData* tileset = nullptr;
        // the tile image sizes (a sheet's frames); null: the tileset's tile size
        const TilesetRuntime* runtime = nullptr;
        TileGridSpec grid;
        TileColliderSource source = TileColliderSource::Tiles;
        // source terrain: the tileset-global terrain indices that collide
        std::vector<int> solidTerrains;
        TileColliderGeometry geometry = TileColliderGeometry::Outlines;
        bool merge = true;
        // moves every collider, local units
        TilePoint offset;
        // the layer's tileAnchor (cells) and dualGrid: where its tiles are drawn (tilegrid::tileShift); a
        // terrain source is its logical cells, so these do not move it
        TilePoint tileAnchor;
        bool dualGrid = false;
    };

    class JASPERENGINE_API TileColliderBuilder{
    public:
        // reads the settings' pointers on every build(): they must outlive this and the cell store
        // must not be written during a build
        explicit TileColliderBuilder(TileColliderSettings settings);

        TileColliderChunk build(ChunkCoord chunk) const;

        // the cells around a chunk that decide its colliders (the margin ring, overhang included)
        int getMargin() const { return margin; }
        // the chunks at most this many chunks away see each other's cells (an edit there may
        // change their shared crossings, R41)
        int chunkReach() const;

    private:
        struct TileInfo{
            const TileCollision* collision = nullptr;
            int width = 0;
            int height = 0;
            int offsetX = 0;
            int offsetY = 0;
        };

        TileColliderSettings settings;
        std::vector<TileInfo> tiles;     // by tile id
        std::vector<uint8_t> solidTerrain;   // by terrain plane value
        int margin = 1;
        TilePoint tileShift;             // tileAnchor + the dual-grid shift, plane units
    };

}

#endif // TILEMAP_TILE_COLLIDER_BUILDER_H
