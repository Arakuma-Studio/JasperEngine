// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S24 R17-R21: the one Ctrl+Z timeline over a scene's history and the project's, linked steps
// and the undo routing.

#include "UndoFakeCommand.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    constexpr uint32_t SCENE_A = 9101;
    constexpr uint32_t SCENE_B = 9102;

    class UndoTimelineTests : public ::testing::Test {
    protected:
        UndoStatics statics;
        FakeState state;
        CommandHistory project{0};
        CommandHistory* a = nullptr;
        CommandHistory* b = nullptr;
        uint32_t selected = SCENE_A;
        std::vector<std::string> warnings;

        void SetUp() override {
            UndoTimeline::setProjectHistory(&project);
            UndoTimeline::selectedScene = [this]() { return selected; };
            UndoTimeline::warn = [this](const std::string& message) { warnings.push_back(message); };
            UndoTimeline::historyName = [](uint32_t sceneId) -> std::string {
                if (sceneId == 0) return "Project";
                return sceneId == SCENE_A ? "Scene 'A'" : "Scene 'B'";
            };
            a = CommandHandle::get(SCENE_A);
            b = CommandHandle::get(SCENE_B);
        }
        void TearDown() override {
            CommandHandle::remove(SCENE_A);
            CommandHandle::remove(SCENE_B);
        }

        UndoFake* make(const std::string& name, int value) {
            return new UndoFake(state, name, value);
        }
        std::string undoName(uint32_t sceneId) {
            std::vector<UndoTimeline::Step> steps;
            if (!UndoTimeline::undo(sceneId, nullptr, &steps)) return "-";
            std::string names;
            for (const UndoTimeline::Step& step : steps) names += (names.empty() ? "" : "+") + step.name;
            return names;
        }
        std::string redoName(uint32_t sceneId) {
            std::vector<UndoTimeline::Step> steps;
            if (!UndoTimeline::redo(sceneId, nullptr, &steps)) return "-";
            std::string names;
            for (const UndoTimeline::Step& step : steps) names += (names.empty() ? "" : "+") + step.name;
            return names;
        }
    };

}

TEST_F(UndoTimelineTests, Interleave) {
    a->addCommand(make("A1", 1));
    project.addCommand(make("P2", 2));
    b->addCommand(make("B3", 3));

    // in B: B3, then P2, then nothing (A1 is another scene's)
    EXPECT_EQ(UndoTimeline::undoLabel(SCENE_B), "Undo B3");
    EXPECT_EQ(undoName(SCENE_B), "B3");
    EXPECT_EQ(undoName(SCENE_B), "P2");
    EXPECT_EQ(undoName(SCENE_B), "-");
    EXPECT_EQ(UndoTimeline::undoLabel(SCENE_B), "Undo");

    // in A: A1; redo gives A1 then P2
    EXPECT_EQ(undoName(SCENE_A), "A1");
    EXPECT_EQ(redoName(SCENE_A), "A1");
    EXPECT_EQ(redoName(SCENE_A), "P2");
    EXPECT_EQ(redoName(SCENE_A), "-");

    // a new step in A frees P's redo tail and leaves B's alone
    project.undo();
    ASSERT_TRUE(project.canRedo());
    ASSERT_TRUE(b->canRedo());
    a->addCommand(make("A4", 4));
    EXPECT_FALSE(project.canRedo());
    EXPECT_TRUE(b->canRedo());

    // a step of P frees the selected scene's redo tail (A), not B's
    a->undo();
    ASSERT_TRUE(a->canRedo());
    selected = SCENE_A;
    project.addCommand(make("P5", 5));
    EXPECT_FALSE(a->canRedo());
    EXPECT_TRUE(b->canRedo());
}

TEST_F(UndoTimelineTests, LinkedBatch) {
    a->addCommand(make("A batch", 1));
    project.addCommand(make("P batch", 2));
    UndoTimeline::link({a, &project});

    // one call undoes both, one redo redoes both
    EXPECT_EQ(undoName(SCENE_A), "P batch+A batch");
    EXPECT_FALSE(a->canUndo());
    EXPECT_FALSE(project.canUndo());
    EXPECT_EQ(redoName(SCENE_A), "A batch+P batch");
    EXPECT_TRUE(warnings.empty());

    // a newer asset step is undone first, then the batch as a whole
    project.addCommand(make("P newer", 3));
    EXPECT_EQ(undoName(SCENE_A), "P newer");
    EXPECT_EQ(undoName(SCENE_A), "P batch+A batch");
    EXPECT_TRUE(warnings.empty());
}

