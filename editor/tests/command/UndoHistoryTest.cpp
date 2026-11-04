// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S24 (docs/specs/S24-undo.md) CommandHistory semantics: names and ids, caps, no-op drops,
// interactive edits, selection steps and the off-thread release, on a recording fake command.

#include "UndoFakeCommand.h"
#include "util/BackgroundRelease.h"

#include "command/CommandGroup.h"
#include "command/CommandNames.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

// every command names itself: Command is abstract in describe() and typeName()
static_assert(std::is_abstract_v<Command>, "Command must stay abstract");
namespace {
    struct NamesOnlyTypeName : Command {
        bool execute() override { return true; }
        void undo() override {}
        const char* typeName() const override { return "x"; }
    };
    static_assert(std::is_abstract_v<NamesOnlyTypeName>, "describe() must be pure");
}

namespace {

    class CommandHistoryTests : public ::testing::Test {
    protected:
        UndoStatics statics;
        FakeState state;

        UndoFake* make(const std::string& name, int value, int mergeKey = 0) {
            auto* cmd = new UndoFake(state, name, value);
            cmd->mergeKey = mergeKey;
            return cmd;
        }
    };

}

TEST_F(CommandHistoryTests, NamesCapturedAtFirstExecute) {
    CommandHistory history(1);
    UndoFake* cmd = make("Move Player", 1);
    history.addCommand(cmd);
    ASSERT_NE(history.top(), nullptr);
    EXPECT_EQ(history.top()->name(), "Move Player");
    const uint64_t id = history.top()->info.id;

    // undo and redo never rename
    cmd->label = "Renamed later";
    history.undo();
    history.redo();
    EXPECT_EQ(history.top()->name(), "Move Player");

    // a merge: the newest name, the oldest id
    history.addCommand(make("Move Hero", 2, 7));
    history.addCommand(make("Move Hero 2", 3, 7));
    const UndoEntry* merged = history.top();
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->name(), "Move Hero 2");
    EXPECT_GT(merged->info.id, id);
    EXPECT_EQ(history.entries().size(), 2u);
    const uint64_t mergedId = merged->info.id;
    history.addCommand(make("Move Hero 3", 4, 7));
    EXPECT_EQ(history.top()->info.id, mergedId);
}

TEST_F(CommandHistoryTests, IdsAreGlobalAndIncreasing) {
    CommandHistory a(1);
    CommandHistory b(2);
    a.addCommand(make("a1", 1));
    b.addCommand(make("b1", 2));
    a.addCommand(make("a2", 3));
    EXPECT_LT(a.entries()[0]->info.id, b.entries()[0]->info.id);
    EXPECT_LT(b.entries()[0]->info.id, a.entries()[1]->info.id);
}

TEST_F(CommandHistoryTests, NamesAreClippedTo80CodePoints) {
    CommandHistory history(1);
    std::string longName = "Set Intensity (";
    while (longName.size() < 220) longName += "\xC3\xA9";  // é, two bytes: 100 code points and more
    history.addCommand(make(longName + ")", 1));
    const std::string& name = history.top()->name();
    size_t points = 0;
    for (unsigned char c : name) if ((c & 0xC0) != 0x80) points++;
    EXPECT_EQ(points, CommandNames::MAX_LENGTH);
    EXPECT_EQ(name.substr(name.size() - 3), "\xE2\x80\xA6");
}

TEST_F(CommandHistoryTests, TrimByCountAndBytes) {
    // The byte limit counts every live history of the process, and the oldest step of any goes
    // first: the headless projects other suites of this binary keep would take part. The check
    // runs in a process of its own (a threadsafe death test re-runs only this test).
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        CommandHistory::maxEntries = 10;
        CommandHistory a(1);
        CommandHistory b(2);
        for (int i = 0; i < 12; i++) a.addCommand(make("a" + std::to_string(i), i + 1));
        // the oldest went first, count per history
        EXPECT_EQ(a.appliedCount(), 10u);
        EXPECT_EQ(a.getTrimmedCount(), 2u);
        EXPECT_EQ(a.entries().front()->name(), "a2");

        // bytes over both histories: the lowest id of any goes
        CommandHistory::maxEntries = 1000;
        CommandHistory::memoryLimit = 1000;
        state.deleted.clear();
        auto* big = make("b0", 100);
        big->bytes = 500;
        b.addCommand(big);
        // a holds 10 x 64 = 640, b 500: 1140 > 1000, so a's oldest steps go, b's newest stays
        EXPECT_LE(CommandHistory::totalBytes(), 1000u);
        EXPECT_FALSE(state.deleted.empty());
        if (!state.deleted.empty()) { EXPECT_EQ(state.deleted.front(), "a2"); }
        EXPECT_TRUE(b.top() && b.top()->name() == "b0");
        std::fflush(nullptr);
        std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), "");
}

