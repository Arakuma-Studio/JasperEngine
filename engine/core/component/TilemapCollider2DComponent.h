// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_COLLIDER_2D_COMPONENT_H
#define TILEMAP_COLLIDER_2D_COMPONENT_H

#include "Export.h"
#include "math/Vector2.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    struct TileColliderState;

    // tiles: the tiles' collision shapes; terrain: full logical cells of the solid terrains (dual
    // grid and LDtk IntGrid layers)
    enum class TileColliderSource : uint8_t { Tiles, Terrain };
    // outlines: chains (no area); polygons: convex pieces
    enum class TileColliderGeometry : uint8_t { Outlines, Polygons };

    // Unity's Tilemap Collider 2D with its Composite Collider (docs/specs/S50-grid-tilemap.md
    // R38-R43, D8): on a Tilemap Layer's GameObject with a Rigidbody2D (static by default), the
    // layer's colliders, generated per chunk into the body's generated list (runtime, never saved).
    struct JASPERENGINE_API TilemapCollider2DComponent{
        bool enabled = true;
        TileColliderSource source = TileColliderSource::Tiles;
        // source terrain: the terrain names that collide
        std::vector<std::string> solidTerrains;
        TileColliderGeometry geometry = TileColliderGeometry::Outlines;
        // union the shapes of neighbouring cells into one outline (off: one shape per cell)
        bool merge = true;
        Vector2 offset = Vector2(0.0f, 0.0f);
        // the Collider2D surface every generated collider shares
        float friction = 0.6f;
        float restitution = 0.0f;
        std::string material;
        uint16_t categoryBits = 1;
        uint16_t maskBits = 0xFFFF;
        int32_t groupIndex = 0;
        bool contactEvents = true;
        bool sensorEvents = false;
        bool enableHitEvents = false;

        // ---- runtime (not saved)
        std::shared_ptr<TileColliderState> state;
        // what the Inspector and get_entity report
        uint32_t generatedChains = 0;
        uint32_t generatedPolygons = 0;
        // a field changed: every chunk's colliders rebuild
        bool needUpdate = true;
    };

}

#endif //TILEMAP_COLLIDER_2D_COMPONENT_H
