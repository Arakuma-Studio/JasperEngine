// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Search palette's engine (S25 R11-R13, §6.2): prefixes, kind priority, recents, de-duplication,
// the cap, the asset: scope over the Project grammar and cancellation by a newer keystroke.

#include "search/SearchEngine.h"

#include "search/FakeAssetIndex.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>

using namespace jasperengine::editor;
using jasperengine::editor::test::FakeAssetIndex;

namespace {

    SearchEntry entry(SearchKind kind, const std::string& key, const std::string& label, const std::string& detail = {}) {
        SearchEntry e;
        e.kind = kind;
        e.key = key;
        e.label = label;
        e.detail = detail;
        return e;
    }

    std::vector<std::string> keys(const SearchSnapshot& snapshot, const std::vector<SearchResult>& results) {
        std::vector<std::string> out;
        for (const SearchResult& result : results) out.push_back(snapshot.entries[result.entry].key);
        return out;
    }

    SearchSnapshot sampleSnapshot() {
        SearchSnapshot snapshot;
        snapshot.entries.push_back(entry(SearchKind::Command, "command:edit.duplicate", "Duplicate", "Edit > Duplicate"));
        snapshot.entries.push_back(entry(SearchKind::Command, "command:file.save", "Save", "File > Save"));
        snapshot.entries.push_back(entry(SearchKind::Panel, "panel:Console", "Console"));
        snapshot.entries.push_back(entry(SearchKind::Panel, "panel:Inspector", "Inspector"));
        snapshot.entries.push_back(entry(SearchKind::Setting, "setting:physics.gravity", "Gravity", "Project Settings > Physics"));
        snapshot.entries.push_back(entry(SearchKind::Component, "component:Light", "Light"));
        snapshot.entries.push_back(entry(SearchKind::GameObject, "gameobject:main:1", "Light", "Light · Main Scene"));
        snapshot.entries.push_back(entry(SearchKind::GameObject, "gameobject:main:2", "Save Point", "Save Point · Main Scene"));
        snapshot.prepare();
        return snapshot;
    }

}

TEST(SearchEngine, ParsesPrefixScopes) {
    EXPECT_EQ(parseSearchQuery(">dup").kind, SearchKind::Command);
    EXPECT_EQ(parseSearchQuery(">dup").text, "dup");
    EXPECT_EQ(parseSearchQuery("/ console").kind, SearchKind::Panel);
    EXPECT_EQ(parseSearchQuery("/ console").text, "console");
    EXPECT_EQ(parseSearchQuery(":grav").kind, SearchKind::Setting);
    EXPECT_EQ(parseSearchQuery("@player").kind, SearchKind::GameObject);
    EXPECT_EQ(parseSearchQuery("+rigid").kind, SearchKind::Component);
    EXPECT_EQ(parseSearchQuery("Asset: t:texture").kind, SearchKind::Asset);
    EXPECT_EQ(parseSearchQuery("Asset: t:texture").text, "t:texture");
    EXPECT_FALSE(parseSearchQuery("  save ").kind.has_value());
    EXPECT_EQ(parseSearchQuery("  save ").text, "save");
}

