// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The palette's budgets (S25 R33, R34; AC23): a snapshot of 20k assets, 10k GameObjects, 300
// commands and 400 settings builds in 25 ms (CI threshold 50), and 20k entries score in 30 ms
// (CI threshold 60). Instrumented builds run the work, not the budgets.

#include "search/SearchEngine.h"

#include "search/FakeAssetIndex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace jasperengine::editor;
using jasperengine::editor::test::FakeAssetIndex;

namespace {

#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    constexpr bool checkBudgets = false;
#else
    constexpr bool checkBudgets = true;
#endif

    using Clock = std::chrono::steady_clock;

    double millisecondsSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    const char* const WORDS[] = {"rock", "tree", "hero", "enemy", "light", "door", "wall", "floor", "crate", "barrel",
                                 "player", "camera", "spawn", "water", "grass", "stone", "metal", "wood", "glass", "boss"};

    std::string nameOf(int i) {
        return std::string(WORDS[i % 20]) + "_" + WORDS[(i / 20) % 20] + "_" + std::to_string(i);
    }

    struct World {
        std::shared_ptr<FakeAssetIndex> assets = std::make_shared<FakeAssetIndex>();
        std::vector<std::pair<std::string, std::string>> objects;   // name, entity path
    };

    World makeWorld() {
        World world;
        world.assets->records.reserve(20000);
        for (int i = 0; i < 20000; i++) world.assets->add("assets/folder" + std::to_string(i % 300) + "/" + nameOf(i) + ".png", "texture");
        world.objects.reserve(10000);
        for (int i = 0; i < 10000; i++) {
            const std::string name = nameOf(i * 7);
            world.objects.push_back({name, "Group " + std::to_string(i / 100) + "/" + name});
        }
        return world;
    }

    SearchSnapshot buildSnapshot(const World& world) {
        SearchSnapshot snapshot;
        snapshot.entries.reserve(world.assets->size() + world.objects.size() + 700);
        for (int i = 0; i < 300; i++) {
            SearchEntry entry;
            entry.kind = SearchKind::Command;
            entry.label = "Command " + nameOf(i);
            entry.key = "command:test." + std::to_string(i);
            entry.detail = "Group > " + entry.label;
            entry.searchText = "Group " + entry.label;
            entry.shortcut = "Ctrl+Shift+F12";
            snapshot.entries.push_back(std::move(entry));
        }
        for (int i = 0; i < 400; i++) {
            SearchEntry entry;
            entry.kind = SearchKind::Setting;
            entry.label = "Setting " + nameOf(i * 3);
            entry.key = "setting:test." + std::to_string(i);
            entry.detail = "Project Settings > Page > " + entry.label;
            entry.searchText = "page section keywords test." + std::to_string(i);
            snapshot.entries.push_back(std::move(entry));
        }
        for (size_t i = 0; i < world.objects.size(); i++) {
            snapshot.entries.push_back(SearchEntries::gameObject(world.objects[i].first, world.objects[i].second, "Main Scene", "scene-guid", 1,
                                                                 1000 + i));
        }
        for (size_t i = 0; i < world.assets->size(); i++) snapshot.entries.push_back(SearchEntries::asset(world.assets->at(i), i));
        snapshot.assets = world.assets;
        snapshot.prepare();
        return snapshot;
    }

}

TEST(SearchSnapshotSlow, BuildUnder25ms) {
    const World world = makeWorld();
    double best = 1e9;
    size_t entries = 0;
    for (int run = 0; run < 5; run++) {
        const auto start = Clock::now();
        const SearchSnapshot snapshot = buildSnapshot(world);
        best = std::min(best, millisecondsSince(start));
        entries = snapshot.entries.size();
    }
    EXPECT_EQ(entries, 30700u);
    std::printf("[ bench ] palette snapshot of %zu entries: %.2f ms (budget 25, CI 50)\n", entries, best);
    if (checkBudgets) {
        EXPECT_LE(best, 50.0);
    }
}

TEST(SearchEngineSlow, TwentyThousandEntries) {
    const World world = makeWorld();
    SearchSnapshot snapshot = buildSnapshot(world);
    snapshot.entries.resize(20000);    // commands, settings, GameObjects and the first assets
    const char* queries[] = {"rock", "hero tree", "crt", "boss_wall_1", "plyer camra", "@enemy", ">command rock", "zzzz"};
    std::vector<double> times;
    for (int round = 0; round < 5; round++) {
        for (const char* query : queries) {
            const auto start = Clock::now();
            const std::vector<SearchResult> results = SearchEngine::query(snapshot, query);
            times.push_back(millisecondsSince(start));
            EXPECT_LE(results.size(), SearchEngine::MAX_RESULTS);
        }
    }
    std::sort(times.begin(), times.end());
    const double p95 = times[times.size() * 95 / 100];
    std::printf("[ bench ] scoring 20000 entries: p95 %.2f ms, median %.2f ms (budget 30, CI 60)\n", p95, times[times.size() / 2]);
    if (checkBudgets) {
        EXPECT_LE(p95, 60.0);
    }
    // an asset: query evaluates the grammar over the snapshot's records
    const auto start = Clock::now();
    const std::vector<SearchResult> assets = SearchEngine::query(buildSnapshot(world), "asset: t:texture rock");
    std::printf("[ bench ] asset: scope over 20000 assets: %.2f ms (snapshot included)\n", millisecondsSince(start));
    EXPECT_FALSE(assets.empty());
}
