// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Where an export may go (Exporter::outputFolderRefusal, export-pipeline spec 6.3): never into the
// project, its assets or its Lua folder, whose scans would import the exported files as project
// assets; below a hidden folder (.jasperengine/builds) or the project's build/ is fine.

#include "Exporter.h"
#include "Project.h"
#include "command/CommandHandle.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

namespace export_output_folder_test {

    struct ScratchProject {
        TestTempDir dir{"export-output"};
        Project project;
        ScratchProject() {
            project.setSkipProjectLock(true);
            std::string error;
            EXPECT_TRUE(project.createProjectAt(dir.path() / "Game", "Game", &error)) << error;
        }
        ~ScratchProject() {
            for (SceneProject& scene : project.getScenes()) CommandHandle::remove(scene.id);
            project.releaseScenes();
        }
        fs::path root() const { return project.getProjectPath(); }
    };

}

using namespace export_output_folder_test;

TEST(ExportOutputFolder, InsideProjectRefused) {
    ScratchProject s;
    for (const fs::path& inside : {s.root(), s.root() / "Builds", s.root() / "Builds" / "Web", s.root() / "assets" / "out"}) {
        const std::string refusal = Exporter::outputFolderRefusal(&s.project, inside);
        EXPECT_NE(refusal.find("inside the project"), std::string::npos) << inside << ": " << refusal;
        EXPECT_NE(refusal.find("Game-build"), std::string::npos) << "it names a folder outside: " << refusal;
    }
}

TEST(ExportOutputFolder, HiddenAndBuildAllowed) {
    ScratchProject s;
    for (const fs::path& allowed : {s.root() / ".jasperengine" / "builds" / "source", s.root() / "build" / "web",
                                    s.root().parent_path() / "Game-build", s.root().parent_path()}) {
        EXPECT_EQ(Exporter::outputFolderRefusal(&s.project, allowed), "") << allowed;
    }
    // a build folder deeper down is an asset folder
    EXPECT_NE(Exporter::outputFolderRefusal(&s.project, s.root() / "art" / "build"), "");
}

TEST(ExportOutputFolder, InsideExternalAssetsDirRefused) {
    ScratchProject s;
    const fs::path assets = s.dir.path() / "SharedAssets";
    fs::create_directories(assets);
    s.project.setAssetsDir(assets);
    EXPECT_NE(Exporter::outputFolderRefusal(&s.project, assets / "out"), "");
    EXPECT_EQ(Exporter::outputFolderRefusal(&s.project, assets / ".cache" / "out"), "");
    EXPECT_EQ(Exporter::outputFolderRefusal(&s.project, s.dir.path() / "Elsewhere"), "");
}

// Windows makes links only with a privilege; the rule resolves them the same way there
#ifndef _WIN32
TEST(ExportOutputFolder, SymlinkIntoProjectRefused) {
    ScratchProject s;
    fs::create_directory_symlink(s.root(), s.dir.path() / "link");
    EXPECT_NE(Exporter::outputFolderRefusal(&s.project, s.dir.path() / "link" / "Builds"), "");
}
#endif

// CMake reads ';' as a list separator: an export path with one is refused up front, saying what to do
TEST(ExportOutputFolder, SemicolonRefused) {
    ScratchProject s;
    const std::string refusal = Exporter::outputFolderRefusal(&s.project, s.dir.path() / "Game;v2-build");
    EXPECT_NE(refusal.find("';'"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("rename"), std::string::npos) << refusal;
    EXPECT_EQ(Exporter::outputFolderRefusal(nullptr, s.dir.path() / "a;b").empty(), false) << "with no project too (generate_shaders)";
}
