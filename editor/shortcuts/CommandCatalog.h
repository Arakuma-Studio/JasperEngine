// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Every editor command that has a shortcut or a menu item, with its key bindings
// (docs/specs/S25-editor-search-organisation.md §6.3, R2). The catalog drives the main menu's
// shortcut texts, the toolbar tooltips, Help > Keyboard Shortcuts, the Shortcuts window, the
// Search palette's commands and the one keyboard dispatcher (ShortcutDispatcher). A command's
// bindings are its defaults unless the user overrode them (ShortcutOverrides,
// <configDir>/shortcuts.yaml). Read-only rows (mouse gestures) are listed but never bound.
//
// The table's layout follows OpenEngine's shortcut catalog (Apps/Editor/Include/Editor/
// Shortcuts/EditorShortcuts.h: group, name, context, default, second default, readOnly,
// FindShortcutConflict), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License (NOTICE.md); here each command also carries its action.

#include "Chord.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor {

    // Where a binding applies. Global fires everywhere, the Code Editor and text fields
    // included; Editor in any editor window but the Code Editor, and not while typing; a panel
    // context while that window has focus (SceneView: focused or under the mouse; Animation: the
    // Animation window editing a clip; PoseAnimation: the Pose Animation window, or a Scene view
    // posing its character; Curve, ColorPicker, SoundBrowser and AudioCaptures: those windows,
    // focused; SpritePreview: the Sprite Preview window; VoxelBrush: a Voxel Volume brush armed and
    // the pointer over the Scene view, T6 R60; TerrainBrush: a Terrain brush armed, T1 R57). A new
    // context goes at the end: parseShortcutContext walks up to the last one.
    enum class ShortcutContext : uint8_t { Global, Editor, SceneView, Hierarchy, Project, Console, Animation, PoseAnimation, Curve, ColorPicker, SoundBrowser, AudioCaptures, SpritePreview, VoxelBrush, TerrainBrush };

    const char* shortcutContextName(ShortcutContext context);       // "Global", "Scene View", ...
    const char* shortcutContextKey(ShortcutContext context);        // "global", "scene_view", ...
    std::optional<ShortcutContext> parseShortcutContext(std::string_view text);  // either spelling

    // Who runs a command: a key, a menu click, the Search palette or an agent (run_command)
    enum class CommandSource { Keyboard, Menu, Palette, Agent };

    struct CommandArgs {
        CommandSource source = CommandSource::Menu;
        std::string payload;
    };

    struct CommandDef {
        std::string id;          // "edit.duplicate": stable, the key of overrides and recents
        std::string group;       // "Edit"
        std::string name;        // "Duplicate"
        ShortcutContext context = ShortcutContext::Editor;
        // further panel contexts the bindings apply in (Cut/Copy/Paste: Hierarchy and Scene View)
        std::vector<ShortcutContext> alsoIn;
        Chord primary;
        Chord secondary;
        Chord macPrimary;        // replaces primary on macOS (Redo: Cmd+Shift+Z)
        bool readOnly = false;   // a reference row: listed, never bound or rebound
        std::string gesture;     // read-only rows: what to do ("Alt+Left Drag")
        bool agentSafe = true;   // run_command may run it (not Exit, New/Open Project...)
        bool repeat = false;     // fires again while the key is held (key repeat)
        std::string description; // one line for Help and the palette
        std::function<void(const CommandArgs&)> run;
        // the menu item's enabled state too; a disabled command does not run
        std::function<bool()> enabled;
        // why it is disabled now (the menu item's tooltip); null: no reason given
        std::function<std::string()> disabledReason;
        // keyboard only: further conditions on firing (no drag in progress, a 3D scene...)
        std::function<bool()> keyGate;
        // the main menu item that runs it (AppMenuCommand and payload), when it has one
        std::optional<uint32_t> menuCommand;
        std::string menuPayload;
        // registered by a project (editor scripts): replaced when the project's scripts change
        bool dynamic = false;
    };

    class CommandCatalog {
    public:
        // The editor's catalog (App registers into it); tests make their own
        static CommandCatalog& editor();

        CommandCatalog();

        // On macOS bindings use macPrimary and texts say Cmd; tests switch it
        void setMac(bool mac);
        bool isMac() const { return mac; }

        // Adds (or, for the same id, replaces) a command; ids are unique
        void add(CommandDef def);
        // Removes the dynamic commands whose id starts with prefix ("script.")
        void removeDynamic(std::string_view prefix);
        const CommandDef* find(std::string_view id) const;
        // The command a main menu item runs (exact payload first, then one registered without a payload)
        const CommandDef* findByMenuCommand(uint32_t menuCommand, std::string_view payload) const;
        const std::vector<CommandDef>& commands() const { return defs; }

        // The defaults on this platform: macPrimary (macOS) or primary, then secondary
        std::vector<Chord> defaults(std::string_view id) const;
        // The effective bindings: the user's override, else the defaults
        std::span<const Chord> bindings(std::string_view id) const;
        bool isOverridden(std::string_view id) const;

        // The other command a chord would collide with (same chord, overlapping context), or
        // null. *shadowed is set when the only overlap is a Global/Editor binding over a panel
        // context's (shown, not blocking): then that command is returned with shadowed true.
        const CommandDef* findConflict(std::string_view id, Chord chord, bool* shadowed = nullptr) const;
        // Sets a command's bindings (empty: unbound). False with *reason for an unknown or
        // read-only command or a chord that is not bindable
        bool setBindings(std::string_view id, std::vector<Chord> chords, std::string* reason = nullptr);
        void resetBindings(std::string_view id);
        void resetAll();

        // The first binding as menus show it ("Ctrl+S", "Cmd+Shift+Z" on macOS), "" when unbound
        std::string menuShortcutText(std::string_view id) const;
        // Every binding for tooltips and Help: "Delete / Backspace"; a read-only row's gesture
        std::string bindingsText(std::string_view id) const;
        // "Rect Tool (T)": a name with its first binding, the name alone when unbound
        std::string tooltip(std::string_view id, std::string_view name) const;

        // Help > Keyboard Shortcuts as Markdown, grouped (docs/editor/keyboard-shortcuts.md is
        // this text; it is written for every platform, so Ctrl is noted as Cmd on macOS)
        std::string markdownReference() const;

        // Overrides as stored: command id -> chord texts (unbound: an empty list). Unknown
        // ids and unparsable chords from the file are kept here too, never applied
        struct StoredOverride {
            std::vector<std::string> chords;
            bool applied = false;    // a known command took it
        };
        const std::map<std::string, StoredOverride>& storedOverrides() const { return stored; }
        // Replaces every override with these (ShortcutOverrides::load), applying the ones
        // that name a command and parse; returns the ids and texts that could not be applied
        struct Rejected {
            std::string id;
            std::string reason;
        };
        std::vector<Rejected> loadOverrides(const std::map<std::string, std::vector<std::string>>& overrides);
        // A dynamic command registered later (a script menu) takes its stored override
        void applyStoredOverride(const std::string& id);
        // Two commands bound to one chord in one context after a load (R7): the binding equal
        // to its command's default wins, otherwise the earlier catalog entry's; the other
        // command loses that chord (in memory: the file keeps it). Returns {winner, loser, chord}.
        struct ResolvedConflict {
            std::string winner;
            std::string loser;
            Chord chord;
        };
        std::vector<ResolvedConflict> resolveConflicts();

        // The menu path a command's item has in the main menu ("Edit > Duplicate"), and the
        // chord that item shows; set by App each time the menu changes
        void setMenuItem(std::string_view id, std::string path, Chord shown);
        void clearMenuItems();
        std::string menuPath(std::string_view id) const;
        // The chord the command's main menu item shows (empty without an item)
        Chord menuChord(std::string_view id) const;

        // Bumped by every change of bindings (menus rebuild their texts)
        uint64_t revision() const { return revisionCounter; }

    private:
        bool mac = isMacPlatform();
        std::vector<CommandDef> defs;
        std::map<std::string, size_t, std::less<>> index;
        // effective bindings by id (overrides applied); every command has an entry
        std::map<std::string, std::vector<Chord>, std::less<>> effective;
        std::map<std::string, StoredOverride> stored;
        struct MenuItem {
            std::string path;
            Chord shown;
        };
        std::map<std::string, MenuItem, std::less<>> menuItems;
        uint64_t revisionCounter = 1;

        void rebuildIndex();
        void refreshEffective(const CommandDef& def);
        size_t orderOf(std::string_view id) const;
    };

    // The contexts a command binds in: its context and alsoIn
    std::vector<ShortcutContext> commandContexts(const CommandDef& def);
    // Global and Editor are the broad contexts: a key in them reaches every panel
    bool isBroadContext(ShortcutContext context);

}
