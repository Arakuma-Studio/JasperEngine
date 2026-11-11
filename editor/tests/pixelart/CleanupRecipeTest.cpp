// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S46 R28: the cleanup recipe's JSON. Canonical text (sorted keys, 6 decimals, no time), the round
// trip, partial recipes taking their defaults, every key error collected at once with its fix,
// unknown keys named with the valid ones, and a newer version refused (recipe_version). The sheet
// cases are ported from ritmo's test/animation.test.ts ("an animation recipe").

#include "pixelart/cleanup/CleanupRecipe.h"
#include "pixelart/cleanup/CleanupSheet.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::cleanup;
using jasperengine::pixelart::Rgba8;
using nlohmann::json;

namespace {

    Recipe decodeOk(const std::string& text) {
        Recipe recipe;
        std::vector<PixelProblem> problems;
        EXPECT_TRUE(decodeRecipeText(text, recipe, problems)) << (problems.empty() ? "" : problems[0].field + ": " + problems[0].message);
        return recipe;
    }

    std::vector<PixelProblem> decodeFails(const std::string& text) {
        Recipe recipe;
        std::vector<PixelProblem> problems;
        EXPECT_FALSE(decodeRecipeText(text, recipe, problems));
        return problems;
    }

    bool mentions(const std::vector<PixelProblem>& problems, const std::string& needle) {
        for (const PixelProblem& p : problems) {
            if (p.field.find(needle) != std::string::npos || p.message.find(needle) != std::string::npos || p.fix.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    Recipe busyRecipe() {
        Recipe recipe;
        recipe.grid.kind = GridKind::Cells;
        recipe.grid.spec = {7.62, 7.62, 3.25, -1.5};
        recipe.follow = Follow::Bend;
        recipe.outlines = Outlines::On;
        recipe.moved = {{"x:3", 41.5}, {"y:0", 2.25}};
        recipe.sample = {0.3, 100};
        recipe.paletteMode = PaletteMode::Fixed;
        recipe.palette = RecipePalette{"Mine", {Rgba8{26, 28, 44, 255}, Rgba8{255, 205, 117, 255}}, {1}, "0123456789abcdef0123456789abcdef", "palettes/mine.palette"};
        recipe.metric = jasperengine::pixelart::ColorMetric::Ciede2000;
        recipe.dither = Dither::Ordered;
        recipe.ditherStrength = 0.75;
        recipe.cleanup = {2, 0.6};
        recipe.cleanKeep = {"32x24", {5, 9}};
        recipe.targetWidth = 16;
        recipe.reduceMode = ReduceMode::Common;
        recipe.removeBackground = true;
        recipe.crop = true;
        recipe.background = {0.1, true, false, true};
        recipe.lostThreshold = 0.05;
        Edit paint;
        paint.x = 3;
        paint.y = 4;
        paint.tool = EditTool::Paint;
        paint.color = Rgba8{1, 2, 3, 255};
        Edit erase;
        erase.x = 1;
        erase.area = true;
        recipe.edits = {"32x24", {{paint, erase}}};
        recipe.alphaCutoff = 200;
        recipe.seed = 42;
        recipe.source = {"Assets/hero.png", "ab12", 2, "body"};
        recipe.resultSha256 = "cd34";
        recipe.format = OutputFormat::Aseprite;
        recipe.forced = true;
        recipe.engineVersion = "1.2.3";
        return recipe;
    }

}

TEST(CleanupRecipe, DefaultsRoundTripAndAreCanonical) {
    const Recipe defaults;
    const json encoded = encodeRecipe(defaults);
    EXPECT_EQ(encoded["format"], "jasperengine.pixel-cleanup-recipe");
    EXPECT_EQ(encoded["version"], 1);
    EXPECT_EQ(encoded["grid"], "auto");
    EXPECT_EQ(encoded["sheet"], "auto");
    // an unset palette is left out: absent means the project's Default Palette
    EXPECT_FALSE(encoded.contains("palette"));
    Recipe back;
    std::vector<PixelProblem> problems;
    ASSERT_TRUE(decodeRecipe(encoded, back, problems));
    EXPECT_EQ(back, defaults);
    // sorted keys and no time: the text is the same on every run
    const std::string text = recipeText(defaults);
    EXPECT_EQ(text, recipeText(back));
    EXPECT_LT(text.find("\"alpha_cutoff\""), text.find("\"background\""));
    EXPECT_LT(text.find("\"background\""), text.find("\"clean_keep\""));
    EXPECT_EQ(text.find("time"), std::string::npos);
}

TEST(CleanupRecipe, EveryKeyRoundTrips) {
    const Recipe recipe = busyRecipe();
    Recipe back;
    std::vector<PixelProblem> problems;
    ASSERT_TRUE(decodeRecipe(encodeRecipe(recipe), back, problems)) << problems[0].field << ": " << problems[0].message;
    EXPECT_EQ(back, recipe);
    EXPECT_EQ(recipeText(back), recipeText(recipe));
    for (const std::string& key : recipeKeys()) {
        EXPECT_TRUE(encodeRecipe(recipe).contains(key)) << key;
    }
}

TEST(CleanupRecipe, FloatsAreRoundedToSixDecimals) {
    Recipe recipe;
    recipe.grid.kind = GridKind::Cells;
    recipe.grid.spec = {7.123456789, 8, 0.1 + 0.2, 0};
    const json grid = encodeRecipe(recipe)["grid"];
    EXPECT_EQ(grid["cell_width"].dump(), "7.123457");
    EXPECT_EQ(grid["cell_height"].dump(), "8");
    EXPECT_EQ(grid["offset_x"].dump(), "0.3");
}

TEST(CleanupRecipe, PartialRecipesTakeDefaults) {
    const Recipe sweetie = decodeOk(R"({"palette": "sweetie-16"})");
    ASSERT_EQ(sweetie.paletteMode, PaletteMode::Fixed);
    ASSERT_TRUE(sweetie.palette);
    EXPECT_EQ(sweetie.palette->name, "Sweetie 16");
    ASSERT_EQ(sweetie.palette->colors.size(), 16u);
    EXPECT_EQ(sweetie.palette->colors[0], (Rgba8{0x1a, 0x1c, 0x2c, 255}));
    EXPECT_EQ(sweetie.grid.kind, GridKind::Auto);
    EXPECT_EQ(sweetie.sample, SampleOptions{});
    EXPECT_EQ(sweetie.alphaCutoff, 128);
    // ritmo's slug: "Sweetie 16" and "PICO-8 (16)" name the presets too
    EXPECT_EQ(decodeOk(R"({"palette": "Sweetie 16"})").palette->colors, sweetie.palette->colors);
    EXPECT_EQ(decodeOk(R"J({"palette": "PICO-8 (16)"})J").palette->name, "PICO-8");

    const Recipe hexes = decodeOk(R"({"palette": ["#000000", "#ffffff"], "grid": {"kind": "cells", "cell_width": 6}})");
    EXPECT_EQ(hexes.palette->name, "Custom");
    EXPECT_EQ(hexes.palette->colors.size(), 2u);
    EXPECT_EQ(hexes.grid.spec.cellHeight, 6);   // cell_height defaults to cell_width

    EXPECT_EQ(decodeOk(R"({"palette": "none"})").paletteMode, PaletteMode::None);
    const Recipe automatic = decodeOk(R"({"palette": {"auto": {"count": 8, "method": "distinct"}}})");
    EXPECT_EQ(automatic.paletteMode, PaletteMode::Auto);
    EXPECT_EQ(automatic.autoPalette->count, 8);
    EXPECT_EQ(automatic.autoPalette->method, ProposeMethod::Distinct);
    EXPECT_EQ(decodeOk(R"({"palette": "auto"})").autoPalette->count, 16);
    const Recipe asset = decodeOk(R"({"palette": "palettes/hero.palette"})");
    EXPECT_EQ(asset.palette->assetPath, "palettes/hero.palette");
    EXPECT_TRUE(asset.palette->colors.empty());
    EXPECT_EQ(decodeOk(R"({"palette": "0123456789abcdef0123456789ABCDEF"})").palette->assetGuid, "0123456789abcdef0123456789ABCDEF");
    // a size grid keeps the aspect ratio when height is left out
    const Recipe sized = decodeOk(R"({"grid": {"kind": "size", "width": 64}})");
    EXPECT_EQ(sized.grid.kind, GridKind::Size);
    EXPECT_FALSE(sized.grid.height);
}

TEST(CleanupRecipe, EveryErrorIsCollectedWithItsFix) {
    const std::vector<PixelProblem> problems = decodeFails(
        R"({"grid": {"kind": "cells", "cell_width": 500}, "follow": "wobbly", "sample": {"inset": 0.9}, "metric": 3,
            "cleanup": {"size": 40}, "palette": "nope", "seed": -1})");
    ASSERT_GE(problems.size(), 7u);
    for (const PixelProblem& p : problems) {
        EXPECT_EQ(p.kind, "bad_recipe");
        EXPECT_FALSE(p.field.empty());
        EXPECT_FALSE(p.fix.empty()) << p.field;
    }
    EXPECT_TRUE(mentions(problems, "cell_width is 1-128"));
    EXPECT_TRUE(mentions(problems, "follow is one of off, straight, bend"));
    EXPECT_TRUE(mentions(problems, "inset is 0-0.45"));
    EXPECT_TRUE(mentions(problems, "metric is one of oklab, ciede2000"));
    EXPECT_TRUE(mentions(problems, "size is a whole number 0-16"));
    EXPECT_TRUE(mentions(problems, "no preset palette named \"nope\""));
    EXPECT_TRUE(mentions(problems, "seed is a whole number 0-4294967295"));
}

TEST(CleanupRecipe, UnknownKeysAreNamedWithTheValidOnes) {
    std::vector<PixelProblem> problems = decodeFails(R"({"pallete": "sweetie-16"})");
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].field, "pallete");
    EXPECT_NE(problems[0].fix.find("palette"), std::string::npos);
    EXPECT_NE(problems[0].fix.find("alpha_cutoff"), std::string::npos);
    // nested objects too
    problems = decodeFails(R"({"background": {"tolerence": 0.1}})");
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].field, "background.tolerence");
    EXPECT_NE(problems[0].fix.find("tolerance, enclosed, specks, shadow"), std::string::npos);
}

