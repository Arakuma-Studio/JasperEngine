// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Why each asset ships (docs/specs/build-size-tools.md R3, R9, R22): the breadth-first walk from
// project.yaml and the scripts, its tie-breaks, the chains it gives and its cost.

#include <gtest/gtest.h>

#include "cook/BuildReport.h"
#include "cook/Reachability.h"
#include "support/EditorTestSupport.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace ReachabilityTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    BuildReportFile shipped(const std::string& path, const std::string& asset, uint64_t bytes) {
        BuildReportFile file;
        file.path = path;
        file.category = BuildReport::categoryOf(path);
        file.bytes = bytes;
        file.asset = asset;
        return file;
    }

}

TEST(ReachabilityTest, ChainsAndRoots) {
    const std::vector<ReachEdge> edges = {
        // listed out of order: the walk sorts by referrer, field, target
        {"materials/wall.material", "textures/wall.png", "material/baseColorTexture", "MaterialTexture", ""},
        {"scenes/level.scene", "materials/wall.material", "entity:7/MeshComponent.submeshes[0].material", "MeshMaterialFile", "Level/Wall"},
        {"project.yaml", "scenes/level.scene", "project/scenes[0]", "SceneList", ""},
        // two referrers of the same texture at the same depth: the first field wins
        {"scenes/level.scene", "textures/shared.png", "entity:9/SpriteComponent.texture", "SceneComponent", "Level/B"},
        {"scenes/level.scene", "textures/shared.png", "entity:8/SpriteComponent.texture", "SceneComponent", "Level/A"},
        {"main.lua", "ui/hud.lua", "text:3", "LuaRequire", ""},
    };
    const auto reached = computeReachability(edges, {"project.yaml", "main.lua", "ui/hud.lua"});
    ASSERT_TRUE(reached.count("textures/wall.png"));
    EXPECT_EQ(reached.at("textures/wall.png").via, "materials/wall.material");
    EXPECT_EQ(reached.at("textures/shared.png").field, "entity:8/SpriteComponent.texture") << "ties broken by field locator";
    EXPECT_EQ(reached.at("textures/shared.png").object, "Level/A");
    EXPECT_EQ(reached.at("ui/hud.lua").kind, "root") << "a shipped script is a root itself";
    EXPECT_EQ(reached.at("project.yaml").kind, "root");
    EXPECT_FALSE(reached.count("textures/orphan.png"));

    BuildReport report;
    report.addFile(shipped("assets/textures/wall.png", "textures/wall.png", 4000));
    report.addFile(shipped("assets/textures/orphan.png", "textures/orphan.png", 9000));
    BuildReportFile map = shipped("assets/je-asset-map.json", "", 10);
    map.origin = "generated";
    report.addFile(map);
    report.reachability = reached;

    const BuildReportReferrers wall = report.referencedBy("assets/textures/wall.png");
    EXPECT_EQ(wall.reason, "reached");
    ASSERT_EQ(wall.chain.size(), 4u);
    EXPECT_EQ(wall.chain[0].asset, "textures/wall.png");
    EXPECT_EQ(wall.chain[0].kind, "MaterialTexture");
    EXPECT_EQ(wall.chain[1].asset, "materials/wall.material");
    EXPECT_EQ(wall.chain[1].object, "Level/Wall");
    EXPECT_EQ(wall.chain[2].asset, "scenes/level.scene");
    EXPECT_EQ(wall.chain[2].field, "project/scenes[0]");
    EXPECT_EQ(wall.chain[3].asset, "project.yaml");
    EXPECT_EQ(wall.chain[3].kind, "root");
    // by its asset path too
    EXPECT_EQ(report.referencedBy("textures/wall.png").chain.size(), 4u);

    const BuildReportReferrers orphan = report.referencedBy("assets/textures/orphan.png");
    EXPECT_EQ(orphan.reason, "unreferenced");
    EXPECT_TRUE(orphan.chain.empty());
    EXPECT_EQ(report.referencedBy("assets/je-asset-map.json").reason, "generated");
    ASSERT_EQ(report.unreferenced().size(), 1u);
    EXPECT_EQ(report.unreferenced()[0]->path, "assets/textures/orphan.png");

    // a cycle ends the chain at the repeat
    BuildReport looped;
    looped.reachability = std::map<std::string, BuildReportReach>{{"a", {"b", "f", "Other", ""}}, {"b", {"a", "f", "Other", ""}}};
    EXPECT_EQ(looped.chainOf("a").size(), 2u);
}

TEST(ReachabilityTest, TenThousandAssetsWithinBudget) {
    // R22: 10,000 assets and 60,000 edges walked in at most 50 ms
    std::vector<ReachEdge> edges;
    edges.reserve(60000);
    char from[64], to[64];
    for (int i = 0; i < 60000; i++) {
        const int a = i % 10000, b = (a * 31 + (i / 10000) * 977 + 7) % 10000;
        std::snprintf(from, sizeof(from), "folder%02d/asset%05d.material", a % 50, a);
        std::snprintf(to, sizeof(to), "folder%02d/asset%05d.material", b % 50, b);
        edges.push_back({from, to, "material/field" + std::to_string(i % 6), "MaterialTexture", ""});
    }
    edges.push_back({"project.yaml", "folder00/asset00000.material", "project/scenes[0]", "SceneList", ""});
    // the best of three runs: a shared builder's load comes and goes
    double ms = 1e9;
    std::map<std::string, BuildReportReach> reached;
    for (int run = 0; run < 3; run++) {
        const auto start = std::chrono::steady_clock::now();
        reached = computeReachability(edges, {"project.yaml"});
        ms = std::min(ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::printf("[ PERF ] reachability of 10000 assets, 60001 edges: %.2f ms (budget 50 ms)\n", ms);
    EXPECT_GT(reached.size(), 9000u);
    if (editor::test::perfSlack() > 0.0) { EXPECT_LE(ms, 50.0 * editor::test::perfSlack()); }
}

}  // namespace ReachabilityTest_cpp