TEST_F(CommandHistoryTests, TrimNeverTakesTheNewestEntryPlayStepsOrAHeldGroup) {
    CommandHistory::memoryLimit = 100;
    CommandHistory history(1);
    auto* huge = make("huge", 1);
    huge->bytes = 1000;
    history.addCommand(huge);
    // the newest step stays even over the budget
    ASSERT_NE(history.top(), nullptr);
    EXPECT_EQ(history.top()->name(), "huge");

    CommandHistory::memoryLimit = 0;
    CommandHistory played(2);
    played.addCommand(make("edit", 1));
    played.beginPlaySession();
    auto* p1 = make("p1", 2);
    p1->bytes = 1000;
    played.addCommand(p1);
    played.addCommand(make("p2", 3));
    CommandHistory::memoryLimit = 100;
    played.addCommand(make("p3", 4));
    // the edit-time step may go, play steps never do
    for (const UndoEntry* entry : played.entries()) EXPECT_NE(entry->name(), "edit");
    EXPECT_EQ(played.entries().size(), 3u);
    played.endPlaySession(true);

    CommandHistory::memoryLimit = 0;
    CommandHistory held(3);
    held.addCommand(make("before", 1));
    const size_t mark = held.mark();
    auto scope = std::make_unique<UndoGroupScope>(&held);
    auto* g1 = make("g1", 2);
    g1->bytes = 1000;
    held.addCommand(g1);
    CommandHistory::memoryLimit = 100;
    held.addCommand(make("g2", 3));
    // the step before the scope's mark may go, the ones after it wait for the scope
    EXPECT_EQ(held.getTrimmedCount(), 1u);
    ASSERT_EQ(held.entries().size(), 2u);
    EXPECT_TRUE(held.groupSince(mark, "Group"));
    scope.reset();
}

TEST_F(CommandHistoryTests, MarksSurviveTrimming) {
    CommandHistory::maxEntries = 50;
    CommandHistory history(1);
    for (int i = 0; i < 50; i++) history.addCommand(make("x" + std::to_string(i), i + 1));
    const size_t mark = history.mark();
    history.addCommand(make("y1", 100));
    history.addCommand(make("y2", 101));
    // two trimmed meanwhile: the absolute mark still groups
    EXPECT_EQ(history.getTrimmedCount(), 2u);
    EXPECT_TRUE(history.groupSince(mark, "Y"));
    EXPECT_EQ(history.top()->name(), "Y");

    // a trimmed mark refuses
    const size_t old = history.getTrimmedCount() - 1;
    EXPECT_FALSE(history.groupSince(old));
}

TEST_F(CommandHistoryTests, NoOpDropped) {
    CommandHistory history(1);
    history.addCommand(make("a", 1));
    history.undo();
    ASSERT_TRUE(history.canRedo());
    // the value already is 0: changes nothing, no step, the redo tail stays
    auto* same = make("same", 0);
    EXPECT_TRUE(history.addCommand(same));
    EXPECT_EQ(history.entries().size(), 1u);
    EXPECT_TRUE(history.canRedo());
    EXPECT_FALSE(history.canUndo());

    // a merge back to where it started is dropped too
    history.redo();
    history.addCommand(make("drag", 5, 3));
    history.addCommand(make("drag", 1, 3));
    EXPECT_EQ(history.entries().size(), 1u);
    EXPECT_EQ(state.value, 1);
}