TEST(CleanupRecipe, ANewerVersionIsRefused) {
    std::vector<PixelProblem> problems = decodeFails(R"({"version": 2, "palette": "sweetie-16"})");
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].kind, "recipe_version");
    problems = decodeFails(R"({"pipeline_version": 9})");
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].kind, "recipe_version");
    problems = decodeFails(R"({"format": "jasperengine.spritenormals"})");
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].kind, "bad_recipe");
    EXPECT_FALSE(decodeFails("not json").empty());
    EXPECT_FALSE(decodeFails("[1, 2]").empty());
}

// ritmo: "round-trips its frames, animations, anchor, and margin"
TEST(CleanupRecipe, ASheetRoundTrips) {
    Recipe recipe;
    recipe.removeBackground = true;
    recipe.sheetMode = SheetMode::Given;
    Sheet sheet;
    sheet.frames = {frameOf({1, 2, 30, 40}), SheetFrame{{40, 2, 30, 40}, false, {1, -2}}, frameOf({80, 2, 30, 40})};
    SheetAnimation walk{"walk", {1, 0}, true, 12, Loop::PingPong};
    SheetAnimation idle{"idle", {2}, false, 4, Loop::Forward};
    sheet.animations = {walk, idle};
    sheet.anchor = Anchor::Center;
    sheet.padding = 2;
    recipe.sheet = sheet;
    Recipe back;
    std::vector<PixelProblem> problems;
    ASSERT_TRUE(decodeRecipe(encodeRecipe(recipe), back, problems));
    EXPECT_EQ(back, recipe);
    EXPECT_EQ(decodeOk(R"({"sheet": "off"})").sheetMode, SheetMode::Off);
}

