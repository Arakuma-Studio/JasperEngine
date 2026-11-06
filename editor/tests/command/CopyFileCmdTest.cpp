// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Project window's copies and moves (project-window spec 6.3): taken names give Unity's free
// name for moves as for copies, a folder never goes into itself, a move into its own folder is no
// step, the report says where things landed, Duplicate names its step.

#include "asset/FileOpsTestProject.h"

#include "asset/AssetOps.h"
#include "command/type/CopyFileCmd.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    class CopyFileCmdTest : public FileOpsProjectTest {
    protected:
        void prepare() override {
            dir.write("assets/a/hero.png", "first hero");
            dir.write("assets/b/hero.png", "second hero");
            dir.write("assets/b/other.png", "other");
            dir.write("assets/Sprites/ship.png", "ship");
            dir.write("assets/Sprites/sub/deep.png", "deep");
            dir.write("assets/my.folder/inside.png", "inside");
        }

        std::vector<std::string> paths(std::initializer_list<const char*> relatives) {
            std::vector<std::string> out;
            for (const char* relative : relatives) out.push_back(abs(relative).string());
            return out;
        }
    };

}

TEST_F(CopyFileCmdTest, MoveOntoATakenNameLandsUnderTheNumberedName) {
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* cmd = new CopyFileCmd(project.get(), paths({"assets/a/hero.png"}), abs("assets/b").string(), false);
    cmd->reportTo(report);
    ASSERT_TRUE(history().addCommand(cmd)) << report->error;
    EXPECT_FALSE(exists("assets/a/hero.png"));
    EXPECT_EQ(text("assets/b/hero.png"), "second hero") << "nothing is overwritten";
    EXPECT_EQ(text("assets/b/hero 1.png"), "first hero");
    ASSERT_EQ(report->landed.size(), 1u);
    EXPECT_EQ(report->landed[0], abs("assets/b/hero 1.png"));
    ASSERT_NE(history().top(), nullptr);
    EXPECT_EQ(history().top()->name(), "Move File hero.png as hero 1.png");

    ASSERT_TRUE(history().undo());
    EXPECT_EQ(text("assets/a/hero.png"), "first hero");
    EXPECT_FALSE(exists("assets/b/hero 1.png"));
    ASSERT_TRUE(history().redo());
    EXPECT_EQ(text("assets/b/hero 1.png"), "first hero") << "a redo keeps the numbered destination";
}

TEST_F(CopyFileCmdTest, CopyOfAFolderIntoItselfIsRefusedAndChangesNothing) {
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* cmd = new CopyFileCmd(project.get(), paths({"assets/Sprites"}), abs("assets/Sprites/sub").string(), true);
    cmd->reportTo(report);
    EXPECT_FALSE(history().addCommand(cmd));
    EXPECT_EQ(report->error, "Can't copy 'Sprites' into itself: pick a folder outside it.");
    EXPECT_FALSE(exists("assets/Sprites/sub/Sprites"));
    EXPECT_EQ(history().top(), nullptr);

    auto moved = std::make_shared<CopyFileCmd::Report>();
    auto* move = new CopyFileCmd(project.get(), paths({"assets/Sprites"}), abs("assets/Sprites").string(), false);
    move->reportTo(moved);
    EXPECT_FALSE(history().addCommand(move));
    EXPECT_EQ(moved->error, "Can't move 'Sprites' into itself: pick a folder outside it.");
    EXPECT_TRUE(exists("assets/Sprites/ship.png"));
}

TEST_F(CopyFileCmdTest, MoveIntoItsOwnFolderAddsNoStep) {
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* cmd = new CopyFileCmd(project.get(), paths({"assets/b/other.png"}), abs("assets/b").string(), false);
    cmd->reportTo(report);
    EXPECT_TRUE(history().addCommand(cmd));
    EXPECT_TRUE(report->error.empty());
    EXPECT_TRUE(report->landed.empty());
    EXPECT_EQ(text("assets/b/other.png"), "other");
    EXPECT_FALSE(exists("assets/b/other 1.png"));
    EXPECT_EQ(history().top(), nullptr) << "nothing changed: no undo step";
}

