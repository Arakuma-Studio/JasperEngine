// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// CommandHistory::beforeChange, the hook the Physics Placement tool settles its running gesture
// through: asked before a command executes (so it records the values the tool commits first),
// before an undo and a redo (which it can drop), and re-entrant (the tool's own commit is a
// command added from inside the hook).

#include <gtest/gtest.h>
#include "object/Model.h"

#include "command/CommandHistory.h"

#include <string>
#include <vector>

using namespace jasperengine::editor;

namespace {

    // A value the commands set, and a log of what happened in order
    struct Model {
        int value = 0;
        std::vector<std::string> log;
    };

    class SetValue : public Command {
        Model& model;
        int newValue;
        int oldValue = 0;
        std::string name;

    public:
        SetValue(Model& model, int newValue, std::string name): model(model), newValue(newValue), name(std::move(name)) {}

        bool execute() override {
            oldValue = model.value;
            model.value = newValue;
            model.log.push_back("execute " + name + " (was " + std::to_string(oldValue) + ")");
            return true;
        }

        void undo() override {
            model.value = oldValue;
            model.log.push_back("undo " + name);
        }

        bool affectsStructure() const override { return false; }
        std::string describe() const override { return "Set " + name; }
        const char* typeName() const override { return "SetValue"; }
    };

    // Restores the hook whatever a test does
    struct HookGuard {
        ~HookGuard() { CommandHistory::beforeChange = nullptr; }
    };

}

TEST(CommandHistoryHook, AskedBeforeEveryChangeWithItsScene) {
    HookGuard guard;
    Model model;
    std::vector<std::pair<size_t, CommandHistory::Change>> asked;
    CommandHistory::beforeChange = [&](size_t sceneId, CommandHistory::Change change) {
        asked.push_back({sceneId, change});
        model.log.push_back("asked");
        return true;
    };

    CommandHistory history(42);
    history.addCommand(new SetValue(model, 1, "a"));
    history.undo();
    history.redo();

    ASSERT_EQ(asked.size(), 3u);
    EXPECT_EQ(asked[0], std::make_pair(size_t(42), CommandHistory::Change::Add));
    EXPECT_EQ(asked[1], std::make_pair(size_t(42), CommandHistory::Change::Undo));
    EXPECT_EQ(asked[2], std::make_pair(size_t(42), CommandHistory::Change::Redo));
    // before the command reads what it replaces
    EXPECT_EQ(model.log, (std::vector<std::string>{"asked", "execute a (was 0)", "asked", "undo a", "asked", "execute a (was 0)"}));
    EXPECT_EQ(model.value, 1);
}

TEST(CommandHistoryHook, FalseDropsUndoAndRedo) {
    HookGuard guard;
    Model model;
    bool busy = false;
    CommandHistory::beforeChange = [&](size_t, CommandHistory::Change change) {
        return !(busy && change != CommandHistory::Change::Add);
    };

    CommandHistory history(1);
    history.addCommand(new SetValue(model, 1, "a"));
    history.addCommand(new SetValue(model, 2, "b"));

    busy = true;
    history.undo();
    EXPECT_EQ(model.value, 2) << "an undo while busy is dropped";
    EXPECT_TRUE(history.canUndo());

    busy = false;
    history.undo();
    EXPECT_EQ(model.value, 1);

    busy = true;
    history.redo();
    EXPECT_EQ(model.value, 1) << "a redo while busy is dropped";
    EXPECT_TRUE(history.canRedo());

    busy = false;
    history.redo();
    EXPECT_EQ(model.value, 2);

    // a command is always added, busy or not
    busy = true;
    EXPECT_TRUE(history.addCommand(new SetValue(model, 3, "c")));
    EXPECT_EQ(model.value, 3);
}

TEST(CommandHistoryHook, ACommitFromInsideTheHookLandsFirst) {
    HookGuard guard;
    Model model;
    CommandHistory history(7);

    // a tool previewing 5 live (not a command): another edit makes it commit first, with its
    // own command added from inside the hook (which must not ask again)
    bool previewing = false;
    int committing = 0;
    CommandHistory::beforeChange = [&](size_t, CommandHistory::Change change) {
        if (committing > 0 || !previewing) return true;
        if (change == CommandHistory::Change::Add) {
            committing++;
            model.value = 0;   // the commit puts the true value back first, then records 5
            history.addCommandNoMerge(new SetValue(model, 5, "commit"));
            committing--;
            previewing = false;
        }
        return true;
    };

    previewing = true;
    model.value = 5;
    history.addCommand(new SetValue(model, 9, "edit"));

    EXPECT_EQ(model.value, 9);
    EXPECT_EQ(model.log, (std::vector<std::string>{"execute commit (was 0)", "execute edit (was 5)"}));

    // two undo steps: the edit, then the committed gesture
    history.undo();
    EXPECT_EQ(model.value, 5);
    history.undo();
    EXPECT_EQ(model.value, 0);
}
