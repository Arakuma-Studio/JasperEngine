// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The jasperengine.spritesheet record, ImportedAssets and the sheet pool
// (docs/specs/S44-aseprite-import.md AC20)

#include <gtest/gtest.h>

#include <map>

#include "TestSystem.h"
#include "AllocationCounter.h"
#include "asset/ImportedAssets.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "support/TestTempDir.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>

using namespace jasperengine;

namespace {

    SpriteSheetData sampleSheet(int frames = 3){
        SpriteSheetData s;
        s.source = "sprites/hero.aseprite";
        s.sourceGuid = Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13");
        s.pixelsPerUnit = 16;
        s.pages.push_back({"je-imported/3f/3fa1/page0.png", 128, 64});
        for (int i = 0; i < frames; i++){
            SpriteFrame f;
            f.name = "hero " + std::to_string(i);
            f.key = "frame#" + std::to_string(i);
            f.rect = Rect(1.0f + i * 16, 1.0f, 14.0f, 22.0f);
            f.sourceSize = Vector2(16, 24);
            f.offset = Vector2(1, 2);
            f.pivot = Vector2(8, 24);
            f.durationMs = 100 + i;
            s.frames.push_back(f);
        }
        SpriteTag walk;
        walk.name = "walk";
        walk.from = 0;
        walk.to = frames - 1;
        walk.direction = SpriteTagDirection::PING_PONG;
        walk.repeat = 2;
        walk.color = Vector4(1, 0, 0, 1);
        walk.userData = {{"properties", {{"loop", false}}}};
        s.tags.push_back(walk);
        SpriteSlice hurt;
        hurt.name = "hurt";
        SpriteSliceKey k0;
        k0.frame = 0;
        k0.bounds = Rect(2, 3, 8, 9);
        k0.hasPivot = true;
        k0.pivot = Vector2(4, 9);
        SpriteSliceKey k1;
        k1.frame = 2;
        k1.bounds = Rect(0, 0, 0, 0);
        hurt.keys = {k0, k1};
        s.slices.push_back(hurt);
        s.userData = {{"properties", {{"light2d.intensity", 2.5}}}};
        return s;
    }

