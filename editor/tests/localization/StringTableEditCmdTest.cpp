// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// String Table edits as undo steps (docs/specs/game-ui-localization.md R28, R41, AC6): one step per
// edit over the table and its translations, a rename that moves the translations and lists the
// references it did not rewrite, and an owner moved with its translations.

#include "asset/AssetOps.h"
#include "localization/StringTableDocument.h"
#include "localization/StringTableEditCmd.h"
#include "util/ProjectChecks.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace st = jasperengine::editor::l10n;

namespace {

    std::string fileText(const fs::path& path) {
        std::string text;
        ProjectChecks::readFile(path, text);
        return text;
    }

    // a table with a de translation, written
    st::StringTableSet makeTable(const jasperengine::test::TempDir& dir) {
        st::StringTableSet set;
        std::string error;
        EXPECT_TRUE(st::createTable(dir.path(), dir.path() / "assets" / "l10n" / "ui.stringtable", "", "en", set, error)) << error;
        EXPECT_TRUE(st::applyOps(set, st::Json::parse(R"([{"op": "set", "key": "menu.play", "text": "Play"},
            {"op": "add_language", "language": "de"}, {"op": "set", "key": "menu.play", "language": "de", "text": "Spielen"}])"), error)) << error;
        EXPECT_TRUE(st::commitSet(nullptr, nullptr, set, "Create String Table ui", &error)) << error;
        return set;
    }

}

TEST(StringTableEditCmd, EachWindowEditIsOneUndoStep) {
    jasperengine::test::TempDir dir;
    const st::StringTableSet created = makeTable(dir);
    const fs::path de = st::translationPathFor(created.owner, "de");
    ASSERT_TRUE(fs::exists(de));
    const std::string tableBefore = fileText(created.owner);
    const std::string deBefore = fileText(de);

    // a source edit changes both files (the translation turns needs_review): one command
    st::StringTableSet edited = created;
    std::string error;
    ASSERT_TRUE(st::applyOps(edited, st::Json::parse(R"([{"op": "set", "key": "menu.play", "text": "Start"}])"), error)) << error;
    std::vector<StringTableEditCmd::FileChange> changes;
    for (const auto& [path, text] : edited.files()) changes.push_back({path, fileText(path), text});
    StringTableEditCmd command(changes, "Edit String Table ui");
    ASSERT_TRUE(command.execute());
    EXPECT_NE(fileText(created.owner), tableBefore);
    EXPECT_NE(fileText(de).find("needs_review"), std::string::npos);
    command.undo();
    EXPECT_EQ(fileText(created.owner), tableBefore);
    EXPECT_EQ(fileText(de), deBefore);

    // a typing session folds into the first step: its befores, the last afters
    StringTableEditCmd first({{created.owner, tableBefore, std::string("A")}}, "Edit Cell", "cell:ui:menu.play:en");
    StringTableEditCmd second({{created.owner, std::string("A"), std::string("AB")}}, "Edit Cell", "cell:ui:menu.play:en");
    ASSERT_TRUE(first.execute());
    ASSERT_TRUE(second.execute());
    ASSERT_TRUE(first.mergeWith(&second));
    first.undo();
    EXPECT_EQ(fileText(created.owner), tableBefore);
    StringTableEditCmd other({{created.owner, tableBefore, std::string("C")}}, "Edit Cell", "cell:ui:menu.quit:en");
    EXPECT_FALSE(first.mergeWith(&other));
}

TEST(StringTableEditCmd, RenameMovesTranslationsAndListsReferences) {
    jasperengine::test::TempDir dir;
    const st::StringTableSet created = makeTable(dir);
    dir.write("assets/ui/menu.html", "<button data-l10n-id=\"ui.menu.play\">Play</button>\n");
    dir.write("assets/scripts/menu.lua", "local t = Localization.get(\"ui.menu.play\")\n");
    st::StringTableSet renamed = created;
    std::string error;
    ASSERT_TRUE(st::applyOps(renamed, st::Json::parse(R"([{"op": "rename", "key": "menu.play", "to": "menu.start"}])"), error)) << error;
    EXPECT_NE(renamed.table.find("menu.start"), nullptr);
    EXPECT_EQ(renamed.translations.at("de").entries.count("menu.start"), 1u);
    EXPECT_EQ(renamed.translations.at("de").entries.count("menu.play"), 0u);
    const std::vector<st::KeyReference> references = st::findReferences(dir.path(), "ui.menu.play");
    ASSERT_EQ(references.size(), 2u);
    EXPECT_EQ(references[0].line, 1);
    EXPECT_TRUE((references[0].kind == "html" && references[1].kind == "script") || (references[0].kind == "script" && references[1].kind == "html"));
}

TEST(StringTableEditCmd, MovingTheOwnerMovesItsTranslations) {
    jasperengine::test::TempDir dir;
    const st::StringTableSet created = makeTable(dir);
    const fs::path to = dir.path() / "assets" / "text" / "menu.stringtable";
    const auto pairs = AssetOps::withCompanions(created.owner, to);
    bool movesTranslation = false;
    for (const auto& [from, target] : pairs) {
        if (from == st::translationPathFor(created.owner, "de")) {
            movesTranslation = true;
            EXPECT_EQ(target, st::translationPathFor(to, "de"));
        }
    }
    EXPECT_TRUE(movesTranslation);
}
