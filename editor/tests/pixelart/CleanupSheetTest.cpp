// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/test/animation.test.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// S46 AC8 (R19-R21): finding frames on a sheet, grouping them into animations, and the per-frame
// fix on one cell size, one palette and one canvas. One for one with ritmo's tests and the same
// generator, seeds and numbers; its GIF and ZIP cases are out of S46's scope (§2), and its recipe
// cases are in CleanupRecipeTest.

#include "pixelart/cleanup/CleanupAnimation.h"
#include "pixelart/cleanup/CleanupSheet.h"
#include "pixelart/cleanup/CleanupSheetOutput.h"
#include "pixelart/cleanup/CleanupSynthetic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <set>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::cleanup;
using jasperengine::pixelart::Rgba8;
using nlohmann::ordered_json;

namespace {

    std::vector<PixelArt> bobbing(int count) {
        const std::vector<Rgba8> sweetie = sweetie16();
        // Warm colours with a dark purple outline: none as dark as the background
        const std::vector<Rgba8> fire = {sweetie[2], sweetie[3], sweetie[4], sweetie[1]};
        const PixelArt sprite = makeSprite(10, 12, fire, 3, true);
        std::vector<PixelArt> frames;
        for (int f = 0; f < count; f++) frames.push_back(place(sprite, 12, 16, 1, 2 + (f % 2)));
        return frames;
    }

    Recipe sheetRecipe() {
        Recipe recipe;
        recipe.removeBackground = true;
        return recipe;
    }

    std::vector<int> opaqueRows(const PixelArt& art) {
        std::vector<int> rows;
        for (int p = 0; p < (int)art.indices.size(); p++) {
            if (art.indices[p] != TRANSPARENT) rows.push_back(p / art.width);
        }
        return rows;
    }

    int maxOf(const std::vector<int>& values) { return *std::max_element(values.begin(), values.end()); }

    RgbaImage paintedImage(int width, int height) {
        RgbaImage image(width, height);
        return image;
    }

    void fillRect(RgbaImage& image, int x, int y, int w, int h, PixelRgba color) {
        for (int j = y; j < y + h; j++) {
            for (int i = x; i < x + w; i++) image.set(i, j, color);
        }
    }

    FrameBox boxAt(int x, int y, int width, int height) { return FrameBox{x, y, width, height}; }

    PixelArt dot(int x, int y) {
        PixelArt art;
        art.width = art.height = 4;
        art.palette = {Rgba8{255, 0, 0, 255}};
        art.indices.assign(16, TRANSPARENT);
        art.indices[y * 4 + x] = 0;
        return art;
    }

    class CleanupSheetFinding : public ::testing::Test {
    protected:
        const std::vector<PixelArt> frames = bobbing(5);
        const FakeSheet fake = fakeSheet(frames, 6);
        const Recipe recipe = sheetRecipe();
    };

}

TEST_F(CleanupSheetFinding, OneBoxPerFrameAroundItsArtAndTheLabelsDropped) {
    AnimationCache cache;
    const std::vector<SheetFrame> found = detectFrames(fake.image, recipe, cache);
    ASSERT_EQ(found.size(), 5u);
    for (size_t i = 0; i < found.size(); i++) {
        const FrameBox& art = fake.frames[i];
        // The box holds the art (the blur may spill a pixel or two)
        EXPECT_LE(found[i].box.x, art.x + 6);
        EXPECT_GE(found[i].box.x + found[i].box.width, art.x + art.width - 6);
        EXPECT_LT(found[i].box.y + found[i].box.height, fake.image.height() - 10);
    }
    EXPECT_TRUE(isAnimationSheet(fake.image, recipe, found, cache));
}

