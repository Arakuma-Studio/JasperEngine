// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_LAYER_COMPONENT_H
#define TILEMAP_LAYER_COMPONENT_H

#include "Export.h"
#include "asset/AssetRef.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "tilemap/TileCells.h"

#include <memory>

namespace jasperengine{

    struct TilemapLayerState;

    // Unity's Tilemap, Godot's TileMapLayer (docs/specs/S50-grid-tilemap.md R2-R6, R27, R31-R37):
    // the cells of one Tileset on the nearest Grid on its GameObject or above (without one, a
    // Rectangle grid of the tileset's tile size). Drawn in chunks through S52's instance batches as
    // the instances of its GameObject's MeshComponent, which holds its material and its Sorting
    // Layer and Order in Layer, as a Sprite's does. The cells are saved per chunk ("chunks").
    struct JASPERENGINE_API TilemapLayerComponent{
        bool enabled = true;
        // the cell encoding's version (R85): a later change bumps it with a migration
        int version = 1;
        AssetRef tileset;
        // the material colour (colour x opacity), linear
        Vector4 tint = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        TilemapRenderMode mode = TilemapRenderMode::Chunk;
        // 8, 16, 32 or 64 cells a side
        int chunkSize = 32;
        // moves every tile's anchor, in cells (0, 0: R8's anchor)
        Vector2 tileAnchor = Vector2(0.0f, 0.0f);
        float animationSpeed = 1.0f;
        // tiles drawn half a cell down-left, between four logical cells (R27)
        bool dualGrid = false;
        // the random and terrain choices' seed (R22)
        uint32_t seed = 0;
        TilemapGpuResidency gpuResidency = TilemapGpuResidency::Visible;
        // the cells (saved as "chunks", R86)
        TileCellsRef cells;

        // ---- runtime (not saved)
        std::shared_ptr<TilemapLayerState> state;
        // what the Inspector shows: non-empty cells and chunks
        uint32_t cellCount = 0;
        uint32_t chunkCount = 0;
        // a field changed (the system re-reads the tileset and rebuilds every chunk)
        bool needUpdate = true;
        // the Scene view's Focus On (S50 R65): multiplies the tint's alpha while another layer has focus
        float focusDim = 1.0f;
    };

}

#endif //TILEMAP_LAYER_COMPONENT_H
