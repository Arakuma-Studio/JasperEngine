// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CommandHistory::insertCommand, the editor's own steps (a model reloaded because its FBX
// changed on disk): undoable, but they keep the redo tail a user edit would drop, and in Play
// they sit below the play floor so Stop keeps them.

#include <gtest/gtest.h>

#include "command/CommandHistory.h"

#include <string>
#include <vector>

using namespace jasperengine::editor;

namespace {

    // Appends its name to the log when applied, removes it when undone
    class LogCmd : public Command {
    public:
        LogCmd(std::vector<std::string>& state, std::string name): state(state), name(std::move(name)) {
            setNoMerge();
        }
        bool execute() override {
            state.push_back(name);
            return true;
        }
        void undo() override {
            ASSERT_FALSE(state.empty());
            EXPECT_EQ(state.back(), name) << "undone out of order";
            state.pop_back();
        }

    private:
        std::vector<std::string>& state;
        std::string name;
    };

    using State = std::vector<std::string>;

} // namespace

TEST(CommandHistory, AddingAnEditEndsTheRedoTail) {
    State state;
    CommandHistory history;
    history.addCommand(new LogCmd(state, "A"));
    history.addCommand(new LogCmd(state, "B"));
    history.undo();
    ASSERT_TRUE(history.canRedo());
    history.addCommand(new LogCmd(state, "C"));
    EXPECT_FALSE(history.canRedo());
    EXPECT_EQ(state, (State{"A", "C"}));
}

TEST(CommandHistory, InsertedStepKeepsTheRedoTail) {
    State state;
    CommandHistory history;
    history.addCommand(new LogCmd(state, "A"));
    history.addCommand(new LogCmd(state, "B"));
    history.addCommand(new LogCmd(state, "C"));
    history.undo();
    history.undo();
    ASSERT_EQ(state, (State{"A"}));

    // The model reloads while B and C are undone: they can still be redone, after it
    ASSERT_TRUE(history.insertCommand(new LogCmd(state, "Reload")));
    EXPECT_EQ(state, (State{"A", "Reload"}));
    ASSERT_TRUE(history.canRedo());
    history.redo();
    history.redo();
    EXPECT_EQ(state, (State{"A", "Reload", "B", "C"}));
    EXPECT_FALSE(history.canRedo());

    // Undo goes back through it in order
    history.undo();
    history.undo();
    history.undo();
    EXPECT_EQ(state, (State{"A"}));
    history.undo();
    EXPECT_EQ(state, (State{}));
    EXPECT_FALSE(history.canUndo());
}

TEST(CommandHistory, InsertedStepAtTheEndIsAPlainStep) {
    State state;
    CommandHistory history;
    history.addCommand(new LogCmd(state, "A"));
    ASSERT_TRUE(history.insertCommand(new LogCmd(state, "Reload")));
    EXPECT_FALSE(history.canRedo());
    history.addCommand(new LogCmd(state, "B"));
    history.undo();
    history.undo();
    EXPECT_EQ(state, (State{"A"}));
}

TEST(CommandHistory, InsertedStepDuringPlayOutlivesStop) {
    State state;
    CommandHistory history;
    history.addCommand(new LogCmd(state, "A"));
    history.addCommand(new LogCmd(state, "B"));
    history.undo();

    // Play startup reloads the model before the scene is captured, so Stop's snapshot has it
    history.beginPlaySession();
    ASSERT_TRUE(history.insertCommand(new LogCmd(state, "Reload")));
    history.addCommand(new LogCmd(state, "Played"));
    EXPECT_EQ(state, (State{"A", "Reload", "Played"}));
    history.undo();
    EXPECT_FALSE(history.canUndo()) << "edit-time steps are not undone during Play";

    // Stop: the snapshot is restored, the play-time steps go, the reload and the redo tail stay
    history.endPlaySession(true);
    state = {"A", "Reload"};
    ASSERT_TRUE(history.canRedo());
    history.redo();
    EXPECT_EQ(state, (State{"A", "Reload", "B"}));
    history.undo();
    history.undo();
    history.undo();
    EXPECT_EQ(state, (State{}));
}

TEST(CommandHistory, FailedInsertLeavesTheHistoryAlone) {
    class FailCmd : public Command {
    public:
        bool execute() override { return false; }
        void undo() override { ADD_FAILURE() << "never applied"; }
    };
    State state;
    CommandHistory history;
    history.addCommand(new LogCmd(state, "A"));
    history.undo();
    EXPECT_FALSE(history.insertCommand(new FailCmd()));
    EXPECT_TRUE(history.canRedo());
    EXPECT_FALSE(history.canUndo());
}
