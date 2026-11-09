// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Shadow LOD Bias of new projects (spec S55 Decisions): 1.0, which S54's SHADOW_LOD_COARSER makes
// one detail level coarser than the camera's rule, as a project.yaml without the key reads. A template
// that set 0.5 would cast two levels coarser.

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "hub/TemplateCatalog.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;
using namespace jasperengine::editor::hub;
namespace fs = std::filesystem;

TEST(NewProjectShadowLodBias, TemplatesAndOldProjectsReadOne) {
    TestTempDir dir("shadow-lod-bias");
    const std::vector<ProjectTemplate> list = templates::all(templates::userDirectory(dir.path() / "settings"));
    for (const char* id : {"empty", "3d", "2d", "2d-pixel-art", "ui"}) {
        const ProjectTemplate* info = templates::find(list, id);
        ASSERT_NE(info, nullptr) << id;
        std::string error;
        const fs::path made = dir.path() / id;
        ASSERT_TRUE(templates::materialize(*info, made, error)) << id << ": " << error;
        Project project;
        Stream::decodeProject(&project, YAML::LoadFile((made / "project.yaml").string()), Workspace());
        EXPECT_FLOAT_EQ(project.getDeviceSettings().shadowLodBias, 1.0f) << id;
    }
    // a project.yaml from before: no device block
    Project old;
    Stream::decodeProject(&old, YAML::Load("name: Old\nvsync: true\n"), Workspace());
    EXPECT_FLOAT_EQ(old.getDeviceSettings().shadowLodBias, 1.0f);
}
