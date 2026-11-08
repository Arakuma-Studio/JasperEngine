// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 R21, review: a capture's frames are drawn on transparent black, premultiplied. Bake to Sprite
// Sheet keys them on alpha (an additive or Alpha-faded pixel keeps its own colour, never a key
// colour's blend), and the contact sheet composites them over its background.

#include "pixelart/particles/ParticleCapture.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor::pixelart;

namespace {

    std::vector<CapturedFrame> oneFrame(){
        CapturedFrame frame;
        frame.image = PixelImage(4, 1);
        frame.image.set(0, 0, PixelRgba{0, 0, 0, 0});          // nothing drawn
        frame.image.set(1, 0, PixelRgba{255, 0, 0, 255});      // an opaque (dithered) pixel
        frame.image.set(2, 0, PixelRgba{64, 32, 0, 128});      // a half-faded orange, premultiplied
        frame.image.set(3, 0, PixelRgba{255, 0, 255, 255});    // the old key colour, drawn
        return {frame};
    }

}

TEST(ParticleCapture, StripKeysOnAlpha){
    const PixelImage strip = particleSpriteStrip(oneFrame(), 4, 1, 1, 0);
    ASSERT_EQ(strip.width(), 4);
    EXPECT_EQ(strip.at(0, 0), (PixelRgba{0, 0, 0, 0}));
    EXPECT_EQ(strip.at(1, 0), (PixelRgba{255, 0, 0, 255}));
    EXPECT_EQ(strip.at(2, 0), (PixelRgba{128, 64, 0, 128})) << "its own colour at its alpha, nothing of a key";
    EXPECT_EQ(strip.at(3, 0), (PixelRgba{255, 0, 255, 255})) << "a magenta pixel the effect drew stays";
}

TEST(ParticleCapture, SheetCompositesOverTheBackground){
    const PixelImage sheet = particleContactSheet(oneFrame(), 4, 1, 1, 1, jasperengine::pixelart::Rgba8{255, 0, 255, 255}, false);
    EXPECT_EQ(sheet.at(0, 0), (PixelRgba{255, 0, 255, 255}));
    EXPECT_EQ(sheet.at(1, 0), (PixelRgba{255, 0, 0, 255}));
    EXPECT_EQ(sheet.at(2, 0), (PixelRgba{64 + 127, 32, 127, 255}));
}
