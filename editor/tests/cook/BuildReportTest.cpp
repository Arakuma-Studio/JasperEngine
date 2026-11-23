// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S99 R10, R11 (build size for agents): the web download estimate counts what a browser
// fetches, gzip -9 per file; the Download Budget's family per platform; W-BUILD-OVER-BUDGET's
// message names the budget, the actual size and the three largest categories; the budget is a
// Project Settings row stored in project.yaml (absent: off) and edited as one undo step.

#include <gtest/gtest.h>

#include "Project.h"
#include "command/type/ProjectSettingsValuesCmd.h"
#include "cook/BuildReport.h"

#include "TempDir.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    void buildReportWrite(const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    }

    // text that compresses like code, longer than one read of the estimate (1 MiB)
    std::string buildReportCompressible(size_t size) {
        std::string text;
        uint32_t state = 12345;
        while (text.size() < size) {
            state = state * 1103515245u + 12345u;
            text += "function f" + std::to_string((state >> 16) % 97) + "() { return " + std::to_string((state >> 8) % 1000) + "; }\n";
        }
        text.resize(size);
        return text;
    }

    std::string buildReportReadText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

}

TEST(BuildReport, WebDownloadIsWhatTheBrowserFetches) {
    for (const char* fetched : {"index.html", "player-loader.js", "jasperengine-player-st.js", "jasperengine-player-st.wasm", "content.jpak",
                                "favicon.png", "plugins/poki.js", "Game.js", "Game.wasm", "Game.data", "plugins/license-check.js"})
        EXPECT_TRUE(BuildReport::isWebDownload(fetched)) << fetched;
    for (const char* skipped : {"build-report.json", "content-manifest.json", "_headers", "licenses/lua-LICENSE", "THIRD-PARTY-NOTICES.txt",
                                "sokol-LICENSE", "LICENSE.txt", "threads/Game.wasm", "symbols/Game.debug", "jasperengine-player-mt.js",
                                "jasperengine-player-mt.wasm", "jasperengine-player-mt.worker.js", "coi-serviceworker.js"})
        EXPECT_FALSE(BuildReport::isWebDownload(skipped)) << skipped;
}

TEST(BuildReport, WebDownloadGzipIsGzipNineOfTheFetchedFiles) {
    jasperengine::test::TempDir dir;
    const std::string wasm = buildReportCompressible(1024 * 1024 + 17);
    const std::string page = "<!doctype html><html><body><canvas></canvas><script src=\"player-loader.js\"></script></body></html>\n";
    buildReportWrite(dir.path() / "jasperengine-player-st.wasm", wasm);
    buildReportWrite(dir.path() / "index.html", page);
    buildReportWrite(dir.path() / "empty.js", "");
    const uint64_t estimate = BuildReport::webDownloadGzipBytes(dir.path());
    // Python's gzip.compress(data, 9) (zlib) of the same three files: 121999 + 95 + 20 bytes. The
    // estimate's deflate is miniz's level 9, streamed in 1 MiB reads: close, not byte-identical
    EXPECT_NEAR((double)estimate, 122114.0, 122114.0 * 0.02);

    // none of these reach the browser
    buildReportWrite(dir.path() / "licenses" / "lua-LICENSE", buildReportCompressible(50000));
    buildReportWrite(dir.path() / "jasperengine-player-mt.wasm", wasm);
    buildReportWrite(dir.path() / BuildReport::kFileName, "{}");
    EXPECT_EQ(BuildReport::webDownloadGzipBytes(dir.path()), estimate);
    EXPECT_EQ(BuildReport::webDownloadGzipBytes(dir.path() / "missing"), 0u);
}

TEST(BuildReport, BudgetFamilyFollowsThePlatform) {
    ModelFamily family = ModelFamily::Desktop;
    ASSERT_TRUE(BuildReport::budgetFamilyOf("web", family));
    EXPECT_EQ(family, ModelFamily::Web);
    for (const char* mobile : {"android", "ios"}) {
        ASSERT_TRUE(BuildReport::budgetFamilyOf(mobile, family)) << mobile;
        EXPECT_EQ(family, ModelFamily::Mobile) << mobile;
    }
    for (const char* desktop : {"windows", "linux", "macos", "desktop"}) {
        ASSERT_TRUE(BuildReport::budgetFamilyOf(desktop, family)) << desktop;
        EXPECT_EQ(family, ModelFamily::Desktop) << desktop;
    }
    EXPECT_FALSE(BuildReport::budgetFamilyOf("source", family)) << "a source export downloads nothing";
    EXPECT_EQ(BuildReport::budgetBytesOf(0.001), 1049u) << "MB are MiB, as the report prints them";
    EXPECT_EQ(BuildReport::budgetBytesOf(10.0), 10u * 1048576u);
    EXPECT_EQ(BuildReport::budgetBytesOf(0.0), 0u);
    EXPECT_EQ(BuildReport::budgetBytesOf(-1.0), 0u);
}

