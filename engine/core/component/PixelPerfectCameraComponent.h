// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXEL_PERFECT_CAMERA_COMPONENT_H
#define PIXEL_PERFECT_CAMERA_COMPONENT_H

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "pixelart/PixelPerfectLayout.h"

namespace jasperengine{

    // Pixel art on a camera (docs/specs/S41-pixel-perfect-camera.md, Unity's Pixel Perfect
    // Camera): one base resolution scaled to the screen by a whole number, the camera and every
    // sprite, tile, glyph and particle on whole base pixels (pixelart::snap). On the main camera
    // of the Engine's main scene it drives the Engine canvas and view rect, the fixed-resolution
    // target and its blit (RenderSystem); on a layer scene's camera it takes the main scene's
    // layout and draws directly; on a render-to-texture camera it snaps to the texture's pixels.
    // Inert on a perspective camera.
    struct JASPERENGINE_API PixelPerfectCameraComponent{
        bool enabled = true;
        // 0 x 0: the project canvas (graphics.canvas_width / canvas_height)
        unsigned int referenceWidth = 0;
        unsigned int referenceHeight = 0;
        PixelUpscaleMode upscaleMode = PixelUpscaleMode::INTEGER;
        // the base image into a target of its own, scaled by the blit; off: drawn directly at the
        // screen's resolution on the same grid
        bool upscaleRenderTexture = true;
        bool pixelSnapping = true;
        // STEPS_90 or ON_GRID (CAMERA is a sprite's own choice)
        PixelRotation rotationMode = PixelRotation::STEPS_90;
        // the camera's residual moves the final image by whole screen pixels
        bool subpixelSmoothing = false;
        Vector4 letterboxColor = Vector4(0, 0, 0, 1);

        // runtime, never saved: RenderSystem's while the component drives a camera, else what the
        // editor computes for the Game view's size
        pixelart::PixelPerfectLayout layout;
        Vector3 snappedPosition = Vector3(0, 0, 0);
        Vector2 subpixelOffset = Vector2(0, 0);   // base px, the residual f of R8
        bool active = false;                       // drives its camera this frame
    };

}

#endif //PIXEL_PERFECT_CAMERA_COMPONENT_H
