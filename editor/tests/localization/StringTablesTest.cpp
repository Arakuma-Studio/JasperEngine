// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's String Tables (docs/specs/S95-lua-runtime-gaps.md REQ-26, REQ-29, REQ-30, AC-22):
// the CSV and XLIFF interchange through S87's parsers, the missing keys per language, the check's
// duplicate key, and the game.json localization block a prebuilt player reads.

#include <gtest/gtest.h>

#include "localization/StringTables.h"
#include "player/PlayerConfig.h"

#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    StringTable stTable() {
        StringTable table;
        std::string error;
        EXPECT_TRUE(StringTable::parse(R"({"version": 1, "entries": [
            {"key": "menu.play", "comment": "Main menu, \"Play\"", "values": {"en": "Play", "es": "Jugar"}},
            {"key": "hud.coins", "values": {"en": {"one": "{count} coin", "other": "{count} coins"}, "es": {"one": "{count} moneda"}}},
            {"key": "multi", "values": {"en": "line one\nline two, with a comma"}}]})", table, &error)) << error;
        return table;
    }

    struct ScratchDir {
        fs::path root = fs::temp_directory_path() / ("je-stringtables-" + std::to_string((long long)std::hash<std::string>()(
                                                          ::testing::UnitTest::GetInstance()->current_test_info()->name())));
        ScratchDir() { fs::remove_all(root); fs::create_directories(root / "assets"); }
        ~ScratchDir() { std::error_code ec; fs::remove_all(root, ec); }
        void write(const std::string& relative, const std::string& text) {
            std::ofstream out(root / relative, std::ios::binary);
            out << text;
        }
    };

}

TEST(StringTablesTest, CsvRoundTripKeepsPluralsCommentsAndNewlines) {
    const StringTable table = stTable();
    const std::string csv = stringtables::exportCsv(table, {"en", "es"});
    EXPECT_NE(csv.find("key,context,English (en),Spanish (es)"), std::string::npos);
    StringTable back;
    stringtables::ImportReport report;
    std::string error;
    ASSERT_TRUE(stringtables::importText(back, csv, "", "en", {"en", "es"}, report, error)) << error;
    EXPECT_EQ(report.added, 3);
    EXPECT_TRUE(back == table) << back.write();
    // again: nothing changes
    ASSERT_TRUE(stringtables::importText(back, csv, "csv", "en", {}, report, error));
    EXPECT_EQ(report.added + report.changed, 0);
}

TEST(StringTablesTest, ImportRefusesOtherLanguagesAndBadKeys) {
    StringTable table = stTable();
    const std::string csv = "key,English (en),French (fr),German (de)\r\nmenu.play,Play,Jouer,Spielen\r\nbad:key,x,y,z\r\n";
    stringtables::ImportReport report;
    std::string error;
    ASSERT_TRUE(stringtables::importText(table, csv, "", "en", {"en", "fr"}, report, error)) << error;
    EXPECT_EQ(table.find("menu.play")->find("fr")->text, "Jouer");
    EXPECT_EQ(table.find("menu.play")->find("de"), nullptr) << "German is not a project language";
    EXPECT_EQ(table.find("bad:key"), nullptr);
    EXPECT_EQ(report.changed, 1);
    EXPECT_EQ(report.notes.size(), 2u) << "the left-out column and the bad key are named";
}

TEST(StringTablesTest, XliffRoundTripPerTargetLanguage) {
    const StringTable table = stTable();
    const auto files = stringtables::exportXliff(table, "assets/UI.stringtable", "en", {"en", "es"});
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].first, "es");
    StringTable back;
    stringtables::ImportReport report;
    std::string error;
    ASSERT_TRUE(stringtables::importText(back, files[0].second, "", "en", {}, report, error)) << error;
    EXPECT_EQ(back.find("menu.play")->find("es")->text, "Jugar");
    EXPECT_EQ(back.find("menu.play")->find("en")->text, "Play");
    EXPECT_TRUE(*back.find("hud.coins")->find("es") == *table.find("hud.coins")->find("es"));
}

TEST(StringTablesTest, MissingKeysCountPluralForms) {
    const StringTable table = stTable();
    EXPECT_TRUE(stringtables::missingKeys(table, "en").empty());
    // es lacks multi, and hud.coins lacks `other` (Spanish uses one, many, other)
    EXPECT_EQ(stringtables::missingKeys(table, "es"), std::vector<std::string>({"hud.coins", "multi"}));
    EXPECT_EQ(stringtables::missingKeys(table, "ja").size(), 3u);
}

TEST(StringTablesTest, CheckReportsDuplicateKeysAndBadFiles) {
    ScratchDir dir;
    dir.write("assets/A.stringtable", R"({"version": 1, "entries": [{"key": "same", "values": {"en": "a"}}]})");
    dir.write("assets/B.stringtable", R"({"version": 1, "entries": [{"key": "same", "values": {"en": "b"}}, {"key": "same"}]})");
    dir.write("assets/C.stringtable", "{ not json");
    const std::vector<ProjectChecks::Finding> findings = stringtables::check(dir.root, dir.root / "assets");
    int errors = 0, warnings = 0;
    bool duplicate = false;
    for (const ProjectChecks::Finding& f : findings) {
        errors += f.severity == "error";
        warnings += f.severity == "warning";
        if (f.message.find("'same' is defined in A, B") != std::string::npos) duplicate = true;
        EXPECT_EQ(f.tool, "strings");
    }
    EXPECT_TRUE(duplicate);
    EXPECT_EQ(errors, 2) << "C does not load; same is in two tables";
    EXPECT_EQ(warnings, 1) << "B defines same twice";
}

TEST(StringTablesTest, GameJsonCarriesTheLocalization) {
    player::PlayerConfig config;
    config.defaultLanguage = "es";
    config.localization.languages = {"es", "en"};
    config.localization.rememberLanguage = false;
    config.localization.tables = {"Localization/UI.stringtable"};
    player::PlayerConfig back;
    std::string error;
    ASSERT_TRUE(player::PlayerConfig::fromJson(config.toJson(), back, error)) << error;
    EXPECT_EQ(back.defaultLanguage, "es");
    EXPECT_TRUE(back.localization == config.localization);
    // an older editor's game.json: English, no tables
    nlohmann::json old = config.toJson();
    old.erase("localization");
    ASSERT_TRUE(player::PlayerConfig::fromJson(old, back, error)) << error;
    EXPECT_EQ(back.defaultLanguage, "en");
    EXPECT_TRUE(back.localization.tables.empty());
}
