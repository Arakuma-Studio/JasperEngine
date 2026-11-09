// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef SPRITE_BATCH_COMPONENT_H
#define SPRITE_BATCH_COMPONENT_H

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine{

    class SpriteInstanceBatch;

    // Retained: the instances stay until scripts change them; Clear Each Frame: what scripts write
    // in a frame is drawn in that frame only
    enum class SpriteBatchMode{
        RETAINED,
        CLEAR_EACH_FRAME
    };

    // Local: instances move with the GameObject; World: their positions are world positions
    enum class SpriteBatchSpace{
        LOCAL,
        WORLD
    };

    // the FX slots of the family contract (spec S52 R19): x Palette Swap row, y FLASH amount,
    // z DITHER_FADE amount, w DISSOLVE progress; every neutral value 0
    enum class SpriteFxSlot : uint8_t{
        PALETTE,
        FLASH,
        FADE,
        DISSOLVE
    };

    // A Sprite Batch's instances as scripts write them (runtime, never saved): parallel streams of
    // `count` entries; the packer turns them into the SpriteInstanceBatch the renderer draws
    struct JASPERENGINE_API SpriteBatchState{
        uint32_t count = 0;
        std::vector<Vector3> positions;
        std::vector<float> rotations;      // radians about z
        std::vector<Vector2> scales;
        std::vector<int32_t> frames;
        std::vector<Vector4> colors;       // linear RGBA
        std::vector<Vector4> fx;           // empty until a script writes an FX slot (hasFx)
        bool hasFx = false;
        // bumped by every write (the packer compares it)
        uint64_t revision = 1;
        // Clear Each Frame: the frame (SokolSystem commit) it was last drawn in, and whether a
        // script wrote since: the first write after a drawn frame starts the next frame's set
        uint64_t drawnFrame = 0;
        bool writtenSinceDraw = false;

        // what the renderer packed and drew (RenderSystem)
        std::unique_ptr<SpriteInstanceBatch> batch;
        uint64_t packedRevision = 0;
        uint64_t packedFrames = 0;         // the Sprite's frame table revision packed with
        uint32_t packedLayout = 0;         // what else the packing depends on (sort, space)
        float packedModel[16] = {};        // World space: the GameObject's matrix packed with

        SpriteBatchState();
        ~SpriteBatchState();

        // resizes every stream to `count`, new entries at their defaults
        void resize(uint32_t newCount);
        void clear();
        void markWritten(){ revision++; writtenSinceDraw = true; }
    };

    // Unity-style bulk sprites (spec S52 R10): on a GameObject with a Sprite, draws instances of
    // that Sprite's frames with GPU instancing, one draw per texture page; the Sprite itself is then
    // drawn only through its instances. Scripts fill it (Lua SpriteBatch); nothing of the instances
    // is saved
    struct JASPERENGINE_API SpriteBatchComponent{
        bool enabled = true;
        SpriteBatchMode mode = SpriteBatchMode::RETAINED;
        SpriteBatchSpace space = SpriteBatchSpace::LOCAL;
        // instances the GPU buffer holds before it grows (the bulk calls grow it)
        uint32_t capacity = 1024;
        // drawn in Transparency Sort Axis order (Project Settings > Graphics), else in index order
        bool sortInstances = false;

        // runtime, not saved: the instances (null until a script writes some) and their count
        std::shared_ptr<SpriteBatchState> state;
        uint32_t count = 0;
    };

}

#endif //SPRITE_BATCH_COMPONENT_H
