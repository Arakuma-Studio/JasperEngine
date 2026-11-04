// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one dispatcher (S25 R2, R3, AC2): every binding of the §6.3 table fires exactly its
// command once in its context, and the typing, Code Editor, modal, Play and capture rules stop
// the keys they must.

#include "shortcuts/ShortcutTestSupport.h"
#include "shortcuts/Chord.h"
#include "shortcuts/CommandCatalog.h"
#include "shortcuts/ShortcutDispatcher.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    ShortcutState stateFor(ShortcutContext context) {
        ShortcutState state;
        if (context != ShortcutContext::Global && context != ShortcutContext::Editor) state.panels = {context};
        return state;
    }

}

TEST(ShortcutDispatcher, CharacterizesTodaysBindings) {
    RecordingCatalog recording;
    ShortcutDispatcher dispatcher(recording.catalog);
    size_t checked = 0;
    for (const CommandDef& def : recording.catalog.commands()) {
        if (def.readOnly) continue;
        for (Chord chord : recording.catalog.bindings(def.id)) {
            recording.ran.clear();
            const std::vector<std::string> fired = dispatcher.process(FakeKeys::press(chord), stateFor(def.context));
            EXPECT_EQ(fired, std::vector<std::string>{def.id}) << def.id << " " << chord.format(false);
            EXPECT_EQ(recording.ran, std::vector<std::string>{def.id}) << def.id << " " << chord.format(false);
            checked++;
        }
    }
    // the table binds every key the editor had before the catalog
    EXPECT_GE(checked, 60u);

    // spot checks of the table as §6.3 lists it
    struct Expected {
        const char* chord;
        ShortcutContext context;
        const char* id;
    };
    const Expected expected[] = {
        {"Ctrl+S", ShortcutContext::Editor, "file.save"},
        {"Ctrl+Shift+S", ShortcutContext::Editor, "file.saveAll"},
        {"Ctrl+Z", ShortcutContext::Editor, "edit.undo"},
        {"Ctrl+Y", ShortcutContext::Editor, "edit.redo"},
        {"Ctrl+Shift+Z", ShortcutContext::Editor, "edit.redo"},
        {"Ctrl+D", ShortcutContext::Editor, "edit.duplicate"},
        {"Ctrl+C", ShortcutContext::Hierarchy, "edit.copy"},
        {"Ctrl+C", ShortcutContext::SceneView, "edit.copy"},
        {"Ctrl+C", ShortcutContext::Project, "project.copy"},
        {"Ctrl+C", ShortcutContext::Console, "console.copy"},
        {"Delete", ShortcutContext::Editor, "edit.delete"},
        {"Backspace", ShortcutContext::Hierarchy, "edit.delete"},
        {"Delete", ShortcutContext::Project, "project.delete"},
        {"F2", ShortcutContext::Project, "project.rename"},
        {"F2", ShortcutContext::Editor, "edit.rename"},
        {"F", ShortcutContext::Editor, "edit.frameSelected"},
        {"Q", ShortcutContext::Editor, "tools.hand"},
        {"W", ShortcutContext::Editor, "tools.move"},
        {"E", ShortcutContext::Editor, "tools.rotate"},
        {"R", ShortcutContext::Editor, "tools.scale"},
        {"T", ShortcutContext::Editor, "tools.rect"},
        {"X", ShortcutContext::Editor, "tools.toggleGlobalLocal"},
        {"Ctrl+P", ShortcutContext::Editor, "edit.play"},
        {"F5", ShortcutContext::Editor, "playmode.playOrResume"},
        {"Ctrl+K", ShortcutContext::Editor, "edit.searchAll"},
        {"Home", ShortcutContext::SceneView, "sceneView.frameAll"},
        {"Alt+F", ShortcutContext::SceneView, "sceneView.frameAll"},     // S26
        {"H", ShortcutContext::SceneView, "sceneView.hideSelection"},
        {"Alt+H", ShortcutContext::SceneView, "sceneView.showAll"},
        {"L", ShortcutContext::SceneView, "sceneView.togglePickLock"},
        {"M", ShortcutContext::SceneView, "sceneView.measure"},
        {"Shift+3", ShortcutContext::SceneView, "sceneView.recallBookmark3"},
        {"Ctrl+Shift+3", ShortcutContext::SceneView, "sceneView.storeBookmark3"},
        {"Numpad7", ShortcutContext::SceneView, "sceneView.viewTop"},
        {"Ctrl+Numpad7", ShortcutContext::SceneView, "sceneView.viewBottom"},
        {"[", ShortcutContext::SceneView, "terrain.brushSmaller"},
        {"Shift+]", ShortcutContext::SceneView, "terrain.brushStronger"},
        {"Ctrl+Shift+N", ShortcutContext::Editor, "gameobject.createEmpty"},
        // the Animation window editing a clip and the Pose Animation window: their keys first
        {"K", ShortcutContext::Animation, "animation.addKey"},
        {"Ctrl+D", ShortcutContext::Animation, "animation.duplicate"},
        {"Delete", ShortcutContext::Animation, "animation.delete"},
        {"Alt+.", ShortcutContext::Animation, "animation.nextKey"},
        {"R", ShortcutContext::Animation, "animation.record"},
        {"E", ShortcutContext::PoseAnimation, "pose.globalEdit"},
        {"Ctrl+Shift+D", ShortcutContext::PoseAnimation, "pose.duplicateMirrored"},
        {"Shift+Left", ShortcutContext::PoseAnimation, "pose.moveLeft"},
        {"Ctrl+C", ShortcutContext::PoseAnimation, "pose.copy"},
    };
    for (const Expected& e : expected) {
        const Chord chord = chordOf(e.chord);
        ASSERT_FALSE(chord.empty()) << e.chord;
        const CommandDef* def = dispatcher.resolve(chord, stateFor(e.context));
        ASSERT_NE(def, nullptr) << e.chord;
        EXPECT_EQ(def->id, e.id) << e.chord;
    }
    // a Scene View key needs the Scene View
    EXPECT_EQ(dispatcher.resolve(chordOf("Home"), ShortcutState()), nullptr);
}

