// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PIXELPERFECTLAYOUT_H
#define PIXELART_PIXELPERFECTLAYOUT_H

// The integer layout of a pixel-art image on a screen (docs/specs/S41-pixel-perfect-camera.md R1):
// one function for the Pixel Perfect Camera, the project's Integer scaling modes, the editor's
// views, MCP, S42's validator and the tests, so none of them can disagree.

#include "Export.h"
#include "math/Rect.h"

namespace jasperengine {

    // how the base image reaches the screen
    enum class PixelUpscaleMode {
        INTEGER,         // k x k screen pixels per base pixel, bars around (letterbox)
        INTEGER_EXPAND,  // k as Integer, the base grows to fill the screen (bars < k px)
        STRETCH_FILL     // the Integer image stretched to the aspect-kept fit, sharp bilinear
    };

    // the rotation policy (R10); CAMERA only on sprites: the camera's
    enum class PixelRotation {
        CAMERA,
        STEPS_90,
        ON_GRID
    };

}

namespace jasperengine::pixelart {

    struct JASPERENGINE_API PixelPerfectLayout {
        int scale = 1;                          // k: screen pixels per base pixel
        int baseWidth = 0, baseHeight = 0;      // B: the base resolution
        int targetWidth = 0, targetHeight = 0;  // T: B, or B + 2 with sub-pixel smoothing
        Rect viewport;                          // whole screen pixels: where the image goes
        float canvasWidth = 0, canvasHeight = 0;// B / PPU world units
        bool cropped = false;                   // the screen is smaller than the reference

        bool operator==(const PixelPerfectLayout& other) const;
        bool operator!=(const PixelPerfectLayout& other) const { return !(*this == other); }
    };

    // forcedScale > 0 caps k (the Game view's Pixel Scale); smoothing adds the one-pixel margin of
    // sub-pixel smoothing to T; a reference of 0 x 0, or a screen of 0 x 0, gives an empty layout
    JASPERENGINE_API PixelPerfectLayout computePixelPerfectLayout(int screenWidth, int screenHeight, int referenceWidth,
        int referenceHeight, PixelUpscaleMode mode, int forcedScale, bool smoothing, int pixelsPerUnit);

    // the largest scale that fits the reference on the screen (at least 1)
    JASPERENGINE_API int fitScale(int screenWidth, int screenHeight, int referenceWidth, int referenceHeight);

}

#endif //PIXELART_PIXELPERFECTLAYOUT_H
