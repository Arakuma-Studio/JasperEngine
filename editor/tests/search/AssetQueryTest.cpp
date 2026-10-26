// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Project search grammar (S25 R20, R27, AC15) and its 50k-asset budget (R35, AC16), over a
// fake index: evaluate() reads only the view, never the file system.

#include "search/AssetQuery.h"

#include "search/FakeAssetIndex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace jasperengine::editor;
using jasperengine::editor::test::FakeAssetIndex;

namespace {

    FakeAssetIndex sampleIndex() {
        FakeAssetIndex index;
        index.add("textures", "folder");
        index.add("textures/hero.png", "texture", {"Hero"});
        index.add("textures/boss_room.png", "texture", {"Boss"});
        index.add("textures/tile_12.png", "texture");
        index.add("textures/tile_x.png", "texture");
        index.add("models/hero.gltf", "model", {"Hero"});
        index.add("models/boss.gltf", "model");
        index.add("audio/hero_theme.ogg", "audio", {"hero", "Music"});
        index.add("scenes/Main.scene", "scene");
        index.add("prefabs/Enemy.prefab", "prefab");
        index.add("materials/Hero.material", "material", {"Hero"});
        return index;
    }

    std::vector<std::string> run(const FakeAssetIndex& index, const std::string& query, const std::string& folder = {}) {
        std::vector<std::string> paths = index.paths(AssetQuery::parse(query).evaluate(index, folder));
        std::sort(paths.begin(), paths.end());
        return paths;
    }

    using Paths = std::vector<std::string>;

}

TEST(AssetQuery, Grammar) {
    const FakeAssetIndex index = sampleIndex();
    struct Case {
        const char* query;
        Paths expected;
    };
    const Case cases[] = {
        // the same key ORs, different keys AND
        {"t:texture t:model", {"models/boss.gltf", "models/hero.gltf", "textures/boss_room.png", "textures/hero.png", "textures/tile_12.png", "textures/tile_x.png"}},
        {"t:texture l:Hero", {"textures/hero.png"}},
        {"t:texture l:hero l:boss", {"textures/boss_room.png", "textures/hero.png"}},
        // labels without case; A OR B
        {"l:HERO", {"audio/hero_theme.ogg", "materials/Hero.material", "models/hero.gltf", "textures/hero.png"}},
        {"t:scene OR t:prefab", {"prefabs/Enemy.prefab", "scenes/Main.scene"}},
        {"l:music OR boss t:model", {"audio/hero_theme.ogg", "models/boss.gltf"}},
        // quoted phrase: an exact substring of the name; bare words in Filter mode
        {"\"boss_room\"", {"textures/boss_room.png"}},
        {"\"boss room\"", {}},
        {"boss room", {"textures/boss_room.png"}},
        {"hreo t:model", {"models/hero.gltf"}},
        // ext:, path:, re:
        {"ext:gltf", {"models/boss.gltf", "models/hero.gltf"}},
        {"ext:.PNG path:textures/t", {"textures/tile_12.png", "textures/tile_x.png"}},
        {"re:^tile_\\d+\\.png$", {"textures/tile_12.png"}},
        // Unity aliases
        {"t:Texture2D", {"textures/boss_room.png", "textures/hero.png", "textures/tile_12.png", "textures/tile_x.png"}},
        {"t:AudioClip", {"audio/hero_theme.ogg"}},
        {"t:SceneAsset", {"scenes/Main.scene"}},
        {"t:GameObject", {"prefabs/Enemy.prefab"}},
        {"t:Folder", {"textures"}},
        {"t:Material", {"materials/Hero.material"}},
        // an unknown type matches nothing; an unknown key is bare text
        {"t:nothing", {}},
        {"main:", {"scenes/Main.scene"}},
        {"enemy", {"prefabs/Enemy.prefab"}},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(run(index, c.query), c.expected) << c.query;
    }

    // a folder scope: what is below it, not the folder itself
    EXPECT_EQ(run(index, "", "textures").size(), 4u);
    EXPECT_EQ(run(index, "hero", "models"), (Paths{"models/hero.gltf"}));

    // a bad re: matches nothing and says why
    const AssetQuery bad = AssetQuery::parse("re:([unclosed");
    EXPECT_FALSE(bad.error().empty());
    EXPECT_TRUE(bad.evaluate(index).empty());

    // the text form round-trips
    const AssetQuery query = AssetQuery::parse("hero  t:texture \"boss room\" path:\"my folder\" OR l:Hero");
    EXPECT_EQ(query.toString(), "hero t:texture \"boss room\" path:\"my folder\" OR l:Hero");
    EXPECT_EQ(AssetQuery::parse(query.toString()).toString(), query.toString());
    EXPECT_TRUE(AssetQuery::parse("   ").empty());
}