    void writeText(const std::filesystem::path& path, const std::string& text){
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

}

TEST(SpriteSheetIOTest, CanonicalRoundTrip){
    const std::string text = writeSheet(sampleSheet());
    SpriteSheetParseResult parsed = parseSheet(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_EQ(writeSheet(*parsed.sheet), text);
    const SpriteSheetData& s = *parsed.sheet;
    EXPECT_EQ(s.findFrame("hero 2"), 2);
    EXPECT_EQ(s.findFrame("nope"), -1);
    EXPECT_EQ(s.frames[1].durationMs, 101u);
    EXPECT_EQ(s.frames[0].offset.y, 2.0f);
    EXPECT_EQ(s.tags[0].direction, SpriteTagDirection::PING_PONG);
    EXPECT_EQ(s.tags[0].userData["properties"]["loop"], false);
    ASSERT_EQ(s.slices[0].keys.size(), 2u);
    EXPECT_TRUE(s.slices[0].keys[0].hasPivot);
    // keys hold until the next one; a zero-size key hides the slice
    EXPECT_EQ(s.slices[0].keyAt(1), &s.slices[0].keys[0]);
    EXPECT_EQ(s.slices[0].keyAt(2), nullptr);
    EXPECT_EQ(s.userData["properties"]["light2d.intensity"], 2.5);
    // the record of §6.11: keys in that order, integers only
    EXPECT_LT(text.find("\"format\""), text.find("\"version\""));
    EXPECT_LT(text.find("\"pages\""), text.find("\"frames\""));
    EXPECT_NE(text.find("\"rect\": [1, 1, 14, 22]"), std::string::npos) << text;
    // no decimals in the geometry (the format name, page file names and user data have dots)
    const size_t frames = text.find("\"frames\""), tags = text.find("\"tags\"");
    ASSERT_LT(frames, tags);
    EXPECT_EQ(text.substr(frames, tags - frames).find('.'), std::string::npos) << text.substr(frames, tags - frames);
}

TEST(SpriteSheetIOTest, LenientAfterFormatAndVersion){
    nlohmann::json j = nlohmann::json::parse(writeSheet(sampleSheet()));
    j["frames"][1]["rect"] = "bad";
    j["frames"][2]["page"] = 7;
    j["frames"].push_back({{"name", "hero 0"}, {"rect", {0, 0, 1, 1}}});
    j["tags"].push_back({{"name", "far"}, {"from", 0}, {"to", 99}});
    j["unknown"] = true;
    SpriteSheetParseResult parsed = parseSheet(j.dump());
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.sheet->frames.size(), 2u);
    EXPECT_EQ(parsed.sheet->frames[1].name, "hero 0 2");   // a duplicate name made unique
    EXPECT_EQ(parsed.sheet->tags.size(), 0u);                // "walk" now spans dropped frames, "far" is out of range
    bool rectWarning = false, pageWarning = false;
    for (const std::string& w : parsed.warnings){
        rectWarning = rectWarning || w.rfind("frames[1].rect", 0) == 0;
        pageWarning = pageWarning || w.rfind("frames[2].page", 0) == 0;
    }
    EXPECT_TRUE(rectWarning);
    EXPECT_TRUE(pageWarning);
}

TEST(SpriteSheetIOTest, RefusesOtherFormatsAndNewerVersions){
    EXPECT_FALSE(parseSheet("not json").ok());
    EXPECT_FALSE(parseSheet("[]").ok());
    EXPECT_FALSE(parseSheet("{\"format\": \"jasperengine.anim\", \"version\": 1}").ok());
    const SpriteSheetParseResult newer = parseSheet("{\"format\": \"jasperengine.spritesheet\", \"version\": 2}");
    EXPECT_FALSE(newer.ok());
    EXPECT_NE(newer.error.find("newer"), std::string::npos);
}

TEST(SpriteSheetIOTest, TagPlayback){
    SpriteTag tag;
    tag.from = 2;
    tag.to = 5;
    auto seq = [&](SpriteTagDirection d, int repeat){
        tag.direction = d;
        tag.repeat = repeat;
        return tag.playback();
    };
    // §6.6: forward, reverse, ping-pong, ping-pong reverse; repeat 0 one pass, N passes
    EXPECT_EQ(seq(SpriteTagDirection::FORWARD, 0), (std::vector<int>{2, 3, 4, 5}));
    EXPECT_EQ(seq(SpriteTagDirection::REVERSE, 0), (std::vector<int>{5, 4, 3, 2}));
    EXPECT_EQ(seq(SpriteTagDirection::PING_PONG, 0), (std::vector<int>{2, 3, 4, 5, 4, 3}));
    EXPECT_EQ(seq(SpriteTagDirection::PING_PONG_REVERSE, 0), (std::vector<int>{5, 4, 3, 2, 3, 4}));
    EXPECT_EQ(seq(SpriteTagDirection::FORWARD, 3), (std::vector<int>{2, 3, 4, 5, 2, 3, 4, 5, 2, 3, 4, 5}));
    // ping-pong repeat counts directions: 1 = one way only, 2 = there and back (ending at the start)
    EXPECT_EQ(seq(SpriteTagDirection::PING_PONG, 1), (std::vector<int>{2, 3, 4, 5}));
    EXPECT_EQ(seq(SpriteTagDirection::PING_PONG, 2), (std::vector<int>{2, 3, 4, 5, 4, 3, 2}));
    EXPECT_EQ(seq(SpriteTagDirection::PING_PONG, 3), (std::vector<int>{2, 3, 4, 5, 4, 3, 2, 3, 4, 5}));
    EXPECT_EQ(seq(SpriteTagDirection::PING_PONG_REVERSE, 2), (std::vector<int>{5, 4, 3, 2, 3, 4, 5}));
    tag.to = 2;
    EXPECT_EQ(seq(SpriteTagDirection::PING_PONG, 3), (std::vector<int>{2}));
}

// AC20: 2,000 frames parse in 5 ms on a desktop (enforced with the benchmark server's
// perfSlack(); printed elsewhere: a shared build machine's load alone misses it)
TEST(SpriteSheetIOTest, SpriteSheetBench){
    const std::string text = writeSheet(sampleSheet(2000));
    const auto start = std::chrono::steady_clock::now();
    SpriteSheetParseResult parsed = parseSheet(text);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.sheet->frames.size(), 2000u);
    std::printf("[bench] SpriteSheetBench: 2000-frame parse %.2f ms (budget 5 ms desktop)\n", ms);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LT(ms, 5.0 * testsupport::perfSlack());
    }
}

