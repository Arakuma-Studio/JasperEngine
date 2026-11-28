// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A large Build Report's costs (docs/specs/build-size-tools.md R22-R24): 20,000 files written,
// parsed, compared and asked why one ships, printed and gated through perfSlack().

#include <gtest/gtest.h>

#include "cook/BuildReport.h"
#include "cook/Reachability.h"
#include "support/EditorTestSupport.h"

#include "TempDir.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace BuildReportBenchTest_cpp {

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
        char path[112], asset[96], scene[64];   // path = "assets/" + asset: room for the whole asset
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
    std::string error;
    ASSERT_TRUE(largeReport(0).write(dir.path() / "base.json", error)) << error;
    // the best of three runs of each step: a shared builder's load comes and goes
    double writeMs = 1e9, parseMs = 1e9, fullParseMs = 1e9, compareMs = 1e9, chainMs = 1e9;
    BuildReport base, loaded;
    BuildReportDiff diff;
    BuildReportReferrers why;
    for (int run = 0; run < 3; run++) {
        auto start = std::chrono::steady_clock::now();
        ASSERT_TRUE(head.write(dir.path() / "head.json", error)) << error;
        writeMs = std::min(writeMs, msSince(start));

        // as get_build_report compare_to reads them: the report whole, the one it is compared with for
        // what the comparison needs
        start = std::chrono::steady_clock::now();
        ASSERT_TRUE(BuildReport::load(dir.path() / "base.json", base, error, true)) << error;
        ASSERT_TRUE(BuildReport::load(dir.path() / "head.json", loaded, error)) << error;
        parseMs = std::min(parseMs, msSince(start));
        start = std::chrono::steady_clock::now();
        BuildReport whole;
        ASSERT_TRUE(BuildReport::load(dir.path() / "base.json", whole, error)) << error;
        fullParseMs = std::min(fullParseMs, msSince(start));
        start = std::chrono::steady_clock::now();
        diff = compareBuildReports(base, loaded);
        compareMs = std::min(compareMs, msSince(start));

        start = std::chrono::steady_clock::now();
        why = loaded.referencedBy("assets/textures/area07/props/prop_19967.png");
        chainMs = std::min(chainMs, msSince(start));
    }
    const uintmax_t bytes = std::filesystem::file_size(dir.path() / "head.json");
    EXPECT_EQ(diff.grown, 6667u);
    EXPECT_EQ(why.chain.size(), 3u);

    std::printf("[ PERF ] build report of 20000 files: write %.1f ms (150), %.2f MB (4), parse two %.1f ms + compare %.1f ms = %.1f ms (80), "
                "chain %.3f ms (1); one report read whole %.1f ms\n", writeMs, (double)bytes / 1048576.0, parseMs, compareMs, parseMs + compareMs,
                chainMs, fullParseMs);
    EXPECT_LE(bytes, 4u * 1048576u) << "a report of 20,000 files fits 4 MB";
    if (slack > 0.0) {
        EXPECT_LE(writeMs, 150.0 * slack);
        EXPECT_LE(parseMs + compareMs, 80.0 * slack);
        EXPECT_LE(chainMs, 1.0 * slack);
    }
}

}  // namespace BuildReportBenchTest_cpp
