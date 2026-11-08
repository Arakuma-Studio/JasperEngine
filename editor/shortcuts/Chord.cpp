// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Chord.h"

#include <cctype>
#include <string>

using namespace jasperengine::editor;

namespace {

    struct KeyName {
        ImGuiKey key;
        const char* name;
    };

    // Names as chords are written (and shown); every key that can be bound is here
    const KeyName keyNames[] = {
        {ImGuiKey_Tab, "Tab"}, {ImGuiKey_LeftArrow, "Left"}, {ImGuiKey_RightArrow, "Right"},
        {ImGuiKey_UpArrow, "Up"}, {ImGuiKey_DownArrow, "Down"}, {ImGuiKey_PageUp, "PageUp"},
        {ImGuiKey_PageDown, "PageDown"}, {ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"},
        {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Delete, "Delete"}, {ImGuiKey_Backspace, "Backspace"},
        {ImGuiKey_Space, "Space"}, {ImGuiKey_Enter, "Enter"}, {ImGuiKey_Escape, "Escape"},
        {ImGuiKey_0, "0"}, {ImGuiKey_1, "1"}, {ImGuiKey_2, "2"}, {ImGuiKey_3, "3"}, {ImGuiKey_4, "4"},
        {ImGuiKey_5, "5"}, {ImGuiKey_6, "6"}, {ImGuiKey_7, "7"}, {ImGuiKey_8, "8"}, {ImGuiKey_9, "9"},
        {ImGuiKey_A, "A"}, {ImGuiKey_B, "B"}, {ImGuiKey_C, "C"}, {ImGuiKey_D, "D"}, {ImGuiKey_E, "E"},
        {ImGuiKey_F, "F"}, {ImGuiKey_G, "G"}, {ImGuiKey_H, "H"}, {ImGuiKey_I, "I"}, {ImGuiKey_J, "J"},
        {ImGuiKey_K, "K"}, {ImGuiKey_L, "L"}, {ImGuiKey_M, "M"}, {ImGuiKey_N, "N"}, {ImGuiKey_O, "O"},
        {ImGuiKey_P, "P"}, {ImGuiKey_Q, "Q"}, {ImGuiKey_R, "R"}, {ImGuiKey_S, "S"}, {ImGuiKey_T, "T"},
        {ImGuiKey_U, "U"}, {ImGuiKey_V, "V"}, {ImGuiKey_W, "W"}, {ImGuiKey_X, "X"}, {ImGuiKey_Y, "Y"},
        {ImGuiKey_Z, "Z"},
        {ImGuiKey_F1, "F1"}, {ImGuiKey_F2, "F2"}, {ImGuiKey_F3, "F3"}, {ImGuiKey_F4, "F4"},
        {ImGuiKey_F5, "F5"}, {ImGuiKey_F6, "F6"}, {ImGuiKey_F7, "F7"}, {ImGuiKey_F8, "F8"},
        {ImGuiKey_F9, "F9"}, {ImGuiKey_F10, "F10"}, {ImGuiKey_F11, "F11"}, {ImGuiKey_F12, "F12"},
        {ImGuiKey_Apostrophe, "'"}, {ImGuiKey_Comma, ","}, {ImGuiKey_Minus, "-"}, {ImGuiKey_Period, "."},
        {ImGuiKey_Slash, "/"}, {ImGuiKey_Semicolon, ";"}, {ImGuiKey_Equal, "="}, {ImGuiKey_LeftBracket, "["},
        {ImGuiKey_Backslash, "\\"}, {ImGuiKey_RightBracket, "]"}, {ImGuiKey_GraveAccent, "`"},
        {ImGuiKey_Keypad0, "Numpad0"}, {ImGuiKey_Keypad1, "Numpad1"}, {ImGuiKey_Keypad2, "Numpad2"},
        {ImGuiKey_Keypad3, "Numpad3"}, {ImGuiKey_Keypad4, "Numpad4"}, {ImGuiKey_Keypad5, "Numpad5"},
        {ImGuiKey_Keypad6, "Numpad6"}, {ImGuiKey_Keypad7, "Numpad7"}, {ImGuiKey_Keypad8, "Numpad8"},
        {ImGuiKey_Keypad9, "Numpad9"}, {ImGuiKey_KeypadDecimal, "NumpadDecimal"},
        {ImGuiKey_KeypadDivide, "NumpadDivide"}, {ImGuiKey_KeypadMultiply, "NumpadMultiply"},
        {ImGuiKey_KeypadSubtract, "NumpadSubtract"}, {ImGuiKey_KeypadAdd, "NumpadAdd"},
        {ImGuiKey_KeypadEnter, "NumpadEnter"},
    };

    // Other spellings parse() accepts
    const KeyName keyAliases[] = {
        {ImGuiKey_Escape, "Esc"}, {ImGuiKey_Enter, "Return"}, {ImGuiKey_Delete, "Del"},
        {ImGuiKey_LeftArrow, "LeftArrow"}, {ImGuiKey_RightArrow, "RightArrow"}, {ImGuiKey_UpArrow, "UpArrow"},
        {ImGuiKey_DownArrow, "DownArrow"}, {ImGuiKey_PageUp, "Page Up"}, {ImGuiKey_PageDown, "Page Down"},
        {ImGuiKey_LeftBracket, "LeftBracket"}, {ImGuiKey_RightBracket, "RightBracket"},
        {ImGuiKey_Keypad0, "Keypad0"}, {ImGuiKey_Keypad1, "Keypad1"}, {ImGuiKey_Keypad2, "Keypad2"},
        {ImGuiKey_Keypad3, "Keypad3"}, {ImGuiKey_Keypad4, "Keypad4"}, {ImGuiKey_Keypad5, "Keypad5"},
        {ImGuiKey_Keypad6, "Keypad6"}, {ImGuiKey_Keypad7, "Keypad7"}, {ImGuiKey_Keypad8, "Keypad8"},
        {ImGuiKey_Keypad9, "Keypad9"}, {ImGuiKey_Keypad1, "Numpad 1"}, {ImGuiKey_Keypad3, "Numpad 3"},
        {ImGuiKey_Keypad7, "Numpad 7"}, {ImGuiKey_Keypad9, "Numpad 9"},
    };

    bool equalsNoCase(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    }

    std::string_view trimmed(std::string_view text) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
        return text;
    }

