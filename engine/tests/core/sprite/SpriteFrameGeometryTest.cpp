// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The trimmed quad of a sheet frame (docs/specs/S44-aseprite-import.md R49, AC21): a trimmed and
// an untrimmed packing of the same frame put every pixel at the same place, for the ten pivots,
// both flips and both camera kinds. The positions are found from createSprite's UV layout (v0 at
// v = flipY ? 1 : 0, v3 at the other end), independently of how the quad is built.

#include <gtest/gtest.h>

#include "sprite/SpriteFrameGeometry.h"

using namespace jasperengine;

namespace {

    // where a source pixel's centre (y down) lands on a quad drawing `frame`
    Vector2 placeOf(const SpriteFrameQuad& quad, const SpriteFrame& frame, bool flipY, float px, float py){
        const float u = (px - frame.offset.x) / frame.rect.getWidth();
        const float v = (py - frame.offset.y) / frame.rect.getHeight();
        const float x = quad.x0 + u * (quad.x1 - quad.x0);
        const float y = flipY ? quad.y0 + (1.0f - v) * (quad.y1 - quad.y0) : quad.y0 + v * (quad.y1 - quad.y0);
        return Vector2(x, y);
    }

    SpriteFrame frame(bool trimmed){
        SpriteFrame f;
        f.name = trimmed ? "trimmed" : "full";
        f.sourceSize = Vector2(16, 24);
        f.pivot = Vector2(5, 20);
        if (trimmed){
            f.rect = Rect(40, 8, 7, 9);
            f.offset = Vector2(3, 5);
        }else{
            f.rect = Rect(0, 30, 16, 24);
            f.offset = Vector2(0, 0);
        }
        return f;
    }

}

TEST(SpriteFrameGeometryTest, TrimmedEqualsUntrimmed){
    const PivotPreset pivots[] = {PivotPreset::CENTER, PivotPreset::TOP_CENTER, PivotPreset::BOTTOM_CENTER, PivotPreset::LEFT_CENTER,
                                  PivotPreset::RIGHT_CENTER, PivotPreset::TOP_LEFT, PivotPreset::BOTTOM_LEFT, PivotPreset::TOP_RIGHT,
                                  PivotPreset::BOTTOM_RIGHT, PivotPreset::FRAME};
    const SpriteFrame trimmed = frame(true);
    const SpriteFrame full = frame(false);
    for (PivotPreset pivot : pivots){
        for (bool ui : {false, true}){
            for (bool flipY : {false, true}){
                SpriteFrameGeometryInput in;
                in.pivot = pivot;
                in.uiCamera = ui;
                in.flipY = flipY;
                in.pixelsPerUnit = 16;
                in.pageWidth = 64;
                in.pageHeight = 64;
                const SpriteFrameQuad a = spriteFrameQuad(trimmed, in);
                const SpriteFrameQuad b = spriteFrameQuad(full, in);
                EXPECT_FALSE(a.empty);
                EXPECT_FLOAT_EQ(a.width, b.width);
                EXPECT_FLOAT_EQ(a.height, b.height);
                for (float py = 5.5f; py < 14.0f; py += 1.0f){
                    for (float px = 3.5f; px < 10.0f; px += 1.0f){
                        const Vector2 p = placeOf(a, trimmed, flipY, px, py);
                        const Vector2 q = placeOf(b, full, flipY, px, py);
                        EXPECT_NEAR(p.x, q.x, 1e-5f) << int(pivot) << " ui " << ui << " flip " << flipY;
                        EXPECT_NEAR(p.y, q.y, 1e-5f) << int(pivot) << " ui " << ui << " flip " << flipY;
                    }
                }
            }
        }
    }
}

TEST(SpriteFrameGeometryTest, UntrimmedIsCreateSpritesQuad){
    // a full frame with width/height 0 and PPU 1 is createSprite's quad of a 16x24 sprite
    const SpriteFrame full = frame(false);
    SpriteFrameGeometryInput in;
    in.pivot = PivotPreset::BOTTOM_CENTER;
    const SpriteFrameQuad q = spriteFrameQuad(full, in);
    EXPECT_FLOAT_EQ(q.x0, -8.0f);
    EXPECT_FLOAT_EQ(q.x1, 8.0f);
    EXPECT_FLOAT_EQ(q.y0, 0.0f);
    EXPECT_FLOAT_EQ(q.y1, 24.0f);
    // From Frame: the frame's pivot (5, 20) from the top-left is (5, 4) from the bottom-left
    in.pivot = PivotPreset::FRAME;
    const SpriteFrameQuad f = spriteFrameQuad(full, in);
    EXPECT_FLOAT_EQ(f.x0, -5.0f);
    EXPECT_FLOAT_EQ(f.y0, -4.0f);
}

TEST(SpriteFrameGeometryTest, SizePixelsPerUnitAndRatio){
    const SpriteFrame full = frame(false);
    SpriteFrameGeometryInput in;
    in.pivot = PivotPreset::BOTTOM_LEFT;
    in.pixelsPerUnit = 8;
    SpriteFrameQuad q = spriteFrameQuad(full, in);
    EXPECT_FLOAT_EQ(q.width, 2.0f);
    EXPECT_FLOAT_EQ(q.height, 3.0f);
    in.pixelRatio = Vector2(2, 1);   // wide pixels
    q = spriteFrameQuad(full, in);
    EXPECT_FLOAT_EQ(q.width, 4.0f);
    EXPECT_FLOAT_EQ(q.height, 3.0f);
    in.width = 32;                    // a set size stretches
    in.height = 12;
    q = spriteFrameQuad(full, in);
    EXPECT_FLOAT_EQ(q.x1 - q.x0, 32.0f);
    EXPECT_FLOAT_EQ(q.y1 - q.y0, 12.0f);
    // the texture rect is the frame's rect in its page
    in.pageWidth = 64;
    in.pageHeight = 128;
    q = spriteFrameQuad(full, in);
    EXPECT_FLOAT_EQ(q.textureRect.getY(), 30.0f / 128.0f);
    EXPECT_FLOAT_EQ(q.textureRect.getWidth(), 16.0f / 64.0f);
}

TEST(SpriteFrameGeometryTest, SameGeometrySwapsOnlyTheRect){
    SpriteFrame a = frame(true);
    SpriteFrame b = a;
    b.rect = Rect(0, 0, 7, 9);   // another place in the page, the same trim
    SpriteFrameGeometryInput in;
    in.pageWidth = 64;
    in.pageHeight = 64;
    EXPECT_TRUE(spriteFrameQuad(a, in).sameGeometry(spriteFrameQuad(b, in)));
    b.offset = Vector2(4, 5);
    EXPECT_FALSE(spriteFrameQuad(a, in).sameGeometry(spriteFrameQuad(b, in)));
    SpriteFrame empty = a;
    empty.rect = Rect(0, 0, 0, 0);
    EXPECT_TRUE(spriteFrameQuad(empty, in).empty);
}