// ritmo: "splits blocks side by side whose rows do not line up, keeps a thrown bomb in its frame,
// and keeps a last faint frame, still an animation"
TEST(CleanupSheet, BlocksSideBySideAThrownBombAndAFaintLastFrame) {
    // Already cleared: opaque art on transparency. Left, walking: 4 frames in each of two rows.
    // Right, throwing: 4 frames, each a body and a bomb 3 pixels from it, in one row that spans
    // the gap between the left block's rows. Below, an explosion ending in a wisp of 3x3 pixels.
    RgbaImage image = paintedImage(600, 140);
    const PixelRgba red{200, 60, 40, 255};
    const std::vector<int> walking = {10, 60, 110, 160};
    for (int x : walking) {
        fillRect(image, x, 10, 30, 30, red);
        fillRect(image, x, 52, 30, 30, red);
    }
    const std::vector<int> throwing = {224, 289, 354, 419};
    for (int x : throwing) {
        fillRect(image, x, 20, 30, 42, red);
        fillRect(image, x + 33, 50, 8, 8, red);
    }
    for (int x : walking) fillRect(image, x, 95, 30, 30, red);
    fillRect(image, 210, 108, 3, 3, red);

    const std::vector<FrameBox> boxes = findFrames(image);
    auto holding = [&](int x, int y, int w, int h) {
        int n = 0;
        for (const FrameBox& box : boxes) {
            if (box.x <= x && box.y <= y && box.x + box.width >= x + w && box.y + box.height >= y + h) n++;
        }
        return n;
    };
    ASSERT_EQ(boxes.size(), 17u);
    for (int x : walking) {
        EXPECT_EQ(holding(x, 10, 30, 30), 1);
        EXPECT_EQ(holding(x, 52, 30, 30), 1);
        EXPECT_EQ(holding(x, 95, 30, 30), 1);
    }
    // Each throw is one frame with its bomb
    for (int x : throwing) EXPECT_EQ(holding(x, 20, 41, 42), 1);
    EXPECT_EQ(holding(210, 108, 3, 3), 1);
    // The faint last frame does not make the sheet a picture
    EXPECT_TRUE(looksAnimated(image, boxes));
    // Walking's two rows, throwing's row, and the explosion: four animations, the wisp in the explosion
    std::vector<int> counts;
    for (const SheetAnimation& animation : groupFrames(boxes)) counts.push_back((int)animation.frames.size());
    EXPECT_EQ(counts, (std::vector<int>{4, 4, 4, 5}));
}

// ritmo: "paints out a grid whose lines are as dark as the art's outline, and keeps the outline"
TEST(CleanupSheet, AGridAsDarkAsTheOutlineIsPaintedOutAndTheOutlineKept) {
    // A navy fill with black lines every 10 pixels, and a black-outlined red square across two lines
    const int width = 60, height = 60;
    RgbaImage image(width, height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const bool inside = x >= 15 && x < 45 && y >= 15 && y < 45;
            const bool edge = inside && (x < 17 || x >= 43 || y < 17 || y >= 43);
            const PixelRgba color = edge ? PixelRgba{0, 4, 14, 255}
                                    : inside ? PixelRgba{200, 40, 30, 255}
                                    : x % 10 == 0 || y % 10 == 0 ? PixelRgba{0, 5, 15, 255}
                                                                 : PixelRgba{28, 36, 53, 255};
            image.set(x, y, color);
        }
    }
    const std::vector<Rgba8> colors = sheetBackground(image);
    ASSERT_EQ(colors.size(), 2u);
    const Unlined unlined = removeGridLines(image, colors, 0.05);
    ASSERT_EQ(unlined.colors.size(), 1u);
    const Cleared cleared = clearSheet(unlined.image, unlined.colors, 0.05);
    // The background is clear, lines included; the outline and the inside are not
    EXPECT_EQ(cleared.mask[5 * width + 5], 1);
    EXPECT_EQ(cleared.mask[10 * width + 5], 1);
    EXPECT_EQ(cleared.mask[30 * width + 10], 1);
    EXPECT_EQ(cleared.mask[30 * width + 15], 0);
    EXPECT_EQ(cleared.mask[15 * width + 30], 0);
    EXPECT_EQ(cleared.mask[30 * width + 30], 0);
}

TEST(CleanupSheet, ASinglePictureIsNotASheet) {
    const RgbaImage image = demoImage();
    EXPECT_FALSE(looksAnimated(image, findFrames(image)));
}

// ritmo: "reads every frame on one cell size, one palette, and one canvas, with the feet on one row"
TEST_F(CleanupSheetFinding, EveryFrameOnOneCellOnePaletteOneCanvasFeetOnOneRow) {
    AnimationCache cache;
    Sheet sheet = withFrames(Sheet{}, detectFrames(fake.image, recipe, cache));
    sheet.anchor = Anchor::Feet;
    const AnimationOutcome outcome = runSheet(fake.image, recipe, sheet, cache);
    EXPECT_GT(outcome.cellWidth, 5.5);
    EXPECT_LT(outcome.cellWidth, 6.5);
    // One row of frames: one animation
    ASSERT_EQ(outcome.clips.size(), 1u);
    const ClipOutcome& clip = outcome.clips[0];
    ASSERT_EQ(clip.placed.size(), 5u);
    const std::vector<Rgba8>& palette = clip.placed[0].art.palette;
    for (const ClipFrame& frame : clip.placed) {
        EXPECT_EQ(frame.art.palette, palette);
        EXPECT_EQ(frame.art.width, clip.width);
        EXPECT_EQ(frame.art.height, clip.height);
        // Feet anchoring: the bob is gone, every frame's lowest pixel is on one row
        EXPECT_EQ(maxOf(opaqueRows(frame.art)), maxOf(opaqueRows(clip.placed[0].art)));
    }
    // About the sprite's own size (12 by 14 with the bob, plus padding)
    EXPECT_GE(clip.width, 10);
    EXPECT_LE(clip.width, 15);
}

