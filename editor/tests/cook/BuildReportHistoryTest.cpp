// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project's Build Report history (docs/specs/build-size-tools.md R6): ten per platform, ids
// in time order, a collision numbered, a write failure a warning and never a failed export.

#include <gtest/gtest.h>

#include "cook/BuildReportHistory.h"

#include "TempDir.h"

#include <filesystem>
#include <string>

namespace BuildReportHistoryTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

TEST(BuildReportHistoryTest, RecordsAndPrunes) {
    jasperengine::test::TempDir dir;
    const fs::path project = dir.path();
    const std::time_t start = 1795000000;   // 2026-11-18
    std::vector<std::string> ids;
    for (int i = 0; i < 12; i++) {
        BuildReport report;
        report.platform = "web";
        report.method = "prebuilt";
        report.id = BuildReportHistory::newId(project, "web", "prebuilt", start + i * 60);
        std::string warning;
        EXPECT_TRUE(BuildReportHistory::record(project, report, report.toText(), warning));
        EXPECT_TRUE(warning.empty()) << warning;
        ids.push_back(report.id);
    }
    const std::vector<BuildReportHistory::Entry> entries = BuildReportHistory::list(project, "web");
    ASSERT_EQ(entries.size(), BuildReportHistory::kKeep);
    EXPECT_EQ(entries.front().id, ids.back()) << "newest first";
    EXPECT_EQ(entries.back().id, ids[2]) << "the two oldest were deleted";
    EXPECT_FALSE(fs::exists(BuildReportHistory::directory(project) / "web" / (ids[0] + ".json")));
    EXPECT_EQ(ids[0].substr(0, 9), "20261118-") << ids[0];

    // the same second again: numbered, and sorted after the first
    const std::string again = BuildReportHistory::newId(project, "web", "prebuilt", start + 11 * 60);
    EXPECT_EQ(again, ids.back() + "-2");
    BuildReport collided;
    collided.platform = "web";
    collided.id = again;
    std::string warning;
    BuildReportHistory::record(project, collided, collided.toText(), warning);
    EXPECT_EQ(BuildReportHistory::list(project, "web").front().id, again);
    EXPECT_EQ(BuildReportHistory::previous(project, collided), ids.back());

    // resolve: an id, a path, and an unknown id naming the history
    std::string error;
    EXPECT_EQ(BuildReportHistory::resolve(project, ids.back(), error), entries.front().file);
    EXPECT_TRUE(BuildReportHistory::resolve(project, "20990101-000000-prebuilt", error).empty());
    EXPECT_NE(error.find(ids.back()), std::string::npos) << error;

    // a history folder that cannot be made: a warning, and the export goes on
    jasperengine::test::TempDir blocked;
    blocked.write(".jasperengine/build-reports", "a file where the folder goes");
    BuildReport report;
    report.platform = "web";
    report.id = "20261118-000000-prebuilt";
    EXPECT_TRUE(BuildReportHistory::record(blocked.path(), report, report.toText(), warning));
    EXPECT_NE(warning.find("could not be written"), std::string::npos) << warning;
}

}  // namespace BuildReportHistoryTest_cpp
