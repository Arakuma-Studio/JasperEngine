// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project folders that hold no assets (docs/specs/S27-editor-tools.md R31, AC27): the
// Recorder's Recordings and the EditorSettings libraries at the project's top are skipped by the
// scans and exports like hidden folders and build, but a folder of that name deeper down is an
// asset folder; the generated .gitignore leaves the recordings out.

#include "Project.h"
#include "command/CommandHandle.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

TEST(Project, NonAssetTopLevelDirs) {
    for (const char* name : {".jasperengine", ".git", "build", "Recordings", "EditorSettings"}) {
        EXPECT_TRUE(Project::isNonAssetTopLevelDir(name)) << name;
    }
    for (const char* name : {"Assets", "recordings", "Scenes", "Settings"}) {
        EXPECT_FALSE(Project::isNonAssetTopLevelDir(name)) << name;
    }

    TestTempDir dir("non-asset-dirs");
    const fs::path root = dir.path() / "Game";
    Project project;
    project.setSkipProjectLock(true);
    std::string error;
    ASSERT_TRUE(project.createProjectAt(root, "Game", &error)) << error;
    const fs::path top = project.getProjectPath();
    EXPECT_TRUE(project.isNonAssetDir(top / "Recordings"));
    EXPECT_TRUE(project.isNonAssetDir(top / "EditorSettings"));
    EXPECT_TRUE(project.isNonAssetDir(top / "build"));
    EXPECT_TRUE(project.isNonAssetDir(top / "art" / ".cache"));
    EXPECT_TRUE(project.isNonAssetDir(top / "art" / "build"));
    // deeper down, these names are the user's
    fs::create_directories(top / "art" / "Recordings");
    EXPECT_FALSE(project.isNonAssetDir(top / "art" / "Recordings"));
    EXPECT_FALSE(project.isNonAssetDir(top / "art" / "EditorSettings"));
    EXPECT_FALSE(project.isNonAssetDir(top / "art"));

    ASSERT_TRUE(project.saveProject());
    std::ifstream in(top / ".gitignore");
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_NE(text.str().find("Recordings/"), std::string::npos) << text.str();
    for (SceneProject& scene : project.getScenes()) CommandHandle::remove(scene.id);
    project.releaseScenes();
}