TEST(SearchEngine, ScopesRanksRecents) {
    SearchSnapshot snapshot = sampleSnapshot();

    // kind priority breaks ties: the Light component (45) before the Light GameObject (40)
    std::vector<std::string> light = keys(snapshot, SearchEngine::query(snapshot, "light"));
    ASSERT_GE(light.size(), 2u);
    EXPECT_EQ(light[0], "component:Light");
    EXPECT_EQ(light[1], "gameobject:main:1");

    // a scope keeps one kind
    EXPECT_EQ(keys(snapshot, SearchEngine::query(snapshot, "@light")), (std::vector<std::string>{"gameobject:main:1"}));
    EXPECT_EQ(keys(snapshot, SearchEngine::query(snapshot, ">save")), (std::vector<std::string>{"command:file.save"}));
    EXPECT_EQ(keys(snapshot, SearchEngine::query(snapshot, ":gravity")), (std::vector<std::string>{"setting:physics.gravity"}));

    // "save": the command's exact match beats the GameObject's prefix
    std::vector<std::string> save = keys(snapshot, SearchEngine::query(snapshot, "save"));
    ASSERT_EQ(save.size(), 2u);
    EXPECT_EQ(save[0], "command:file.save");

    // a recent entry is boosted (150 for the newest): the Save Point GameObject passes the command
    snapshot.recents = {"gameobject:main:2"};
    save = keys(snapshot, SearchEngine::query(snapshot, "save"));
    ASSERT_EQ(save.size(), 2u);
    EXPECT_EQ(save[0], "command:file.save");     // 1200 + 70 against 994 + 40 + 150
    std::vector<std::string> sav = keys(snapshot, SearchEngine::query(snapshot, "sav"));
    ASSERT_EQ(sav.size(), 2u);
    EXPECT_EQ(sav[0], "gameobject:main:2");      // both prefixes: 999 + 70 against 993 + 40 + 150

    // an empty query: recents first (newest first), then commands and panels only
    snapshot.recents = {"setting:physics.gravity", "command:file.save"};
    std::vector<std::string> empty = keys(snapshot, SearchEngine::query(snapshot, ""));
    ASSERT_EQ(empty.size(), 5u);
    EXPECT_EQ(empty[0], "setting:physics.gravity");
    EXPECT_EQ(empty[1], "command:file.save");
    for (size_t i = 2; i < empty.size(); i++) {
        EXPECT_TRUE(empty[i].starts_with("command:") || empty[i].starts_with("panel:")) << empty[i];
    }
    // an empty scoped query lists that kind
    EXPECT_EQ(SearchEngine::query(snapshot, "@").size(), 2u);
}

TEST(SearchEngine, DeduplicatesAndCaps) {
    SearchSnapshot snapshot;
    for (int i = 0; i < 300; i++) {
        snapshot.entries.push_back(entry(SearchKind::GameObject, "gameobject:s:" + std::to_string(i), "Cube", "Cube" + std::to_string(i)));
    }
    snapshot.entries.push_back(entry(SearchKind::Panel, "panel:a", "Cube", "same"));
    snapshot.entries.push_back(entry(SearchKind::Panel, "panel:b", "Cube", "same"));
    snapshot.prepare();
    const std::vector<SearchResult> results = SearchEngine::query(snapshot, "cube");
    EXPECT_EQ(results.size(), SearchEngine::MAX_RESULTS);
    size_t panels = 0;
    for (const SearchResult& result : results) panels += snapshot.entries[result.entry].kind == SearchKind::Panel;
    EXPECT_EQ(panels, 1u);
}

TEST(SearchEngine, AssetScopeUsesTheProjectGrammar) {
    auto assets = std::make_shared<FakeAssetIndex>();
    assets->add("textures/hero.png", "texture", {"Hero"});
    assets->add("textures/boss.png", "texture");
    assets->add("models/hero.gltf", "model", {"Hero"});
    SearchSnapshot snapshot;
    for (size_t i = 0; i < assets->records.size(); i++) snapshot.entries.push_back(SearchEntries::asset(assets->records[i], i));
    snapshot.assets = assets;
    snapshot.prepare();

    EXPECT_EQ(keys(snapshot, SearchEngine::query(snapshot, "asset: t:texture l:hero")),
              (std::vector<std::string>{"asset:guid-textures/hero.png"}));
    EXPECT_EQ(SearchEngine::query(snapshot, "asset: t:texture").size(), 2u);
    EXPECT_EQ(SearchEngine::query(snapshot, "hero").size(), 2u);   // unscoped: fuzzy over the names
}

TEST(SearchEngine, NewerKeystrokeCancels) {
    SearchSnapshot snapshot = sampleSnapshot();
    std::atomic<uint64_t> latest{2};
    bool cancelled = false;
    EXPECT_TRUE(SearchEngine::query(snapshot, "light", &latest, 1, &cancelled).empty());
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(SearchEngine::query(snapshot, "light", &latest, 2, &cancelled).empty());
    EXPECT_FALSE(cancelled);
}
