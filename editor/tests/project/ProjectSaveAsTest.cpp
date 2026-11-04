// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Save Project As (project-window spec 6.13, Doriax 41a2ed0d5): a saved project is copied to the
// new folder and the editor continues there; a failure leaves the project where it was; a folder
// inside the project, or holding it, is refused; each editor gets a temporary folder of its own.

#include "asset/FileOpsTestProject.h"

#include <gtest/gtest.h>

#include <sys/stat.h>

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

TEST_F(ProjectSaveAsTest, FailedSaveAsKeepsTheProjectWhereItWas) {
    // a FIFO is no file a copy can make: the copy fails half way (as root too)
    jasperengine::test::TempDir other;
    const fs::path target = other.path() / "Copy";
    fs::create_directories(abs("assets/zz"));
    ASSERT_EQ(::mkfifo(abs("assets/zz/pipe").c_str(), 0644), 0);
    EXPECT_FALSE(project->saveProjectToPath(target));
    EXPECT_EQ(project->getProjectPath(), dir.path()) << "the project stays where it was";
    EXPECT_FALSE(fs::exists(target)) << "what the copy made is gone";
    EXPECT_TRUE(exists("assets/hero.png"));
}

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

TEST(ProjectTempDir, SkipsBusyFolders) {
    const fs::path root = "/tmp/jasper-root";
    std::set<fs::path> busy = {root / "MyProject", root / "MyProject 2"};
    const fs::path chosen = Project::chooseTempProjectDir(root, "MyProject", [&](const fs::path& candidate) { return busy.count(candidate) > 0; });
    EXPECT_EQ(chosen, root / "MyProject 3");
    EXPECT_EQ(Project::chooseTempProjectDir(root, "MyProject", [](const fs::path&) { return false; }), root / "MyProject");
}
