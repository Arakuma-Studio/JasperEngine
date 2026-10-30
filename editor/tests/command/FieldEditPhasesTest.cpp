// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The field windows' undo contract (docs/specs/S27-editor-tools.md R3, §6.1, AC10): a session is
// one named undo step whatever the changes in it, Esc restores and records nothing, a new session
// commits the open one.

#include "widget/FieldEdit.h"

#include "UndoFakeCommand.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    int owner;   // a window's address
    int other;

}

TEST(FieldEditPhases, OneSessionIsOneStep) {
    UndoStatics statics;
    CommandHistory history;
    FakeState state;
    ui::FieldEditSession::begin(&history, "Change Color", &owner);
    EXPECT_TRUE(ui::FieldEditSession::isOpen());
    EXPECT_TRUE(ui::FieldEditSession::isOpenFor(&owner));
    // a drag in the picker: many changes (merging or not)
    for (int i = 1; i <= 20; i++) {
        auto* cmd = new UndoFake(state, "set " + std::to_string(i), i);
        cmd->mergeKey = i % 3;   // some merge, some do not
        history.addCommand(cmd);
    }
    EXPECT_EQ(state.value, 20);
    // another owner cannot end it
    ui::FieldEditSession::commit(&other);
    EXPECT_TRUE(ui::FieldEditSession::isOpen());
    ui::FieldEditSession::commit(&owner);
    EXPECT_FALSE(ui::FieldEditSession::isOpen());
    ASSERT_EQ(history.appliedCount(), 1u);
    EXPECT_EQ(history.top()->name(), "Change Color");
    history.undo();
    EXPECT_EQ(state.value, 0);
    history.redo();
    EXPECT_EQ(state.value, 20);
}

TEST(FieldEditPhases, CancelRestoresAndRecordsNothing) {
    UndoStatics statics;
    CommandHistory history;
    FakeState state;
    history.addCommand(new UndoFake(state, "before", 5));
    const size_t steps = history.appliedCount();
    ui::FieldEditSession::begin(&history, "Change Color", &owner);
    history.addCommand(new UndoFake(state, "a", 7));
    history.addCommand(new UndoFake(state, "b", 9));
    ui::FieldEditSession::cancel(&owner);
    EXPECT_EQ(state.value, 5);
    EXPECT_EQ(history.appliedCount(), steps);
    EXPECT_FALSE(ui::FieldEditSession::isOpen());
}

TEST(FieldEditPhases, RetargetCommitsThePreviousSession) {
    UndoStatics statics;
    CommandHistory history;
    FakeState state;
    ui::FieldEditSession::begin(&history, "Change Light Color", &owner);
    history.addCommand(new UndoFake(state, "light", 3));
    // the window opened on another field: its session starts, the first is one step
    ui::FieldEditSession::begin(&history, "Change Fog Color", &owner);
    history.addCommand(new UndoFake(state, "fog", 4));
    ui::FieldEditSession::commit(&owner);
    ASSERT_EQ(history.appliedCount(), 2u);
    EXPECT_EQ(history.top()->name(), "Change Fog Color");
    // a session without changes adds nothing
    ui::FieldEditSession::begin(&history, "Change Nothing", &owner);
    ui::FieldEditSession::commit(&owner);
    EXPECT_EQ(history.appliedCount(), 2u);
    // the glue's beginFieldEdit goes to the window that opened last
    static CommandHistory* handled = nullptr;
    ui::setBeginEditHandler([](CommandHistory* h, const char*) { handled = h; });
    ui::beginFieldEdit(&history, "Curve");
    EXPECT_EQ(handled, &history);
    ui::setBeginEditHandler(nullptr);
}
