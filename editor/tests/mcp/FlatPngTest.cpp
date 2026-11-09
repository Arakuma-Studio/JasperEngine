// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The screenshot tools' blank-render check (BF23 F15): noise of a level or two on a flat image
// is still a blank frame, real content is not.

#include "mcp/tools/PlayViewCommon.h"

#include "stb_image_write.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

    std::vector<uint8_t> flatPngOf(const std::vector<uint8_t>& rgb, int width, int height) {
        std::vector<uint8_t> png;
        auto append = [](void* context, void* data, int size) {
            auto* bytes = static_cast<std::vector<uint8_t>*>(context);
            const uint8_t* begin = static_cast<const uint8_t*>(data);
            bytes->insert(bytes->end(), begin, begin + size);
        };
        EXPECT_NE(stbi_write_png_to_func(append, &png, width, height, 3, rgb.data(), width * 3), 0);
        return png;
    }

    std::vector<uint8_t> flatPngPixels(int width, int height, uint8_t r, uint8_t g, uint8_t b) {
        std::vector<uint8_t> rgb;
        for (int i = 0; i < width * height; i++) rgb.insert(rgb.end(), {r, g, b});
        return rgb;
    }

}

using jasperengine::editor::mcp::w3::isFlatPng;

TEST(FlatPng, ExactlyFlatIsFlat) {
    std::string colour;
    EXPECT_TRUE(isFlatPng(flatPngOf(flatPngPixels(16, 16, 10, 20, 30), 16, 16), colour));
    EXPECT_EQ(colour, "#0a141e");
}

TEST(FlatPng, NoiseOfOneLevelIsStillFlat) {
    std::vector<uint8_t> rgb = flatPngPixels(16, 16, 10, 20, 30);
    for (size_t i = 0; i < rgb.size(); i += 7) rgb[i] += 1;
    std::string colour;
    EXPECT_TRUE(isFlatPng(flatPngOf(rgb, 16, 16), colour));
    EXPECT_FALSE(colour.empty());
}

TEST(FlatPng, RealContentIsNotFlat) {
    std::vector<uint8_t> small = flatPngPixels(16, 16, 10, 20, 30);
    small[3] += 8;   // one pixel a visible step away
    std::string colour;
    EXPECT_FALSE(isFlatPng(flatPngOf(small, 16, 16), colour));

    // a slow gradient differs little between neighbours, a lot from end to end
    std::vector<uint8_t> gradient;
    for (int i = 0; i < 64; i++) gradient.insert(gradient.end(), {static_cast<uint8_t>(i), 0, 0});
    EXPECT_FALSE(isFlatPng(flatPngOf(gradient, 64, 1), colour));
}

TEST(FlatPng, TheToleranceIsTwoLevels) {
    std::vector<uint8_t> rgb = flatPngPixels(16, 16, 10, 20, 30);
    rgb[3] += 2;
    std::string colour;
    EXPECT_TRUE(isFlatPng(flatPngOf(rgb, 16, 16), colour));
    rgb[3] += 1;
    EXPECT_FALSE(isFlatPng(flatPngOf(rgb, 16, 16), colour));
}
