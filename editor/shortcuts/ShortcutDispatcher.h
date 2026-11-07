// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The one place editor key bindings fire (S25 R3): once per frame it matches the keys pressed
// against the catalog's effective bindings and runs at most one command per press. Precedence:
// the focused window's context first, then Editor, then Global. Typing keys (Chord::
// isTypingChord) never fire while a text field or the Code Editor has the keyboard, or while the
// game has it in Play; Editor commands never fire while typing; nothing fires while a popup
// (a modal, a menu, the Search palette) is open or while the Shortcuts window captures a key.
// While macOS's native menu is active, a menu item's own key equivalent belongs to NSMenu,
// which runs it: the dispatcher leaves that chord to it, so no chord fires twice (R9).
// A focused panel's claim on the edit commands (PanelClaims.h) acts through the commands' own
// enabled state and key gate: nothing here knows the panels.
//
// Keys that stay local are not catalog commands: Esc in dialogs, the Console's arrow keys, the
// text editors' bindings and Play input.

#include "CommandCatalog.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // The keyboard as the dispatcher reads it: ImGui's in the editor, a fake in tests
    class ShortcutKeys {
    public:
        virtual ~ShortcutKeys() = default;
        // the key went down this frame (or repeated, when repeat)
        virtual bool pressed(ImGuiKey key, bool repeat) const = 0;
        // the key is held
        virtual bool down(ImGuiKey key) const = 0;
        // the modifiers held (ImGuiMod_*)
        virtual ImGuiKeyChord mods() const = 0;
    };

    // ImGui's current frame
    class ImGuiShortcutKeys final : public ShortcutKeys {
    public:
        bool pressed(ImGuiKey key, bool repeat) const override;
        bool down(ImGuiKey key) const override;
        ImGuiKeyChord mods() const override;
    };

    struct ShortcutState {
        bool wantTextInput = false;       // a text field has the keyboard (io.WantTextInput)
        bool codeEditorFocused = false;
        bool gameHasKeyboard = false;     // Play, the Game view taking the keys
        bool popupOpen = false;           // any popup or modal (menus, dialogs, the Search palette)
        bool nativeMenuOwnsKeys = false;  // macOS: NSMenu runs its items' key equivalents
        // macOS: the chords its Edit items show for a panel's own commands (PanelClaims)
        std::vector<Chord> nativeMenuChords;
        // the focused panel contexts (SceneView, Hierarchy, Project, Console, Curve...)
        std::vector<ShortcutContext> panels;
    };

    class ShortcutDispatcher {
    public:
        explicit ShortcutDispatcher(CommandCatalog& catalog);

        // Runs the commands the keys pressed this frame bind; returns their ids
        std::vector<std::string> process(const ShortcutKeys& keys, const ShortcutState& state);
        // The command process() would run for a chord in this state (nullptr: none)
        const CommandDef* resolve(Chord chord, const ShortcutState& state) const;

        // The Shortcuts window listening for a chord: nothing fires, and the key it took does
        // not fire on a later frame either (its repeat or the release frame) until it is up
        // again (OpenEngine's SetShortcutCaptureActive / NoteShortcutCaptureConsumedKey latch)
        void setCaptureActive(bool active);
        bool isCaptureActive() const { return capturing; }
        void noteCaptureConsumedKey(ImGuiKey key);

    private:
        CommandCatalog& catalog;
        bool capturing = false;
        ImGuiKey consumedKey = ImGuiKey_None;

        bool contextActive(ShortcutContext context, const ShortcutState& state) const;
        bool mayFire(const CommandDef& def, Chord chord, const ShortcutState& state) const;
    };

}
