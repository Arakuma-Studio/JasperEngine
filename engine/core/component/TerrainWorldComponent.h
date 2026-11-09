// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_WORLD_COMPONENT_H
#define TERRAIN_WORLD_COMPONENT_H

// T4 R1, R15, R41: a Terrain World is a GameObject with Transform, Mesh, Terrain and this. The Terrain
// keeps its layers, material and foliage layers; its height map, size and LOD settings are unused, and
// the tiles of the .terrainworld asset hold the maps. The fields stay in every build, as navigation's
// do: a player without the module whose content needs one exits 5 (S34 R3). 0 is "the platform's
// default" (R34) for each budget.

#include "Export.h"
#include "asset/AssetRef.h"
#include "ecs/Entity.h"

namespace jasperengine{

    struct JASPERENGINE_API TerrainWorldComponent{
        bool enabled = true;

        // the Name.terrainworld folder asset (R48)
        AssetRef worldData;

        // R15: the active camera streams the world (in edit mode, the Scene view's camera)
        bool useCameraFocus = true;
        // R15: one more focus, following this GameObject (a player, a vehicle); scripts add others
        Entity focus = NULL_ENTITY;

        // R41's settings
        int poolSize = 0;               // resident pages
        float lodDistance = 0.0f;       // near band multiplier
        float uploadBudget = 0.0f;      // MiB a frame
        float collisionRadius = 0.0f;   // metres
        int collisionChunkQuads = 64;   // R20: quads a side of a paged collider, >= 32, a power of two
        // R29: brushes, edit_terrain and generation make the tiles they write over
        bool autoAddTiles = true;
        // R9: the far overview casts no shadow unless asked
        bool overviewCastsShadows = false;
    };

}

#endif // TERRAIN_WORLD_COMPONENT_H
