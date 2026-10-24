// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A recording fake command for the S24 undo tests: a configurable name, size, no-op flag and
// merge key; it sets an int to its value on execute and back on undo, so a test can read the
// state the history left.

#include "command/CommandHistory.h"
#include "command/CommandHandle.h"
#include "command/UndoTimeline.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::test {

    struct FakeState {
        int value = 0;
        std::vector<std::string> deleted;   // names, in the order the history freed them
    };

    class UndoFake : public Command {
    public:
        FakeState& state;
        std::string label;
        int newValue;
        int oldValue = 0;
        size_t bytes = 64;
        bool noOp = false;
        int mergeKey = 0;       // 0: never merges; equal keys merge
        bool offThread = false;
        bool executes = true;

        UndoFake(FakeState& state, std::string label, int newValue): state(state), label(std::move(label)), newValue(newValue) {}
        ~UndoFake() override { state.deleted.push_back(label); }

        bool execute() override {
            if (!executes) return false;
            oldValue = state.value;
            state.value = newValue;
            return true;
        }
        void undo() override { state.value = oldValue; }
        bool mergeWith(Command* other) override {
            UndoFake* older = dynamic_cast<UndoFake*>(other);
            if (!older || mergeKey == 0 || older->mergeKey != mergeKey) return false;
            oldValue = older->oldValue;
            return true;
        }
        std::string describe() const override { return label; }
        const char* typeName() const override { return "UndoFake"; }
        bool isNoOp() const override { return noOp || oldValue == newValue; }
        size_t memoryBytes() const override { return bytes; }
        bool releaseOffThread() const override { return offThread; }
    };

    // Resets the histories' static settings around a test (other tests and a Project install
    // hooks)
    class UndoStatics {
    public:
        UndoStatics() { reset(); }
        ~UndoStatics() { reset(); }

        static void reset() {
            CommandHistory::maxEntries = CommandHistory::DEFAULT_MAX_ENTRIES;
            CommandHistory::memoryLimit = CommandHistory::DEFAULT_MEMORY_LIMIT;
            CommandHistory::offThreadRelease = nullptr;
            CommandHistory::beforeMutation = nullptr;
            CommandHistory::beforeRecord = nullptr;
            CommandHistory::deferReleases = false;
            CommandHistory::selectionHooks = CommandHistory::SelectionHooks();
            CommandHistory::onSceneModified = nullptr;
            UndoTimeline::setProjectHistory(nullptr);
            UndoTimeline::selectedScene = nullptr;
            UndoTimeline::historyName = nullptr;
            UndoTimeline::warn = nullptr;
        }
    };

}
