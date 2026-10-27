// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A catalog of the built-in commands whose actions only record that they ran, and a fake
// keyboard, for the shortcut tests

#include "shortcuts/CommandCatalog.h"
#include "shortcuts/DefaultCommands.h"
#include "shortcuts/ShortcutDispatcher.h"

#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor::test {

    struct RecordingCatalog {
        CommandCatalog catalog;
        std::vector<std::string> ran;

        explicit RecordingCatalog(bool mac = false) {
            catalog.setMac(mac);
            for (CommandDef def : defaultCommands()) {
                const std::string id = def.id;
                if (!def.readOnly) def.run = [this, id](const CommandArgs&) { ran.push_back(id); };
                catalog.add(std::move(def));
            }
        }
    };

    // Keys going down this frame (pressed), keys held from before (held: they repeat when
    // repeating is set), and the modifiers
    class FakeKeys final : public ShortcutKeys {
    public:
        std::set<ImGuiKey> pressedKeys;
        std::set<ImGuiKey> heldKeys;
        bool repeating = false;
        ImGuiKeyChord modifiers = 0;

        static FakeKeys press(Chord chord) {
            FakeKeys keys;
            keys.pressedKeys.insert(chord.key());
            keys.modifiers = chord.mods();
            return keys;
        }

        bool pressed(ImGuiKey key, bool repeat) const override {
            if (pressedKeys.count(key)) return true;
            return repeat && repeating && heldKeys.count(key) > 0;
        }
        bool down(ImGuiKey key) const override { return pressedKeys.count(key) > 0 || heldKeys.count(key) > 0; }
        ImGuiKeyChord mods() const override { return modifiers; }
    };

    inline Chord chordOf(const char* text) {
        return Chord::parse(text).value_or(Chord());
    }

}