// ritmo: "reports bad frames, and animations that miss a frame or share one"
TEST(CleanupRecipe, BadSheetFramesAndAnimationsAreReported) {
    std::vector<PixelProblem> problems = decodeFails(R"({"sheet": {"frames": [{"box": {"x": 0, "y": 0, "width": 0, "height": 4}}], "fps": 0}})");
    EXPECT_TRUE(mentions(problems, "sheet.frames[0]"));
    EXPECT_TRUE(mentions(problems, "sheet.fps"));
    const std::string box = R"({"box": {"x": 0, "y": 0, "width": 4, "height": 4}})";
    problems = decodeFails(R"({"sheet": {"frames": [)" + box + "," + box + "," + box +
                           R"(], "animations": [{"name": "a", "frames": [0, 1]}, {"name": "b", "frames": [1]}]}})");
    EXPECT_TRUE(mentions(problems, "frame 1 is in more than one animation"));
    EXPECT_TRUE(mentions(problems, "every frame must be in an animation (1 are not)"));
    problems = decodeFails(R"({"sheet": {"frames": [)" + box + R"(], "animations": [{"name": "walk left", "frames": [0]}]}})");
    EXPECT_TRUE(mentions(problems, "needs a name"));
}

// ritmo: "reads a recipe from before animations as one animation of every frame, at its speed"
TEST(CleanupRecipe, ASheetWithoutAnimationsIsOneAnimation) {
    const std::string box = R"({"box": {"x": 0, "y": 0, "width": 4, "height": 4}})";
    const Recipe recipe = decodeOk(R"({"sheet": {"frames": [)" + box + "," + box + R"(], "fps": 12, "loop": "pingpong"}})");
    ASSERT_TRUE(recipe.sheet);
    ASSERT_EQ(recipe.sheet->animations.size(), 1u);
    EXPECT_EQ(recipe.sheet->animations[0], (SheetAnimation{"animation-1", {0, 1}, true, 12, Loop::PingPong}));
}

