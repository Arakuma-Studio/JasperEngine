// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Save Project As (project-window spec 6.13, Doriax 41a2ed0d5): a saved project is copied to the
// new folder and the editor continues there; a failure leaves the project where it was; a folder
// inside the project, or holding it, is refused; each editor gets a temporary folder of its own.

#include "asset/FileOpsTestProject.h"

#include <gtest/gtest.h>

#include "command/type/CopyFileCmd.h"
#include "io/ScopedFileLock.h"

#include <sys/stat.h>
#include <cstdlib>

#include <filesystem>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    class ProjectSaveAsTest : public FileOpsProjectTest {
    protected:
        void prepare() override {
            dir.write("assets/hero.png", "hero");
        }
    };

}

TEST_F(ProjectSaveAsTest, SaveAsOfASavedProjectCopiesIt) {
    jasperengine::test::TempDir other;
    const fs::path target = other.path() / "Copy";
    ASSERT_TRUE(project->saveProjectToPath(target));
    EXPECT_EQ(project->getProjectPath(), target);
    EXPECT_TRUE(fs::exists(target / "assets/hero.png")) << "the assets go with the project";
    EXPECT_TRUE(fs::exists(target / "Main Scene.scene"));
    EXPECT_TRUE(fs::exists(target / "prefabs/Bullet.prefab"));
    EXPECT_TRUE(fs::exists(target / "project.yaml"));
    EXPECT_TRUE(exists("assets/hero.png")) << "the original stays as it was saved";
    EXPECT_FALSE(fs::exists(target / ".jasperengine/build")) << "no build tree is copied";
}

// The undo steps name the old folder's files: after Save As an undo would write into the original
TEST_F(ProjectSaveAsTest, SaveAsDropsTheOldProjectsUndoSteps) {
    FileCopyData entry;
    entry.filename = "hero.png";
    entry.sourceDirectory = abs("assets");
    entry.targetDirectory = entry.sourceDirectory;
    auto* cmd = new CopyFileCmd(project.get(), std::vector<FileCopyData>{entry}, true);
    cmd->setDuplicate(true);
    ASSERT_TRUE(history().addCommand(cmd));
    ASSERT_TRUE(history().canUndo());
    jasperengine::test::TempDir other;
    ASSERT_TRUE(project->saveProjectToPath(other.path() / "Copy"));
    EXPECT_FALSE(history().canUndo()) << "no step of the original's files is left to undo";
    EXPECT_TRUE(exists("assets/hero 1.png")) << "the original keeps what was done";
}

TEST_F(ProjectSaveAsTest, SaveAsLeavesTheTrashBehind) {
    // written once the project is open: opening a project empties its trash
    dir.write(".trash/old.png", "deleted before");
    jasperengine::test::TempDir other;
    ASSERT_TRUE(project->saveProjectToPath(other.path() / "Copy"));
    EXPECT_FALSE(fs::exists(other.path() / "Copy" / ".trash")) << "deleted files stay with the original";
    EXPECT_TRUE(exists(".trash/old.png"));
}

#ifndef _WIN32
TEST_F(ProjectSaveAsTest, FailedSaveAsKeepsTheProjectWhereItWas) {
    // a FIFO (POSIX) is no file a copy can make: the copy fails half way (as root too)
    jasperengine::test::TempDir other;
    const fs::path target = other.path() / "Copy";
    fs::create_directories(abs("assets/zz"));
    ASSERT_EQ(::mkfifo(abs("assets/zz/pipe").c_str(), 0644), 0);
    EXPECT_FALSE(project->saveProjectToPath(target));
    EXPECT_EQ(project->getProjectPath(), dir.path()) << "the project stays where it was";
    EXPECT_FALSE(fs::exists(target)) << "what the copy made is gone";
    EXPECT_TRUE(exists("assets/hero.png"));
}
#endif

TEST_F(ProjectSaveAsTest, SaveAsIntoTheProjectIsRefused) {
    EXPECT_FALSE(project->saveProjectToPath(abs("assets/Copy")));
    EXPECT_EQ(project->getProjectPath(), dir.path());
    EXPECT_FALSE(exists("assets/Copy"));
    EXPECT_NE(project->saveAsRefusal(abs("assets/Copy")).find("into itself"), std::string::npos);
    EXPECT_NE(project->saveAsRefusal(dir.path().parent_path()).find("holds it"), std::string::npos);
}

TEST_F(ProjectSaveAsTest, SaveAsIntoANonEmptyFolderIsRefused) {
    jasperengine::test::TempDir other;
    other.write("taken.txt", "x");
    EXPECT_FALSE(project->saveProjectToPath(other.path()));
    EXPECT_EQ(project->getProjectPath(), dir.path());
    EXPECT_FALSE(fs::exists(other.path() / "project.yaml"));
}

// A failed Save As keeps the temporary project's lock: no other editor takes that folder (a FIFO
// fails the copy: POSIX)
#ifndef _WIN32
TEST(ProjectSaveAsTemp, FailedSaveAsFromTheTemporaryProjectKeepsItsLock) {
    jasperengine::test::TempDir temp;
    const char* previous = std::getenv("TMPDIR");
    const std::string saved = previous ? previous : "";
    setenv("TMPDIR", temp.str().c_str(), 1);
    {
        Project project;
        ASSERT_TRUE(project.createTempProject("PwTempLock"));
        ASSERT_TRUE(project.holdsProjectLock());
        ASSERT_EQ(::mkfifo((project.getProjectPath() / "pipe").c_str(), 0644), 0);
        jasperengine::test::TempDir other;
        EXPECT_FALSE(project.saveProjectToPath(other.path() / "Copy"));
        EXPECT_TRUE(project.holdsProjectLock());
        io::ScopedFileLock probe(project.getProjectLockPath(), io::ScopedFileLock::Mode::Exclusive, false);
        EXPECT_EQ(probe.getStatus(), io::ScopedFileLock::Status::Busy) << "another editor can't take the folder";
        closeProject(project);
    }
    if (previous) setenv("TMPDIR", saved.c_str(), 1);
    else unsetenv("TMPDIR");
}
#endif

TEST(ProjectTempDir, SkipsBusyFolders) {
    const fs::path root = "/tmp/jasper-root";
    std::set<fs::path> busy = {root / "MyProject", root / "MyProject 2"};
    const fs::path chosen = Project::chooseTempProjectDir(root, "MyProject", [&](const fs::path& candidate) { return busy.count(candidate) > 0; });
    EXPECT_EQ(chosen, root / "MyProject 3");
    EXPECT_EQ(Project::chooseTempProjectDir(root, "MyProject", [](const fs::path&) { return false; }), root / "MyProject");
}
