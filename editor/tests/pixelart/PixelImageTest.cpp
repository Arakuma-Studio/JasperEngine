// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// PixelImage (docs/specs/S44-aseprite-import.md R10, AC7): the index plane's invariant, hashes,
// PNG round trips.

#include "pixelart/PixelImage.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace jasperengine::editor::pixelart;

namespace {

    const std::vector<uint8_t> kPalette = {
        0, 0, 0, 255,       // 0
        255, 0, 0, 255,     // 1
        0, 255, 0, 128,     // 2
        0, 0, 255, 255,     // 3 (transparent below)
    };

    PixelImage indexed() {
        std::vector<uint8_t> idx = {0, 1, 2, 3, 1, 1, 3, 0, 2, 2, 2, 2};
        return PixelImage::fromIndices(4, 3, idx, kPalette, 3);
    }

    bool consistent(const PixelImage& image) {
        if (!image.hasIndices()) return true;
        for (int y = 0; y < image.height(); y++) {
            for (int x = 0; x < image.width(); x++) {
                const uint8_t i = image.indexAt(x, y);
                PixelRgba want{0, 0, 0, 0};
                if (static_cast<int>(i) != image.transparentIndex()) want = {kPalette[i * 4], kPalette[i * 4 + 1], kPalette[i * 4 + 2], kPalette[i * 4 + 3]};
                if (image.at(x, y) != want) return false;
            }
        }
        return true;
    }

}

TEST(PixelImageTest, IndicesAreThePaletteLookup) {
    const PixelImage image = indexed();
    ASSERT_TRUE(image.hasIndices());
    EXPECT_EQ(image.at(1, 0), (PixelRgba{255, 0, 0, 255}));
    EXPECT_EQ(image.at(3, 0), (PixelRgba{0, 0, 0, 0}));   // the transparent index
    EXPECT_EQ(image.at(2, 0), (PixelRgba{0, 255, 0, 128}));
    EXPECT_TRUE(consistent(image));
    // a size mismatch is refused
    EXPECT_TRUE(PixelImage::fromIndices(4, 3, {1, 2}, kPalette, -1).empty());
}

TEST(PixelImageTest, OperationsKeepOrDropThePlane) {
    PixelImage image = indexed();
    // crop inside keeps it; crop past the edge keeps it with the transparent index outside
    PixelImage inside = image.crop({1, 1, 2, 2});
    EXPECT_TRUE(inside.hasIndices());
    EXPECT_TRUE(consistent(inside));
    PixelImage outside = image.crop({2, 1, 4, 4});
    EXPECT_TRUE(outside.hasIndices());
    EXPECT_TRUE(consistent(outside));
    EXPECT_EQ(outside.indexAt(3, 3), 3);
    // without a transparent index the plane cannot cover outside pixels: dropped
    PixelImage opaque = PixelImage::fromIndices(2, 2, {0, 1, 1, 0}, kPalette, -1);
    EXPECT_FALSE(opaque.crop({1, 1, 2, 2}).hasIndices());
    EXPECT_TRUE(opaque.crop({0, 0, 2, 1}).hasIndices());

    // flips keep it
    PixelImage flipped = image;
    flipped.flipX();
    EXPECT_TRUE(flipped.hasIndices());
    EXPECT_EQ(flipped.indexAt(0, 0), 3);
    EXPECT_TRUE(consistent(flipped));
    flipped.flipY();
    EXPECT_EQ(flipped.indexAt(0, 0), 2);
    EXPECT_TRUE(consistent(flipped));

    // blit of an indexed image keeps it; of an RGBA one drops it (and says so)
    PixelImage target = indexed();
    EXPECT_TRUE(target.blit(inside, 0, 0));
    EXPECT_TRUE(target.hasIndices());
    EXPECT_TRUE(consistent(target));
    PixelImage rgba(2, 2);
    EXPECT_FALSE(target.blit(rgba, 0, 0));
    EXPECT_FALSE(target.hasIndices());

    // set drops it and returns that it did; a second set has nothing left to drop
    PixelImage edited = indexed();
    EXPECT_TRUE(edited.set(0, 0, {9, 9, 9, 255}));
    EXPECT_FALSE(edited.hasIndices());
    EXPECT_FALSE(edited.set(1, 0, {9, 9, 9, 255}));
    EXPECT_EQ(edited.at(0, 0), (PixelRgba{9, 9, 9, 255}));

    // setIndex keeps it
    PixelImage byIndex = indexed();
    EXPECT_TRUE(byIndex.setIndex(0, 0, 1, {255, 0, 0, 255}));
    EXPECT_TRUE(consistent(byIndex));
    // blending drops it
    PixelImage blended = indexed();
    EXPECT_TRUE(blended.blendOver(rgba, 0, 0, 255));
    EXPECT_FALSE(blended.hasIndices());
    // editing the bytes drops it
    bool dropped = false;
    PixelImage bytes = indexed();
    bytes.editPixels(&dropped);
    EXPECT_TRUE(dropped);
}