TEST_F(CommandHistoryTests, InteractiveCommitCancel) {
    CommandHistory history(1);
    history.addCommand(make("first", 1));
    history.addCommand(make("redo me", 2));
    history.undo();
    ASSERT_TRUE(history.canRedo());

    // commit: 30 merging and non-merging pushes become one step
    {
        InteractiveEdit edit = history.beginInteractive("Drag");
        EXPECT_FALSE(history.canRedo());
        for (int i = 0; i < 30; i++) {
            history.addCommand(make("step", 10 + i, i % 2 == 0 ? 9 : 0));
        }
        edit.commit();
        EXPECT_FALSE(edit.isOpen());
    }
    EXPECT_EQ(history.entries().size(), 2u);
    EXPECT_EQ(history.top()->name(), "Drag");
    EXPECT_FALSE(history.canRedo());   // a kept edit ends the redo tail
    EXPECT_EQ(state.value, 39);
    history.undo();
    EXPECT_EQ(state.value, 1);
    history.redo();
    EXPECT_EQ(state.value, 39);

    // cancel: the values and the redo tail come back
    history.undo();
    ASSERT_TRUE(history.canRedo());
    {
        InteractiveEdit edit = history.beginInteractive("Drag");
        history.addCommand(make("s1", 50));
        history.addCommand(make("s2", 60, 4));
        history.addCommand(make("s3", 70, 4));
        edit.cancel();
    }
    EXPECT_EQ(state.value, 1);
    EXPECT_TRUE(history.canRedo());
    EXPECT_EQ(history.entries().size(), 2u);

    // Ctrl+Z in an open edit cancels it and undoes nothing more
    {
        InteractiveEdit edit = history.beginInteractive("Drag");
        history.addCommand(make("s", 80));
        EXPECT_TRUE(history.undo());
        EXPECT_FALSE(edit.isOpen());
    }
    EXPECT_EQ(state.value, 1);
    EXPECT_EQ(history.appliedCount(), 1u);

    // a no-op commit restores the redo tail
    {
        InteractiveEdit edit = history.beginInteractive("Drag");
        history.addCommand(make("there", 5, 6));
        history.addCommand(make("back", 1, 6));
        edit.commit();
    }
    EXPECT_TRUE(history.canRedo());
    EXPECT_EQ(history.entries().size(), 2u);

    // destroyed open, it commits
    {
        InteractiveEdit edit = history.beginInteractive("Forgotten");
        history.addCommand(make("kept", 90));
        history.addCommand(make("kept 2", 91));
    }
    EXPECT_FALSE(history.hasOpenInteractive());
    EXPECT_EQ(history.top()->name(), "Forgotten");
    EXPECT_EQ(state.value, 91);

    // a second begin commits the first
    InteractiveEdit first = history.beginInteractive("One");
    history.addCommand(make("o", 100));
    InteractiveEdit second = history.beginInteractive("Two");
    EXPECT_FALSE(first.isOpen());
    EXPECT_TRUE(second.isOpen());
    second.cancel();
    EXPECT_EQ(state.value, 100);
}

TEST_F(CommandHistoryTests, InteractiveEditOfAClosedHistoryIsHarmless) {
    InteractiveEdit edit;
    {
        CommandHistory history(1);
        edit = history.beginInteractive("Drag");
        history.addCommand(make("x", 1));
    }
    EXPECT_FALSE(edit.isOpen());
    edit.commit();
    edit.cancel();
}

TEST_F(CommandHistoryTests, SelectionSteps) {
    // a fake scene selection read and written through the hooks
    SelectionState selection;
    CommandHistory::selectionHooks.read = [&selection](size_t, SelectionState& out) { out = selection; return true; };
    CommandHistory::selectionHooks.restore = [&selection](size_t, const SelectionState& in) { selection = in; };

    class Select : public UndoFake {
    public:
        SelectionState& target;
        SelectionState after;
        Select(FakeState& state, SelectionState& target, SelectionState after, int value)
            : UndoFake(state, "Select", value), target(target), after(std::move(after)) {}
        bool execute() override { UndoFake::execute(); target = after; return true; }
        bool isNoOp() const override { return false; }
    };

    CommandHistory history(1);
    SelectionState one;
    one.entities = {5};
    one.active = 5;
    history.addCommand(new Select(state, selection, one, 1));
    SelectionState two;
    two.entities = {5, 6};
    two.active = 6;
    history.addCommand(new Select(state, selection, two, 2));
    EXPECT_EQ(history.entries().size(), 2u);
    EXPECT_EQ(history.entries()[1]->info.selectionBefore, one);
    EXPECT_EQ(history.entries()[1]->info.selectionAfter, two);

    // undo restores the selection before, redo the one after
    history.undo();
    EXPECT_EQ(selection, one);
    history.redo();
    EXPECT_EQ(selection, two);

    // a selection change inside a command belongs to its step
    EXPECT_FALSE(CommandHistory::isApplying());
    bool insideApplying = false;
    class Probe : public UndoFake {
    public:
        bool& inside;
        Probe(FakeState& state, bool& inside): UndoFake(state, "Probe", 77), inside(inside) {}
        bool execute() override { inside = CommandHistory::isApplying(); return UndoFake::execute(); }
    };
    history.addCommand(new Probe(state, insideApplying));
    EXPECT_TRUE(insideApplying);
}

