// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A focused panel's claim on the edit commands (docs/specs/S61-window-commands.md R2, R3, R5,
// R6, AC3), through the real catalog table and the real dispatcher: the key, the menu and the
// palette reach the panel and nothing else, and a panel's own command really moves when rebound.

#include "shortcuts/PanelClaims.h"
#include "shortcuts/ShortcutOverrides.h"
#include "shortcuts/ShortcutTestSupport.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    // The catalog with the claims wrapped over it, a panel that has its own commands (the Curve
    // window) and one whose handler is a function (the Input Actions window)
    struct Claims {
        RecordingCatalog recording;
        PanelClaims claims{recording.catalog};
        ShortcutDispatcher dispatcher{recording.catalog};
        std::string focused;
        bool typing = false;
        bool canDelete = true;
        int handled = 0;

        Claims() {
            ClaimHandler frame, del, copy, paste;
            frame.command = "curve.frame";
            del.command = "curve.delete";
            copy.command = "curve.copy";
            paste.command = "curve.paste";
            claims.add({"Curve", [this]() { return focused == "Curve"; },
                        {{EditVerb::Frame, frame}, {EditVerb::Delete, del}, {EditVerb::Copy, copy}, {EditVerb::Paste, paste}}});
            ClaimHandler handler;
            handler.enabled = [this]() { return canDelete; };
            handler.run = [this]() { handled++; };
            claims.add({"Input Actions", [this]() { return focused == "Input Actions"; }, {{EditVerb::Delete, handler}}});
            claims.add({"Jobs & Memory", [this]() { return focused == "Jobs & Memory"; }, {}});
            claims.setTyping([this]() { return typing; });
            claims.bindEditCommands();
        }

        void focus(const std::string& panel) {
            focused = panel;
            claims.refresh();
        }

        ShortcutState state(bool sceneHovered = false) const {
            ShortcutState out;
            out.wantTextInput = typing;
            if (focused == "Curve") out.panels.push_back(ShortcutContext::Curve);
            if (sceneHovered) out.panels.push_back(ShortcutContext::SceneView);
            return out;
        }

        // what a press runs, in order
        std::vector<std::string> press(const char* chord, bool sceneHovered = false) {
            recording.ran.clear();
            dispatcher.process(FakeKeys::press(chordOf(chord)), state(sceneHovered));
            return recording.ran;
        }

        // the command as the Edit menu and the palette reach it
        bool enabled(const char* id) const {
            const CommandDef* def = recording.catalog.find(id);
            return !def->enabled || def->enabled();
        }
        std::vector<std::string> menu(const char* id) {
            recording.ran.clear();
            recording.catalog.find(id)->run(CommandArgs{CommandSource::Menu, {}});
            return recording.ran;
        }
    };

    using Ran = std::vector<std::string>;

}

TEST(PanelClaims, WithoutAClaimTheEditCommandsActAsBefore) {
    Claims c;
    c.focus("");
    EXPECT_EQ(c.claims.active(), nullptr);
    EXPECT_EQ(c.press("Delete"), Ran{"edit.delete"});
    EXPECT_EQ(c.press("F"), Ran{"edit.frameSelected"});
    EXPECT_EQ(c.press("Ctrl+D"), Ran{"edit.duplicate"});
    EXPECT_EQ(c.press("Ctrl+C", true), Ran{"edit.copy"});
    EXPECT_EQ(c.menu("edit.delete"), Ran{"edit.delete"});
}

TEST(PanelClaims, KeysGoToThePanelsOwnCommandsAndNowhereElse) {
    Claims c;
    c.focus("Curve");
    EXPECT_EQ(c.press("Delete"), Ran{"curve.delete"});
    EXPECT_EQ(c.press("Backspace"), Ran{"curve.delete"});
    EXPECT_EQ(c.press("F"), Ran{"curve.frame"});
    EXPECT_EQ(c.press("Ctrl+C"), Ran{"curve.copy"});
    EXPECT_EQ(c.press("Ctrl+V"), Ran{"curve.paste"});
    // the Scene view's keys are active under the pointer: its edit keys stay the panel's
    EXPECT_EQ(c.press("Ctrl+C", true), Ran{"curve.copy"});
    EXPECT_EQ(c.press("Ctrl+V", true), Ran{"curve.paste"});
    EXPECT_EQ(c.press("Ctrl+X", true), Ran{}) << "a verb the panel does not have is off";
    EXPECT_EQ(c.press("Ctrl+D"), Ran{});
}

