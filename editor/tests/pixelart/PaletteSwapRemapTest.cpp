// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC9 (docs/specs/S43-palettes.md R14, R23): Apply Palette's Remap Indices of a 64 x 64 sprite to
// a target palette equals Palette Swap's lookup byte for byte. PaletteSwapData::swap is the CPU
// twin the GPU test (engine/tests/core/render/PaletteSwapGpuTest.cpp) checks the shader against,
// so the remap, the twin and the GPU agree.

#include "pixelart/ApplyPalette.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteSwap.h"
#include "pixelart/RgbaImage.h"

#include <gtest/gtest.h>

using namespace jasperengine;
namespace pxc = jasperengine::pixelart;
namespace pxe = jasperengine::editor::pixelart;

TEST(PaletteSwapRemap, MatchesTheSwapByteForByte) {
    pxc::Palette source;
    source.name = "Source";
    source.colors = {{0, 0, 0, 0}, {26, 28, 44}, {93, 39, 93}, {177, 62, 83}, {239, 125, 87}, {255, 205, 117}, {167, 240, 112},
                     {56, 183, 100}, {37, 113, 121}, {41, 54, 111}, {59, 93, 201}, {65, 166, 246}, {115, 239, 247}, {244, 244, 244},
                     {148, 176, 194}, {86, 108, 134}, {51, 60, 87}};
    std::vector<pxc::Rgba8> target = source.colors;
    for (size_t i = 1; i < target.size(); i++) target[i] = pxc::Rgba8{(uint8_t)(255 - target[i].r), target[i].b, target[i].g, 255};
    target[3].a = 0;     // a hidden part
    target[5].a = 128;   // a translucent one

    pxe::RgbaImage image(64, 64);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            const int pick = (x * 7 + y * 13) % 20;
            pxc::Rgba8 color = pick < (int)source.colors.size() ? source.colors[pick] : pxc::Rgba8{(uint8_t)(x * 4), (uint8_t)(y * 4), 9};
            color.a = (x + y) % 9 == 0 ? (uint8_t)(x * 4) : 255;   // some partial alpha
            if (pick == 0) color = pxc::Rgba8{0, 0, 0, 255};        // the transparent slot's RGB, opaque
            image.set(x, y, color);
        }
    }

    pxc::PaletteSwapData data;
    std::vector<pxc::Diagnostic> diagnostics;
    ASSERT_TRUE(pxc::buildPaletteSwapData(source, {{"Target", target}}, data, diagnostics));
    const pxe::RgbaImage remapped = pxe::remapIndices(image, source.colors, target);
    int differing = 0;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            if (remapped.at(x, y) != data.swap(image.at(x, y), 1)) differing++;
        }
    }
    EXPECT_EQ(differing, 0);
    // and something did swap: the image is not unchanged
    EXPECT_NE(remapped, image);
}
