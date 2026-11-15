// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The shape rules port pixelfx's drawing code (src/core/sim.js: sparkle, the SPARK streak, SMOKE's
// three discs, Bits' SHARD and CHUNK, Lights' glow), MIT License, Copyright (c) 2026 Patch; sim.js
// adapts Sprite Fusion's pixel art VFX, MIT License, Copyright (c) 2026 Hugo Duprez. See NOTICE.md.

#ifndef PARTICLES_PARTICLEPIXEL_H
#define PARTICLES_PARTICLEPIXEL_H

// The CPU twin of the Pixel render mode (docs/specs/S49-pixel-particles.md R2, R9, R11): where a
// particle's pixel box lands on the base-pixel grid and which of its pixels each shape lights, in
// the same float32 operations and order as includes/particles_vs.glsl (pixelLayout) and
// includes/particles_pixel.glsl (pixelShape). The GPU tests compare read-backs with it, and the
// inspector's shape icons are drawn by it.

#include "Export.h"
#include "math/Vector2.h"
#include "particles/ParticleEffect.h"

#include <cstdint>
#include <vector>

namespace jasperengine { namespace particles {

    struct PixelParams;

    // a shape and its Renderer settings
    struct JASPERENGINE_API PixelShapeSpec {
        int shape = 0;                  // PixelShape, or -1 for a billboard of the Pixel Art module
        float streakLengthScale = 0.022f, streakMaxLength = 9.0f, streakHeadFraction = 0.45f;
        int streakWidth = 1, ringWidth = 1, glowBands = 3, rotationSteps = 16;
        bool discRim = false, dotShadeBottom = false;

        static PixelShapeSpec fromRenderer(const RendererModule& renderer);
        static PixelShapeSpec fromParams(const PixelParams& params);
    };

    // a particle's box on the grid: its lower-left corner (base pixels), its size in whole pixels,
    // Streak's and Shard's line (local pixels, head first) and Shard's / Chunk's angle
    struct PixelLayout {
        float cornerX = 0.0f, cornerY = 0.0f;
        float boxW = 1.0f, boxH = 1.0f;
        float n = 1.0f;                 // the size (x)
        int headX = 0, headY = 0, tailX = 0, tailY = 0;
        float angle = 0.0f;             // radians, counter-clockwise
    };

    // one pixel of a shape: the top tone and coverage, and the one under it (Puff's cap over
    // its core). Tones: 0 primary, 1 secondary, 2 + k the ramp's step k.
    struct PixelSample {
        bool drawn = false;
        int tone = 0;
        float cover = 1.0f;
        int tone2 = -1;
        float cover2 = 0.0f;
    };

    // a lit pixel of the grid (rasterizePixelShape)
    struct PixelCell {
        int x = 0, y = 0;               // base pixels of the grid
        int tone = 0;
        float cover = 1.0f;
    };

    class JASPERENGINE_API ParticlePixel {
    public:
        // the drawn size: n = max(1, round(size)) per axis (Integer Size), else max(1, size)
        static Vector2 drawnSize(Vector2 size, bool integerSize);
        // the box of a shape around `centre` (base pixels), R2's corner snap when `snap`;
        // velocity in base pixels per second (Streak), rotation in radians, clockwise as seen
        static PixelLayout layout(const PixelShapeSpec& spec, Vector2 centre, Vector2 n, Vector2 velocityPx, float rotation, bool snap);
        // the shape at local pixel (x, y) of the box; age is normalized (Puff's cap), rampCount
        // 0 without a ramp
        static PixelSample sample(const PixelShapeSpec& spec, const PixelLayout& layout, int x, int y, float age, int rampCount);
        // every pixel of the box the shape lights (any coverage), in grid pixels
        static void rasterize(const PixelShapeSpec& spec, const PixelLayout& layout, float age, int rampCount, std::vector<PixelCell>& out);
        // R11's line test: p on the pixel-perfect line h -> t widened to `width` across the minor
        // axis; `along` the step from h, `steps` the last step
        static bool onLine(int px, int py, int hx, int hy, int tx, int ty, int width, int& along, int& steps);
        // floor(a / b) for b > 0
        static int floorDiv(int a, int b);
        // the unit axis of an angle held to 1/65536 (the shaders' pixelAxis)
        static Vector2 axis(float angle);
    };

}}

#endif // PARTICLES_PARTICLEPIXEL_H
