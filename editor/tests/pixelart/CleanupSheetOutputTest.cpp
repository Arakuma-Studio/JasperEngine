// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S46 AC9 (R23): a cleaned sheet written as `.aseprite` reads back through S44's reader with the
// frames pixel for pixel, the tags and the durations; as PNG plus JSON, the JSON's frame rects
// index the picture exactly and its frameTags match.

#include "pixelart/aseprite/AsepriteDocument.h"
#include "pixelart/cleanup/CleanupAnimation.h"
#include "pixelart/cleanup/CleanupRecipe.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSheet.h"
#include "pixelart/cleanup/CleanupSheetOutput.h"
#include "pixelart/cleanup/CleanupSynthetic.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::cleanup;
using jasperengine::pixelart::Rgba8;
using nlohmann::ordered_json;

namespace {

    std::vector<AtlasClip> cleanedSheetClips() {
        const std::vector<Rgba8> sweetie = sweetie16();
        const std::vector<Rgba8> fire = {sweetie[2], sweetie[3], sweetie[4], sweetie[1]};
        const PixelArt sprite = makeSprite(10, 12, fire, 3, true);
        std::vector<PixelArt> frames;
        for (int f = 0; f < 5; f++) frames.push_back(place(sprite, 12, 16, 1, 2 + (f % 2)));
        const FakeSheet fake = fakeSheet(frames, 6);
        Recipe recipe;
        recipe.removeBackground = true;
        recipe.paletteMode = PaletteMode::Fixed;
        recipe.palette = RecipePalette{"Fire", fire, {}, "", ""};
        AnimationCache cache;
        Sheet sheet = withFrames(Sheet{}, detectFrames(fake.image, recipe, cache));
        // two animations: the frame splits a walk (fps) from a hit (ping-pong)
        sheet = splitAnimation(sheet, 0, 3);
        sheet = renameAnimation(sheet, 0, "walk");
        sheet = renameAnimation(sheet, 1, "hit");
        sheet.animations[0].fps = 10;
        sheet.animations[1].fps = 4;
        sheet.animations[1].loop = Loop::PingPong;
        return atlasClips(runSheet(fake.image, recipe, sheet, cache));
    }

}

TEST(CleanupSheetOutput, AsepriteReadsBackFrameByFrameWithTagsAndDurations) {
    const std::vector<AtlasClip> clips = cleanedSheetClips();
    ASSERT_EQ(clips.size(), 2u);
    ASSERT_EQ(clips[0].frames.size(), 3u);
    const aseprite::Document doc = asepriteDocument(clips);
    EXPECT_EQ(doc.mode, aseprite::ColorMode::Indexed);
    std::vector<uint8_t> bytes;
    aseprite::CodecError error;
    ASSERT_TRUE(aseprite::write(doc, bytes, &error)) << error.message;
    const aseprite::ReadResult back = aseprite::read(bytes);
    ASSERT_TRUE(back.ok()) << back.error.message;
    const aseprite::Document& read = *back.document;
    ASSERT_EQ(read.frameCount(), 5);
    ASSERT_EQ(read.layers.size(), 1u);
    EXPECT_EQ(read.layers[0].name, "Cleanup");
    EXPECT_EQ(read.transparentIndex, 0);
    EXPECT_EQ(read.frameDurations, (std::vector<uint16_t>{100, 100, 100, 250, 250}));
    ASSERT_EQ(read.tags.size(), 2u);
    EXPECT_EQ(read.tags[0].name, "walk");
    EXPECT_EQ(read.tags[0].from, 0);
    EXPECT_EQ(read.tags[0].to, 2);
    EXPECT_EQ(read.tags[0].direction, aseprite::TagDirection::Forward);
    EXPECT_EQ(read.tags[1].name, "hit");
    EXPECT_EQ(read.tags[1].from, 3);
    EXPECT_EQ(read.tags[1].to, 4);
    EXPECT_EQ(read.tags[1].direction, aseprite::TagDirection::PingPong);
    int f = 0;
    for (const AtlasClip& clip : clips) {
        for (const PixelArt& art : clip.frames) {
            PixelImage expected(read.width, read.height);
            expected.blit(toRgba(art), 0, 0);
            PixelImage rendered = aseprite::renderFrame(read, f);
            rendered.dropIndices();
            EXPECT_EQ(rendered, expected) << "frame " << f;
            f++;
        }
    }
}

