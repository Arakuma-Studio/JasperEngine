// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The command catalog (S25 R1, R2, R5, R7; AC1, AC4, AC5): chords, conflicts and shadows, menu
// and tooltip texts, and the generated reference that docs/editor/keyboard-shortcuts.md is.

#include "shortcuts/ShortcutTestSupport.h"
#include "shortcuts/Chord.h"
#include "shortcuts/CommandCatalog.h"

#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <set>
#include <vector>

using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

TEST(Chord, ParsesAndFormats) {
    EXPECT_EQ(chordOf("Ctrl+Shift+S").value, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S);
    EXPECT_EQ(chordOf("cmd+s"), chordOf("Ctrl+S"));
    EXPECT_EQ(chordOf("Option+P"), chordOf("Alt+P"));
    EXPECT_EQ(chordOf("Shift+["), Chord(ImGuiMod_Shift | ImGuiKey_LeftBracket));
    EXPECT_EQ(chordOf("Numpad7"), Chord(ImGuiKey_Keypad7));
    EXPECT_FALSE(Chord::parse("").has_value());
    EXPECT_FALSE(Chord::parse("Ctrl+").has_value());
    EXPECT_FALSE(Chord::parse("Ctrl+Shift").has_value());
    EXPECT_FALSE(Chord::parse("Hyper+K").has_value());
    EXPECT_EQ(chordOf("Ctrl+Shift+Z").format(false), "Ctrl+Shift+Z");
    EXPECT_EQ(chordOf("Ctrl+Shift+Z").format(true), "Cmd+Shift+Z");
    EXPECT_EQ(chordOf("Alt+Shift+N").format(true), "Option+Shift+N");
    for (const char* text : {"F2", "Delete", "Ctrl+Alt+P", "Shift+]", "Ctrl+Numpad1", "Home", "Ctrl+1"}) {
        EXPECT_EQ(chordOf(text).format(false), text);
    }
    // typing chords: what a focused field would take
    EXPECT_TRUE(chordOf("W").isTypingChord());
    EXPECT_TRUE(chordOf("Shift+]").isTypingChord());
    EXPECT_TRUE(chordOf("Delete").isTypingChord());
    EXPECT_FALSE(chordOf("Ctrl+S").isTypingChord());
    EXPECT_FALSE(chordOf("F2").isTypingChord());
}

TEST(CommandCatalog, DefaultTableIsConsistent) {
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    std::set<std::string> ids;
    for (const CommandDef& def : catalog.commands()) {
        EXPECT_TRUE(ids.insert(def.id).second) << "duplicate id " << def.id;
        EXPECT_FALSE(def.group.empty()) << def.id;
        EXPECT_FALSE(def.name.empty()) << def.id;
        if (def.readOnly) {
            EXPECT_FALSE(def.gesture.empty()) << def.id;
        }
    }
    // no two defaults collide (a broad binding over a panel's is a shadow, allowed)
    EXPECT_TRUE(catalog.resolveConflicts().empty());
    for (const CommandDef& def : catalog.commands()) {
        for (Chord chord : catalog.bindings(def.id)) {
            bool shadowed = false;
            const CommandDef* other = catalog.findConflict(def.id, chord, &shadowed);
            EXPECT_TRUE(!other || shadowed) << def.id << " and " << other->id << " share " << chord.format(false);
        }
    }
    // the agent refusals of R39
    for (const char* id : {"file.exit", "file.newProject", "file.openProject", "assets.emptyTrash"}) {
        ASSERT_NE(catalog.find(id), nullptr) << id;
        EXPECT_FALSE(catalog.find(id)->agentSafe) << id;
    }
}