TEST_F(CommandHistoryTests, BackgroundRelease) {
    // the sink keeps what it gets: nothing it holds is deleted on this thread
    std::vector<std::unique_ptr<Command>> sink;
    CommandHistory::offThreadRelease = [&sink](std::unique_ptr<Command> cmd) {
        sink.push_back(std::move(cmd));
    };
    CommandHistory history(1);
    auto* big = make("big", 1);
    big->bytes = CommandHistory::OFF_THREAD_RELEASE_BYTES;
    big->offThread = true;
    auto* bigInline = make("big inline", 2);
    bigInline->bytes = CommandHistory::OFF_THREAD_RELEASE_BYTES;
    auto* small = make("small", 3);
    small->offThread = true;
    history.addCommand(big);
    history.addCommand(bigInline);
    history.addCommand(small);
    history.undo();
    history.undo();
    history.undo();
    // a new step frees the redo tail: only the big off-thread one goes to the sink
    state.deleted.clear();
    history.addCommand(make("new", 4));
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink[0]->name(), "big");
    EXPECT_EQ(state.deleted.size(), 2u);   // big inline + small, deleted on this thread

    // what CommandHandle::remove clears goes the same way
    sink.clear();
    CommandHistory* handled = CommandHandle::get(4242);
    auto* cleared = make("cleared", 5);
    cleared->bytes = CommandHistory::OFF_THREAD_RELEASE_BYTES * 2;
    cleared->offThread = true;
    handled->addCommand(cleared);
    CommandHandle::remove(4242);
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink[0]->name(), "cleared");
}

TEST_F(CommandHistoryTests, DeferredReleaseWaitsForFlush) {
    CommandHistory::deferReleases = true;
    CommandHistory history(1);
    history.addCommand(make("a", 1));
    state.deleted.clear();
    history.addCommand(make("noop", 1));
    EXPECT_TRUE(state.deleted.empty());
    CommandHistory::flushReleased();
    ASSERT_EQ(state.deleted.size(), 1u);
    EXPECT_EQ(state.deleted[0], "noop");
}

TEST_F(CommandHistoryTests, BeforeMutationRefuses) {
    CommandHistory history(1);
    history.addCommand(make("a", 1));
    CommandHistory::beforeMutation = [](std::string& why) { why = "Play mode is starting"; return false; };
    EXPECT_FALSE(history.addCommand(make("b", 2)));
    EXPECT_FALSE(history.undo());
    EXPECT_EQ(CommandHistory::lastBusyRefusal(), "Play mode is starting");
    EXPECT_EQ(state.value, 1);
    CommandHistory::beforeMutation = nullptr;
    EXPECT_TRUE(history.undo());
}

TEST_F(CommandHistoryTests, PushCommitsAnotherHistorysEdit) {
    CommandHistory a(1);
    CommandHistory b(2);
    InteractiveEdit edit = a.beginInteractive("Drag");
    a.addCommand(make("drag", 1));
    b.addCommand(make("other", 2));
    EXPECT_FALSE(edit.isOpen());
    EXPECT_EQ(a.entries().size(), 1u);
}

TEST_F(CommandHistoryTests, GroupNames) {
    CommandHistory history(1);
    const size_t mark = history.mark();
    history.addCommand(make("Move Player", 1));
    history.addCommand(make("Rotate Player", 2));
    history.addCommand(make("Scale Player", 3));
    ASSERT_TRUE(history.groupSince(mark));
    EXPECT_EQ(history.top()->name(), "Move Player (+2 more)");
    EXPECT_STREQ(history.top()->cmd->typeName(), "CommandGroup");

    const size_t labelled = history.mark();
    history.addCommand(make("x", 4));
    history.addCommand(make("y", 5));
    ASSERT_TRUE(history.groupSince(labelled, "Batch (2 calls)"));
    EXPECT_EQ(history.top()->name(), "Batch (2 calls)");
}

TEST_F(CommandHistoryTests, AgentStepsCarryTheClientName) {
    CommandHistory history(1);
    UndoOrigin origin;
    origin.kind = UndoOriginKind::Agent;
    origin.client = "Claude Code";
    origin.call = 9;
    {
        UndoOriginScope scope(origin);
        history.addCommand(make("Set Intensity (Sun)", 1));
    }
    EXPECT_EQ(history.top()->name(), "Claude Code: Set Intensity (Sun)");
    EXPECT_EQ(history.top()->info.origin.kind, UndoOriginKind::Agent);
    EXPECT_EQ(UndoOriginScope::current().kind, UndoOriginKind::User);
    // an agent step never merges into a user step
    history.addCommand(make("user", 2, 1));
    {
        UndoOriginScope scope(origin);
        history.addCommand(make("agent", 3, 1));
    }
    EXPECT_EQ(history.entries().size(), 3u);
}
