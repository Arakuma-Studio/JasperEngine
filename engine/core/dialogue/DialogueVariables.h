// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIALOGUE_VARIABLES_H
#define DIALOGUE_VARIABLES_H

// Contract (docs/specs/dialogue-graphs.md R5, R27; savegame.md)
// - The game-wide dialogue variable store (Lua `Dialogue.variables`): name -> bool, number or
//   string, and the visit counts per `<dialogue GUID>#<node id>` (`#<node>.<option>` for a once
//   option chosen). A dialogue's graph variables, then Project Settings > Dialogue's declared
//   ones, give a default that is written to the store on its first read when the name is absent.
// - onChanged(name, value) fires after every set that changes a value (Lua; cleared at Stop by
//   Engine::clearAllSubscriptions); C++ listeners (the runners' onVariableChanged) hear the same.
// - toJson / fromJson are the store as {variables: {...}, visits: {"<guid>#n7": 2}}: Lua's
//   toTable / fromTable and SaveHooks, which a save system calls (savegame.md).
// - reset() empties values and visits (Stop, tests); the project defaults stay.
// - Thread-safety: main thread.

#include "Export.h"
#include "dialogue/DialogueValue.h"
#include "json.hpp"
#include "util/FunctionSubscribe.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::dialogue {

    class Program;

    class JASPERENGINE_API Variables {
    public:
        // nothing when absent (no defaults)
        static Value get(std::string_view name);
        // a runner's read: an absent name takes the program's default, else the project's, and
        // keeps it (written without onChanged)
        static Value read(std::string_view name, const Program* program);
        static void set(std::string_view name, const Value& value);
        static bool has(std::string_view name);
        static std::vector<std::pair<std::string, Value>> all();

        static uint32_t visits(std::string_view identity, std::string_view node);
        static void addVisit(std::string_view identity, std::string_view node);
        static void resetVisits();

        static nlohmann::ordered_json toJson();
        // replaces values and visits; false (nothing changed) for a value of another shape;
        // `notify`: what changed is told as a set would (the editor's walker puts the store back silently)
        static bool fromJson(const nlohmann::ordered_json& json, std::string* error = nullptr, bool notify = true);

        // Project Settings > Dialogue > Variables: the editor sets them for Play; a game reads
        // SETTINGS_FILE (its export writes it when the project declares any) on the first lookup
        static constexpr const char* SETTINGS_FILE = "je-dialogue.json";
        static constexpr const char* SETTINGS_FORMAT = "jasperengine.dialogue-settings";
        static void setProjectDefaults(std::vector<std::pair<std::string, Value>> defaults);
        static bool parseSettings(std::string_view text, std::vector<std::pair<std::string, Value>>& out, std::string* error = nullptr);
        static const Value* projectDefault(std::string_view name);
        static void reset();

        // Lua Dialogue.variables.onChanged
        static FunctionSubscribe<void(const std::string&, const Value&)> onChanged;
        static int addListener(std::function<void(const std::string&, const Value&)> listener);
        static void removeListener(int id);
    };

    // What a save slot keeps of the dialogue system: the store's values and visit counts. The save
    // system (savegame.md) calls these; nothing here depends on it.
    class JASPERENGINE_API SaveHooks {
    public:
        // the state as JSON text
        static std::string capture();
        // false (nothing changed) for text that is not a captured state
        static bool restore(std::string_view text, std::string* error = nullptr);
    };

}

#endif //DIALOGUE_VARIABLES_H