// ritmo: "keeps each frame's height as drawn on the ground anchor, so the bob stays"
TEST_F(CleanupSheetFinding, TheGroundAnchorKeepsTheBob) {
    // Frames on one pixel grid, as a generator draws them: their heights are whole pixels apart.
    // The cell size is given: on a sprite this small, finding it is off by a percent, and that
    // alone moves rows.
    const FakeSheet onGrid = fakeSheet(frames, 6, 5, false);
    Recipe given = recipe;
    given.grid.kind = GridKind::Cells;
    given.grid.spec = {6, 6, 0, 0};
    AnimationCache cache;
    const Sheet sheet = withFrames(Sheet{}, detectFrames(onGrid.image, given, cache));
    const AnimationOutcome outcome = runSheet(onGrid.image, given, sheet, cache);
    ASSERT_FALSE(outcome.clips.empty());
    const std::vector<ClipFrame>& placed = outcome.clips[0].placed;
    std::vector<int> lowest;
    for (const ClipFrame& frame : placed) lowest.push_back(maxOf(opaqueRows(frame.art)));
    // The sprite is one pixel lower on odd frames
    std::vector<int> relative;
    for (int row : lowest) relative.push_back(row - lowest[0]);
    EXPECT_EQ(relative, (std::vector<int>{0, 1, 0, 1, 0}));
    // Sideways, the bodies still line up
    std::set<int> left;
    for (const ClipFrame& frame : placed) {
        int least = std::numeric_limits<int>::max();
        for (int p = 0; p < (int)frame.art.indices.size(); p++) {
            if (frame.art.indices[p] != TRANSPARENT) least = std::min(least, p % frame.art.width);
        }
        left.insert(least);
    }
    EXPECT_EQ(left.size(), 1u);
}

TEST_F(CleanupSheetFinding, AnExcludedFrameIsLeftOutAndTheRestKept) {
    AnimationCache cache;
    std::vector<SheetFrame> found = detectFrames(fake.image, recipe, cache);
    ASSERT_EQ(found.size(), 5u);
    found[2].include = false;
    const AnimationOutcome outcome = runSheet(fake.image, recipe, withFrames(Sheet{}, found), cache);
    std::vector<int> indices;
    for (const ClipFrame& frame : outcome.clips[0].placed) indices.push_back(frame.index);
    EXPECT_EQ(indices, (std::vector<int>{0, 1, 3, 4}));
}

// S46's own: an auto palette is proposed once for the whole animation, never per frame (R21)
TEST_F(CleanupSheetFinding, AnAutoPaletteIsSharedByEveryFrame) {
    Recipe automatic = recipe;
    automatic.paletteMode = PaletteMode::Auto;
    automatic.autoPalette = AutoPalette{4, ProposeMethod::Balanced};
    AnimationCache cache;
    const Sheet sheet = withFrames(Sheet{}, detectFrames(fake.image, automatic, cache));
    const AnimationOutcome outcome = runSheet(fake.image, automatic, sheet, cache);
    ASSERT_TRUE(outcome.palette);
    EXPECT_LE(outcome.palette->colors.size(), 4u);
    EXPECT_FALSE(outcome.palette->colors.empty());
    for (const FixedFrame& frame : outcome.frames) EXPECT_EQ(frame.art.palette, outcome.frames[0].art.palette);
}

