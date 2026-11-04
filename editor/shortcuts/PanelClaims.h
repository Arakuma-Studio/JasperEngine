// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A focused panel's claim on the edit commands (docs/specs/S61-window-commands.md R2). While a
// panel with a claim has focus, Edit > Delete, Duplicate, Frame Selected, Copy, Cut, Paste...
// go to its handlers and nowhere else, from a key, the Edit menu, the Search palette or an agent
// alike. A verb the panel has no handler for is disabled: it never falls through to the scene.
//
// A handler is one of two kinds. With `command` the panel has its own catalog command for the
// verb, in its own context (curve.delete): that command's bindings are the panel's keys, so
// edit.delete's key stays quiet while the panel has focus and a rebinding of the panel's command
// really moves the key; the menu and the palette run the panel's command. Without it, `run` and
// `enabled` are the handler and edit.delete's own key reaches it.
//
// The focus is latched by refresh() while no popup is open, as App's lastFocusedWindow is: a
// menu or the palette holds the ImGui focus while its item is picked, and the item still means
// the panel the user came from.

#include "CommandCatalog.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor {

    enum class EditVerb : uint8_t { Delete, Duplicate, Frame, Copy, Cut, Paste, PasteAsChild, SelectAll, Rename };

    struct ClaimHandler {
        std::string command;             // the panel's own catalog command, or
        std::function<bool()> enabled;   // null: always
        std::function<void()> run;
    };

    struct PanelClaim {
        std::string panel;               // "Curve": replaces an earlier claim of that name
        std::function<bool()> focused;
        std::map<EditVerb, ClaimHandler> handlers;
    };

    class PanelClaims {
    public:
        // The editor's claims, over the editor's catalog (App registers them); tests make their own
        static PanelClaims& editor();

        explicit PanelClaims(CommandCatalog& catalog);

        void add(PanelClaim claim);
        // A text field has the keyboard: every claimed verb reads as off (a native menu's enabled
        // item would take the key), so the field keeps its keys. Asked of the enabled state only:
        // what the palette or an agent runs has already been decided
        void setTyping(std::function<bool()> typing);
        // Wraps the catalog's edit.* commands so they obey the claims; after their own binds
        void bindEditCommands();

        // Latches the focused panel's claim; call once a frame while no popup is open
        void refresh();
        // The claim in force (null: none, the edit commands act as they do without claims)
        const PanelClaim* active() const;

        // edit.delete and the like: runs it on the active claim. False when it is not one of the
        // edit commands or no claim is active: the caller goes on as usual
        bool route(std::string_view commandId, CommandSource source = CommandSource::Menu);
        // The command whose binding a main menu item shows: the claim's own one, else commandId
        std::string shownCommand(std::string_view commandId) const;
        // The chords the native menu's claimed items take (macOS NSMenu runs those: the
        // dispatcher leaves them to it)
        std::vector<Chord> nativeMenuChords() const;

    private:
        CommandCatalog& catalog;
        std::vector<PanelClaim> claims;
        std::function<bool()> typing;
        int current = -1;

        const ClaimHandler* handlerFor(EditVerb verb) const;
        bool canRun(const ClaimHandler* handler) const;
        void runVerb(EditVerb verb, CommandSource source);
    };

}