TEST(BuildReport, OverBudgetMessageNamesBudgetActualAndLargestCategories) {
    BuildReport web;
    web.platform = "web";
    web.totalBytes = 20000;
    web.downloadBytesGzip = 5000;
    web.categories = {{"textures", 3000}, {"engine", 9000}, {"models", 1000}, {"scripts", 10}, {"audio", 0}};
    EXPECT_EQ(web.budgetedBytes(), 5000u) << "the web is held to its download";
    const std::string message = web.overBudgetMessage(BuildReport::budgetBytesOf(0.001));
    EXPECT_NE(message.find("downloads 4.9 KB (gzip estimate)"), std::string::npos) << message;
    EXPECT_NE(message.find("Download Budget of 1.0 KB (player.download_budget_mb.web)"), std::string::npos) << message;
    EXPECT_NE(message.find("largest categories: engine 8.8 KB, textures 2.9 KB, models 1000 B"), std::string::npos) << message;
    EXPECT_EQ(message.find("scripts"), std::string::npos) << "three categories only: " << message;
    EXPECT_EQ(web.overBudgetMessage(5000), "") << "at the budget is within it";
    EXPECT_EQ(web.overBudgetMessage(0), "") << "0 is off";

    BuildReport desktop = web;
    desktop.platform = "linux";
    desktop.downloadBytesGzip = 0;
    EXPECT_EQ(desktop.budgetedBytes(), 20000u) << "a desktop build is held to its bytes";
    EXPECT_NE(desktop.overBudgetMessage(10000).find("the linux build weighs 19.5 KB, over its Download Budget of 9.8 KB "
                                                  "(player.download_budget_mb.desktop)"), std::string::npos) << desktop.overBudgetMessage(10000);
    BuildReport source = desktop;
    source.platform = "source";
    EXPECT_EQ(source.overBudgetMessage(1), "");

    BuildReport loaded;
    ASSERT_TRUE(BuildReport::fromJson(web.toJson(), loaded));
    EXPECT_EQ(loaded.downloadBytesGzip, 5000u);
}

TEST(BuildReport, DownloadBudgetIsAnUndoableSettingInProjectYaml) {
    jasperengine::test::TempDir dir;
    const fs::path root = dir.path();
    fs::create_directories(root / "assets");
    std::ofstream(root / "project.yaml") << "name: budget\nassetsDir: assets\n";

    Project project;
    project.setSkipProjectLock(true);
    ASSERT_TRUE(project.loadProject(root, false));
    for (size_t f = 0; f < kModelFamilyCount; f++) EXPECT_EQ(project.getDownloadBudgetMB((ModelFamily)f), 0.0) << "an old project has none";

    auto* command = new ProjectSettingsValuesCmd(&project, {{"player.download_budget_mb.web", 12.5}, {"player.download_budget_mb.mobile", 150}},
                                                 "Change Download Budget");
    command->setNoMerge();
    ASSERT_TRUE(project.getProjectCommandHistory()->addCommand(command));
    EXPECT_EQ(project.getDownloadBudgetMB(ModelFamily::Web), 12.5);
    EXPECT_EQ(project.getDownloadBudgetMB(ModelFamily::Mobile), 150.0);
    const YAML::Node saved = YAML::Load(buildReportReadText(root / "project.yaml"));
    ASSERT_TRUE(saved["export"] && saved["export"]["downloadBudgetMB"]) << buildReportReadText(root / "project.yaml");
    EXPECT_EQ(saved["export"]["downloadBudgetMB"]["web"].as<double>(), 12.5);
    EXPECT_FALSE(saved["export"]["downloadBudgetMB"]["desktop"]) << "a family without a budget writes no key";

    ASSERT_TRUE(project.getProjectCommandHistory()->undo());
    EXPECT_EQ(project.getDownloadBudgetMB(ModelFamily::Web), 0.0);
    EXPECT_EQ(project.getDownloadBudgetMB(ModelFamily::Mobile), 0.0);
    const YAML::Node undone = YAML::Load(buildReportReadText(root / "project.yaml"));
    EXPECT_FALSE(undone["export"] && undone["export"]["downloadBudgetMB"]) << "off again: the key is gone";

    ProjectSettingsValuesCmd negative(&project, {{"player.download_budget_mb.desktop", -1}}, "Change Download Budget");
    EXPECT_FALSE(negative.execute()) << "a budget is 0 (off) or more";

    // a hand-edited file: a negative or non-numeric value is off
    std::ofstream(root / "project.yaml") << "name: budget\nassetsDir: assets\nexport:\n  downloadBudgetMB: {web: 20, mobile: -3, desktop: big}\n";
    Project reloaded;
    reloaded.setSkipProjectLock(true);
    ASSERT_TRUE(reloaded.loadProject(root, false));
    EXPECT_EQ(reloaded.getDownloadBudgetMB(ModelFamily::Web), 20.0);
    EXPECT_EQ(reloaded.getDownloadBudgetMB(ModelFamily::Mobile), 0.0);
    EXPECT_EQ(reloaded.getDownloadBudgetMB(ModelFamily::Desktop), 0.0);
}
