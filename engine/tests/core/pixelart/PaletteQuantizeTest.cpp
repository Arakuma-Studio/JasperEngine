// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Palette Quantize's table on the CPU (docs/specs/S43-palettes.md R20, R30, R47, AC12, AC14): the
// identity, palette-only output, the dither's share per block, determinism whatever the slicing,
// and the bake's time (printed: budgets on a quiet machine, docs/testing.md).

#include "pixelart/PaletteQuantize.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <set>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    // 64 colours spread over the cube (deterministic)
    Palette sixtyFour() {
        Palette palette;
        palette.name = "64";
        for (int i = 0; i < 64; i++) {
            palette.colors.push_back(unpackRgb(((uint32_t)i * 2654435761u) & 0xFFFFFFu));
        }
        return palette;
    }

    Palette blackAndWhite() {
        Palette palette;
        palette.colors = {{0, 0, 0}, {255, 255, 255}};
        return palette;
    }

    bool inPalette(const Palette& palette, Rgba8 color) {
        for (const Rgba8& entry : palette.colors) {
            if (entry.r == color.r && entry.g == color.g && entry.b == color.b) return true;
        }
        return false;
    }

}

TEST(PaletteQuantize, PaletteColoursPassThroughWithEveryDither) {
    const Palette palette = sixtyFour();
    // every dither on the Normal grid, and the High grid once (its bake is the slow one)
    const std::pair<int, int> cases[] = {{0, 32}, {2, 32}, {4, 32}, {8, 32}, {4, 64}};
    for (const auto& [dither, cells] : cases) {
        const QuantizeLut lut = bakeQuantizeLut(palette, ColorMetric::OkLab, dither, 1.0f, cells);
        for (const Rgba8& color : palette.colors) {
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    const Rgba8 out = lut.pick(color, dither, x, y);
                    // two colours in one cell keep the first one's identity only (a warning says so)
                    if (lut.warnings.empty()) {
                        EXPECT_EQ(out, color) << toHex(color) << " dither " << dither << " cells " << cells;
                    } else {
                        EXPECT_TRUE(inPalette(palette, out));
                    }
                }
            }
        }
    }
}

TEST(PaletteQuantize, EveryOutputIsAPaletteColour) {
    const Palette palette = sixtyFour();
    const QuantizeLut lut = bakeQuantizeLut(palette, ColorMetric::OkLab, 4, 1.0f, 32);
    for (uint32_t k = 0; k < 20000; k++) {
        const Rgba8 color = unpackRgb((k * 2246822519u) & 0xFFFFFFu);
        EXPECT_TRUE(inPalette(palette, lut.pick(color, 4, (int)(k % 7), (int)(k % 5)))) << toHex(color);
    }
}

TEST(PaletteQuantize, DitherShowsTheRatioPerBlock) {
    const Palette palette = blackAndWhite();
    const QuantizeLut lut = bakeQuantizeLut(palette, ColorMetric::OkLab, 4, 1.0f, 32);
    // linear light's mid grey (sRGB 188): about half the cells of a 4 x 4 block white
    const Rgba8 grey{188, 188, 188, 255};
    int white = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (lut.pick(grey, 4, x, y).r == 255) white++;
        }
    }
    EXPECT_GE(white, 5);
    EXPECT_LE(white, 11);
    // the pattern repeats every 4 cells
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            EXPECT_EQ(lut.pick(grey, 4, x, y), lut.pick(grey, 4, x + 4, y + 8));
        }
    }
    // no dither: the nearest alone, everywhere
    const QuantizeLut nearest = bakeQuantizeLut(palette, ColorMetric::OkLab, 0, 1.0f, 32);
    std::set<uint8_t> shades;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) shades.insert(nearest.pick(grey, 0, x, y).r);
    }
    EXPECT_EQ(shades.size(), 1u);
}

TEST(PaletteQuantize, SlicedBakeEqualsTheWholeOne) {
    const Palette palette = sixtyFour();
    const QuantizeLut whole = bakeQuantizeLut(palette, ColorMetric::OkLab, 4, 0.75f, 32);
    QuantizeLutJob job(palette, ColorMetric::OkLab, 4, 0.75f, 32);
    int slices = 0;
    while (!job.step(1e-9) && slices < 100000) slices++;
    EXPECT_TRUE(job.done());
    EXPECT_GT(slices, 1) << "it was baked in slices";
    EXPECT_EQ(job.result().cells, whole.cells);
    EXPECT_EQ(job.result().row, whole.row);
    EXPECT_EQ(bakeQuantizeLut(palette, ColorMetric::OkLab, 4, 0.75f, 32).cells, whole.cells);
    // on the job system's workers: the same bytes
    QuantizeLutJob parallel(palette, ColorMetric::OkLab, 4, 0.75f, 32);
    parallel.bakeParallel();
    EXPECT_TRUE(parallel.done());
    EXPECT_EQ(parallel.result().cells, whole.cells);
}

TEST(PaletteQuantize, LayoutAndSharedCells) {
    const QuantizeLut normal = bakeQuantizeLut(sixtyFour(), ColorMetric::Rgb, 0, 1.0f, 32);
    EXPECT_EQ(normal.width, 256);
    EXPECT_EQ(normal.height, 128);
    const QuantizeLut high = bakeQuantizeLut(blackAndWhite(), ColorMetric::OkLab, 8, 1.0f, 64);
    EXPECT_EQ(high.width, 512);
    EXPECT_EQ(high.height, 512);
    Palette close;
    // one 8-wide cell of the Normal grid, two 4-wide ones of the High grid
    close.colors = {{97, 100, 100}, {101, 100, 100}, {0, 0, 0}};
    const QuantizeLut shared = bakeQuantizeLut(close, ColorMetric::OkLab, 4, 1.0f, 32);
    ASSERT_EQ(shared.warnings.size(), 1u);
    EXPECT_EQ(shared.warnings[0].kind, "palette_cell_shared");
    EXPECT_TRUE(bakeQuantizeLut(close, ColorMetric::OkLab, 4, 1.0f, 64).warnings.empty());
}

// R30: the Normal bake of 64 colours with dithering (≤ 15 ms with jobs on a mid desktop, one job
// here) and the High one (≤ 120 ms): printed, judged on an idle machine
TEST(PaletteQuantize, BakeTimes) {
    const Palette palette = sixtyFour();
    for (int cells : {32, 64}) {
        const auto start = std::chrono::steady_clock::now();
        const QuantizeLut lut = bakeQuantizeLut(palette, ColorMetric::OkLab, 4, 1.0f, cells);
        const double single = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto parallelStart = std::chrono::steady_clock::now();
        QuantizeLutJob job(palette, ColorMetric::OkLab, 4, 1.0f, cells);
        job.bakeParallel();
        const double parallel = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parallelStart).count();
        std::printf("[bench] palette quantize bake %d^3, 64 colours, dithered: %.1f ms on one thread, %.1f ms on the job system\n",
                    cells, single, parallel);
        EXPECT_FALSE(lut.cells.empty());
        EXPECT_EQ(job.result().cells, lut.cells);
    }
}
