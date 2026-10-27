// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The recipe of a generated sprite normal map (SpriteNormalsRecipe, Phase 2 spec 6): the
// canonical JSON, settings <-> JSON, validation of garbled recipes, the PNG chunk (keyword
// jasper.spritenormals) written and read back, next to a Phase 5 paint chunk and a texture
// synthesis recipe without disturbing either.

#include <gtest/gtest.h>

#include "EditorVersion.h"
#include "spritenormals/SpriteNormalsRecipe.h"
#include "texsynth/SynthesisRecipe.h"

#include "json.hpp"

#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sn = jasperengine::editor::spritenormals;
namespace ts = jasperengine::editor::texsynth;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

    sn::Image rgba(int w, int h, uint8_t seed) {
        sn::Image image = sn::Image::make(w, h, 4, 8);
        for (size_t i = 0; i < image.data.size(); i++) image.data[i] = static_cast<uint8_t>((i * 31 + seed * 7) % 256);
        return image;
    }

    sn::RecipeInput gifInput(sn::Result& result) {
        result.status = sn::Status::Ok;
        result.radiusPerIsland = false;
        result.radiusMin = result.radiusMax = 4.8f;
        sn::RecipeInput in;
        in.role = "normal";
        in.sourcePath = "assets/hero.gif";
        in.sourceSha1 = "0123456789abcdef0123456789abcdef01234567";
        in.sourceKind = sn::SourceKind::Gif;
        in.framesFrom = sn::FramesFrom::Gif;
        in.cells = {{0, 0, 32, 32}, {32, 0, 32, 32}};
        in.columns = 2;
        in.rows = 1;
        in.delays = {{80, 1}, {80, 1}};
        in.loopCount = 0;
        in.settings = sn::Settings();
        in.result = &result;
        in.normalOutput = "assets/hero_sheet_normal.png";
        in.atlasOutput = "assets/hero_sheet.png";
        return in;
    }

    // A small LCG (deterministic random settings)
    struct Lcg {
        uint32_t state;
        uint32_t next() {
            state = state * 1664525u + 1013904223u;
            return state >> 8;
        }
        float unit() { return static_cast<float>(next() % 1000001u) / 1000000.0f; }
        int below(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
    };

    sn::Settings randomSettings(Lcg& r) {
        sn::Settings s;
        const sn::Preset presets[] = {sn::Preset::Soft, sn::Preset::Default, sn::Preset::Crisp, sn::Preset::Detailed, sn::Preset::Chunky};
        const sn::Profile profiles[] = {sn::Profile::Rounded, sn::Profile::Dome, sn::Profile::Bevel};
        const sn::Edges edges[] = {sn::Edges::Transparent, sn::Edges::Wrap, sn::Edges::Extend};
        s.preset = presets[r.below(5)];
        s.strength = r.unit() * 8.0f;
        s.detail = r.unit() * 8.0f;
        s.smoothing = r.unit() * 8.0f;
        s.volume = r.unit() * 8.0f;
        if (r.below(2)) s.radius = 0.5f + r.unit() * 511.5f;
        s.softness = r.unit() * 8.0f;
        s.alphaThreshold = r.unit() * 0.99f;
        s.profile = profiles[r.below(3)];
        s.edges = edges[r.below(3)];
        s.ignoreOutline = r.below(2) != 0;
        s.directx = r.below(2) != 0;
        s.height = r.below(2) != 0;
        const int regions = r.below(4);
        for (int i = 0; i < regions; i++) {
            sn::Region region;
            const int colors = 1 + r.below(3);
            for (int c = 0; c < colors; c++) region.colors.push_back(r.next() & 0xFFFFFFu);
            region.tolerance = r.unit() * 0.5f;
            region.detail = r.unit() * 8.0f;
            region.lift = r.unit() * 32.0f - 16.0f;
            region.flat = r.below(2) != 0;
            s.regions.push_back(region);
        }
        return s;
    }

    void expectSameSettings(const sn::Settings& a, const sn::Settings& b) {
        EXPECT_EQ(a.preset, b.preset);
        EXPECT_NEAR(a.strength, b.strength, 1e-6);
        EXPECT_NEAR(a.detail, b.detail, 1e-6);
        EXPECT_NEAR(a.smoothing, b.smoothing, 1e-6);
        EXPECT_NEAR(a.volume, b.volume, 1e-6);
        ASSERT_EQ(a.radius.has_value(), b.radius.has_value());
        if (a.radius) {
            EXPECT_NEAR(*a.radius, *b.radius, 1e-4);
        }
        EXPECT_NEAR(a.softness, b.softness, 1e-6);
        EXPECT_NEAR(a.alphaThreshold, b.alphaThreshold, 1e-6);
        EXPECT_EQ(a.profile, b.profile);
        EXPECT_EQ(a.edges, b.edges);
        EXPECT_EQ(a.ignoreOutline, b.ignoreOutline);
        EXPECT_EQ(a.directx, b.directx);
        EXPECT_EQ(a.height, b.height);
        ASSERT_EQ(a.regions.size(), b.regions.size());
        for (size_t i = 0; i < a.regions.size(); i++) {
            EXPECT_EQ(a.regions[i].colors, b.regions[i].colors);
            EXPECT_NEAR(a.regions[i].tolerance, b.regions[i].tolerance, 1e-6);
            EXPECT_NEAR(a.regions[i].detail, b.regions[i].detail, 1e-6);
            EXPECT_NEAR(a.regions[i].lift, b.regions[i].lift, 1e-5);
            EXPECT_EQ(a.regions[i].flat, b.regions[i].flat);
        }
    }

    fs::path tempFile(const std::string& name) {
        return fs::temp_directory_path() / ("jasper-spritenormals-recipe-" + std::to_string(::getpid()) + "-" + name);
    }

    void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

}