TEST(ShortcutDispatcher, TypingCodeEditorModalAndPlayFireNothing) {
    RecordingCatalog recording;
    ShortcutDispatcher dispatcher(recording.catalog);

    ShortcutState typing;
    typing.wantTextInput = true;
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("W")), typing).empty());
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Delete")), typing).empty());
    // an Editor command never fires while typing, a Global one does
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Ctrl+D")), typing).empty());
    EXPECT_EQ(dispatcher.process(FakeKeys::press(chordOf("Ctrl+S")), typing), std::vector<std::string>{"file.save"});

    ShortcutState code;
    code.codeEditorFocused = true;
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Q")), code).empty());
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Ctrl+Z")), code).empty());   // the editor's own undo
    EXPECT_EQ(dispatcher.process(FakeKeys::press(chordOf("Ctrl+S")), code), std::vector<std::string>{"file.save"});

    ShortcutState modal;
    modal.popupOpen = true;
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Ctrl+D")), modal).empty());
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Ctrl+S")), modal).empty());

    ShortcutState play;
    play.gameHasKeyboard = true;
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("T")), play).empty());
    EXPECT_EQ(dispatcher.process(FakeKeys::press(chordOf("Ctrl+P")), play), std::vector<std::string>{"edit.play"});
    EXPECT_EQ(dispatcher.process(FakeKeys::press(chordOf("F7")), play), std::vector<std::string>{"playmode.stop"});

    // modifiers must match exactly: Ctrl+Shift+D is not Ctrl+D
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Ctrl+Shift+D")), ShortcutState()).empty());
}

TEST(ShortcutDispatcher, OnePressFiresOnceAndRepeatsOnlyWhenAsked) {
    RecordingCatalog recording;
    ShortcutDispatcher dispatcher(recording.catalog);
    FakeKeys held;
    held.heldKeys = {ImGuiKey_D};
    held.modifiers = ImGuiMod_Ctrl;
    held.repeating = true;
    // Duplicate does not repeat; Undo does
    EXPECT_TRUE(dispatcher.process(held, ShortcutState()).empty());
    held.heldKeys = {ImGuiKey_Z};
    EXPECT_EQ(dispatcher.process(held, ShortcutState()), std::vector<std::string>{"edit.undo"});
}

TEST(ShortcutDispatcher, NativeMenuOwnsItsKeyEquivalents) {
    RecordingCatalog recording(true);
    ShortcutDispatcher dispatcher(recording.catalog);
    recording.catalog.setMenuItem("file.save", "File > Save", chordOf("Ctrl+S"));
    ShortcutState state;
    state.nativeMenuOwnsKeys = true;
    // NSMenu runs Cmd+S: the dispatcher does not, so it saves once
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Ctrl+S")), state).empty());
    // a key with no menu item still fires
    EXPECT_EQ(dispatcher.process(FakeKeys::press(chordOf("W")), state), std::vector<std::string>{"tools.move"});
    // Redo's macOS chord is Cmd+Shift+Z
    EXPECT_EQ(recording.catalog.menuShortcutText("edit.redo"), "Cmd+Shift+Z");
    state.nativeMenuOwnsKeys = false;
    EXPECT_EQ(dispatcher.process(FakeKeys::press(chordOf("Ctrl+S")), state), std::vector<std::string>{"file.save"});
}

TEST(ShortcutDispatcher, CaptureSwallowsTheKeyUntilReleased) {
    RecordingCatalog recording;
    ShortcutDispatcher dispatcher(recording.catalog);
    dispatcher.setCaptureActive(true);
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Ctrl+S")), ShortcutState()).empty());
    dispatcher.noteCaptureConsumedKey(ImGuiKey_S);
    dispatcher.setCaptureActive(false);
    // the captured key is still down on the next frame (and repeats): nothing fires
    FakeKeys held;
    held.heldKeys = {ImGuiKey_S};
    held.modifiers = ImGuiMod_Ctrl;
    held.repeating = true;
    EXPECT_TRUE(dispatcher.process(held, ShortcutState()).empty());
    held.repeating = false;
    EXPECT_TRUE(dispatcher.process(held, ShortcutState()).empty());
    // released, then pressed again: it fires
    EXPECT_TRUE(dispatcher.process(FakeKeys(), ShortcutState()).empty());
    EXPECT_EQ(dispatcher.process(FakeKeys::press(chordOf("Ctrl+S")), ShortcutState()), std::vector<std::string>{"file.save"});
}

TEST(ShortcutDispatcher, GatesAndDisabledCommandsDoNotFire) {
    RecordingCatalog recording;
    CommandCatalog& catalog = recording.catalog;
    CommandDef save = *catalog.find("file.save");
    save.enabled = [] { return false; };
    catalog.add(save);
    ShortcutDispatcher dispatcher(catalog);
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("Ctrl+S")), ShortcutState()).empty());
    CommandDef move = *catalog.find("tools.move");
    move.keyGate = [] { return false; };
    catalog.add(move);
    EXPECT_TRUE(dispatcher.process(FakeKeys::press(chordOf("W")), ShortcutState()).empty());
}
