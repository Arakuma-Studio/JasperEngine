// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/demo.ts and packages/tools/test/synthetic.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// Fake "AI pixel art" from known sprites (docs/specs/S46-pixel-art-cleanup.md §4.1): a sprite
// upscaled by a factor that need not be whole, drifting, blurred and noisy; sheets with painted
// grids and labels; warped grids. The ported tests check the cleanup gets each sprite back
// exactly, with ritmo's generator and seeds, so their numbers hold. The Pixel Art Cleanup window's
// "Try a Demo" shows demoImage.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // demo.ts's seeded LCG: the same seed draws the same picture
    class Random {
    public:
        explicit Random(uint32_t seed) : state(seed) {}
        double operator()() {
            state = state * 1664525u + 1013904223u;
            return state / 4294967296.0;
        }

    private:
        uint32_t state;
    };

    struct Upscale {
        double scale = 1;    // source pixels per sprite pixel
        double offset = 0;   // where the first full cell starts; the pixels before belong to the first cell
        double drift = 0;    // how much the cell grows from the first to the last (0.1: 10 % bigger)
        int blur = 0;        // 3 x 3 box blur passes
        double noise = 0;    // noise amplitude per channel
        uint32_t seed = 7;
    };

    // The boundaries of each sprite pixel along one axis, in source pixels
    std::vector<double> boundaries(int count, const Upscale& upscale);
    RgbaImage upscale(const PixelArt& sprite, const Upscale& options);

    // The demo's sprite: a 16 x 16 mushroom in Sweetie 16's colours
    PixelArt demoSprite();
    // The mushroom as an image generator might draw it: about 23x, uneven, blurred, noisy, every
    // colour slightly off its palette
    RgbaImage demoImage();

    // Sweetie 16, the palette ritmo's tests draw with
    std::vector<Rgba8> sweetie16();

    // A sprite in random blobs of the palette's colours, with a transparent border when asked
    PixelArt makeSprite(int width, int height, const std::vector<Rgba8>& palette, uint32_t seed, bool transparentBorder = false);
    // The sprite in the middle of a transparent area `margin` pixels wider on every side
    PixelArt pad(const PixelArt& sprite, int margin);
    // The sprite moved by whole pixels inside a transparent picture of the given size
    PixelArt place(const PixelArt& sprite, int width, int height, int x, int y);

    // An animation sheet as image generators make them: a dark background with a painted grid, a
    // box around each frame, the frames upscaled and blurred (each at its own offset unless
    // `jitter` is off) and a light label under each box. `frames` are where each frame's art was drawn.
    struct FakeSheet {
        RgbaImage image;
        std::vector<FrameBox> frames;
    };
    FakeSheet fakeSheet(const std::vector<PixelArt>& frames, double scale, uint32_t seed = 5, bool jitter = true);

    // The sprite on a warped grid: every line bent by a sine of `bend` cells (`period` cells long)
    RgbaImage warped(const PixelArt& sprite, double scale, double offset, double bend, double period);

}
