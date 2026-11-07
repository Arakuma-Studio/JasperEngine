// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPRITE_COMPONENT_H
#define SPRITE_COMPONENT_H

#include "util/SpriteFrameData.h"
#include "util/GrowArray.h"
#include "Engine.h"
#include "pixelart/PixelPerfectLayout.h"

namespace jasperengine{

    enum class PivotPreset{
        CENTER,
        TOP_CENTER,
        BOTTOM_CENTER,
        LEFT_CENTER,
        RIGHT_CENTER,
        TOP_LEFT,
        BOTTOM_LEFT,
        TOP_RIGHT,
        BOTTOM_RIGHT
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
    };

    // the texels per world unit a pixelsPerUnit of a sprite or a tilemap stands for (0: the
    // project's), always positive
    JASPERENGINE_API float resolvePixelsPerUnit(float pixelsPerUnit);
    // the sprite's quad in world units (its pixel size over its Pixels Per Unit): what bounds,
    // colliders fitted to it and world-size readers use
    JASPERENGINE_API Vector2 spriteWorldSize(const SpriteComponent& sprite);
    
}

#endif //SPRITE_COMPONENT_H