// ---- canonical JSON

TEST(SpriteNormalsRecipe, MakeRecipeIsCanonical) {
    sn::Result result;
    const json recipe = sn::makeRecipe(gifInput(result));
    std::string text, error;
    ASSERT_TRUE(sn::recipeText(recipe, text, error)) << error;
    const std::string expected =
        std::string(R"({"format":"jasperengine.spritenormals","frames":{"cells":[[0,0,32,32],[32,0,32,32]],"columns":2,)"
                    R"("delays_ms":[[80,1],[80,1]],"from":"gif","loop":0,"rows":1},"generator":"JasperEngine )") +
        JASPERENGINE_EDITOR_VERSION +
        R"(","outputs":{"atlas":"assets/hero_sheet.png","height":null,"normal":"assets/hero_sheet_normal.png"},)"
        R"("radius_resolved":4.8,"role":"normal","settings":{"alpha_threshold":0.03,"detail":3,"directx":false,)"
        R"("edges":"transparent","height":false,"ignore_outline":false,"preset":"default","profile":"rounded",)"
        R"("radius":"auto","regions":[],"smoothing":1,"softness":1.15,"strength":1.5,"volume":1.15},)"
        R"("source":{"kind":"gif","path":"assets/hero.gif","sha1":"0123456789abcdef0123456789abcdef01234567"},"version":1})";
    EXPECT_EQ(text, expected);
    std::string invalid;
    EXPECT_TRUE(sn::validateRecipe(recipe, invalid)) << invalid;
}

