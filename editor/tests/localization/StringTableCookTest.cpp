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
