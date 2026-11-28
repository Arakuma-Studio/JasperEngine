// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The export's cooked String Tables (docs/specs/game-ui-localization.md R6, R21, R27, D5, AC10,
// AC14): one .strbin per namespace and shipped language plus the manifest, the lookups of the
// cooked files matching the JSON, the export warnings, and nothing written without tables.

#include "Project.h"
#include "localization/Localization.h"
#include "localization/StringTableCook.h"
#include "localization/StringTableDocument.h"
#include "localization/StringTableEditCmd.h"
#include "localization/StringTableRuntime.h"
#include "util/ProjectChecks.h"
#include "util/TitleArtLanguage.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace StringTableCookTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace st = jasperengine::editor::l10n;

namespace {

    bool hasWarning(const st::CookResult& result, const std::string& code) {
        return std::any_of(result.warnings.begin(), result.warnings.end(), [&](const std::string& w) { return w.rfind(code, 0) == 0; });
    }

    void makeProject(const jasperengine::test::TempDir& dir) {
        st::StringTableSet set;
        std::string error;
        ASSERT_TRUE(st::createTable(dir.path(), dir.path() / "assets" / "l10n" / "ui.stringtable", "", "en", set, error)) << error;
        ASSERT_TRUE(st::applyOps(set, st::Json::parse(R"([
            {"op": "set", "key": "menu.play", "text": "Play"},
            {"op": "set", "key": "menu.quit", "text": "Quit"},
            {"op": "set", "key": "shop.coins", "text": "{n, plural, one {# coin} other {# coins}}"},
            {"op": "add_language", "language": "de"},
            {"op": "add_language", "language": "fr"},
            {"op": "set", "key": "menu.play", "language": "de", "text": "Spielen"},
            {"op": "set", "key": "shop.coins", "language": "de", "text": "{n, plural, one {# Münze} other {# Münzen}}"},
            {"op": "set", "key": "menu.play", "language": "fr", "text": "Jouer"}])"), error)) << error;
        ASSERT_TRUE(st::commitSet(nullptr, nullptr, set, "Create", &error)) << error;
        dir.write("assets/ui/menu.html", "<button data-l10n-id=\"ui.menu.play\">Play</button><p data-l10n-id=\"ui.menu.gone\">x</p>\n");
    }

}

TEST(StringTableCook, CookedLookupMatchesJson) {
    jasperengine::test::TempDir dir;
    makeProject(dir);
    LocalizationSettings settings;
    const fs::path out = dir.path() / "export" / "assets";
    const st::CookResult result = st::cookLocalization(dir.path(), settings, out);
    ASSERT_TRUE(result.ok) << result.error;
    for (const char* file : {"l10n/manifest.json", "l10n/ui.en.strbin", "l10n/ui.de.strbin", "l10n/ui.fr.strbin"}) {
        EXPECT_TRUE(fs::exists(out / file)) << file;
    }
    EXPECT_TRUE(hasWarning(result, "W-L10N-UNTRANSLATED")) << "fr lacks two entries, de one";
    EXPECT_TRUE(hasWarning(result, "W-L10N-UNKNOWN-KEY")) << "ui.menu.gone";
    EXPECT_FALSE(hasWarning(result, "W-L10N-LANGUAGE-DROPPED"));

    // the game's lookups on the cooked files equal the editor's on the JSON, key by key
    std::string text;
    ASSERT_TRUE(ProjectChecks::readFile(out / "l10n" / "manifest.json", text));
    LocalizationManifest cooked;
    ASSERT_TRUE(LocalizationManifest::fromJson(text, cooked));
    for (auto& ns : cooked.namespaces) for (auto& [code, path] : ns.files) path = (out / path).generic_string();
    const LocalizationManifest json = st::editorManifest(settings, st::loadProjectTables(dir.path()));
    EXPECT_EQ(cooked.languages, json.languages);
    for (const char* language : {"en", "de", "fr"}) {
        for (const char* key : {"ui.menu.play", "ui.menu.quit", "ui.shop.coins"}) {
            TitleArtLanguage::resetForTesting();
            Localization::resetForTesting();
            Localization::setManifest(cooked);
            Localization::setLanguage(language);
            const std::string fromCooked = Localization::get(key, {{"n", int64_t(2)}});
            Localization::setManifest(json);
            Localization::setLanguage(language);
            EXPECT_EQ(fromCooked, Localization::get(key, {{"n", int64_t(2)}})) << language << " " << key;
        }
    }
    Localization::resetForTesting();
    TitleArtLanguage::resetForTesting();

    // a non-empty language list leaves the others out
    settings.languages = {"en", "de"};
    fs::remove_all(out);
    const st::CookResult narrowed = st::cookLocalization(dir.path(), settings, out);
    ASSERT_TRUE(narrowed.ok) << narrowed.error;
    EXPECT_FALSE(fs::exists(out / "l10n" / "ui.fr.strbin"));
    EXPECT_TRUE(hasWarning(narrowed, "W-L10N-LANGUAGE-DROPPED"));
}

TEST(StringTableCook, HashCollisionIsAnExportError) {
    // two keys of one table with one hash: an export error naming both (forced: one key twice)
    jasperengine::test::TempDir dir;
    dir.write("assets/l10n/ui.stringtable",
        R"({"format": "jasperengine.stringtable", "version": 1, "guid": "1", "namespace": "ui", "source_language": "en",)"
        R"( "entries": [{"key": "a", "text": "A"}, {"key": "a", "text": "B"}]})");
    const st::CookResult result = st::cookLocalization(dir.path(), LocalizationSettings(), dir.path() / "out");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("\"a\" and \"a\""), std::string::npos) << result.error;
}

