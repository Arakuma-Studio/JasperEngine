// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPRITE_COMPONENT_H
#define SPRITE_COMPONENT_H

#include "util/SpriteFrameData.h"
#include "util/GrowArray.h"
#include "asset/AssetRef.h"
#include "Engine.h"
#include "pixelart/PixelPerfectLayout.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    struct SpriteSheetData;   // sprite/SpriteSheet.h

    enum class PivotPreset{
        CENTER,
        TOP_CENTER,
        BOTTOM_CENTER,
        LEFT_CENTER,
        RIGHT_CENTER,
        TOP_LEFT,
        BOTTOM_LEFT,
        TOP_RIGHT,
        BOTTOM_RIGHT,
        // the frame's own pivot (sheet frames: the Aseprite slice, the import settings); appended
        FRAME
    };

    struct JASPERENGINE_API SpriteComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        unsigned int width = 0;
        unsigned int height = 0;

        bool automaticFlipY = true;
        bool flipY = false;

        float textureScaleFactor = 0.0;

        unsigned int numFramesRect = 0;
        GrowArray<SpriteFrameData> framesRect;
        // what its arrays hold outside the component (ComponentArray::arrayHeapBytes)
        size_t arrayHeapBytes() const { return framesRect.storageBytes(); }

        PivotPreset pivotPreset = PivotPreset::BOTTOM_LEFT;

        // S41: texels per world unit; 0 = the project's Pixels Per Unit (Engine::getPixelsPerUnit).
        // width and height stay in pixels; the quad is their size divided by it (spriteWorldSize)
        float pixelsPerUnit = 0.0f;
        // under a Pixel Perfect Camera: the camera's rotation policy, or one of its own
        PixelRotation pixelRotation = PixelRotation::CAMERA;

        // Frame chosen before the texture size was known, in pixels
        Rect pendingFrameRect;
        bool needUpdateFrameRect = false;

        bool needUpdateSprite = true;

        // A sheet sprite (docs/specs/S44-aseprite-import.md R48): its frames come from a sprite
        // sheet (an .aseprite file's import, a texture; S45: atlases and sliced textures), the
        // inline frames and the mesh texture are not used and not saved. "" = a legacy sprite.
        AssetRef sheet;
        int frame = 0;                    // index into the sheet's frames
        std::string frameName;            // the frame's name when saved: found again by name on load

        // runtime, not saved
        std::shared_ptr<const SpriteSheetData> sheetData;
        std::string loadedSheet;          // the `sheet` path sheetData was loaded for
        uint64_t sheetGeneration = 0;     // SpriteSheetPool's generation of sheetData
        uint64_t sheetRevision = 0;       // the pool revision last checked (a cheap per-frame "anything new?")
        int builtFrame = -1;              // the frame whose geometry the buffer holds
        bool resolveFrameName = true;     // find `frame` by `frameName` on the next sheet load

        // Sprite:playTag (R38): a sheet tag stepped by MeshSystem with the sheet's durations, no
        // frame limit; runtime, not saved
        std::string playingTag;           // "" = none
        std::vector<int> tagFrames;       // the tag's playback (direction and repeat unrolled)
        bool tagLoop = false;             // repeat 0: the sequence over and over
        bool tagPlaying = false;          // false: finished or stopped (playingTag stays the last tag)
        size_t tagStep = 0;
        double tagElapsedMs = 0.0;
        float tagSpeed = 1.0f;
    };

    // the texels per world unit a pixelsPerUnit of a sprite or a tilemap stands for (0: the
    // project's), always positive
    JASPERENGINE_API float resolvePixelsPerUnit(float pixelsPerUnit);
    // the sprite's quad in world units (its pixel size over its Pixels Per Unit): what bounds,
    // colliders fitted to it and world-size readers use
    JASPERENGINE_API Vector2 spriteWorldSize(const SpriteComponent& sprite);
    
}

#endif //SPRITE_COMPONENT_H