// moved positions are source pixels (0-4096, R1's limit) and line indices fit a grid of that size:
// anything else is refused by name, never cast to an index or a pixel
TEST(CleanupRecipe, MovedLinesStayInTheSourceRange) {
    for (const nlohmann::json& bad : {nlohmann::json::array({"x:3", 1e300}), nlohmann::json::array({"x:3", -5.0}),
                                      nlohmann::json::array({"y:2", 4097.0}), nlohmann::json::array({"x:99999999999999999999", 10.0})}) {
        Recipe recipe;
        std::vector<PixelProblem> problems;
        EXPECT_FALSE(decodeRecipe(nlohmann::json{{"moved", nlohmann::json::array({bad})}}, recipe, problems)) << bad.dump();
        ASSERT_FALSE(problems.empty()) << bad.dump();
        EXPECT_EQ(problems.front().field, "moved[0]") << bad.dump();
    }
    Recipe recipe;
    std::vector<PixelProblem> problems;
    const nlohmann::json good = nlohmann::json::array({nlohmann::json::array({"x:3", 41.5}), nlohmann::json::array({"y:0", 0.0}), nlohmann::json::array({"x:1", 4096.0})});
    EXPECT_TRUE(decodeRecipe(nlohmann::json{{"moved", good}}, recipe, problems)) << (problems.empty() ? "" : problems.front().message);
    EXPECT_EQ(recipe.moved.size(), 3u);
}
