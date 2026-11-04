// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Shadow LOD Bias of new projects (spec S55 R5, Decisions): every built-in template makes a
// project whose shadow maps take coarser detail levels (quality.shadow_lod_bias 0.5, S56's
// multiplier), while a project.yaml without the key (every project made before) reads 1.0 and
// renders as it did.

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

TEST(NewProjectShadowLodBias, TemplatesCoarsenShadowsOldProjectsKeepOne) {
    TestTempDir dir("shadow-lod-bias");
    const std::vector<ProjectTemplate> list = templates::all(templates::userDirectory(dir.path() / "settings"));
    for (const char* id : {"empty", "3d", "2d", "ui"}) {
        const ProjectTemplate* info = templates::find(list, id);
        ASSERT_NE(info, nullptr) << id;
        std::string error;
        const fs::path made = dir.path() / id;
        ASSERT_TRUE(templates::materialize(*info, made, error)) << id << ": " << error;
        Project project;
        Stream::decodeProject(&project, YAML::LoadFile((made / "project.yaml").string()), Workspace());
        EXPECT_FLOAT_EQ(project.getDeviceSettings().shadowLodBias, 0.5f) << id;
    }
    // a project.yaml from before: no device block
    Project old;
    Stream::decodeProject(&old, YAML::Load("name: Old\nvsync: true\n"), Workspace());
    EXPECT_FLOAT_EQ(old.getDeviceSettings().shadowLodBias, 1.0f);
}
