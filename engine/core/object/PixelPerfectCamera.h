// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pixel Perfect Camera of a camera GameObject (docs/specs/S41-pixel-perfect-camera.md §6.3):
// Lua `PixelPerfectCamera(scene, entity)`, `camera:getPixelPerfectCamera()`, and C++ scripts.
// - PixelPerfectCamera(scene, entity) wraps the camera's component and ADDS one when it has none.
// - The read-only values (pixelRatio, baseResolution, ...) are the layout RenderSystem made this
//   frame while the component drives its camera; before that, an empty layout.
// - Pixels are base pixels of the camera's image: x right, y up from its bottom-left corner.
// - Main thread only (ECS).

#ifndef PIXEL_PERFECT_CAMERA_H
#define PIXEL_PERFECT_CAMERA_H

#include "object/EntityHandle.h"
#include "component/PixelPerfectCameraComponent.h"
#include "math/Rect.h"

namespace jasperengine{

    class JASPERENGINE_API PixelPerfectCamera: public EntityHandle{

    public:
        // the camera's Pixel Perfect Camera, added when it has none
        PixelPerfectCamera(Scene* scene, Entity entity);
        virtual ~PixelPerfectCamera();

        PixelPerfectCamera(const PixelPerfectCamera& rhs);
        PixelPerfectCamera& operator=(const PixelPerfectCamera& rhs);

        void setEnabled(bool enabled);
        bool isEnabled() const;

        // (0, 0): the project canvas
        void setReferenceResolution(Vector2 resolution);
        Vector2 getReferenceResolution() const;

        void setUpscaleMode(PixelUpscaleMode mode);
        PixelUpscaleMode getUpscaleMode() const;

        void setUpscaleRenderTexture(bool upscaleRenderTexture);
        bool isUpscaleRenderTexture() const;

        void setPixelSnapping(bool pixelSnapping);
        bool isPixelSnapping() const;

        // STEPS_90 or ON_GRID (CAMERA is a sprite's choice and reads as STEPS_90 here)
        void setRotationMode(PixelRotation mode);
        PixelRotation getRotationMode() const;

        void setSubpixelSmoothing(bool subpixelSmoothing);
        bool isSubpixelSmoothing() const;

        void setLetterboxColor(Vector4 color);
        Vector4 getLetterboxColor() const;

        // the layout in effect (read only)
        int getPixelRatio() const;
        Vector2 getBaseResolution() const;
        Vector2 getTargetResolution() const;
        Rect getViewport() const;
        Vector3 getSnappedPosition() const;
        Vector2 getSubpixelOffset() const;
        bool isCropped() const;

        // a world position to base pixels of the image the camera draws (unsnapped), and back
        Vector2 worldToPixel(Vector3 world) const;
        Vector3 pixelToWorld(Vector2 pixel) const;
        // the pixel-snap rule (pixelart::snapWorld) on a world position, at the project's PPU
        Vector3 roundToPixel(Vector3 world) const;

    private:
        // where the camera draws from, and the base image's height
        Vector3 drawnOrigin() const;
        float baseHeight() const;
        bool isUICamera() const;
    };

}

#endif //PIXEL_PERFECT_CAMERA_H
