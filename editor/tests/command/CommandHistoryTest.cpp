// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Characterization tests of editor/command/CommandHistory: undo/redo order, merging, undo groups
// (mark/groupSince) and the play-session floor. A recording fake command logs what the history
// does to it.

#include "command/CommandHistory.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // what happened to the fake commands, in order: "+a" executed, "-a" undone, "~a" deleted,
    // "a<b" b merged into a (a is the new command, b the one before it)
    struct Journal {
        std::vector<std::string> events;

        std::string str() const{
            std::string text;
            for (const std::string& event : events){
                if (!text.empty())
                    text += " ";
                text += event;
            }
            return text;
        }
        void clear(){ events.clear(); }
    };

    class FakeCommand : public Command {
    public:
        FakeCommand(Journal& journal, std::string name, bool executes = true, bool merges = false, bool structural = true):
            journal(journal), name(std::move(name)), executes(executes), merges(merges), structural(structural){
        }
        ~FakeCommand() override{
            journal.events.push_back("~" + name);
        }

        bool execute() override{
            if (!executes)
                return false;
            journal.events.push_back("+" + name);
            return true;
        }
        void undo() override{
            journal.events.push_back("-" + name);
        }
        bool mergeWith(Command* other) override{
            if (!merges)
                return false;
            journal.events.push_back(name + "<" + static_cast<FakeCommand*>(other)->name);
            return true;
        }
        bool affectsStructure() const override{
            return structural;
        }
        std::string describe() const override{
            return name;
        }
        const char* typeName() const override{
            return "FakeCommand";
        }

    private:
        Journal& journal;
        std::string name;
        bool executes;
        bool merges;
        bool structural;
    };

    class CommandHistoryTest : public ::testing::Test {
    protected:
        Journal journal;
        std::unique_ptr<CommandHistory> history = std::make_unique<CommandHistory>(7);
        std::vector<std::pair<size_t, bool>> modified;

        void SetUp() override{
            CommandHistory::onSceneModified = [this](size_t sceneId, bool structural){
                modified.emplace_back(sceneId, structural);
            };
        }
        void TearDown() override{
            CommandHistory::onSceneModified = nullptr;
        }

        FakeCommand* make(const std::string& name, bool merges = false, bool structural = true){
            return new FakeCommand(journal, name, true, merges, structural);
        }
        void add(const std::string& name, bool merges = false){
            history->addCommand(make(name, merges));
        }
    };

}

TEST_F(CommandHistoryTest, StartsWithNothingToUndoOrRedo){
    EXPECT_FALSE(history->canUndo());
    EXPECT_FALSE(history->canRedo());
    EXPECT_EQ(history->position(), 0u);
}

TEST_F(CommandHistoryTest, AddCommandExecutesIt){
    EXPECT_TRUE(history->addCommand(make("a")));
    EXPECT_EQ(journal.str(), "+a");
    EXPECT_TRUE(history->canUndo());
    EXPECT_FALSE(history->canRedo());
    EXPECT_EQ(history->position(), 1u);
}

TEST_F(CommandHistoryTest, FailedCommandIsDeletedAndNotRecorded){
    EXPECT_FALSE(history->addCommand(new FakeCommand(journal, "x", false)));
    EXPECT_EQ(journal.str(), "~x");
    EXPECT_FALSE(history->canUndo());
    EXPECT_TRUE(modified.empty());
}

TEST_F(CommandHistoryTest, UndoesInReverseOrder){
    add("a");
    add("b");
    add("c");
    journal.clear();
    history->undo();
    history->undo();
    history->undo();
    EXPECT_EQ(journal.str(), "-c -b -a");
    EXPECT_FALSE(history->canUndo());
    EXPECT_TRUE(history->canRedo());
}

TEST_F(CommandHistoryTest, RedoesInOriginalOrder){
    add("a");
    add("b");
    history->undo();
    history->undo();
    journal.clear();
    history->redo();
    history->redo();
    EXPECT_EQ(journal.str(), "+a +b");
    EXPECT_FALSE(history->canRedo());
    EXPECT_TRUE(history->canUndo());
}

