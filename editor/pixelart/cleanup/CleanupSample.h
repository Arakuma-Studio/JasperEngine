// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/fix.ts, export.ts, sprite.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// Turning the cells of a grid into true pixels (docs/specs/S46-pixel-art-cleanup.md R5, R7, R8):
// each cell is read from its middle only, and with a palette every pixel read votes for its
// nearest palette colour (S43's PaletteMatcher), so no in-between colour is ever made up. Also
// the fixed picture's crop and RGBA form, and a picture taken as pixel art at its own size.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // Reads the middle of every cell of a grid (inset clamped to 0-0.45, at least the centre pixel)
    CellSamples sampleCells(const RgbaImage& image, const Grid& grid, const SampleOptions& options);

    // The per-channel median of a cell's opaque colours (the upper middle of an even count)
    Rgba8 medianColor(const std::vector<uint32_t>& colors);

    // The colour each opaque cell would keep without a palette: what a palette is proposed from
    std::vector<Rgba8> cellColors(const CellSamples& samples);

    struct QuantizeOptions {
        std::vector<Rgba8> palette;           // empty: each cell keeps its median colour
        jasperengine::pixelart::ColorMetric metric = jasperengine::pixelart::ColorMetric::OkLab;
        Dither dither = Dither::None;
        double ditherStrength = 0.5;          // 0-1: how far a shade may be pushed (ordered dither)
    };

    // One colour per cell: a cell with more transparent pixels read than opaque ones is transparent
    PixelArt quantize(const CellSamples& samples, const QuantizeOptions& options);

    struct UsedColor {
        Rgba8 color;
        int count = 0;
    };
    // The palette colours the picture uses, in palette order, with their pixel counts
    std::vector<UsedColor> usedColors(const PixelArt& art);

    // The smallest box holding every opaque pixel (an empty or already tight picture is returned as is)
    PixelArt cropToContent(const PixelArt& art);

    // The pixels, each repeated `scale` times on both axes; transparent pixels are {0, 0, 0, 0}
    RgbaImage toRgba(const PixelArt& art, int scale = 1);

    // A picture taken as pixel art at its own size: alpha below `alphaThreshold` is transparent;
    // `partial` counts the pixels with 0 < alpha < 255 (pixel art has none)
    struct ArtOfImage {
        PixelArt art;
        int partial = 0;
    };
    ArtOfImage artOfImage(const RgbaImage& image, int alphaThreshold = 128);

}
