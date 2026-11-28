// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/dialogue-graphs.md R20, R25, D7)
// - Purpose: plays a dialogue headless with the runtime's own runner (so a walk predicts Play):
//   handler calls never run; a condition takes the value given for its call ("quest.done" or
//   "quest.*"), else false and it is listed; actions are recorded as they would fire. The game's
//   variable store is saved around a walk and put back, so a walk in edit mode or in Play leaves
//   it as it was.
// - walk(): choices apply in order at each menu (a line without an enabled option advances),
//   until a menu runs out of choices, the end, or max_steps lines.
// - explore(): every path breadth-first (each menu's options, each unlisted condition both ways)
//   up to max_paths walks: the nodes covered, the endings with the choices leading there, the loops
//   no ending is reachable from (menus told apart by node and variables), the lines never shown.
// - Thread-safety: main thread (the store, Localization).

#pragma once

#include "dialogue/DialogueProgram.h"
#include "dialogue/DialogueRunner.h"
#include "json.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::dialogues {

    struct WalkOptions {
        std::string entry;
        std::vector<int> choices;
        std::vector<std::pair<std::string, jasperengine::dialogue::Value>> variables;     // the store's values before the walk
        std::map<std::string, jasperengine::dialogue::Value> conditions;                  // "quest.done", "quest.*", or a call as described
        std::string language;                                                             // "" = the editor's current
        uint64_t seed = 0;
        int maxSteps = 200;
        bool keepStore = false;     // the walk's variables stay in the store
        // the Dialogue window's preview: stop at every line; `steps` are what the user did there
        // in turn, 0 an advance and k the k-th option (choices is not used)
        bool stepwise = false;
        std::vector<int> steps;
    };

    struct WalkStep {
        std::string node, speaker, text, key, variant;
        std::vector<jasperengine::dialogue::Option> options;
        std::vector<std::string> actions;
        std::vector<std::pair<std::string, jasperengine::dialogue::Value>> assigned;
        int chosen = 0;             // the option chosen at it (0: advanced or not yet)
    };

    struct WalkResult {
        std::vector<WalkStep> transcript;
        std::string state;          // line | waiting | ended
        std::string node;           // where it stopped ("" when ended)
        std::string result;         // the end's result
        std::vector<std::string> endActions;    // what the last step ran after the last line
        std::vector<std::string> nodes;         // every node entered, in order
        std::vector<std::string> unknownCalls;  // conditions given no value (assumed false)
        std::vector<std::pair<std::string, jasperengine::dialogue::Value>> variables;     // the store after the walk
        std::string error;          // dialogue-loop, a missing jump target, a choice that is no enabled option
        bool truncated = false;     // max_steps lines shown
    };

    WalkResult walk(const std::shared_ptr<const jasperengine::dialogue::Program>& program, const WalkOptions& options);

    struct ExploreResult {
        struct Ending {
            std::string result;
            std::string node;
            std::vector<int> choices;
            std::map<std::string, bool> assumed;    // conditions taken true or false on the way
        };
        struct Loop {
            std::vector<std::string> nodes;         // the menus of the loop
            std::vector<int> choices;               // a path into it
        };
        size_t nodes = 0;
        size_t covered = 0;
        std::vector<std::string> uncovered;         // nodes no path entered
        std::vector<std::string> linesNeverShown;
        std::vector<Ending> endings;
        std::vector<Loop> loops;
        std::vector<std::string> errors;            // dialogue-loop and other stops, with their choices
        size_t paths = 0;
        bool truncated = false;                     // max_paths reached before every path was walked
    };

    ExploreResult explore(const std::shared_ptr<const jasperengine::dialogue::Program>& program, const WalkOptions& options, int maxPaths);

    nlohmann::json toJson(const jasperengine::dialogue::Value& value);
    nlohmann::json toJson(const WalkResult& result);
    nlohmann::json toJson(const ExploreResult& result);

}
