// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Sliced draw mode's geometry (docs/specs/S45-sprite-atlas.md R6): a bordered frame's 4 x 4
// lattice keeps its corners at their native size at every sprite size, stretches the edges and
// the centre, shrinks the corners together only below their sum, keeps a trim, and has 54
// indices covering 18 triangles.

#include <gtest/gtest.h>

#include "sprite/SpriteFrameGeometry.h"

#include <set>

using namespace jasperengine;

namespace {

    SpriteFrame panelFrame() {
        SpriteFrame f;
        f.name = "panel";
        f.rect = Rect(8, 4, 24, 16);
        f.sourceSize = Vector2(24, 16);
        f.border = Vector4(4, 3, 4, 3);
        f.pivot = Vector2(12, 8);
        return f;
    }

    SpriteFrameGeometryInput sized(unsigned w, unsigned h) {
        SpriteFrameGeometryInput in;
        in.width = w;
        in.height = h;
        in.pivot = PivotPreset::BOTTOM_LEFT;
        in.flipY = true;   // a world camera's upright sprite
        in.pageWidth = 64;
        in.pageHeight = 32;
        return in;
    }

}

TEST(SpriteSlicedGeometry, CornersKeepTheirSizeAtEverySize) {
    const SpriteFrame frame = panelFrame();
    EXPECT_TRUE(spriteFrameHasBorder(frame));
    for (unsigned w : {24u, 48u, 101u}) {
        for (unsigned h : {16u, 32u, 77u}) {
            const SpriteSlicedMesh m = spriteFrameSliced(frame, sized(w, h));
            ASSERT_FALSE(m.empty);
            EXPECT_FLOAT_EQ(m.width, static_cast<float>(w));
            EXPECT_FLOAT_EQ(m.x[0], 0.0f);
            EXPECT_FLOAT_EQ(m.x[1], 4.0f);
            EXPECT_FLOAT_EQ(m.x[2], static_cast<float>(w) - 4.0f);
            EXPECT_FLOAT_EQ(m.x[3], static_cast<float>(w));
            // rows go up: the bottom border, the top border
            EXPECT_FLOAT_EQ(m.y[0], 0.0f);
            EXPECT_FLOAT_EQ(m.y[1], 3.0f);
            EXPECT_FLOAT_EQ(m.y[2], static_cast<float>(h) - 3.0f);
            EXPECT_FLOAT_EQ(m.y[3], static_cast<float>(h));
            // the UVs cut the frame at its border, the bottom row reading the image's bottom
            EXPECT_FLOAT_EQ(m.u[1], 4.0f / 24.0f);
            EXPECT_FLOAT_EQ(m.u[2], 20.0f / 24.0f);
            EXPECT_FLOAT_EQ(m.v[0], 1.0f);
            EXPECT_FLOAT_EQ(m.v[1], 13.0f / 16.0f);
            EXPECT_FLOAT_EQ(m.v[3], 0.0f);
        }
    }
    const SpriteSlicedMesh m = spriteFrameSliced(frame, sized(48, 32));
    EXPECT_FLOAT_EQ(m.textureRect.getX(), 8.0f / 64.0f);
    EXPECT_FLOAT_EQ(m.textureRect.getWidth(), 24.0f / 64.0f);
}

TEST(SpriteSlicedGeometry, NativeSizePivotAndSmallSprites) {
    const SpriteFrame frame = panelFrame();
    SpriteFrameGeometryInput native = sized(0, 0);
    native.pivot = PivotPreset::CENTER;
    const SpriteSlicedMesh n = spriteFrameSliced(frame, native);
    EXPECT_FLOAT_EQ(n.width, 24.0f);
    EXPECT_FLOAT_EQ(n.x[0], -12.0f);
    EXPECT_FLOAT_EQ(n.x[3], 12.0f);
    EXPECT_FLOAT_EQ(n.y[0], -8.0f);
    // narrower than both borders: the corners share the width
    const SpriteSlicedMesh small = spriteFrameSliced(frame, sized(6, 4));
    EXPECT_FLOAT_EQ(small.x[1], 3.0f);
    EXPECT_FLOAT_EQ(small.x[2], 3.0f);
    EXPECT_FLOAT_EQ(small.y[1], 2.0f);
    // a UI camera (y down): the image's top row at the lowest y, as createSprite's quad has it
    SpriteFrameGeometryInput ui = sized(48, 32);
    ui.uiCamera = true;
    ui.flipY = false;
    const SpriteSlicedMesh u = spriteFrameSliced(frame, ui);
    EXPECT_FLOAT_EQ(u.y[0], -32.0f);
    EXPECT_FLOAT_EQ(u.y[1], -29.0f);
    EXPECT_FLOAT_EQ(u.y[3], 0.0f);
    EXPECT_FLOAT_EQ(u.v[0], 0.0f);
    EXPECT_FLOAT_EQ(u.v[3], 1.0f);
}

TEST(SpriteSlicedGeometry, ATrimmedFrameKeepsItsTrim) {
    SpriteFrame frame = panelFrame();
    frame.offset = Vector2(2, 0);       // two transparent columns on the left trimmed away
    frame.rect = Rect(8, 4, 22, 16);
    const SpriteSlicedMesh m = spriteFrameSliced(frame, sized(48, 32));
    EXPECT_FLOAT_EQ(m.x[0], 2.0f);     // where the trimmed pixels start inside the corner
    EXPECT_FLOAT_EQ(m.x[1], 4.0f);
    EXPECT_FLOAT_EQ(m.u[0], 0.0f);
    EXPECT_FLOAT_EQ(m.u[1], 2.0f / 22.0f);
    EXPECT_FLOAT_EQ(m.x[3], 48.0f);
}

TEST(SpriteSlicedGeometry, IndicesCoverTheNineQuads) {
    const uint16_t* indices = spriteSlicedIndices();
    std::set<uint16_t> used;
    for (unsigned i = 0; i < SPRITE_SLICED_INDICES; i++) {
        ASSERT_LT(indices[i], SPRITE_SLICED_VERTICES);
        used.insert(indices[i]);
    }
    EXPECT_EQ(used.size(), SPRITE_SLICED_VERTICES);
    EXPECT_EQ(SPRITE_SLICED_INDICES, 54u);
    SpriteFrame plain = panelFrame();
    plain.border = Vector4(0, 0, 0, 0);
    EXPECT_FALSE(spriteFrameHasBorder(plain));
}