TEST(PanelClaims, TheMenuAndThePaletteGoToThePanelToo) {
    Claims c;
    c.focus("Curve");
    EXPECT_EQ(c.menu("edit.delete"), Ran{"curve.delete"});
    EXPECT_EQ(c.menu("edit.frameSelected"), Ran{"curve.frame"});
    EXPECT_EQ(c.menu("edit.duplicate"), Ran{}) << "disabled: it does not reach the scene";
    EXPECT_FALSE(c.enabled("edit.duplicate"));
    EXPECT_FALSE(c.enabled("edit.cut"));
    EXPECT_FALSE(c.enabled("edit.pasteAsChild"));
    EXPECT_TRUE(c.enabled("edit.delete"));
    // App's Edit menu clicks ask route() before they run the menu command
    c.recording.ran.clear();
    EXPECT_TRUE(c.claims.route("edit.copy"));
    EXPECT_EQ(c.recording.ran, Ran{"curve.copy"});
    EXPECT_FALSE(c.claims.route("file.save")) << "not an edit command";
    c.focus("");
    EXPECT_FALSE(c.claims.route("edit.copy")) << "no claim: the caller runs it";
}

TEST(PanelClaims, AHandlerDecidesAndNothingFallsThrough) {
    Claims c;
    c.focus("Input Actions");
    // Delete's own key reaches a panel without its own command
    EXPECT_EQ(c.press("Delete"), Ran{});
    EXPECT_EQ(c.handled, 1);
    EXPECT_TRUE(c.enabled("edit.delete"));
    c.canDelete = false;
    EXPECT_FALSE(c.enabled("edit.delete")) << "the menu item follows the handler";
    EXPECT_EQ(c.press("Delete"), Ran{}) << "nothing selected: it does nothing";
    EXPECT_EQ(c.handled, 1);
    EXPECT_EQ(c.press("F"), Ran{}) << "a verb it has no handler for";
    c.focus("Jobs & Memory");
    EXPECT_EQ(c.press("Delete"), Ran{});
    EXPECT_FALSE(c.enabled("edit.delete"));
    EXPECT_FALSE(c.enabled("edit.frameSelected"));
}

TEST(PanelClaims, TextFieldsKeepTheirKeys) {
    Claims c;
    c.focus("Curve");
    c.typing = true;
    EXPECT_FALSE(c.enabled("edit.delete")) << "the native menu's item would steal the key";
    EXPECT_EQ(c.press("Delete"), Ran{});
    EXPECT_EQ(c.press("F"), Ran{});
    // the palette's field has the keyboard as it runs its entry: that entry still runs
    EXPECT_EQ(c.menu("edit.frameSelected"), Ran{"curve.frame"});
    c.focus("Input Actions");
    EXPECT_EQ(c.press("Delete"), Ran{});
    EXPECT_EQ(c.handled, 0);
}

TEST(PanelClaims, TheFocusIsLatchedWhileAPopupIsOpen) {
    Claims c;
    c.focus("Curve");
    // a menu takes the ImGui focus; App does not refresh while a popup is open
    c.focused.clear();
    EXPECT_EQ(c.menu("edit.delete"), Ran{"curve.delete"});
    c.claims.refresh();
    EXPECT_EQ(c.menu("edit.delete"), Ran{"edit.delete"});
}

