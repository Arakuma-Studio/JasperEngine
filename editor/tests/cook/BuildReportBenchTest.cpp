// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A large Build Report's costs (docs/specs/build-size-tools.md R22-R24): 20,000 files written,
// parsed, compared and asked why one ships, printed and gated through perfSlack().

#include <gtest/gtest.h>

#include "cook/BuildReport.h"
#include "cook/Reachability.h"
#include "support/EditorTestSupport.h"

#include "TempDir.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    double msSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    BuildReport largeReport(uint64_t grow) {
        BuildReport report;
        report.platform = "android";
        report.method = "prebuilt";
        report.id = "20261119-142233-prebuilt";
        std::vector<ReachEdge> edges;
        char path[96], asset[96], scene[64];
        for (int i = 0; i < 20000; i++) {
            std::snprintf(asset, sizeof(asset), "textures/area%02d/props/prop_%05d.png", i % 40, i);
            std::snprintf(path, sizeof(path), "assets/%s", asset);
            std::snprintf(scene, sizeof(scene), "scenes/level%02d.scene", i % 40);
            BuildReportFile file;
            file.path = path;
            file.category = "textures";
            file.bytes = 20000 + (uint64_t)i + (i % 3 == 0 ? grow : 0);
            file.asset = asset;
            report.addFile(file);
            edges.push_back({scene, asset, "entity:" + std::to_string(i) + "/SpriteComponent.texture", "SceneComponent", ""});
        }
        for (int s = 0; s < 40; s++) {
            std::snprintf(scene, sizeof(scene), "scenes/level%02d.scene", s);
            edges.push_back({"project.yaml", scene, "project/scenes[" + std::to_string(s) + "]", "SceneList", ""});
        }
        report.reachability = computeReachability(edges, {"project.yaml"});
        return report;
    }

}

TEST(BuildReportBench, LargeReport) {
    const double slack = editor::test::perfSlack();
    jasperengine::test::TempDir dir;
    const BuildReport head = largeReport(100);
    auto start = std::chrono::steady_clock::now();
    std::string error;
    ASSERT_TRUE(head.write(dir.path() / "head.json", error)) << error;
    const double writeMs = msSince(start);
    const uintmax_t bytes = std::filesystem::file_size(dir.path() / "head.json");
    ASSERT_TRUE(largeReport(0).write(dir.path() / "base.json", error)) << error;

    start = std::chrono::steady_clock::now();
    BuildReport base, loaded;
    ASSERT_TRUE(BuildReport::load(dir.path() / "base.json", base, error)) << error;
    ASSERT_TRUE(BuildReport::load(dir.path() / "head.json", loaded, error)) << error;
    const BuildReportDiff diff = compareBuildReports(base, loaded);
    const double compareMs = msSince(start);
    EXPECT_EQ(diff.grown, 6667u);

    start = std::chrono::steady_clock::now();
    const BuildReportReferrers why = loaded.referencedBy("assets/textures/area07/props/prop_19967.png");
    const double chainMs = msSince(start);
    EXPECT_EQ(why.chain.size(), 3u);

    std::printf("[ PERF ] build report of 20000 files: write %.1f ms (150), %.2f MB (4), parse two + compare %.1f ms (80), chain %.3f ms (1)\n",
                writeMs, (double)bytes / 1048576.0, compareMs, chainMs);
    EXPECT_LE(bytes, 4u * 1048576u) << "a report of 20,000 files fits 4 MB";
    if (slack > 0.0) {
        EXPECT_LE(writeMs, 150.0 * slack);
        EXPECT_LE(compareMs, 80.0 * slack);
        EXPECT_LE(chainMs, 1.0 * slack);
    }
}
