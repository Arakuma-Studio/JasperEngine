// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A tools/fixtures project (prefab-runtime unless a test picks another; assets dir "assets", Lua
// dir "scripts") copied to a temp dir and loaded headless as the CLI loads a project: the Project
// window's file operations, asset-root changes and Save Project As run on it

#include "Project.h"
#include "command/CommandHistory.h"
#include "support/EditorTestSupport.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace jasperengine::editor::test {

    class FileOpsProjectTest : public ::testing::Test {
    protected:
        jasperengine::test::TempDir dir;
        std::unique_ptr<Project> project;

        // what a test adds before the project loads
        virtual void prepare() {}
        // the fixture (tools/fixtures/<name>) copied
        virtual const char* fixture() const { return "prefab-runtime"; }

        void SetUp() override {
            namespace fs = std::filesystem;
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures" / fixture(), dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            prepare();
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
        }
        void TearDown() override {
            // closes the scenes as a project switch does: a Project frees none itself
            if (project) closeProject(*project);
            project.reset();
        }

        std::filesystem::path abs(const std::string& relative) const { return dir.path() / relative; }
        bool exists(const std::string& relative) const { return std::filesystem::exists(abs(relative)); }
        std::string text(const std::string& relative) const { return dir.read(relative); }
        CommandHistory& history() { return *project->getProjectCommandHistory(); }
    };

}
