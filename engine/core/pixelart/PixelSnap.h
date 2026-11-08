// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PIXELSNAP_H
#define PIXELART_PIXELSNAP_H

// The pixel-snap rule (docs/specs/S41-pixel-perfect-camera.md §6.2): the one rounding every pixel-art
// drawable goes through, the CPU twin of shaderlib/includes/pixel_snap.glsl. Both evaluate the same
// float32 operations in the same order, so a position snapped here is the pixel the vertex stage
// draws it on.
//
// Space: base pixels of the drawing target (x right, y up from its bottom-left corner, 1 unit = 1
// base pixel), whatever the backend's Y flip. snap(p) = floor(p + 0.5 + 2^-7): the bias moves the
// half-pixel lattice (CENTER pivots of odd sizes) away from the CPU/GPU rounding differences of
// matrix products and FMA (<= 1e-3 px), so both sides round a k + 0.5 to k + 1.

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "pixelart/PixelRotation.h"

#include <cmath>

namespace jasperengine::pixelart {

    inline constexpr float kSnapBias = 0.0078125f;   // 2^-7 pixel

    // written as the GLSL twin: (p + 0.5) + bias, two additions
    inline float snap(float p) { return std::floor(p + 0.5f + kSnapBias); }

    // The frame block's pixelSnap / pixelSnapOffset of a pass
    struct JASPERENGINE_API SnapGrid {
        float width = 0, height = 0;    // T: the drawing target in base pixels
        float flipY = 1;                // -1 when the pass's MVP is Y-flipped (GL render targets)
        float offsetX = 0, offsetY = 0; // R12 direct mode: s/k base px; 0 otherwise

        bool isValid() const { return width > 0.0f && height > 0.0f; }
    };

    // What the vertex stage reads of a grid (frame.pixelSnap.xy, frame.pixelSnapOffset.xy): half
    // the target, and where NDC 0 lands in base pixels. A pixel is ndc * half + center; the CPU
    // rounds the same terms the GPU is given.
    struct SnapTerms {
        float halfWidth = 0, halfHeight = 0;
        float centerX = 0, centerY = 0;
    };
    inline SnapTerms snapTerms(const SnapGrid& grid){
        SnapTerms terms;
        terms.halfWidth = grid.width * 0.5f;
        terms.halfHeight = grid.height * 0.5f;
        terms.centerX = terms.halfWidth - grid.offsetX;
        terms.centerY = terms.halfHeight - grid.offsetY;
        return terms;
    }

    // a clip-space position to base pixels of the grid, and back (pixelSnapToPixels,
    // pixelSnapFromPixels): z and w are kept
    JASPERENGINE_API Vector2 toPixels(const Vector4& clip, const SnapGrid& grid);
    JASPERENGINE_API Vector4 fromPixels(const Vector2& pixels, const Vector4& clip, const SnapGrid& grid);

    // per vertex: the clip position moved onto the nearest base pixel corner
    JASPERENGINE_API Vector4 snapClip(const Vector4& clip, const SnapGrid& grid);
    // the clip-space delta that puts a pivot on the grid: added to every vertex of the draw
    JASPERENGINE_API Vector4 snapCenterDelta(const Vector4& clipCenter, const SnapGrid& grid);

    // the 2D linear part of mvp, in base pixels of the grid, is a signed permutation within a
    // relative 1e-4 (0, 90, 180, 270 degrees and flips, any scale): its vertices can snap one by one
    JASPERENGINE_API bool isGridAligned(const Matrix4& mvp, const SnapGrid& grid);

    // R8 (the camera) and the editor's move snap: snap(v * ppu) / ppu on x and y, z kept
    JASPERENGINE_API Vector3 snapWorld(const Vector3& world, int pixelsPerUnit);
    JASPERENGINE_API float snapWorld(float world, int pixelsPerUnit);

}

#endif //PIXELART_PIXELSNAP_H
