// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CommandHistory::insertCommand, the editor's own steps (a model reloaded because its FBX
// changed on disk): undoable, but they keep the redo tail a user edit would drop, and in Play
// they sit below the play floor so Stop keeps them. Such a step has the newest id although it
// sits below steps made before it: the history finds it by id and tells play-time steps by
// their position (S24's timeline, UndoTimeline, orders by id).

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
        std::string describe() const override { return name; }
        const char* typeName() const override { return "LogCmd"; }

    private:
        std::vector<std::string>& state;
        std::string name;
    };

    using State = std::vector<std::string>;

} // namespace

TEST(CommandHistoryInsert, AddingAnEditEndsTheRedoTail) {
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

TEST(CommandHistoryInsert, InsertedStepKeepsTheRedoTail) {
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

TEST(CommandHistoryInsert, InsertedStepAtTheEndIsAPlainStep) {
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

TEST(CommandHistoryInsert, InsertedStepDuringPlayOutlivesStop) {
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

TEST(CommandHistoryInsert, FailedInsertLeavesTheHistoryAlone) {
    class FailCmd : public Command {
    public:
        bool execute() override { return false; }
        void undo() override { ADD_FAILURE() << "never applied"; }
        std::string describe() const override { return "Fail"; }
        const char* typeName() const override { return "FailCmd"; }
    };
    State state;
    CommandHistory history;
    history.addCommand(new LogCmd(state, "A"));
    history.undo();
    EXPECT_FALSE(history.insertCommand(new FailCmd()));
    EXPECT_TRUE(history.canRedo());
    EXPECT_FALSE(history.canUndo());
}

TEST(CommandHistoryInsert, InsertedStepIsFoundAndIsNoPlayStep) {
    State state;
    CommandHistory history;
    history.addCommand(new LogCmd(state, "A"));
    history.beginPlaySession();
    history.addCommand(new LogCmd(state, "Played"));
    const uint64_t played = history.top()->info.id;
    ASSERT_TRUE(history.insertCommand(new LogCmd(state, "Reload")));

    // below the floor with a newer id than the play-time step above it
    const std::vector<const UndoEntry*> entries = history.entries();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[1]->name(), "Reload");
    const uint64_t reload = entries[1]->info.id;
    EXPECT_GT(reload, played);
    EXPECT_EQ(history.find(reload), entries[1]);
    EXPECT_TRUE(history.isApplied(reload));
    EXPECT_FALSE(history.isPlayStep(reload)) << "an edit-time step, whatever its id";
    EXPECT_TRUE(history.isPlayStep(played));
    EXPECT_EQ(history.top()->info.id, played) << "undo takes the play-time step first";

    history.endPlaySession(true);
    state = {"A", "Reload"};
    ASSERT_EQ(history.entries().size(), 2u);
    EXPECT_FALSE(history.isPlayStep(reload));
    EXPECT_EQ(history.top()->info.id, reload);
}