// AC20: the editor's provider and an export's index.json resolve the same keys
TEST(ImportedAssetsTest, ProviderAndExportIndexAgree){
    test::TestTempDir dir("imported");
    const std::filesystem::path assets = dir.path() / "assets";
    writeText(assets / "je-imported" / "ab" / "abcd" / "sheet.json", writeSheet(sampleSheet()));
    writeText(assets / "je-imported" / "index.json",
              "{\"format\": \"jasperengine.imported\", \"version\": 1, \"files\": {"
              "\"sprites/hero.aseprite#sheet\": \"je-imported/ab/abcd/sheet.json\","
              "\"sprites/hero.aseprite#image#0\": \"je-imported/ab/abcd/page0.png\","
              "\"sprites/hero.aseprite#animation:walk\": \"je-imported/ab/abcd/clip-0.anim\"}}");
    test::TestSystem::get().setAssetPath(assets.string());
    ImportedAssets::setProvider(nullptr);
    ImportedAssets::reset();
    const char* keys[] = {"sprites/hero.aseprite#sheet", "sprites/hero.aseprite#image#0", "sprites/hero.aseprite#animation:walk"};
    std::vector<std::string> fromIndex;
    for (const char* key : keys){
        std::string file;
        EXPECT_TRUE(ImportedAssets::find(key, file)) << key;
        fromIndex.push_back(file);
    }
    std::string file;
    EXPECT_TRUE(ImportedAssets::find("asset://sprites/../sprites/hero.aseprite#sheet", file));
    EXPECT_FALSE(ImportedAssets::find("sprites/other.aseprite#sheet", file));
    EXPECT_FALSE(ImportedAssets::find("sprites/hero.aseprite", file));

    // the editor's provider answers the same references
    ImportedAssets::setProvider([](const std::string& ref, std::string& out){
        if (ref.rfind("sprites/hero.aseprite#", 0) != 0) return false;
        // the cache's file names, as AsepriteImporter answers them
        static const std::map<std::string, std::string> names = {{"sheet", "sheet.json"}, {"image#0", "page0.png"}, {"animation:walk", "clip-0.anim"}};
        auto it = names.find(ref.substr(ref.find('#') + 1));
        if (it == names.end()) return false;
        out = "je-imported/ab/abcd/" + it->second;
        return true;
    });
    for (size_t i = 0; i < std::size(keys); i++){
        std::string viaProvider;
        EXPECT_TRUE(ImportedAssets::find(keys[i], viaProvider)) << keys[i];
        EXPECT_EQ(viaProvider, fromIndex[i]) << keys[i];
    }
    // (the editor side, that both come from one table of the import: AsepriteImportTest.ProviderAndExportAgree)

    // the pool loads the imported sheet, its pages through the same index
    ImportedAssets::setProvider(nullptr);
    SpriteSheetPool::clear();
    std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get("sprites/hero.aseprite");
    ASSERT_TRUE(sheet);
    EXPECT_EQ(sheet->frames.size(), 3u);
    EXPECT_EQ(sheet->pages[0].texture, fromIndex[1]);
    const uint64_t generation = SpriteSheetPool::generation("sprites/hero.aseprite");
    EXPECT_GT(generation, 0u);
    EXPECT_EQ(SpriteSheetPool::get("sprites/hero.aseprite"), sheet);   // shared
    SpriteSheetPool::invalidate("sprites/hero.aseprite");
    EXPECT_NE(SpriteSheetPool::generation("sprites/hero.aseprite"), generation);
    EXPECT_FALSE(SpriteSheetPool::find("sprites/hero.aseprite"));
    EXPECT_TRUE(SpriteSheetPool::get("sprites/hero.aseprite"));
    // a missing source is a null sheet, remembered until invalidated
    EXPECT_FALSE(SpriteSheetPool::get("sprites/missing.aseprite"));

    SpriteSheetPool::clear();
    ImportedAssets::reset();
    test::TestSystem::get().setAssetPath("");
}

TEST(SpriteSheetPoolTest, RegisteredSourcesComeFirst){
    SpriteSheetPool::registerSource([](const std::string& path){ return path.rfind("test-sheet:", 0) == 0; },
                                    [](const std::string& path){
                                        auto s = std::make_shared<SpriteSheetData>();
                                        s->source = path;
                                        SpriteFrame f;
                                        f.name = "only";
                                        s->frames.push_back(f);
                                        return std::shared_ptr<const SpriteSheetData>(s);
                                    });
    std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get("test-sheet:a");
    ASSERT_TRUE(sheet);
    EXPECT_EQ(sheet->frames[0].name, "only");
    SpriteSheetPool::clear();
}