TEST(CommandCatalog, HelpTextsComeFromTheCatalog) {
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    // the T / X / Q fix (R1): T is the Rect Tool, X toggles Global/Local
    EXPECT_EQ(catalog.tooltip("tools.rect", "Rect Tool"), "Rect Tool (T)");
    EXPECT_EQ(catalog.tooltip("tools.toggleGlobalLocal", "Toggle Global/Local"), "Toggle Global/Local (X)");
    EXPECT_EQ(catalog.tooltip("tools.hand", "Hand Tool"), "Hand Tool (Q)");
    EXPECT_EQ(catalog.bindingsText("edit.delete"), "Delete / Backspace");
    EXPECT_EQ(catalog.bindingsText("sceneView.orbit"), "Alt+Left Drag");
    EXPECT_EQ(catalog.menuShortcutText("edit.redo"), "Ctrl+Y");
    catalog.setMac(true);
    EXPECT_EQ(catalog.menuShortcutText("edit.redo"), "Cmd+Shift+Z");
    EXPECT_EQ(catalog.menuShortcutText("file.save"), "Cmd+S");
    catalog.setMac(false);

    const std::string reference = catalog.markdownReference();
    for (const char* text : {"| Q |", "Hand Tool", "| T |", "Rect Tool", "| X |", "Toggle Global/Local"}) {
        EXPECT_NE(reference.find(text), std::string::npos) << text;
    }
    // no line pairs T with global/local
    size_t start = 0;
    while (start < reference.size()) {
        size_t end = reference.find('\n', start);
        if (end == std::string::npos) end = reference.size();
        const std::string line = reference.substr(start, end - start);
        if (line.find("| T |") != std::string::npos) {
            EXPECT_EQ(line.find("Global"), std::string::npos) << line;
        }
        start = end + 1;
    }

    // a rebinding changes the texts
    ASSERT_TRUE(catalog.setBindings("tools.rect", {chordOf("Y")}));
    EXPECT_EQ(catalog.tooltip("tools.rect", "Rect Tool"), "Rect Tool (Y)");
    ASSERT_TRUE(catalog.setBindings("tools.rect", {}));
    EXPECT_EQ(catalog.tooltip("tools.rect", "Rect Tool"), "Rect Tool");
    EXPECT_EQ(catalog.menuShortcutText("tools.rect"), "");
}

TEST(CommandCatalog, MarkdownReferenceMatchesDocs) {
    RecordingCatalog recording;
    const std::string reference = recording.catalog.markdownReference();
    const std::filesystem::path doc = std::filesystem::path(JASPER_TEST_REPO_DIR) / "docs/editor/keyboard-shortcuts.md";
    if (const char* update = std::getenv("JASPER_UPDATE_SHORTCUT_DOCS"); update && std::string(update) == "1") {
        std::ofstream(doc, std::ios::binary) << reference;
    }
    EXPECT_EQ(readText(doc), reference) << "docs/editor/keyboard-shortcuts.md is out of date: rerun with JASPER_UPDATE_SHORTCUT_DOCS=1";
}

