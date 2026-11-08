// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PIXELROTATION_H
#define PIXELART_PIXELROTATION_H

// The rotation policy of pixel-art draws (docs/specs/S41-pixel-perfect-camera.md R10): Steps90
// draws a rotation rounded to the nearest 90 degrees, Rotate On Grid keeps the angle and snaps the
// pivot (PixelSnap.h). The decision of the nearest step is the one of
// includes/pixel_snap.glsl's pixelSnapRotationStep: ties at 45 degrees go to the step nearer 0.

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Vector2.h"

namespace jasperengine::pixelart {

    // the unit axis of the 90-degree step nearest the direction axisX (the rotated x axis): (1, 0),
    // (0, 1), (-1, 0) or (0, -1); (0, 0) for a zero axis
    JASPERENGINE_API Vector2 snapRotationStep(const Vector2& axisX);

    // the model matrix with the rotation of its 2D (x, y) linear part rounded to the nearest 90
    // degrees, its scale and flips kept (polar decomposition), about its own origin (the world
    // pivot); z and the translation are untouched
    JASPERENGINE_API Matrix4 roundRotation90(const Matrix4& model);

    // its 2D linear part has a rotation off the 90-degree grid (relative 1e-4): roundRotation90
    // would change it
    JASPERENGINE_API bool hasOffGridRotation(const Matrix4& model);

}

#endif //PIXELART_PIXELROTATION_H