TEST_F(CommandHistoryTest, UndoWithNothingToUndoDoesNothing){
    history->undo();
    add("a");
    history->undo();
    journal.clear();
    history->undo();
    EXPECT_EQ(journal.str(), "");
    EXPECT_EQ(history->position(), 0u);
}

TEST_F(CommandHistoryTest, RedoWithNothingToRedoDoesNothing){
    add("a");
    journal.clear();
    history->redo();
    EXPECT_EQ(journal.str(), "");
    EXPECT_EQ(history->position(), 1u);
}

TEST_F(CommandHistoryTest, NewCommandDropsTheRedoTail){
    add("a");
    add("b");
    add("c");
    history->undo();
    history->undo();
    journal.clear();
    add("d");
    EXPECT_EQ(journal.str(), "+d ~b ~c");
    EXPECT_FALSE(history->canRedo());
    journal.clear();
    history->undo();
    history->undo();
    EXPECT_EQ(journal.str(), "-d -a");
}

TEST_F(CommandHistoryTest, MergeableCommandMergesIntoThePreviousOne){
    add("a", true);
    add("b", true);
    EXPECT_EQ(journal.str(), "+a +b b<a ~a");
    EXPECT_EQ(history->position(), 1u);
    journal.clear();
    history->undo();
    EXPECT_EQ(journal.str(), "-b");
    EXPECT_FALSE(history->canUndo());
}

TEST_F(CommandHistoryTest, CommandThatDeclinesTheMergeIsKeptSeparately){
    add("a", false);
    add("b", false);
    EXPECT_EQ(history->position(), 2u);
    EXPECT_EQ(journal.str(), "+a +b");
}

TEST_F(CommandHistoryTest, SetNoMergeOnTheNewCommandPreventsMerging){
    add("a", true);
    FakeCommand* b = make("b", true);
    b->setNoMerge();
    history->addCommand(b);
    EXPECT_EQ(history->position(), 2u);
    EXPECT_EQ(journal.str(), "+a +b");
}

TEST_F(CommandHistoryTest, AddCommandNoMergeNeverMerges){
    add("a", true);
    history->addCommandNoMerge(make("b", true));
    EXPECT_EQ(history->position(), 2u);
    EXPECT_EQ(journal.str(), "+a +b");
}

TEST_F(CommandHistoryTest, NoMergeOnThePreviousCommandPreventsMerging){
    history->addCommandNoMerge(make("a", true));
    add("b", true);
    EXPECT_EQ(history->position(), 2u);
}

TEST_F(CommandHistoryTest, MergeAfterUndoDropsTheRedoTailFirst){
    add("a", true);
    add("b");
    history->undo();
    journal.clear();
    add("c", true);
    EXPECT_EQ(journal.str(), "+c ~b c<a ~a");
    EXPECT_EQ(history->position(), 1u);
}

TEST_F(CommandHistoryTest, ClearDeletesEveryCommand){
    add("a");
    add("b");
    history->undo();
    journal.clear();
    history->clear();
    EXPECT_EQ(journal.str(), "~a ~b");
    EXPECT_FALSE(history->canUndo());
    EXPECT_FALSE(history->canRedo());
    EXPECT_EQ(history->position(), 0u);
}

TEST_F(CommandHistoryTest, DestructorDeletesEveryCommand){
    add("a");
    add("b");
    journal.clear();
    history.reset();
    EXPECT_EQ(journal.str(), "~a ~b");
}

TEST_F(CommandHistoryTest, OnSceneModifiedReportsAddUndoAndRedo){
    history->addCommand(make("a", false, true));
    history->addCommand(make("b", false, false));
    history->undo();
    history->redo();
    const std::vector<std::pair<size_t, bool>> expected = {{7, true}, {7, false}, {7, false}, {7, false}};
    EXPECT_EQ(modified, expected);
}

TEST_F(CommandHistoryTest, OnSceneModifiedReportsTheUndoneCommandsStructure){
    history->addCommand(make("a", false, true));
    modified.clear();
    history->undo();
    ASSERT_EQ(modified.size(), 1u);
    EXPECT_EQ(modified[0], std::make_pair(size_t(7), true));
}

TEST_F(CommandHistoryTest, NoCallbackIsFineToo){
    CommandHistory::onSceneModified = nullptr;
    add("a");
    history->undo();
    history->redo();
    EXPECT_EQ(journal.str(), "+a -a +a");
}