TEST(CommandCatalog, DetectsConflictAndShadow) {
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    bool shadowed = true;
    const CommandDef* other = catalog.findConflict("edit.rename", chordOf("Ctrl+D"), &shadowed);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(other->id, "edit.duplicate");
    EXPECT_FALSE(shadowed);
    // Global and Editor are one context for collisions
    other = catalog.findConflict("tools.move", chordOf("Ctrl+S"), &shadowed);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(other->id, "file.save");
    EXPECT_FALSE(shadowed);
    // a Scene View binding under a Global one: shadowed, not a conflict
    other = catalog.findConflict("sceneView.frameAll", chordOf("Ctrl+S"), &shadowed);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(other->id, "file.save");
    EXPECT_TRUE(shadowed);
    // different panels never collide
    EXPECT_EQ(catalog.findConflict("console.copy", chordOf("Ctrl+V"), &shadowed), nullptr);
    EXPECT_EQ(catalog.findConflict("tools.move", chordOf("Ctrl+Alt+Shift+F12"), &shadowed), nullptr);

    // read-only rows and unknown commands cannot be bound
    std::string reason;
    EXPECT_FALSE(catalog.setBindings("sceneView.orbit", {chordOf("O")}, &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_FALSE(catalog.setBindings("no.such", {chordOf("O")}, &reason));
    EXPECT_FALSE(catalog.setBindings("tools.move", {Chord()}, &reason));
    // setting the defaults back stores nothing
    ASSERT_TRUE(catalog.setBindings("tools.move", {chordOf("M")}));
    EXPECT_TRUE(catalog.isOverridden("tools.move"));
    ASSERT_TRUE(catalog.setBindings("tools.move", {chordOf("W")}));
    EXPECT_FALSE(catalog.isOverridden("tools.move"));
}

TEST(CommandCatalog, DynamicCommandsTakeTheirStoredOverride) {
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    EXPECT_TRUE(catalog.loadOverrides({{"script.tools/align.lua", {"Ctrl+Alt+L"}}}).empty());
    CommandDef script;
    script.id = "script.tools/align.lua";
    script.group = "Scripts";
    script.name = "Align Scripted Objects";
    script.dynamic = true;
    catalog.add(script);
    catalog.applyStoredOverride(script.id);
    ASSERT_EQ(catalog.bindings(script.id).size(), 1u);
    EXPECT_EQ(catalog.bindings(script.id)[0], chordOf("Ctrl+Alt+L"));
    catalog.removeDynamic("script.");
    EXPECT_EQ(catalog.find(script.id), nullptr);
    // the override stays stored for the next registration
    EXPECT_EQ(catalog.storedOverrides().count(script.id), 1u);
    // dynamic rows are not in the generated reference
    catalog.add(script);
    EXPECT_EQ(catalog.markdownReference().find("Align Scripted Objects"), std::string::npos);
}

// Ctrl+D duplicates assets in the Project window (one copy per press) and F11 is Maximize On Play
// everywhere (project-window spec D5, D14)
TEST(CommandCatalog, ProjectDuplicateAndMaximizeOnPlayAreBound) {
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    const CommandDef* duplicate = catalog.find("project.duplicate");
    ASSERT_NE(duplicate, nullptr);
    EXPECT_EQ(duplicate->context, ShortcutContext::Project);
    EXPECT_FALSE(duplicate->repeat) << "a held Ctrl+D makes one copy";
    ASSERT_EQ(catalog.bindings("project.duplicate").size(), 1u);
    EXPECT_EQ(catalog.bindings("project.duplicate")[0], chordOf("Ctrl+D"));
    const CommandDef* maximize = catalog.find("playmode.maximizeOnPlay");
    ASSERT_NE(maximize, nullptr);
    EXPECT_EQ(maximize->context, ShortcutContext::Global);
    ASSERT_EQ(catalog.bindings("playmode.maximizeOnPlay").size(), 1u);
    EXPECT_EQ(catalog.bindings("playmode.maximizeOnPlay")[0], chordOf("F11"));
    EXPECT_TRUE(maximize->menuCommand.has_value());
    for (const char* id : {"assets.duplicate", "assets.createCppSource", "assets.createCppHeader", "assets.createPhysicsMaterial",
                           "assets.createPhysicsMaterial2D"}) {
        EXPECT_NE(catalog.find(id), nullptr) << id;
    }
}

// macOS takes F11 for Show Desktop: Maximize On Play is Control+Cmd+F there first, F11 still
// behind it; every other platform keeps F11 alone (project-window spec, Follow-ups)
TEST(CommandCatalog, MaximizeOnPlayIsControlCmdFOnMac) {
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    EXPECT_EQ(chordOf("Ctrl+Super+F").format(true), "Control+Cmd+F");
    EXPECT_EQ(chordOf("Ctrl+Super+F").format(false), "Ctrl+Super+F");
    catalog.setMac(true);
    const std::vector<Chord> mac(catalog.bindings("playmode.maximizeOnPlay").begin(),
                                 catalog.bindings("playmode.maximizeOnPlay").end());
    EXPECT_EQ(mac, (std::vector<Chord>{chordOf("Ctrl+Super+F"), chordOf("F11")}));
    EXPECT_EQ(catalog.menuShortcutText("playmode.maximizeOnPlay"), "Control+Cmd+F");
    for (Chord chord : mac) {
        EXPECT_EQ(catalog.findConflict("playmode.maximizeOnPlay", chord), nullptr) << chord.format(true);
    }
    catalog.setMac(false);
    ASSERT_EQ(catalog.bindings("playmode.maximizeOnPlay").size(), 1u);
    EXPECT_EQ(catalog.bindings("playmode.maximizeOnPlay")[0], chordOf("F11"));
    EXPECT_EQ(catalog.menuShortcutText("playmode.maximizeOnPlay"), "F11");
}

// every chord list_shortcuts and the menus print on a platform reads back as that chord there
// (an agent echoes them to set_shortcut): "Control+Cmd+F" on macOS, "Ctrl+Super+F" elsewhere
TEST(Chord, EveryPrintedChordParsesBack) {
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    EXPECT_EQ(Chord::parse("Control+Cmd+F", true), chordOf("Ctrl+Super+F"));
    EXPECT_EQ(Chord::parse("Control+S", false), chordOf("Ctrl+S"));
    size_t checked = 0;
    for (bool mac : {false, true}) {
        catalog.setMac(mac);
        for (const CommandDef& def : catalog.commands()) {
            std::vector<Chord> chords(catalog.bindings(def.id).begin(), catalog.bindings(def.id).end());
            for (Chord chord : catalog.defaults(def.id)) chords.push_back(chord);
            for (Chord chord : chords) {
                const std::string text = chord.format(mac);
                EXPECT_EQ(Chord::parse(text, mac), std::optional<Chord>(chord)) << def.id << ": " << text << (mac ? " (macOS)" : "");
                checked++;
            }
        }
    }
    catalog.setMac(false);
    EXPECT_GT(checked, 100u);
}