TEST(AssetQuery, RefAndMissingAskTheIndex) {
    FakeAssetIndex index = sampleIndex();
    index.referrers["textures/hero.png"] = {"guid-materials/Hero.material", "guid-prefabs/Enemy.prefab"};
    index.missing = {"guid-scenes/Main.scene"};
    EXPECT_EQ(run(index, "ref:textures/hero.png"), (Paths{"materials/Hero.material", "prefabs/Enemy.prefab"}));
    EXPECT_EQ(index.referrerCalls, 1);   // once per evaluation, not per asset
    EXPECT_EQ(run(index, "ref:textures/hero.png t:prefab"), (Paths{"prefabs/Enemy.prefab"}));
    EXPECT_EQ(run(index, "missing:"), (Paths{"scenes/Main.scene"}));
    EXPECT_EQ(index.missingCalls, 1);
}

TEST(AssetQuery, RegisteredTokenWorks) {
    // S29's vcs: registers this way
    AssetQuery::registerToken("testvcs", [](const AssetRecord& record, const std::string& value, AssetQuery::Context&) {
        return value == "modified" && record.path.starts_with("models/");
    }, "test token");
    EXPECT_TRUE(AssetQuery::isToken("testvcs"));
    const FakeAssetIndex index = sampleIndex();
    EXPECT_EQ(run(index, "testvcs:modified hero"), (Paths{"models/hero.gltf"}));
    const auto tokens = AssetQuery::tokens();
    EXPECT_TRUE(std::any_of(tokens.begin(), tokens.end(), [](const auto& token) { return token.first == "testvcs"; }));
}

TEST(AssetSearchSlow, FiftyThousandUnder30ms) {
#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    constexpr bool checkBudgets = false;
#else
    constexpr bool checkBudgets = true;
#endif
    FakeAssetIndex index;
    index.records.reserve(50000);
    const char* types[] = {"texture", "model", "material", "audio", "prefab"};
    const char* exts[] = {".png", ".gltf", ".material", ".ogg", ".prefab"};
    for (int i = 0; i < 50000; i++) {
        std::vector<std::string> labels;
        if (i % 7 == 0) labels.push_back("Hero");
        if (i % 11 == 0) labels.push_back("Environment");
        index.add("folder" + std::to_string(i % 200) + "/asset_" + std::to_string(i) + "_rock" + exts[i % 5], types[i % 5], labels);
    }
    const char* queries[] = {"t:texture l:Hero", "rock 123", "path:folder12/ ext:png", "l:environment OR t:audio"};
    using Clock = std::chrono::steady_clock;
    for (const char* text : queries) {
        double best = 1e9;
        size_t matches = 0;
        for (int run = 0; run < 3; run++) {
            const auto start = Clock::now();
            const AssetQuery query = AssetQuery::parse(text);
            matches = query.evaluate(index).size();
            best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        std::printf("[ bench ] Project search \"%s\" over 50000 assets: %.2f ms, %zu matches (budget 30, CI 60)\n", text, best, matches);
        EXPECT_GT(matches, 0u) << text;
        if (checkBudgets) {
            EXPECT_LE(best, 60.0) << text;
        }
    }
}
