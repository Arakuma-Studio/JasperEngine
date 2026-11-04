// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The user's rebindings file (S25 R7, R8; AC5, AC6): round trip, what a newer editor wrote
// survives, [] unbinds, and a file that makes two commands share a chord resolves to the default.

#include "shortcuts/ShortcutTestSupport.h"
#include "shortcuts/ShortcutDispatcher.h"
#include "shortcuts/ShortcutOverrides.h"

#include "LogCapture.h"
#include "TempDir.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

TEST(ShortcutOverrides, RoundTripKeepsUnknown) {
    jasperengine::test::TempDir dir;
    const auto file = dir.write("shortcuts.yaml",
                                "shortcutsVersion: 1\n"
                                "overrides:\n"
                                "  tools.rect: [\"Y\"]\n"
                                "  edit.redo: [\"Ctrl+Y\", \"Ctrl+Shift+Z\", \"Ctrl+Alt+Z\"]\n"
                                "  window.console: []\n"
                                "  future.command: [\"Ctrl+Alt+F\"]\n"
                                "  tools.move: [\"Hyper+W\"]\n");
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    jasperengine::test::LogCapture log;
    ShortcutOverrides::load(file, catalog);
    // one warning names both entries it cannot use
    EXPECT_EQ(log.count(D_LOG_WARN, "future.command"), 1u);
    EXPECT_EQ(log.count(D_LOG_WARN, "Hyper+W"), 1u);
    EXPECT_EQ(log.warnings().size(), 1u);

    ASSERT_EQ(catalog.bindings("tools.rect").size(), 1u);
    EXPECT_EQ(catalog.bindings("tools.rect")[0], chordOf("Y"));
    EXPECT_EQ(catalog.bindings("edit.redo").size(), 3u);
    EXPECT_TRUE(catalog.bindings("window.console").empty());          // [] unbinds
    EXPECT_EQ(catalog.bindings("tools.move").size(), 1u);              // a bad chord: the defaults
    EXPECT_EQ(catalog.bindings("tools.move")[0], chordOf("W"));

    // saved and loaded again: the same, the unknown id and the bad chord included
    ASSERT_TRUE(catalog.setBindings("tools.scale", {chordOf("Shift+R")}));
    std::string error;
    ASSERT_TRUE(ShortcutOverrides::save(file, catalog, &error)) << error;
    std::map<std::string, std::vector<std::string>> saved;
    ASSERT_TRUE(ShortcutOverrides::parse(dir.read("shortcuts.yaml"), saved));
    EXPECT_EQ(saved["future.command"], std::vector<std::string>{"Ctrl+Alt+F"});
    EXPECT_EQ(saved["tools.move"], std::vector<std::string>{"Hyper+W"});
    EXPECT_TRUE(saved["window.console"].empty());
    EXPECT_EQ(saved["tools.scale"], std::vector<std::string>{"Shift+R"});

    RecordingCatalog again;
    ShortcutOverrides::load(file, again.catalog);
    for (const CommandDef& def : catalog.commands()) {
        const auto a = catalog.bindings(def.id);
        const auto b = again.catalog.bindings(def.id);
        EXPECT_TRUE(std::equal(a.begin(), a.end(), b.begin(), b.end())) << def.id;
    }
    // resetting a command removes it from the file
    again.catalog.resetBindings("window.console");
    EXPECT_EQ(ShortcutOverrides::serialize(again.catalog).find("window.console"), std::string::npos);
    EXPECT_EQ(again.catalog.bindings("window.console")[0], chordOf("Ctrl+Shift+C"));
}

TEST(ShortcutOverrides, ConflictingFileResolvesToDefault) {
    jasperengine::test::TempDir dir;
    // Rect takes Duplicate's default, and Move takes Rect's default T
    const auto file = dir.write("shortcuts.yaml", "shortcutsVersion: 1\noverrides: {tools.rect: [\"Ctrl+D\"], tools.move: [\"T\"]}\n");
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    jasperengine::test::LogCapture log;
    ShortcutOverrides::load(file, catalog);
    // Duplicate holds Ctrl+D by default: it keeps it, Rect loses it
    EXPECT_EQ(catalog.bindings("edit.duplicate")[0], chordOf("Ctrl+D"));
    EXPECT_TRUE(catalog.bindings("tools.rect").empty());
    EXPECT_EQ(catalog.bindings("tools.move")[0], chordOf("T"));
    // exactly one warning, naming the commands and where to fix them
    ASSERT_EQ(log.warnings().size(), 1u);
    const std::string message = log.warnings()[0].message;
    EXPECT_NE(message.find("Duplicate"), std::string::npos) << message;
    EXPECT_NE(message.find("Rect Tool"), std::string::npos) << message;
    EXPECT_NE(message.find("Edit > Shortcuts..."), std::string::npos) << message;
    // the file keeps what the user wrote
    EXPECT_NE(ShortcutOverrides::serialize(catalog).find("Ctrl+D"), std::string::npos);
    // the dispatcher runs Duplicate for Ctrl+D
    ShortcutDispatcher dispatcher(catalog);
    EXPECT_EQ(dispatcher.process(FakeKeys::press(chordOf("Ctrl+D")), ShortcutState()), std::vector<std::string>{"edit.duplicate"});
}

TEST(ShortcutOverrides, UnreadableFileWarnsAndUsesDefaults) {
    jasperengine::test::TempDir dir;
    const auto file = dir.write("shortcuts.yaml", "overrides: [this is: not, a map\n");
    RecordingCatalog recording;
    jasperengine::test::LogCapture log;
    ShortcutOverrides::load(file, recording.catalog);
    EXPECT_EQ(log.warnings().size(), 1u);
    EXPECT_EQ(recording.catalog.bindings("tools.rect")[0], chordOf("T"));
    // a missing file is no overrides and no warning
    jasperengine::test::LogCapture quiet;
    ShortcutOverrides::load(dir.path() / "missing.yaml", recording.catalog);
    EXPECT_TRUE(quiet.warnings().empty());
}