TEST(PixelImageTest, BlendOverIsAsepriteNormal) {
    PixelImage dst(1, 1);
    dst.set(0, 0, {200, 100, 50, 255});
    PixelImage src(1, 1);
    src.set(0, 0, {100, 200, 250, 255});
    dst.blendOver(src, 0, 0, 128);
    // Rc = Bc + (Sc - Bc) * 128 / 255
    EXPECT_EQ(dst.at(0, 0), (PixelRgba{static_cast<uint8_t>(200 + (100 - 200) * 128 / 255), static_cast<uint8_t>(100 + 100 * 128 / 255),
                                       static_cast<uint8_t>(50 + 200 * 128 / 255), 255}));
}

TEST(PixelImageTest, OpaqueBounds) {
    PixelImage image(10, 8);
    EXPECT_TRUE(image.opaqueBounds().empty());
    image.set(3, 2, {1, 1, 1, 10});
    image.set(6, 5, {1, 1, 1, 255});
    EXPECT_EQ(image.opaqueBounds(), (IntRect{3, 2, 4, 4}));
    EXPECT_EQ(image.opaqueBounds(10), (IntRect{6, 5, 1, 1}));
}

TEST(PixelImageTest, HashIsStableAndCoversThePlane) {
    const PixelImage image = indexed();
    // a fixed value: the same on every platform and run (XXH64 of size, RGBA and indices)
    const uint64_t h = image.hash();
    EXPECT_EQ(h, indexed().hash());
    PixelImage rgbaOnly = image;
    rgbaOnly.dropIndices();
    EXPECT_NE(rgbaOnly.hash(), h);
    EXPECT_FALSE(rgbaOnly == image);
    PixelImage changed = image;
    changed.setIndex(0, 0, 1, {255, 0, 0, 255});
    EXPECT_NE(changed.hash(), h);
    EXPECT_EQ(PixelImage(3, 3).hash(), PixelImage(3, 3).hash());
    EXPECT_NE(PixelImage(3, 3).hash(), PixelImage(9, 1).hash());
}

TEST(PixelImageTest, PngRoundTripIsLossless) {
    PixelImage image(7, 5);
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 7; x++) image.set(x, y, {static_cast<uint8_t>(x * 37), static_cast<uint8_t>(y * 51), static_cast<uint8_t>(x * y), static_cast<uint8_t>(x * 40)});
    }
    const std::string png = image.encodePng();
    ASSERT_FALSE(png.empty());
    EXPECT_EQ(png, image.encodePng());   // deterministic
    std::string error;
    std::optional<PixelImage> back = PixelImage::decodePng(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(png.data()), png.size()), &error);
    ASSERT_TRUE(back.has_value()) << error;
    EXPECT_EQ(*back, image);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "je-pixelimage-test.png";
    ASSERT_TRUE(image.savePng(path));
    std::optional<PixelImage> loaded = PixelImage::loadPng(path, &error);
    std::filesystem::remove(path);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(*loaded, image);
    EXPECT_FALSE(PixelImage::decodePng(std::span<const uint8_t>(), &error).has_value());
}
