// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Texture sheets (docs/specs/S45-sprite-atlas.md AC11, R10, R11, §6.4): the meta form of a
// texture's Sprite Mode round-trips; a grid with offset, spacing and skipped empty cells and
// multiple frames with pivots and borders build their frames from the texture's pixels;
// "coin@16x16.png" is a grid without any meta write, a size that does not divide the image warns
// with the leftover pixels; a newer meta version is read-only; the MCP form validates; Aseprite's
// json-hash and json-array data fill the frames (Slice... > From JSON).

#include "SpriteAtlasTestSupport.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor::sprite;
using namespace jasperengine::editor::sprite::test;
using jasperengine::editor::pixelart::IntRect;
using jasperengine::editor::pixelart::PixelImage;

TEST(SpriteSheetBuilder, MetaFormRoundTrips) {
    TextureSheetSettings grid;
    grid.mode = TextureSpriteMode::GRID;
    grid.grid = {16, 16, 2, 1, 1, 1, 0, true, "coin_"};
    grid.pivotPreset = PivotPreset::BOTTOM_CENTER;
    TextureSheetSettings multiple;
    multiple.mode = TextureSpriteMode::MULTIPLE;
    TextureSheetFrame f;
    f.name = "idle_0";
    f.rect = Rect(0, 0, 16, 16);
    f.hasPivot = true;
    f.pivot = Vector2(8, 16);
    f.border = Vector4(1, 2, 3, 4);
    multiple.frames = {f};
    multiple.customPivot = true;
    multiple.pivotNormalized = Vector2(0.5f, 0.25f);
    for (const TextureSheetSettings& s : {grid, multiple}) {
        const YAML::Node node = sheetSettingsToYaml(s);
        EXPECT_EQ(node["version"].as<int>(), 1);
        TextureSheetSettings back;
        std::vector<editor::pixelart::PixelProblem> problems;
        ASSERT_TRUE(sheetSettingsFromYaml(YAML::Load(YAML::Dump(node)), back, problems));
        EXPECT_TRUE(problems.empty());
        EXPECT_TRUE(back == s) << YAML::Dump(node);
        // and the MCP form
        TextureSheetSettings fromJson;
        ASSERT_TRUE(sheetSettingsFromJson(sheetSettingsToJson(s), TextureSheetSettings(), fromJson, problems));
        EXPECT_TRUE(fromJson == s);
    }
    // a newer version: read-only
    YAML::Node newer = sheetSettingsToYaml(grid);
    newer["version"] = 2;
    TextureSheetSettings out;
    std::vector<editor::pixelart::PixelProblem> problems;
    EXPECT_FALSE(sheetSettingsFromYaml(newer, out, problems));
    ASSERT_FALSE(problems.empty());
    EXPECT_EQ(problems[0].field, "spriteSheet.version");
    // the MCP form names what is wrong
    problems.clear();
    EXPECT_FALSE(sheetSettingsFromJson(nlohmann::json{{"mode", "grid"}, {"grid", {{"cell", {0, 16}}}}}, TextureSheetSettings(), out, problems));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].field, "grid.cell");
}

TEST(SpriteSheetBuilder, GridAndMultipleFromTheTexturesPixels) {
    editor::test::ensureEngine();
    ScratchDir dir("texture-sheets");
    // 3 x 2 cells of 8 x 8 at (2, 1), spacing 1; cell (2, 1) transparent
    PixelImage image(2 + 3 * 8 + 2 + 3, 1 + 2 * 8 + 1);
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) {
            if (r == 1 && c == 2) continue;
            image.blit(memberImage(8, 8, IntRect{1, 1, 6, 6}, r * 3 + c), 2 + c * 9, 1 + r * 9);
        }
    }
    const std::filesystem::path file = dir.assets() / "hero.png";
    ASSERT_TRUE(image.savePng(file));
    TextureSheetSettings grid;
    grid.mode = TextureSpriteMode::GRID;
    grid.grid = {8, 8, 2, 1, 1, 1, 0, true, "hero_"};
    writeSpriteMode(file, grid);
    const TextureSheetInfo info = textureSheetSettings(file);
    EXPECT_EQ(info.source, TextureSheetInfo::Source::Meta);
    ASSERT_TRUE(info.isSheet());
    const TextureSheetBuild built = buildTextureSheet(file, "hero.png", info.settings);
    ASSERT_TRUE(built.ok);
    ASSERT_EQ(built.sheet.frames.size(), 5u);
    EXPECT_EQ(built.sheet.frames[4].name, "hero_4");
    Rect fifth = built.sheet.frames[4].rect;
    EXPECT_FALSE(fifth != Rect(11, 10, 8, 8));
    EXPECT_FALSE(built.warnings.empty()) << "3 pixels are left on the right";

    // multiple: explicit frames with pivots, the rest from the default pivot
    TextureSheetSettings multiple;
    multiple.mode = TextureSpriteMode::MULTIPLE;
    multiple.pivotPreset = PivotPreset::TOP_LEFT;
    TextureSheetFrame a;
    a.name = "a";
    a.rect = Rect(2, 1, 8, 8);
    a.hasPivot = true;
    a.pivot = Vector2(4, 8);
    TextureSheetFrame b;
    b.name = "b";
    b.rect = Rect(11, 1, 8, 8);
    multiple.frames = {a, b};
    writeSpriteMode(file, multiple);
    const TextureSheetBuild multi = buildTextureSheet(file, "hero.png", textureSheetSettings(file).settings);
    ASSERT_EQ(multi.sheet.frames.size(), 2u);
    EXPECT_EQ(multi.sheet.frames[0].pivot, Vector2(4, 8));
    EXPECT_EQ(multi.sheet.frames[1].pivot, Vector2(0, 0));
}