TEST(PanelClaims, MenuShowsThePanelsBinding) {
    Claims c;
    EXPECT_EQ(c.claims.shownCommand("edit.frameSelected"), "edit.frameSelected");
    c.focus("Curve");
    EXPECT_EQ(c.claims.shownCommand("edit.frameSelected"), "curve.frame");
    EXPECT_EQ(c.claims.shownCommand("edit.duplicate"), "edit.duplicate");
    EXPECT_EQ(c.claims.shownCommand("file.save"), "file.save");
    c.focus("Input Actions");
    EXPECT_EQ(c.claims.shownCommand("edit.delete"), "edit.delete");
}

TEST(PanelClaims, NativeMenuRunsWhatItShowsOnce) {
    Claims c;
    c.focus("Curve");
    ShortcutState state = c.state();
    state.nativeMenuOwnsKeys = true;
    state.nativeMenuChords = c.claims.nativeMenuChords();
    // NSMenu runs Edit > Delete on Delete: the dispatcher leaves that chord to it; Backspace is
    // no item's key equivalent and stays the dispatcher's
    c.recording.ran.clear();
    c.dispatcher.process(FakeKeys::press(chordOf("Delete")), state);
    EXPECT_EQ(c.recording.ran, Ran{});
    c.dispatcher.process(FakeKeys::press(chordOf("Backspace")), state);
    EXPECT_EQ(c.recording.ran, Ran{"curve.delete"});
}

TEST(PanelClaims, RebindingThePanelsCommandMovesItsKey) {
    Claims c;
    c.focus("Curve");
    std::string reason;
    ASSERT_TRUE(c.recording.catalog.setBindings("curve.frame", {chordOf("J")}, &reason)) << reason;
    EXPECT_EQ(c.press("F"), Ran{}) << "the old key no longer frames: not the curve, not the scene";
    EXPECT_EQ(c.press("J"), Ran{"curve.frame"});
    // the Edit menu says the panel's key
    EXPECT_EQ(c.recording.catalog.menuShortcutText(c.claims.shownCommand("edit.frameSelected")), "J");
    // the scene keeps F once the panel has lost the focus
    c.focus("");
    EXPECT_EQ(c.press("F"), Ran{"edit.frameSelected"});

    // the settings path: a saved file reloads it
    const std::filesystem::path file = std::filesystem::path(::testing::TempDir()) / "s61-shortcuts.yaml";
    std::string error;
    ASSERT_TRUE(ShortcutOverrides::save(file, c.recording.catalog, &error)) << error;

    Claims reloaded;
    ShortcutOverrides::load(file, reloaded.recording.catalog);
    std::filesystem::remove(file);
    reloaded.focus("Curve");
    EXPECT_EQ(reloaded.press("F"), Ran{});
    EXPECT_EQ(reloaded.press("J"), Ran{"curve.frame"});
}

TEST(PanelClaims, CatalogListsTheNewCommandsWithTheOldDefaults) {
    RecordingCatalog recording;
    const std::pair<const char*, const char*> expected[] = {
        {"curve.frame", "F"}, {"curve.delete", "Delete / Backspace"}, {"curve.copy", "Ctrl+C"},
        {"curve.paste", "Ctrl+V"}, {"colorPicker.eyedropper", "I"}};
    for (const auto& [id, keys] : expected) {
        ASSERT_NE(recording.catalog.find(id), nullptr) << id;
        EXPECT_EQ(recording.catalog.bindingsText(id), keys) << id;
    }
    ShortcutDispatcher dispatcher(recording.catalog);
    ShortcutState state;
    state.panels = {ShortcutContext::ColorPicker};
    const CommandDef* eyedropper = dispatcher.resolve(chordOf("I"), state);
    ASSERT_NE(eyedropper, nullptr);
    EXPECT_EQ(eyedropper->id, "colorPicker.eyedropper");
    state.panels.clear();
    EXPECT_EQ(dispatcher.resolve(chordOf("I"), state), nullptr) << "only while the window has focus";
    EXPECT_EQ(parseShortcutContext("color_picker"), ShortcutContext::ColorPicker);
    EXPECT_EQ(parseShortcutContext("Curve"), ShortcutContext::Curve);
    EXPECT_EQ(parseShortcutContext("sound_browser"), ShortcutContext::SoundBrowser);
}