TEST(StringTableCook, ProjectWithoutTablesWritesNothing) {
    jasperengine::test::TempDir dir;
    dir.write("assets/ui/menu.html", "<p>Hello</p>\n");
    const st::CookResult result = st::cookLocalization(dir.path(), LocalizationSettings(), dir.path() / "out");
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.written.empty());
    EXPECT_FALSE(fs::exists(dir.path() / "out" / "l10n"));
}

// D-15: the manifest's Default Table is the setting, else the project's only String Table (a
// dialogue's texts are a namespace too, and do not count), else none
TEST(StringTableCook, DefaultTableIsTheSettingElseTheOnlyStringTable) {
    st::ProjectTables tables;
    auto add = [&](const std::string& owner, const std::string& ns) {
        st::StringTableSet set;
        set.owner = fs::path("assets") / owner;
        set.table.ns = ns;
        set.table.sourceLanguage = "en";
        set.dialogue = fs::path(owner).extension() == ".dialogue";
        tables.sets.push_back(set);
    };
    LocalizationSettings settings;
    add("ui.stringtable", "ui");
    add("guard.dialogue", "guard");
    EXPECT_EQ(st::defaultTableOf(settings, tables), "ui");
    EXPECT_EQ(st::editorManifest(settings, tables).defaultTable, "ui");
    add("hud.stringtable", "hud");
    EXPECT_EQ(st::defaultTableOf(settings, tables), "");
    settings.defaultTable = "hud";
    EXPECT_EQ(st::editorManifest(settings, tables).defaultTable, "hud");
}

// R21a, D-15 (from S95): `check`'s tool strings finds what the export would warn about, plus the
// errors a game cannot run right with (a key twice, a bad message, a Default Table naming nothing),
// and keys Lua literals name; a bare key resolves through the Default Table as the game does
TEST(StringTableCook, CheckStringsReportsTablesAndKeys) {
    jasperengine::test::TempDir dir;
    makeProject(dir);
    dir.write("scripts/menu.lua", "local a = Localization.get(\"menu.play\")\nlocal b = Localization.has('ui.shop.nope')\n"
                                  "label:setLocalized(\"menu.missing\")\nlocal c = Localization.get(\"ui.menu.quit\")\n");
    auto codes = [](const st::StringsCheck& check, const std::string& code) {
        std::vector<std::string> out;
        for (const ProjectChecks::Finding& f : check.findings) if (f.code == code) out.push_back(f.message);
        return out;
    };
    LocalizationSettings settings;
    st::StringsCheck check = st::checkStrings(dir.path(), settings);
    EXPECT_EQ(check.tables, 1u);
    const std::vector<std::string> unknown = codes(check, "W-L10N-UNKNOWN-KEY");
    // ui.menu.gone (HTML), ui.shop.nope and menu.missing (Lua); menu.play reads the only table
    EXPECT_EQ(unknown.size(), 3u) << ::testing::PrintToString(unknown);
    EXPECT_TRUE(codes(check, "L10N-DUPLICATE-KEY").empty());

    settings.defaultTable = "nope";
    check = st::checkStrings(dir.path(), settings);
    EXPECT_EQ(codes(check, "L10N-DEFAULT-TABLE").size(), 1u);

    // a key twice and a message that does not parse, written by hand
    std::string text;
    ASSERT_TRUE(ProjectChecks::readFile(dir.path() / "assets" / "l10n" / "ui.stringtable", text));
    const size_t at = text.find("\"entries\": [");
    ASSERT_NE(at, std::string::npos);
    text.insert(at + 12, "{\"key\": \"menu.play\", \"text\": \"Again\"}, {\"key\": \"bad\", \"text\": \"{n, plural, one {x}\"}, ");
    dir.write("assets/l10n/ui.stringtable", text);
    check = st::checkStrings(dir.path(), LocalizationSettings());
    EXPECT_EQ(codes(check, "L10N-DUPLICATE-KEY").size(), 1u);
    EXPECT_EQ(codes(check, "L10N-BAD-MESSAGE").size(), 1u);
    for (const ProjectChecks::Finding& f : check.findings) EXPECT_EQ(f.tool, "strings");
}

}  // namespace StringTableCookTest_cpp
