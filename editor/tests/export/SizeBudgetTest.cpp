// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Size budgets (docs/specs/build-size-tools.md R12-R14, R30): which row applies to a build, the
// MB the report shows, source exports and empty tables left alone, and the Project Settings rows
// edited as one undo step each.

#include <gtest/gtest.h>

#include "Project.h"
#include "command/type/ProjectSettingsValuesCmd.h"
#include "cook/BuildReport.h"
#include "export/SizeBudget.h"
#include "settings/SettingsRegistry.h"

#include "TempDir.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace SizeBudgetTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string readBytes(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    BuildReport reportOf(const std::string& platform, const std::string& method) {
        BuildReport report;
        report.platform = platform;
        report.method = method;
        BuildReportFile big;
        big.path = "assets/textures/big.png";
        big.category = "textures";
        big.bytes = 3 * 1048576;
        report.addFile(big);
        BuildReportFile engine;
        engine.path = "game.wasm";
        engine.category = "engine";
        engine.bytes = 1048576;
        report.addFile(engine);
        return report;
    }

}

TEST(SizeBudgetTest, RowsAndSpecificity) {
    const std::vector<SizeBudgetRow> rows = {
        {"all", "textures", 10.0}, {"web", "textures", 1.0}, {"desktop", "textures", 5.0}, {"all", "total", 2.0}, {"android", "audio", 1.0}};
    // a platform's row replaces the all row of its category; desktop sits between them
    const std::vector<SizeBudgetRow> web = sizebudget::applicable(rows, "web");
    ASSERT_EQ(web.size(), 2u);
    EXPECT_EQ(web[0].category, "total");
    EXPECT_EQ(web[1].platform, "web");
    const std::vector<SizeBudgetRow> desktop = sizebudget::applicable(rows, "linux");
    ASSERT_EQ(desktop.size(), 2u);
    EXPECT_EQ(desktop[1].platform, "desktop");
    EXPECT_EQ(sizebudget::applicable(rows, "ios")[1].platform, "all");

    // 1 MB is 1,048,576 bytes, as the report shows it
    EXPECT_EQ(sizebudget::limitBytes(1.0), 1048576u);
    EXPECT_EQ(sizebudget::limitBytes(2.5), 2621440u);

    const BuildReport report = reportOf("web", "prebuilt");
    const std::vector<BuildReportBudget> content = sizebudget::evaluate(rows, report, true);
    ASSERT_EQ(content.size(), 1u) << "textures before the engine; total after it";
    EXPECT_EQ(content[0].category, "textures");
    EXPECT_EQ(content[0].limitBytes, 1048576u);
    EXPECT_EQ(content[0].overBytes(), 2u * 1048576u);
    const std::string text = sizebudget::overText(content[0], report);
    EXPECT_NE(text.find("assets/textures/big.png"), std::string::npos) << text;
    EXPECT_NE(text.find("Project Settings > Player > Size Budgets"), std::string::npos) << text;
    const std::vector<BuildReportBudget> rest = sizebudget::evaluate(rows, report, false);
    ASSERT_EQ(rest.size(), 1u);
    EXPECT_EQ(rest[0].category, "total");
    EXPECT_EQ(rest[0].bytes, 4u * 1048576u);

    // source exports are never checked, and no rows evaluate nothing
    EXPECT_TRUE(sizebudget::evaluate(rows, reportOf("source", "source"), true).empty());
    const uint64_t before = sizebudget::evaluations();
    EXPECT_TRUE(sizebudget::evaluate({}, report, true).empty());
    EXPECT_TRUE(sizebudget::evaluate({}, report, false).empty());
    EXPECT_EQ(sizebudget::evaluations(), before) << "no rows: nothing evaluated";

    // the settings value: validated with the fix in the error
    std::vector<SizeBudgetRow> parsed;
    std::string error;
    EXPECT_TRUE(sizebudget::fromJson(sizebudget::toJson(rows), parsed, error)) << error;
    EXPECT_EQ(parsed, rows);
    EXPECT_FALSE(sizebudget::fromJson(Json::array({{{"platform", "web"}, {"category", "textures"}, {"limit_mb", 0}}}), parsed, error));
    EXPECT_NE(error.find("limit_mb above 0"), std::string::npos) << error;
    EXPECT_FALSE(sizebudget::fromJson(Json::array({{{"platform", "switch"}, {"limit_mb", 1}}}), parsed, error));
}

TEST(SizeBudgetTest, SettingsRowsUndo) {
    jasperengine::test::TempDir dir;
    const std::filesystem::path root = dir.path();
    std::filesystem::create_directories(root / "assets");
    std::ofstream(root / "project.yaml") << "name: budgets\nassetsDir: assets\n";
    Project project;
    project.setSkipProjectLock(true);
    ASSERT_TRUE(project.loadProject(root, false));
    ASSERT_TRUE(project.saveProjectFile());
    const std::string saved = readBytes(root / "project.yaml");
    ASSERT_EQ(saved.find("sizeBudget"), std::string::npos);

    auto* add = new ProjectSettingsValuesCmd(&project, {{"player.size_budgets", Json::array({{{"platform", "web"}, {"category", "textures"}, {"limit_mb", 1}}})}},
                                             "Edit Size Budgets");
    add->setNoMerge();
    ASSERT_TRUE(project.getProjectCommandHistory()->addCommand(add));
    auto* fail = new ProjectSettingsValuesCmd(&project, {{"player.size_budget_action", "fail"}}, "Set When Over Budget");
    fail->setNoMerge();
    ASSERT_TRUE(project.getProjectCommandHistory()->addCommand(fail));
    ASSERT_EQ(project.getSizeBudgets().size(), 1u);
    EXPECT_TRUE(project.isSizeBudgetFail());
    const std::string edited = readBytes(root / "project.yaml");
    EXPECT_NE(edited.find("sizeBudgets"), std::string::npos) << edited;
    EXPECT_NE(edited.find("sizeBudgetAction: fail"), std::string::npos) << edited;

    ASSERT_TRUE(project.getProjectCommandHistory()->undo());
    EXPECT_FALSE(project.isSizeBudgetFail()) << "each edit is its own step";
    EXPECT_EQ(project.getSizeBudgets().size(), 1u);
    ASSERT_TRUE(project.getProjectCommandHistory()->undo());
    EXPECT_TRUE(project.getSizeBudgets().empty());
    EXPECT_EQ(readBytes(root / "project.yaml"), saved) << "undone byte for byte";

    // Project Settings' search finds both rows
    std::vector<std::string> keys;
    for (const SettingRow* row : SettingsRegistry::get().search("size budget")) keys.push_back(row->key);
    EXPECT_NE(std::find(keys.begin(), keys.end(), "player.size_budgets"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "player.size_budget_action"), keys.end());
}

}  // namespace SizeBudgetTest_cpp