TEST_F(UndoTimelineTests, LinkedStepUnderNewerEditsStaysWithAWarning) {
    // a batch over scenes A and B, then a newer edit in B
    b->addCommand(make("B part", 1));
    a->addCommand(make("A part", 2));
    UndoTimeline::link({a, b});
    b->addCommand(make("B newer", 3));

    // undo in A takes A's part; B's part is under B's newer edit and stays applied
    std::vector<UndoTimeline::Step> steps;
    ASSERT_TRUE(UndoTimeline::undo(SCENE_A, nullptr, &steps));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].name, "A part");
    EXPECT_TRUE(b->isApplied(b->entries()[0]->info.id));
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0], "Batch step partly undone: Scene 'B' has newer edits");

    // B's newer edit undone, B's part is on top again: undoing it in B takes it alone (A's
    // part is undone already)
    warnings.clear();
    EXPECT_EQ(undoName(SCENE_B), "B newer");
    EXPECT_EQ(undoName(SCENE_B), "B part");
    EXPECT_TRUE(warnings.empty());

    // redo in A brings both parts back once both are next in their histories
    EXPECT_EQ(redoName(SCENE_A), "A part+B part");
}

TEST_F(UndoTimelineTests, JumpToAndLabels) {
    a->addCommand(make("A1", 1));
    project.addCommand(make("P2", 2));
    a->addCommand(make("A3", 3));
    const uint64_t first = a->entries()[0]->info.id;
    EXPECT_EQ(UndoTimeline::undoLabel(SCENE_A), "Undo A3");
    ASSERT_TRUE(UndoTimeline::jumpTo(SCENE_A, first));
    EXPECT_EQ(state.value, 1);
    EXPECT_EQ(UndoTimeline::redoLabel(SCENE_A), "Redo P2");
    ASSERT_TRUE(UndoTimeline::jumpTo(SCENE_A, a->entries()[1]->info.id));
    EXPECT_EQ(state.value, 3);
    ASSERT_TRUE(UndoTimeline::jumpTo(SCENE_A, 0));
    EXPECT_FALSE(UndoTimeline::canUndo(SCENE_A));
    std::string why;
    EXPECT_FALSE(UndoTimeline::jumpTo(SCENE_A, 999999, &why));
    EXPECT_FALSE(why.empty());

    // entries: T(A) by id, applied flags
    UndoTimeline::redo(SCENE_A);
    const std::vector<UndoTimeline::EntryView> views = UndoTimeline::entries(SCENE_A, false);
    ASSERT_EQ(views.size(), 3u);
    EXPECT_TRUE(views[0].applied);
    EXPECT_FALSE(views[1].applied);
    EXPECT_TRUE(views[1].project);
}

TEST_F(UndoTimelineTests, PlayHidesTheEditTimeAssetSteps) {
    project.addCommand(make("P before", 1));
    a->beginPlaySession();
    EXPECT_FALSE(UndoTimeline::canUndo(SCENE_A));
    a->addCommand(make("A play", 2));
    EXPECT_EQ(undoName(SCENE_A), "A play");
    EXPECT_EQ(undoName(SCENE_A), "-");
    a->endPlaySession(true);
    EXPECT_EQ(undoName(SCENE_A), "P before");
}

TEST_F(UndoTimelineTests, RoutingIgnoresWindowFocus) {
    using UndoTimeline::FocusedWindow;
    using UndoTimeline::Target;
    for (FocusedWindow window : {FocusedWindow::Resources, FocusedWindow::Scene, FocusedWindow::Hierarchy,
                                 FocusedWindow::Inspector, FocusedWindow::Console, FocusedWindow::None}) {
        EXPECT_EQ(UndoTimeline::resolveTarget(false, false, window), Target::Timeline);
    }
    EXPECT_EQ(UndoTimeline::resolveTarget(true, false, FocusedWindow::Resources), Target::TextField);
    EXPECT_EQ(UndoTimeline::resolveTarget(false, true, FocusedWindow::Scene), Target::CodeEditor);
    EXPECT_EQ(UndoTimeline::resolveTarget(false, false, FocusedWindow::Code), Target::CodeEditor);
    EXPECT_EQ(UndoTimeline::resolveTarget(true, true, FocusedWindow::Code), Target::TextField);
}

TEST_F(UndoTimelineTests, ClosingASceneUnlinksTheProjectSteps) {
    a->addCommand(make("A part", 1));
    project.addCommand(make("P part", 2));
    const uint64_t link = UndoTimeline::link({a, &project});
    ASSERT_EQ(project.top()->info.linkId, link);
    CommandHandle::remove(SCENE_A);
    a = nullptr;
    EXPECT_EQ(project.top()->info.linkId, 0u);
    CommandHandle::get(SCENE_A);
}
