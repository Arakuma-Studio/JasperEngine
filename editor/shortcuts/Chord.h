// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A key with its modifiers, as the command catalog binds it (docs/specs/S25 §6.3). "Ctrl" is the
// Command key on macOS: ImGui's ImGuiMod_Ctrl with ConfigMacOSXBehaviors, and the native menu's
// key equivalents (BackendMac.mm) read "Ctrl" the same way. Chords bind to the ImGuiKey the
// backends deliver, which follows the keyboard layout for letters (D7).

#include "imgui.h"

#include <optional>
#include <string>
#include <string_view>

namespace jasperengine::editor {

    struct Chord {
        ImGuiKeyChord value = 0;

        Chord() = default;
        constexpr explicit Chord(ImGuiKeyChord chord) : value(chord) {}

        bool empty() const { return value == 0; }
        ImGuiKey key() const { return static_cast<ImGuiKey>(value & ~ImGuiMod_Mask_); }
        ImGuiKeyChord mods() const { return value & ImGuiMod_Mask_; }
        bool operator==(const Chord& other) const { return value == other.value; }
        bool operator!=(const Chord& other) const { return value != other.value; }

        // "Ctrl+Shift+S", "F2", "Numpad7", "Shift+[", "Delete"; Cmd and Command read as Ctrl,
        // Option as Alt. Case-insensitive; nullopt for anything else (an empty text included)
        static std::optional<Chord> parse(std::string_view text);
        // "Ctrl+Shift+S", or on macOS "Cmd+Shift+S" (Super there: "Control+Cmd+F"); "" for an empty chord
        std::string format(bool mac) const;
        // The form the menu backends parse (BackendMac.mm reads Ctrl as Command): format(false)
        std::string menuText() const { return format(false); }

        // A chord a focused text field would take as typing: no Ctrl, Alt or Super, and a key
        // that types or edits text (letters, digits, punctuation, Space, Delete, arrows...).
        // Function keys and Escape are not.
        bool isTypingChord() const;
        // A key that only modifies (Ctrl, Shift...): never a chord's key
        static bool isModifierKey(ImGuiKey key);
        // A key a chord may use (a named keyboard key, no gamepad or mouse)
        static bool isBindableKey(ImGuiKey key);
        // The key's name in chords ("A", "F5", "[", "Numpad1"), "" when not bindable
        static const char* keyName(ImGuiKey key);
    };

    // true on macOS builds: the default for formatting
    constexpr bool isMacPlatform() {
#if defined(__APPLE__)
        return true;
#else
        return false;
#endif
    }

}