TEST_F(CommandHistoryTest, MarkStopsTheNextCommandMerging){
    add("a", true);
    history->mark();
    add("b", true);
    EXPECT_EQ(history->position(), 2u);
}

TEST_F(CommandHistoryTest, GroupSinceCollapsesCommandsIntoOneUndoStep){
    add("a");
    const size_t mark = history->mark();
    add("b");
    add("c");
    add("d");
    EXPECT_TRUE(history->groupSince(mark));
    EXPECT_EQ(history->position(), 2u);
    journal.clear();
    history->undo();
    EXPECT_EQ(journal.str(), "-d -c -b");
    EXPECT_TRUE(history->canUndo());
    journal.clear();
    history->redo();
    EXPECT_EQ(journal.str(), "+b +c +d");
}

TEST_F(CommandHistoryTest, GroupIsNeverMergedInto){
    const size_t mark = history->mark();
    add("a");
    add("b");
    ASSERT_TRUE(history->groupSince(mark));
    add("c", true);
    EXPECT_EQ(history->position(), 2u);
}

TEST_F(CommandHistoryTest, GroupIsStructuralWhenAnyCommandIs){
    const size_t mark = history->mark();
    history->addCommand(make("a", false, false));
    history->addCommand(make("b", false, true));
    ASSERT_TRUE(history->groupSince(mark));
    modified.clear();
    history->undo();
    ASSERT_EQ(modified.size(), 1u);
    EXPECT_TRUE(modified[0].second);
}

TEST_F(CommandHistoryTest, GroupOfNonStructuralCommandsIsNotStructural){
    const size_t mark = history->mark();
    history->addCommand(make("a", false, false));
    history->addCommand(make("b", false, false));
    ASSERT_TRUE(history->groupSince(mark));
    modified.clear();
    history->undo();
    ASSERT_EQ(modified.size(), 1u);
    EXPECT_FALSE(modified[0].second);
}

TEST_F(CommandHistoryTest, GroupSinceWithOneCommandLeavesItAsItIs){
    const size_t mark = history->mark();
    add("a");
    EXPECT_TRUE(history->groupSince(mark));
    EXPECT_EQ(history->position(), 1u);
    journal.clear();
    history->undo();
    EXPECT_EQ(journal.str(), "-a");
}

TEST_F(CommandHistoryTest, GroupSinceWithNoCommandsIsANoOp){
    add("a");
    const size_t mark = history->mark();
    EXPECT_TRUE(history->groupSince(mark));
    EXPECT_EQ(history->position(), 1u);
}

TEST_F(CommandHistoryTest, GroupSinceFailsWhenUndonePastTheMark){
    add("a");
    const size_t mark = history->mark();
    add("b");
    history->undo();
    history->undo();
    EXPECT_FALSE(history->groupSince(mark));
    EXPECT_EQ(history->position(), 0u);
    EXPECT_TRUE(history->canRedo());
}

TEST_F(CommandHistoryTest, GroupSinceFailsWithARedoTail){
    const size_t mark = history->mark();
    add("a");
    add("b");
    add("c");
    history->undo();
    EXPECT_FALSE(history->groupSince(mark));
    EXPECT_EQ(history->position(), 2u);
    EXPECT_TRUE(history->canRedo());
}

TEST_F(CommandHistoryTest, GroupSinceFailsWhenAPlaySessionBeganAfterTheMark){
    const size_t mark = history->mark();
    add("a");
    history->beginPlaySession();
    add("b");
    EXPECT_FALSE(history->groupSince(mark));
}

TEST_F(CommandHistoryTest, PlaySessionFloorStopsUndoOfEditCommands){
    add("a");
    history->beginPlaySession();
    EXPECT_FALSE(history->canUndo());
    journal.clear();
    history->undo();
    EXPECT_EQ(journal.str(), "");
    add("p");
    EXPECT_TRUE(history->canUndo());
    history->undo();
    EXPECT_FALSE(history->canUndo());
    EXPECT_EQ(journal.str(), "+p -p");
}

