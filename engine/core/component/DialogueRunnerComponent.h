// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIALOGUE_RUNNER_COMPONENT_H
#define DIALOGUE_RUNNER_COMPONENT_H

#include "Export.h"
#include "asset/AssetRef.h"
#include "ecs/Entity.h"
#include "util/FunctionSubscribe.h"

#include <memory>
#include <string>

namespace jasperengine{

    class DialogueRunner;
    namespace dialogue { class Runner; struct Line; struct Value; }

    // Dialogue Runner (docs/specs/dialogue-graphs.md R13): plays a .dialogue on its GameObject and
    // hands each line and its choices to whatever view the game has. Auto Start starts it after
    // every script's init of the frame it appears in (Play or instantiate), so the handlers those
    // scripts register exist; Seed 0 seeds the variants from the entity id. It has no update: it acts
    // only in calls (DialogueRunner, Lua) and its events.
    struct JASPERENGINE_API DialogueRunnerComponent{
        AssetRef dialogue;              // a .dialogue (asset path rules of FileData)
        std::string startEntry;         // "" = `start`, else the entry with the lowest id
        bool autoStart = false;
        int seed = 0;
        Entity view = NULL_ENTITY;      // a GameObject with a UI Document showing it; NULL = none

        FunctionSubscribe<void(DialogueRunner)> onStart;
        FunctionSubscribe<void(DialogueRunner, const dialogue::Line&)> onLine;
        FunctionSubscribe<void(DialogueRunner, const std::string&)> onEnd;
        FunctionSubscribe<void(DialogueRunner, const std::string&, const dialogue::Value&)> onVariableChanged;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onStart); f(onLine); f(onEnd); f(onVariableChanged); }

        // runtime, not saved: the runner (made on first use) and what the Inspector and
        // get_runtime_entity read of it
        std::shared_ptr<dialogue::Runner> runner;
        bool isRunning = false;
        std::string state = "idle";     // idle | line | waiting | ended
        std::string currentNode;
    };

}

#endif //DIALOGUE_RUNNER_COMPONENT_H
