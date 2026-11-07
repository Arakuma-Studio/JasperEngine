// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sprite:playTag's stepping (docs/specs/S44-aseprite-import.md R38): the sheet's durations, loops
// and the end of a tag, and steps of any size taking one pass (a huge speed never hangs a frame)

#include <gtest/gtest.h>

#include "component/SpriteComponent.h"
#include "sprite/SpriteSheet.h"
#include "subsystem/MeshSystem.h"

#include <cmath>
#include <limits>
#include <memory>

using namespace jasperengine;

namespace {

    // four frames of 100, 200, 100 and 100 ms playing 0..3
    SpriteComponent tagSprite(bool loop) {
        auto sheet = std::make_shared<SpriteSheetData>();
        const uint32_t durations[] = {100, 200, 100, 100};
        for (int i = 0; i < 4; i++) {
            SpriteFrame f;
            f.name = "f" + std::to_string(i);
            f.durationMs = durations[i];
            sheet->frames.push_back(f);
        }
        SpriteComponent sprite;
        sprite.sheetData = sheet;
        sprite.tagFrames = {0, 1, 2, 3};
        sprite.tagLoop = loop;
        sprite.tagPlaying = true;
        sprite.playingTag = "run";
        return sprite;
    }

}

TEST(SpriteTagPlayback, FollowsTheSheetsDurations) {
    SpriteComponent sprite = tagSprite(true);
    MeshSystem::stepSpriteTag(sprite, 0.05);
    EXPECT_EQ(sprite.frame, 0);
    MeshSystem::stepSpriteTag(sprite, 0.10);    // 150 ms: in the 200 ms frame
    EXPECT_EQ(sprite.frame, 1);
    MeshSystem::stepSpriteTag(sprite, 0.20);    // 350 ms
    EXPECT_EQ(sprite.frame, 2);
    MeshSystem::stepSpriteTag(sprite, 0.20);    // 550 ms: round the 500 ms loop to 50 ms
    EXPECT_EQ(sprite.frame, 0);
    EXPECT_TRUE(sprite.tagPlaying);

    SpriteComponent once = tagSprite(false);
    MeshSystem::stepSpriteTag(once, 0.45);
    EXPECT_EQ(once.frame, 3);
    EXPECT_TRUE(once.tagPlaying);
    MeshSystem::stepSpriteTag(once, 0.10);      // past the end: the last frame stays
    EXPECT_EQ(once.frame, 3);
    EXPECT_FALSE(once.tagPlaying);
}

// a step of 1e300 seconds (or a speed of 1e30) goes round the loop at once (stepping frame by
// frame would never return); a NaN or infinite step changes nothing
TEST(SpriteTagPlayback, HugeOrBrokenStepsTakeOnePass) {
    SpriteComponent sprite = tagSprite(true);
    sprite.tagSpeed = 1e30f;
    MeshSystem::stepSpriteTag(sprite, 1.0);
    EXPECT_GE(sprite.frame, 0);
    EXPECT_LE(sprite.frame, 3);
    EXPECT_TRUE(sprite.tagPlaying);
    sprite.tagSpeed = 1.0f;
    MeshSystem::stepSpriteTag(sprite, 1e300);
    EXPECT_LE(sprite.frame, 3);

    SpriteComponent once = tagSprite(false);
    MeshSystem::stepSpriteTag(once, 1e300);
    EXPECT_EQ(once.frame, 3);
    EXPECT_FALSE(once.tagPlaying);

    SpriteComponent broken = tagSprite(true);
    broken.frame = 2;
    MeshSystem::stepSpriteTag(broken, std::numeric_limits<double>::quiet_NaN());
    MeshSystem::stepSpriteTag(broken, std::numeric_limits<double>::infinity());
    broken.tagSpeed = std::numeric_limits<float>::infinity();
    MeshSystem::stepSpriteTag(broken, 0.016);
    EXPECT_EQ(broken.frame, 2);
}