TEST_F(CommandHistoryTest, PlayCommandsNeverMergeIntoEditCommands){
    add("a", true);
    history->beginPlaySession();
    add("p", true);
    EXPECT_EQ(history->position(), 2u);
    EXPECT_EQ(journal.str(), "+a +p");
}

TEST_F(CommandHistoryTest, PlaySessionSetsTheEditRedoTailAside){
    add("a");
    add("b");
    history->undo();
    history->beginPlaySession();
    EXPECT_FALSE(history->canRedo());
}

TEST_F(CommandHistoryTest, RolledBackPlaySessionDropsPlayCommandsAndRestoresTheRedoTail){
    add("a");
    add("b");
    history->undo();
    history->beginPlaySession();
    add("p");
    add("q");
    journal.clear();
    history->endPlaySession(true);
    EXPECT_EQ(journal.str(), "~p ~q");
    EXPECT_EQ(history->position(), 1u);
    EXPECT_TRUE(history->canRedo());
    journal.clear();
    history->redo();
    EXPECT_EQ(journal.str(), "+b");
    history->undo();
    history->undo();
    EXPECT_EQ(journal.str(), "+b -b -a");
}

TEST_F(CommandHistoryTest, KeptPlaySessionKeepsPlayCommandsAndDropsTheRedoTail){
    add("a");
    add("b");
    history->undo();
    history->beginPlaySession();
    add("p");
    journal.clear();
    history->endPlaySession(false);
    EXPECT_EQ(journal.str(), "~b");
    EXPECT_FALSE(history->canRedo());
    EXPECT_EQ(history->position(), 2u);
    journal.clear();
    history->undo();
    history->undo();
    EXPECT_EQ(journal.str(), "-p -a");
}

TEST_F(CommandHistoryTest, KeptPlaySessionWithoutCommandsRestoresTheRedoTail){
    add("a");
    add("b");
    history->undo();
    history->beginPlaySession();
    history->endPlaySession(false);
    EXPECT_TRUE(history->canRedo());
    EXPECT_EQ(history->position(), 1u);
}

TEST_F(CommandHistoryTest, BeginPlaySessionTwiceKeepsTheFirstFloor){
    add("a");
    history->beginPlaySession();
    add("p");
    history->beginPlaySession();
    history->undo();
    EXPECT_FALSE(history->canUndo());
    EXPECT_EQ(history->position(), 1u);
}

TEST_F(CommandHistoryTest, EndPlaySessionWithoutOneDoesNothing){
    add("a");
    add("b");
    history->undo();
    journal.clear();
    history->endPlaySession(true);
    EXPECT_EQ(journal.str(), "");
    EXPECT_EQ(history->position(), 1u);
    EXPECT_TRUE(history->canRedo());
}

TEST_F(CommandHistoryTest, UndoReachesEditCommandsAgainAfterThePlaySession){
    add("a");
    history->beginPlaySession();
    history->endPlaySession(true);
    EXPECT_TRUE(history->canUndo());
    journal.clear();
    history->undo();
    EXPECT_EQ(journal.str(), "-a");
}

TEST_F(CommandHistoryTest, MarkAtThePlayFloorDoesNotTouchEditCommands){
    add("a", true);
    history->beginPlaySession();
    EXPECT_EQ(history->mark(), 1u);
    history->endPlaySession(true);
    add("b", true);
    // the edit command stayed mergeable: mark() below the floor left it alone
    EXPECT_EQ(history->position(), 1u);
}

TEST_F(CommandHistoryTest, GroupSinceInsideAPlaySessionGroupsPlayCommands){
    add("a");
    history->beginPlaySession();
    const size_t mark = history->mark();
    add("p");
    add("q");
    EXPECT_TRUE(history->groupSince(mark));
    journal.clear();
    history->undo();
    EXPECT_EQ(journal.str(), "-q -p");
    EXPECT_FALSE(history->canUndo());
}

TEST_F(CommandHistoryTest, ClearEndsThePlaySessionAndDeletesTheRedoTail){
    add("a");
    add("b");
    history->undo();
    history->beginPlaySession();
    journal.clear();
    history->clear();
    EXPECT_EQ(journal.str(), "~a ~b");
    add("c");
    EXPECT_TRUE(history->canUndo());
    history->undo();
    EXPECT_FALSE(history->canUndo());
}
