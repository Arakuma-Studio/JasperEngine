// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A save between a step and its undo (CommandHistory::markSaved): the undo calls onUndoPastSave
// (the editor marks the scene modified, its file holds the step); an undo with no save since does
// not. A save also stops the step before it merging with the next. test_undo_save_state.py checks
// it in a running editor.

#include "UndoFakeCommand.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    struct PastSave {
        std::vector<size_t> scenes;
        PastSave() {
            CommandHistory::onUndoPastSave = [this](size_t sceneId) { scenes.push_back(sceneId); };
        }
    };

}

TEST(CommandHistorySave, AnUndoPastASaveIsReported) {
    UndoStatics statics;
    PastSave past;
    FakeState state;
    CommandHistory history(7);
    history.addCommand(new UndoFake(state, "a", 1));
    history.addCommand(new UndoFake(state, "b", 2));
    history.markSaved();
    history.addCommand(new UndoFake(state, "c", 3));

    ASSERT_TRUE(history.undo());   // c: done after the save
    EXPECT_TRUE(past.scenes.empty());
    ASSERT_TRUE(history.undo());   // b: in the file
    EXPECT_EQ(past.scenes, (std::vector<size_t>{7}));
    ASSERT_TRUE(history.undo());   // a too
    EXPECT_EQ(past.scenes, (std::vector<size_t>{7, 7}));
    EXPECT_EQ(state.value, 0);
}

TEST(CommandHistorySave, TheLastSaveDecides) {
    UndoStatics statics;
    PastSave past;
    FakeState state;
    CommandHistory history(3);
    history.addCommand(new UndoFake(state, "a", 1));
    history.markSaved();
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(past.scenes.size(), 1u);
    // saved without it, then redone and undone: the file holds the undone state
    history.markSaved();
    ASSERT_TRUE(history.redo());
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(past.scenes.size(), 1u);
    // redone and saved: its undo is past the save again
    ASSERT_TRUE(history.redo());
    history.markSaved();
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(past.scenes.size(), 2u);
}

TEST(CommandHistorySave, ASaveStopsAMerge) {
    UndoStatics statics;
    FakeState state;
    CommandHistory history(1);
    auto* first = new UndoFake(state, "first", 1);
    first->mergeKey = 9;
    history.addCommand(first);
    history.markSaved();
    auto* second = new UndoFake(state, "second", 2);
    second->mergeKey = 9;
    history.addCommand(second);
    EXPECT_EQ(history.appliedCount(), 2u);
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(state.value, 1);
}

TEST(CommandHistorySave, AGroupHoldingASavedPartIsPastTheSave) {
    UndoStatics statics;
    PastSave past;
    FakeState state;
    CommandHistory history(5);
    const size_t mark = history.mark();
    history.addCommand(new UndoFake(state, "a", 1));
    history.markSaved();
    history.addCommand(new UndoFake(state, "b", 2));
    ASSERT_TRUE(history.groupSince(mark, "batch"));
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(past.scenes, (std::vector<size_t>{5}));
    EXPECT_EQ(state.value, 0);
}