// More than 255 colours cannot be indexed with index 0 transparent: the file is RGBA (R23)
TEST(CleanupSheetOutput, ManyColoursWriteAnRgbaFile) {
    PixelArt art;
    art.width = 20;
    art.height = 15;
    for (int i = 0; i < 300; i++) {
        art.palette.push_back(Rgba8{uint8_t(i % 256), uint8_t(i / 2), uint8_t(255 - i % 256), 255});
        art.indices.push_back(i);
    }
    art.indices[7] = TRANSPARENT;
    AtlasClip clip;
    clip.name = "all";
    clip.frames = {art};
    const aseprite::Document doc = asepriteDocument({clip});
    EXPECT_EQ(doc.mode, aseprite::ColorMode::Rgba);
    std::vector<uint8_t> bytes;
    aseprite::CodecError error;
    ASSERT_TRUE(aseprite::write(doc, bytes, &error)) << error.message;
    const aseprite::ReadResult back = aseprite::read(bytes);
    ASSERT_TRUE(back.ok()) << back.error.message;
    EXPECT_EQ(aseprite::renderFrame(*back.document, 0), toRgba(art));
}

// An untagged single picture (R23's "aseprite is allowed for one frame too")
TEST(CleanupSheetOutput, ASinglePictureWritesOneFrameWithoutATag) {
    AtlasClip clip;
    clip.frames = {demoSprite()};
    const aseprite::Document doc = asepriteDocument({clip});
    EXPECT_EQ(doc.frameCount(), 1);
    EXPECT_TRUE(doc.tags.empty());
    std::vector<uint8_t> bytes;
    aseprite::CodecError error;
    ASSERT_TRUE(aseprite::write(doc, bytes, &error)) << error.message;
    const aseprite::ReadResult back = aseprite::read(bytes);
    ASSERT_TRUE(back.ok()) << back.error.message;
    PixelImage rendered = aseprite::renderFrame(*back.document, 0);
    rendered.dropIndices();
    EXPECT_EQ(rendered, toRgba(demoSprite()));
}

TEST(CleanupSheetOutput, PngAndJsonRectsIndexThePictureExactly) {
    const std::vector<AtlasClip> clips = cleanedSheetClips();
    const Atlas atlas = pngSheet(clips);
    const RgbaImage sheet = toRgba(atlas.art);
    Recipe recipe;
    recipe.seed = 9;
    SpritesheetInfo info;
    info.image = "hero_clean.png";
    info.clips = &clips;
    info.atlas = &atlas;
    info.recipe = encodeRecipe(recipe);
    const ordered_json json = ordered_json::parse(spritesheetJson(info));
    EXPECT_EQ(json["meta"]["app"], "jasperengine");
    EXPECT_EQ(json["meta"]["image"], "hero_clean.png");
    EXPECT_EQ(json["meta"]["size"]["w"], sheet.width());
    EXPECT_EQ(json["meta"]["size"]["h"], sheet.height());
    EXPECT_EQ(json["meta"]["jasperengine"]["recipe"]["seed"], 9);
    // the frames are in play order, whatever their numbers sort as
    ASSERT_EQ(json["frames"].size(), 5u);
    EXPECT_EQ(json["frames"].begin().key(), "walk-0");
    int index = 0;
    for (size_t c = 0; c < clips.size(); c++) {
        for (size_t f = 0; f < clips[c].frames.size(); f++) {
            const std::string key = clips[c].name + "-" + std::to_string(f);
            ASSERT_TRUE(json["frames"].contains(key)) << key;
            const ordered_json& rect = json["frames"][key]["frame"];
            const RgbaImage cut = sheet.crop({rect["x"].get<int>(), rect["y"].get<int>(), rect["w"].get<int>(), rect["h"].get<int>()});
            EXPECT_EQ(cut, toRgba(clips[c].frames[f])) << key;
            EXPECT_EQ(json["frames"][key]["duration"], clips[c].durationsMs[f]);
            index++;
        }
    }
    EXPECT_EQ(index, 5);
    // each animation on its own row, frames left to right, 1 px apart
    EXPECT_EQ(json["frames"]["walk-1"]["frame"]["x"], clips[0].frames[0].width + 1);
    EXPECT_EQ(json["frames"]["walk-1"]["frame"]["y"], 0);
    EXPECT_EQ(json["frames"]["hit-0"]["frame"]["x"], 0);
    EXPECT_EQ(json["frames"]["hit-0"]["frame"]["y"], clips[0].frames[0].height + 1);
    const ordered_json tags = json["meta"]["frameTags"];
    ASSERT_EQ(tags.size(), 2u);
    EXPECT_EQ(tags[0], ordered_json({{"name", "walk"}, {"from", 0}, {"to", 2}, {"direction", "forward"}}));
    EXPECT_EQ(tags[1], ordered_json({{"name", "hit"}, {"from", 3}, {"to", 4}, {"direction", "pingpong"}}));
}