TEST_F(CopyFileCmdTest, CopyIntoItsOwnFolderIsADuplicate) {
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* cmd = new CopyFileCmd(project.get(), paths({"assets/b/other.png"}), abs("assets/b").string(), true);
    cmd->reportTo(report);
    ASSERT_TRUE(history().addCommand(cmd));
    EXPECT_EQ(text("assets/b/other 1.png"), "other");
}

TEST_F(CopyFileCmdTest, FolderWithADotGetsTheWholeNameSuffix) {
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* cmd = new CopyFileCmd(project.get(), paths({"assets/my.folder"}), abs("assets").string(), true);
    cmd->reportTo(report);
    ASSERT_TRUE(history().addCommand(cmd)) << report->error;
    EXPECT_TRUE(exists("assets/my.folder 1/inside.png"));
    EXPECT_FALSE(exists("assets/my 1.folder"));
}

TEST_F(CopyFileCmdTest, ReportListsLandedPathsAndSurvivesAFailedCommand) {
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* cmd = new CopyFileCmd(project.get(), paths({"assets/a/hero.png", "assets/b/missing.png"}), abs("assets/Sprites").string(), true);
    cmd->reportTo(report);
    ASSERT_TRUE(history().addCommand(cmd));
    ASSERT_EQ(report->landed.size(), 1u);
    EXPECT_EQ(report->landed[0], abs("assets/Sprites/hero.png"));
    EXPECT_EQ(report->gone, 1u);

    // the history deletes a failed command; the report stays
    auto refused = std::make_shared<CopyFileCmd::Report>();
    auto* bad = new CopyFileCmd(project.get(), paths({"assets/Sprites"}), abs("assets/Sprites/sub").string(), false);
    bad->reportTo(refused);
    EXPECT_FALSE(history().addCommand(bad));
    EXPECT_FALSE(refused->error.empty());
}

TEST_F(CopyFileCmdTest, AllSourcesGoneIsNoStep) {
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* cmd = new CopyFileCmd(project.get(), paths({"assets/a/nothing.png"}), abs("assets/b").string(), true);
    cmd->reportTo(report);
    EXPECT_TRUE(history().addCommand(cmd));
    EXPECT_EQ(report->gone, 1u);
    EXPECT_EQ(history().top(), nullptr);
}

TEST_F(CopyFileCmdTest, DuplicateNamesItsStepAndCopiesBesideEachSource) {
    std::vector<FileCopyData> entries;
    for (const char* relative : {"assets/a/hero.png", "assets/b/hero.png", "assets/Sprites"}) {
        FileCopyData entry;
        entry.filename = abs(relative).filename();
        entry.sourceDirectory = abs(relative).parent_path();
        entry.targetDirectory = entry.sourceDirectory;
        entries.push_back(entry);
    }
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* cmd = new CopyFileCmd(project.get(), entries, true);
    cmd->setDuplicate(true);
    cmd->reportTo(report);
    ASSERT_TRUE(history().addCommand(cmd)) << report->error;
    EXPECT_EQ(text("assets/a/hero 1.png"), "first hero");
    EXPECT_EQ(text("assets/b/hero 1.png"), "second hero");
    EXPECT_TRUE(exists("assets/Sprites 1/sub/deep.png"));
    EXPECT_EQ(report->landed.size(), 3u);
    EXPECT_EQ(history().top()->name(), "Duplicate 3 Files");
    ASSERT_TRUE(history().undo());
    EXPECT_FALSE(exists("assets/a/hero 1.png"));
    EXPECT_FALSE(exists("assets/Sprites 1"));
}

TEST(AssetOpsRefusal, CopyAndMoveRefuseIntoThemselvesAfterResolvingLinks) {
    jasperengine::test::TempDir dir;
    dir.write("Sprites/a.png", "a");
    dir.write("Other/b.png", "b");
    // a link inside Other leading back into Sprites: a copy through it is still into itself
    fs::create_directory_symlink(dir.path() / "Sprites", dir.path() / "Other" / "back");
    EXPECT_EQ(AssetOps::copyRefusal(dir.path() / "Sprites", dir.path() / "Sprites" / "Sprites"), "Can't copy 'Sprites' into itself: pick a folder outside it.");
    EXPECT_EQ(AssetOps::copyRefusal(dir.path() / "Sprites", dir.path() / "Other" / "back" / "Sprites"), "Can't copy 'Sprites' into itself: pick a folder outside it.");
    EXPECT_EQ(AssetOps::moveRefusal(dir.path() / "Sprites", dir.path() / "Other" / "back" / "Sprites"), "Can't move 'Sprites' into itself: pick a folder outside it.");
    EXPECT_EQ(AssetOps::copyRefusal(dir.path() / "Sprites", dir.path() / "Other" / "Sprites"), "");
    // a file is never "into itself"
    EXPECT_EQ(AssetOps::copyRefusal(dir.path() / "Sprites" / "a.png", dir.path() / "Sprites" / "a 1.png"), "");
}

