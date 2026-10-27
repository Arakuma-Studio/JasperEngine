// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// tools/fixtures/prefab-runtime copied to a temp dir with a texture and a folder added, loaded
// headless as the CLI loads a project (the S25 organisation tests)

#include "Project.h"
#include "command/CommandHandle.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace jasperengine::editor::test {

    class OrganisationProjectTest : public ::testing::Test {
    protected:
        jasperengine::test::TempDir dir;
        std::unique_ptr<Project> project;

        void SetUp() override {
            namespace fs = std::filesystem;
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            // the contents do not matter: the database knows a texture by its extension
            dir.write("assets/textures/hero.png", "not really a png");
            dir.write("assets/textures/boss.png", "not really a png either");
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            project->getAssetLabels().waitUntilReady();
            project->getAssetLabels().update();
        }
        void TearDown() override {
            if (project) {
                for (SceneProject& candidate : project->getScenes()) CommandHandle::remove(candidate.id);
            }
            project.reset();
        }

        Guid guidOf(const std::string& path) { return project->getAssetDatabase().getAssetGuid(path); }
        std::string text(const std::string& relative) { return dir.read(relative); }
        CommandHistory& history() { return *project->getProjectCommandHistory(); }
    };

}