TEST(SpriteNormalsRecipe, OptionalKeysFollowTheRules) {
    // a still from a Sprite with per-island auto radius, an outline and a height map
    sn::Result result;
    result.radiusPerIsland = true;
    result.radiusMin = 4.0f;
    result.radiusMax = 7.25f;
    result.outlineColor = 0x1a1c2cu;
    result.heightMin = 0.0f;
    result.heightMax = 7.25f;
    sn::RecipeInput in;
    in.role = "height";
    in.sourcePath = "assets/hero.png";
    in.sourceSha1 = "aa";
    in.fromSprite = true;
    in.spriteEntity = "Player/Hero";
    in.framesFrom = sn::FramesFrom::Sprite;
    in.cells = {{0, 0, 16, 16}, {16, 0, 16, 16}};
    in.settings.ignoreOutline = true;
    in.settings.height = true;
    in.settings.regions.push_back({{0xff0000u, 0x00ff00u}, 0.05f, 0.0f, 2.5f, true});
    in.result = &result;
    in.normalOutput = "assets/hero_normal.png";
    in.heightOutput = "assets/hero_height.png";
    const json r = sn::makeRecipe(in);
    EXPECT_EQ(r["source"]["kind"], "sprite");
    EXPECT_EQ(r["source"]["entity"], "Player/Hero");
    EXPECT_EQ(r["frames"]["from"], "sprite");
    EXPECT_FALSE(r["frames"].contains("columns")) << "columns/rows/delays/loop only for animations";
    EXPECT_FALSE(r["frames"].contains("delays_ms"));
    EXPECT_EQ(r["radius_resolved"], "per-island");
    EXPECT_EQ(r["radius_range"], json::array({4, 7.25}));
    EXPECT_EQ(r["outline"], "#1a1c2c");
    EXPECT_EQ(r["height_range_px"], json::array({0, 7.25}));
    EXPECT_TRUE(r["outputs"]["atlas"].is_null());
    EXPECT_EQ(r["outputs"]["height"], "assets/hero_height.png");
    EXPECT_EQ(r["settings"]["regions"], json::parse(R"([{"colors":["#ff0000","#00ff00"],"tolerance":0.05,"detail":0,"lift":2.5,"flat":true}])"));
    std::string invalid;
    EXPECT_TRUE(sn::validateRecipe(r, invalid)) << invalid;

    // a plain image: no entity, empty cells, no outline / height range; an explicit radius
    sn::Result plain;
    sn::RecipeInput still;
    still.role = "normal";
    still.sourcePath = "/abs/ball.png";
    still.settings.radius = 8.0f;
    still.result = &plain;
    const json s = sn::makeRecipe(still);
    EXPECT_FALSE(s["source"].contains("entity"));
    EXPECT_EQ(s["source"]["kind"], "image");
    EXPECT_EQ(s["frames"]["cells"], json::array());
    EXPECT_EQ(s["frames"]["from"], "single");
    EXPECT_EQ(s["radius_resolved"], 8);
    EXPECT_EQ(s["settings"]["radius"], 8);
    EXPECT_FALSE(s.contains("outline"));
    EXPECT_FALSE(s.contains("height_range_px"));
    EXPECT_FALSE(s.contains("radius_range"));
}

TEST(SpriteNormalsRecipe, NonUtf8PathsAreRefusedNotThrown) {
    sn::Result result;
    sn::RecipeInput in = gifInput(result);
    in.sourcePath = "assets/caf\xE9.gif";
    std::string text, error;
    bool ok = true;
    EXPECT_NO_THROW(ok = sn::recipeText(sn::makeRecipe(in), text, error));
    EXPECT_FALSE(ok);
    EXPECT_NE(error.find("source.path"), std::string::npos) << error;
    EXPECT_NE(error.find("not valid UTF-8"), std::string::npos) << error;
}

// ---- settings <-> JSON

TEST(SpriteNormalsRecipe, SettingsRoundTripThroughJson) {
    Lcg r{12345u};
    for (int i = 0; i < 200; i++) {
        const sn::Settings s = randomSettings(r);
        const json j = sn::settingsToJson(s);
        sn::Settings back;
        std::string error;
        ASSERT_TRUE(sn::settingsFromJson(j, back, error)) << error << " " << j.dump();
        expectSameSettings(s, back);
        EXPECT_EQ(sn::settingsToJson(back), j) << "canonical: a second trip changes nothing";
    }
    // every enum value by name
    const json names = json::parse(R"({"preset":"chunky","profile":"bevel","edges":"wrap","radius":"auto"})");
    sn::Settings s;
    s.radius = 3.0f;
    std::string error;
    ASSERT_TRUE(sn::settingsFromJson(names, s, error)) << error;
    EXPECT_EQ(s.preset, sn::Preset::Chunky);
    EXPECT_EQ(s.profile, sn::Profile::Bevel);
    EXPECT_EQ(s.edges, sn::Edges::Wrap);
    EXPECT_FALSE(s.radius.has_value());
}

TEST(SpriteNormalsRecipe, SettingsFromJsonKeepsMissingKeysAndNamesWrongOnes) {
    sn::Settings s = sn::presetSettings(sn::Preset::Chunky);
    std::string error;
    ASSERT_TRUE(sn::settingsFromJson(json::parse(R"({"strength":2,"unknown":true})"), s, error)) << error;
    EXPECT_FLOAT_EQ(s.strength, 2.0f);
    EXPECT_FLOAT_EQ(s.volume, 2.0f) << "missing keys keep the values";
    EXPECT_EQ(s.preset, sn::Preset::Chunky);
    // out of range is not settingsFromJson's business
    ASSERT_TRUE(sn::settingsFromJson(json::parse(R"({"strength":99})"), s, error));
    EXPECT_FLOAT_EQ(s.strength, 99.0f);

    const std::vector<std::pair<std::string, std::string>> wrong = {
        {R"({"strength":"high"})", "strength is a string"},
        {R"({"profile":"cone"})", "profile \"cone\""},
        {R"({"edges":3})", "edges is a number"},
        {R"({"radius":"big"})", "radius \"big\""},
        {R"({"directx":1})", "directx is a number"},
        {R"({"regions":{}})", "regions is an object"},
        {R"({"regions":[{"colors":["#12345"]}]})", "regions[0].colors[0]"},
        {R"({"regions":[{"colors":["#123456"],"flat":"yes"}]})", "regions[0].flat is a string"},
    };
    for (const auto& [text, needle] : wrong) {
        sn::Settings before = sn::presetSettings(sn::Preset::Soft);
        sn::Settings t = before;
        error.clear();
        EXPECT_FALSE(sn::settingsFromJson(json::parse(text), t, error)) << text;
        EXPECT_NE(error.find(needle), std::string::npos) << text << " -> " << error;
        EXPECT_EQ(sn::settingsToJson(t), sn::settingsToJson(before)) << "a failure leaves the settings as they were";
    }
}

// ---- validation

TEST(SpriteNormalsRecipe, ValidateRecipeNamesWhatIsGarbled) {
    sn::Result result;
    const json good = sn::makeRecipe(gifInput(result));
    const std::vector<std::pair<std::string, std::string>> edits = {
        {R"({"format":"jasperengine.synthesis"})", "not a sprite normals recipe"},
        {R"({"version":"1"})", "version is a string"},
        {R"({"source":"a.png"})", "source is a string"},
        {R"({"source":{"path":null}})", "source.path is missing"},   // merge_patch: null removes the key
        {R"({"source":{"path":7}})", "source.path is a number"},
        {R"({"frames":{"cells":[[0,0,32]]}})", "frames.cells[0]"},
        {R"({"frames":{"cells":[[0,0,32,1.5]]}})", "frames.cells[0][3]"},
        {R"({"frames":{"from":"video"}})", "frames.from \"video\""},
        {R"({"frames":{"delays_ms":[[80,0]]}})", "frames.delays_ms[0][1]"},
        {R"({"settings":{"strength":"x"}})", "settings.strength is a string"},
        {R"({"settings":{"strength":1e9}})", "settings.strength"},
        {R"({"radius_resolved":"huge"})", "radius_resolved"},
        {R"({"height_range_px":[1]})", "height_range_px"},
        {R"({"outputs":{"normal":3}})", "outputs.normal is a number"},
    };
    for (const auto& [patch, needle] : edits) {
        json bad = good;
        bad.merge_patch(json::parse(patch));
        std::string error;
        EXPECT_FALSE(sn::validateRecipe(bad, error)) << patch;
        EXPECT_NE(error.find(needle), std::string::npos) << patch << " -> " << error;
    }
    std::string error;
    EXPECT_FALSE(sn::validateRecipe(json::array(), error));
    EXPECT_FALSE(sn::validateRecipe(json::object(), error));
}

// ---- the PNG chunk

TEST(SpriteNormalsRecipe, EncodeThenReadBack) {
    sn::Result result;
    const json recipe = sn::makeRecipe(gifInput(result));
    std::vector<uint8_t> png;
    std::string error;
    ASSERT_TRUE(sn::encodeWithRecipe(rgba(8, 6, 1), recipe, png, error)) << error;
    // right after IHDR (8 signature + 25 IHDR bytes)
    ASSERT_GT(png.size(), 41u);
    EXPECT_EQ(std::string(png.begin() + 37, png.begin() + 41), "iTXt");
    const std::optional<json> fromBytes = sn::readRecipe(png.data(), png.size(), &error);
    ASSERT_TRUE(fromBytes) << error;
    EXPECT_EQ(*fromBytes, recipe);

    const fs::path file = tempFile("roundtrip.png");
    writeBytes(file, png);
    EXPECT_EQ(sn::readRecipe(file, &error).value_or(json()), recipe) << error;
    EXPECT_EQ(sn::readRecipeHead(file, &error).value_or(json()), recipe) << error;
    // the pixels are still a PNG stb decodes
    ts::Image decoded;
    ASSERT_TRUE(ts::loadImageFromMemory(png.data(), png.size(), decoded, error)) << error;
    EXPECT_EQ(decoded.width, 8);
    EXPECT_EQ(decoded.data, rgba(8, 6, 1).data);

    // no recipe: nullopt, error empty; garbled: nullopt with why
    std::vector<uint8_t> plain;
    ASSERT_TRUE(ts::encodePng(rgba(4, 4, 2), plain));
    error = "stale";
    EXPECT_FALSE(sn::readRecipe(plain.data(), plain.size(), &error));
    EXPECT_TRUE(error.empty()) << error;
    std::vector<uint8_t> notJson;
    ASSERT_TRUE(ts::insertRecipeChunk(plain, "not json", notJson, error, sn::kRecipeKeyword));
    EXPECT_FALSE(sn::readRecipe(notJson.data(), notJson.size(), &error));
    EXPECT_NE(error.find("not a JSON object"), std::string::npos) << error;
    std::vector<uint8_t> flipped = png;
    flipped[80] ^= 0x01;   // inside the recipe text (after the 20-byte keyword): CRC mismatch
    EXPECT_FALSE(sn::readRecipe(flipped.data(), flipped.size(), &error));
    EXPECT_NE(error.find("garbled jasper.spritenormals chunk"), std::string::npos) << error;
    fs::remove(file);
}

TEST(SpriteNormalsRecipe, BothKeywordsInOnePng) {
    std::vector<uint8_t> plain, one, two, again;
    ASSERT_TRUE(ts::encodePng(rgba(4, 4, 3), plain));
    std::string error;
    const std::string paint = "jasper.spritenormals.paint";
    ASSERT_TRUE(ts::insertRecipeChunk(plain, R"({"format":"jasperengine.spritenormals","v":"recipe"})", one, error, sn::kRecipeKeyword)) << error;
    ASSERT_TRUE(ts::insertRecipeChunk(one, R"({"paint":1})", two, error, paint.c_str())) << error;
    EXPECT_EQ(ts::readRecipeTextFromPng(two.data(), two.size(), &error, sn::kRecipeKeyword).value_or(""),
              R"({"format":"jasperengine.spritenormals","v":"recipe"})");
    EXPECT_EQ(ts::readRecipeTextFromPng(two.data(), two.size(), &error, paint.c_str()).value_or(""), R"({"paint":1})");
    // replacing one keyword's chunk keeps the other (only the same keyword is dropped)
    ASSERT_TRUE(ts::insertRecipeChunk(two, R"({"format":"jasperengine.spritenormals","v":"new"})", again, error, sn::kRecipeKeyword));
    EXPECT_EQ(ts::readRecipeTextFromPng(again.data(), again.size(), &error, sn::kRecipeKeyword).value_or(""),
              R"({"format":"jasperengine.spritenormals","v":"new"})");
    EXPECT_EQ(ts::readRecipeTextFromPng(again.data(), again.size(), &error, paint.c_str()).value_or(""), R"({"paint":1})");
    EXPECT_EQ(again.size(), two.size() - 6 + 3) << "exactly one recipe chunk ('recipe' became 'new')";
    // the head reader sees both too
    const fs::path file = tempFile("both.png");
    writeBytes(file, again);
    EXPECT_EQ(ts::readRecipeTextHead(file, &error, paint.c_str()).value_or(""), R"({"paint":1})");
    EXPECT_TRUE(sn::readRecipeHead(file, &error).has_value()) << error;
    // a keyword prefix is not the keyword
    EXPECT_FALSE(ts::readRecipeTextFromPng(again.data(), again.size(), &error, "jasper.sprite").has_value());
    fs::remove(file);
}

TEST(SpriteNormalsRecipe, ATextureSynthesisRecipeIsUntouched) {
    std::vector<uint8_t> plain, synth, both;
    ASSERT_TRUE(ts::encodePng(rgba(4, 4, 4), plain));
    std::string error;
    const std::string synthesis = R"({"format":1,"mode":"tileable"})";
    ASSERT_TRUE(ts::insertRecipeChunk(plain, synthesis, synth, error)) << error;   // default keyword: jasper.synthesis
    EXPECT_FALSE(sn::readRecipe(synth.data(), synth.size(), &error)) << "a synthesis recipe is not a sprite normals one";
    EXPECT_TRUE(error.empty()) << error;
    ASSERT_TRUE(ts::insertRecipeChunk(synth, R"({"format":"jasperengine.spritenormals"})", both, error, sn::kRecipeKeyword));
    EXPECT_EQ(ts::readRecipeTextFromPng(both.data(), both.size(), &error).value_or(""), synthesis);
    EXPECT_EQ(sn::readRecipe(both.data(), both.size(), &error).value_or(json())["format"], "jasperengine.spritenormals");
    // and the synthesis writer replacing its own recipe keeps the sprite normals one
    std::vector<uint8_t> resynth;
    ASSERT_TRUE(ts::insertRecipeChunk(both, R"({"format":1,"mode":"extend"})", resynth, error));
    EXPECT_EQ(ts::readRecipeTextFromPng(resynth.data(), resynth.size(), &error).value_or(""), R"({"format":1,"mode":"extend"})");
    EXPECT_TRUE(sn::readRecipe(resynth.data(), resynth.size(), &error).has_value());
}

TEST(SpriteNormalsRecipe, FramesFromNames) {
    for (sn::FramesFrom f : {sn::FramesFrom::Single, sn::FramesFrom::Sprite, sn::FramesFrom::Gif, sn::FramesFrom::Apng, sn::FramesFrom::Given}) {
        sn::FramesFrom back = sn::FramesFrom::Single;
        EXPECT_TRUE(sn::parseFramesFrom(sn::toString(f), back));
        EXPECT_EQ(back, f);
    }
    sn::FramesFrom out;
    EXPECT_FALSE(sn::parseFramesFrom("video", out));
}