TEST(AssetOpsRefusal, CopyRefusesAFolderHoldingAProjectFile) {
    jasperengine::test::TempDir dir;
    dir.write("Game/inner/project.yaml", "name: inner\n");
    const std::string why = AssetOps::copyRefusal(dir.path() / "Game", dir.path() / "Copy");
    EXPECT_NE(why.find("project.yaml"), std::string::npos) << why;
    EXPECT_EQ(AssetOps::moveRefusal(dir.path() / "Game", dir.path() / "Moved"), "") << "a move makes no second project";
}

// Unity numbers on: duplicating "hero 1.png" gives "hero 2.png", not "hero 1 1.png"
TEST_F(CopyFileCmdTest, DuplicateOfANumberedCopyTakesTheNextNumber) {
    for (const char* relative : {"assets/a/hero.png", "assets/a/hero 1.png"}) {
        if (!exists(relative)) dir.write(relative, "hero");
        FileCopyData entry;
        entry.filename = abs(relative).filename();
        entry.sourceDirectory = abs(relative).parent_path();
        entry.targetDirectory = entry.sourceDirectory;
        auto* cmd = new CopyFileCmd(project.get(), std::vector<FileCopyData>{entry}, true);
        cmd->setDuplicate(true);
        ASSERT_TRUE(history().addCommand(cmd));
    }
    EXPECT_TRUE(exists("assets/a/hero 1.png"));
    EXPECT_TRUE(exists("assets/a/hero 2.png"));
    EXPECT_FALSE(exists("assets/a/hero 1 1.png"));
}

TEST_F(CopyFileCmdTest, UndoAndRedoKeepTheNumberedDestination) {
    auto* cmd = new CopyFileCmd(project.get(), paths({"assets/a/hero.png"}), abs("assets/b").string(), true);
    ASSERT_TRUE(history().addCommand(cmd));
    EXPECT_EQ(text("assets/b/hero 1.png"), "first hero");
    ASSERT_TRUE(history().undo());
    EXPECT_FALSE(exists("assets/b/hero 1.png"));
    // a file taking that name meanwhile is no reason for the redo to pick another
    ASSERT_TRUE(history().redo());
    EXPECT_EQ(text("assets/b/hero 1.png"), "first hero");
    EXPECT_FALSE(exists("assets/b/hero 2.png"));
}

// A Vertex Animation's .vat and .vat.glb move as a pair, under one free name
TEST_F(CopyFileCmdTest, VertexAnimationPairMovesUnderPairedFreeNames) {
    dir.write("assets/a/walk.vat", "{}");
    dir.write("assets/a/walk.vat.glb", "glb");
    dir.write("assets/b/walk.vat", "{}");
    dir.write("assets/b/walk.vat.glb", "glb");
    auto report = std::make_shared<CopyFileCmd::Report>();
    auto* cmd = new CopyFileCmd(project.get(), paths({"assets/a/walk.vat"}), abs("assets/b").string(), false);
    cmd->reportTo(report);
    ASSERT_TRUE(history().addCommand(cmd)) << report->error;
    EXPECT_TRUE(exists("assets/b/walk 1.vat"));
    EXPECT_TRUE(exists("assets/b/walk 1.vat.glb")) << "the mesh took the pair's free name";
    EXPECT_FALSE(exists("assets/a/walk.vat"));
    EXPECT_FALSE(exists("assets/a/walk.vat.glb"));
    EXPECT_EQ(text("assets/b/walk.vat.glb"), "glb") << "nothing overwritten";
}