TEST(SpriteSheetBuilder, CellSizeFileNameNeedsNoMeta) {
    editor::test::ensureEngine();
    ScratchDir dir("cell-size");
    PixelImage image(16 * 3 + 5, 16);
    for (int c = 0; c < 3; c++) image.blit(memberImage(16, 16, IntRect{2, 2, 12, 12}, c), c * 16, 0);
    const std::filesystem::path file = dir.assets() / "coin@16x16.png";
    ASSERT_TRUE(image.savePng(file));
    const TextureSheetInfo info = textureSheetSettings(file);
    EXPECT_EQ(info.source, TextureSheetInfo::Source::FileName);
    EXPECT_EQ(info.cellWidth, 16);
    const TextureSheetBuild built = buildTextureSheet(file, "coin@16x16.png", info.settings);
    ASSERT_EQ(built.sheet.frames.size(), 3u);
    EXPECT_EQ(built.sheet.frames[0].name, "coin_0");
    EXPECT_EQ(built.sheet.frames[2].name, "coin_2");
    ASSERT_FALSE(built.warnings.empty());
    EXPECT_NE(built.warnings[0].find("leaves 5 pixel(s) on the right"), std::string::npos) << built.warnings[0];
    EXPECT_FALSE(std::filesystem::exists(editor::SetAssetMetaCmd::metaPathOf(file))) << "nothing is written to the meta";
}

TEST(SpriteSheetBuilder, FramesFromAsepriteJson) {
    const std::string hash = R"({"frames": {
        "hero 0.aseprite": {"frame": {"x": 0, "y": 0, "w": 16, "h": 24}, "duration": 100},
        "hero 1.aseprite": {"frame": {"x": 16, "y": 0, "w": 16, "h": 24}, "duration": 100},
        "hero 2.aseprite": {"frame": {"x": 32, "y": 0, "w": 15, "h": 24}, "duration": 100}},
      "meta": {"slices": [{"name": "nine", "keys": [{"frame": 1, "bounds": {"x": 0, "y": 0, "w": 16, "h": 24},
                                                    "center": {"x": 4, "y": 5, "w": 8, "h": 12}, "pivot": {"x": 8, "y": 24}}]}]}})";
    std::vector<TextureSheetFrame> frames;
    editor::pixelart::PixelProblem problem;
    ASSERT_TRUE(framesFromAsepriteJson(hash, frames, problem)) << problem.message;
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0].name, "hero 0");
    EXPECT_EQ(frames[2].name, "hero 2");
    Rect third = frames[2].rect;
    EXPECT_FALSE(third != Rect(32, 0, 15, 24));
    EXPECT_EQ(frames[1].border, Vector4(4, 5, 4, 7));
    EXPECT_TRUE(frames[1].hasPivot);
    EXPECT_EQ(frames[1].pivot, Vector2(8, 24));

    const std::string array = R"({"frames": [{"filename": "a", "frame": {"x": 0, "y": 0, "w": 8, "h": 8}},
                                             {"filename": "b", "frame": {"x": 8, "y": 0, "w": 8, "h": 8}}]})";
    ASSERT_TRUE(framesFromAsepriteJson(array, frames, problem));
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[1].name, "b");
    EXPECT_FALSE(framesFromAsepriteJson("{\"nope\": 1}", frames, problem));
    EXPECT_EQ(problem.kind, "invalid_settings");
}