// R22: an animated source's frames skip slicing and keep their own delays
TEST(CleanupSheet, AnimatedSourcesSkipSlicingAndKeepTheirDelays) {
    const std::vector<PixelArt> frames = bobbing(3);
    std::vector<RgbaImage> images;
    Upscale options;
    options.scale = 6;
    for (const PixelArt& art : frames) images.push_back(upscale(art, options));
    Recipe recipe;
    AnimationCache cache;
    const AnimationOutcome outcome = runFrames(images, {50, 120, 200}, "hero", recipe, std::nullopt, cache);
    ASSERT_EQ(outcome.clips.size(), 1u);
    EXPECT_EQ(outcome.clips[0].name, "hero");
    ASSERT_EQ(outcome.clips[0].placed.size(), 3u);
    const std::vector<AtlasClip> clips = atlasClips(outcome);
    ASSERT_EQ(clips.size(), 1u);
    EXPECT_EQ(clips[0].durationsMs, (std::vector<int>{50, 120, 200}));
    // A name a game cannot look up falls back to a generated one
    AnimationCache other;
    EXPECT_EQ(runFrames(images, {}, "my hero", recipe, std::nullopt, other).clips[0].name, "animation-1");
}

// ritmo: "makes one animation per row, splits a row across two blocks where they meet, and keeps a
// row of another count whole"
TEST(CleanupSheet, OneAnimationPerRowSplitAcrossTwoBlocks) {
    std::vector<FrameBox> boxes;
    // Across both blocks: 4 walking frames over the left block, 4 throws over the right one
    for (int x : {0, 20, 40, 60}) boxes.push_back(boxAt(x, 0, 20, 20));
    for (int x : {100, 130, 160, 190}) boxes.push_back(boxAt(x, 0, 30, 20));
    // The left block's next two rows, beside one taller row of the right block
    for (int x : {0, 20, 40, 60}) boxes.push_back(boxAt(x, 20, 20, 20));
    for (int x : {100, 130, 160, 190}) boxes.push_back(boxAt(x, 20, 30, 40));
    for (int x : {0, 20, 40, 60}) boxes.push_back(boxAt(x, 40, 20, 20));
    // An explosion under both, with a box edge right where the blocks meet: 6 frames, not 4 and 4
    for (int x : {0, 30, 60, 90, 120, 150}) boxes.push_back(boxAt(x, 60, 30, 20));
    const std::vector<SheetAnimation> animations = groupFrames(boxes);
    std::vector<std::vector<int>> frames;
    std::vector<std::string> names;
    for (const SheetAnimation& animation : animations) {
        frames.push_back(animation.frames);
        names.push_back(animation.name);
    }
    EXPECT_EQ(frames, (std::vector<std::vector<int>>{{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11}, {12, 13, 14, 15}, {16, 17, 18, 19}, {20, 21, 22, 23, 24, 25}}));
    EXPECT_EQ(names, (std::vector<std::string>{"animation-1", "animation-2", "animation-3", "animation-4", "animation-5", "animation-6"}));
}

// ritmo: "keeps every frame in one animation as frames and animations change"
TEST(CleanupSheet, EveryFrameStaysInOneAnimationAsTheyChange) {
    auto frameLists = [](const Sheet& sheet) {
        std::vector<std::vector<int>> lists;
        for (const SheetAnimation& animation : sheet.animations) lists.push_back(animation.frames);
        return lists;
    };
    std::vector<SheetFrame> four;
    for (int x : {0, 20, 40, 60}) four.push_back(frameOf(boxAt(x, 0, 20, 20)));
    Sheet sheet = withFrames(Sheet{}, four);
    EXPECT_EQ(frameLists(sheet), (std::vector<std::vector<int>>{{0, 1, 2, 3}}));
    sheet = splitAnimation(sheet, 0, 2);
    ASSERT_EQ(sheet.animations.size(), 2u);
    EXPECT_EQ(sheet.animations[0].name, "animation-1");
    EXPECT_EQ(sheet.animations[1].name, "animation-1-2");
    EXPECT_EQ(frameLists(sheet), (std::vector<std::vector<int>>{{0, 1}, {2, 3}}));
    // A frame added after frame 1 joins its animation, and the later frames move up one
    sheet = insertFrame(sheet, 2, frameOf(boxAt(80, 0, 20, 20)));
    EXPECT_EQ(frameLists(sheet), (std::vector<std::vector<int>>{{0, 1, 2}, {3, 4}}));
    // Removing every frame of an animation removes the animation
    sheet = removeFrame(removeFrame(sheet, 3), 3);
    EXPECT_EQ(frameLists(sheet), (std::vector<std::vector<int>>{{0, 1, 2}}));
    sheet = splitAnimation(sheet, 0, 1);
    sheet = renameAnimation(sheet, 1, "walk left");
    EXPECT_EQ(sheet.animations[1].name, "walk-left");
    // A taken name gets a number; one a game cannot look up is refused
    EXPECT_EQ(renameAnimation(sheet, 0, "walk-left").animations[0].name, "walk-left-2");
    EXPECT_EQ(renameAnimation(sheet, 0, "walk/left").animations[0].name, "animation-1");
    sheet = joinAnimation(sheet, 0);
    ASSERT_EQ(sheet.animations.size(), 1u);
    EXPECT_EQ(sheet.animations[0].name, "animation-1");
    EXPECT_EQ(sheet.animations[0].frames, (std::vector<int>{0, 1, 2}));
}