    ImGuiKey parseKey(std::string_view name) {
        for (const KeyName& entry : keyNames) {
            if (equalsNoCase(name, entry.name)) return entry.key;
        }
        for (const KeyName& entry : keyAliases) {
            if (equalsNoCase(name, entry.name)) return entry.key;
        }
        return ImGuiKey_None;
    }

}

const char* Chord::keyName(ImGuiKey key) {
    for (const KeyName& entry : keyNames) {
        if (entry.key == key) return entry.name;
    }
    return "";
}

bool Chord::isModifierKey(ImGuiKey key) {
    switch (key) {
        case ImGuiKey_LeftCtrl: case ImGuiKey_RightCtrl:
        case ImGuiKey_LeftShift: case ImGuiKey_RightShift:
        case ImGuiKey_LeftAlt: case ImGuiKey_RightAlt:
        case ImGuiKey_LeftSuper: case ImGuiKey_RightSuper:
        case ImGuiKey_ReservedForModCtrl: case ImGuiKey_ReservedForModShift:
        case ImGuiKey_ReservedForModAlt: case ImGuiKey_ReservedForModSuper:
            return true;
        default:
            return false;
    }
}

bool Chord::isBindableKey(ImGuiKey key) {
    return keyName(key)[0] != '\0';
}

std::optional<Chord> Chord::parse(std::string_view text) {
    return parse(text, isMacPlatform());
}

std::optional<Chord> Chord::parse(std::string_view text, bool mac) {
    text = trimmed(text);
    if (text.empty()) return std::nullopt;
    ImGuiKeyChord mods = 0;
    ImGuiKey key = ImGuiKey_None;
    size_t start = 0;
    while (start <= text.size()) {
        size_t plus = text.find('+', start);
        // a "+" key itself is not bindable; "Ctrl++" is malformed
        std::string_view token = trimmed(text.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start));
        if (token.empty()) return std::nullopt;
        const bool last = plus == std::string_view::npos;
        if (!last) {
            if (equalsNoCase(token, "Ctrl") || equalsNoCase(token, "Cmd") || equalsNoCase(token, "Command")) {
                mods |= ImGuiMod_Ctrl;
            } else if (equalsNoCase(token, "Shift")) {
                mods |= ImGuiMod_Shift;
            } else if (equalsNoCase(token, "Alt") || equalsNoCase(token, "Option")) {
                mods |= ImGuiMod_Alt;
            } else if (equalsNoCase(token, "Super") || equalsNoCase(token, "Win") || equalsNoCase(token, "Meta")) {
                mods |= ImGuiMod_Super;
            } else if (equalsNoCase(token, "Control")) {
                // the key labelled Control: ImGui's Super on macOS (swapped with Cmd), Ctrl elsewhere
                mods |= mac ? ImGuiMod_Super : ImGuiMod_Ctrl;
            } else {
                return std::nullopt;
            }
            start = plus + 1;
            continue;
        }
        key = parseKey(token);
        break;
    }
    if (key == ImGuiKey_None) return std::nullopt;
    return Chord(mods | key);
}

std::string Chord::format(bool mac) const {
    if (empty()) return {};
    std::string text;
    const ImGuiKeyChord modifiers = mods();
    // macOS: Super is the Control key (ConfigMacOSXBehaviors swaps it with Cmd), named before Cmd as
    // the Mac's menus do
    if (mac && (modifiers & ImGuiMod_Super)) text += "Control+";
    if (modifiers & ImGuiMod_Ctrl) text += mac ? "Cmd+" : "Ctrl+";
    if (!mac && (modifiers & ImGuiMod_Super)) text += "Super+";
    if (modifiers & ImGuiMod_Alt) text += mac ? "Option+" : "Alt+";
    if (modifiers & ImGuiMod_Shift) text += "Shift+";
    text += keyName(key());
    return text;
}

bool Chord::isTypingChord() const {
    if (mods() & (ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Super)) return false;
    const ImGuiKey k = key();
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24) return false;
    if (k == ImGuiKey_Escape) return false;
    return true;
}