// ritmo: "puts every frame's anchor on one spot, on the smallest canvas"
TEST(CleanupSheet, LayOutPutsAnchorsOnOneSpotOnTheSmallestCanvas) {
    const Layout laid = layOut({dot(0, 0), dot(3, 3)}, {Point{0, 0}, Point{-3, -3}}, 0);
    EXPECT_EQ(laid.width, 1);
    EXPECT_EQ(laid.height, 1);
    ASSERT_EQ(laid.frames.size(), 2u);
    EXPECT_EQ(laid.frames[0].art.indices[0], 0);
    EXPECT_EQ(laid.frames[1].art.indices[0], 0);
}

TEST(CleanupSheet, PlayOrderGoesThereAndBackWithoutRepeatingTheEnds) {
    EXPECT_EQ(playOrder(4, Loop::Forward), (std::vector<int>{0, 1, 2, 3}));
    EXPECT_EQ(playOrder(4, Loop::PingPong), (std::vector<int>{0, 1, 2, 3, 2, 1}));
}

// ritmo: "lays each animation out from a new row and names its frames"; frames are keyed
// "<anim>-<n>" in S46 (R23), "<anim> <n>" in ritmo
TEST(CleanupSheet, EachAnimationFromANewRowWithNamedFrames) {
    AtlasClip fire;
    fire.name = "fire";
    for (int i = 0; i < 5; i++) fire.frames.push_back(dot(1, 1));
    fire.anchor = {1, 3};
    fire.fps = 10;
    fire.loop = Loop::PingPong;
    // A second animation of another size: it starts its own row, under the first
    PixelArt big;
    big.width = 6;
    big.height = 2;
    big.palette = fire.frames[0].palette;
    big.indices.assign(12, 0);
    AtlasClip smoke;
    smoke.name = "smoke";
    smoke.frames = {big, big};
    smoke.anchor = {3, 1};
    smoke.fps = 5;
    const std::vector<AtlasClip> clips = {fire, smoke};
    const Atlas atlas = atlasOf(clips, {2, 1});
    // Fire: 3 rows of 4 high with 1 between (14), then 1, then smoke: 1 row of 2 (17)
    EXPECT_EQ(atlas.art.width, 13);
    EXPECT_EQ(atlas.art.height, 17);
    SpritesheetInfo info;
    info.image = "fx.png";
    info.clips = &clips;
    info.atlas = &atlas;
    info.scale = 2;
    const ordered_json json = ordered_json::parse(spritesheetJson(info));
    EXPECT_EQ(json["frames"]["fire-3"]["frame"], ordered_json({{"x", 10}, {"y", 10}, {"w", 8}, {"h", 8}}));
    EXPECT_EQ(json["frames"]["fire-0"]["duration"], 100);
    EXPECT_EQ(json["frames"]["fire-0"]["anchor"], ordered_json({{"x", 0.375}, {"y", 1}}));
    EXPECT_EQ(json["frames"]["smoke-1"]["frame"], ordered_json({{"x", 14}, {"y", 30}, {"w", 12}, {"h", 4}}));
    EXPECT_EQ(json["frames"]["smoke-0"]["duration"], 200);
    EXPECT_EQ(json["animations"]["smoke"], ordered_json({"smoke-0", "smoke-1"}));
    EXPECT_EQ(json["meta"]["size"], ordered_json({{"w", 26}, {"h", 34}}));
    EXPECT_EQ(json["meta"]["app"], "jasperengine");
    ASSERT_EQ(json["meta"]["frameTags"].size(), 2u);
    EXPECT_EQ(json["meta"]["frameTags"][0], ordered_json({{"name", "fire"}, {"from", 0}, {"to", 4}, {"direction", "pingpong"}}));
    EXPECT_EQ(json["meta"]["frameTags"][1], ordered_json({{"name", "smoke"}, {"from", 5}, {"to", 6}, {"direction", "forward"}}));
}
