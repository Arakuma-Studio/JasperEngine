// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Build Settings > Shaders (added and excluded shader keys) edited as one project undo step: the
// Build Settings window, edit_build_shaders and build_project shaders go through
// ShaderOverridesCmd; undo puts both lists back.

#include <gtest/gtest.h>

#include "Project.h"
#include "command/type/ShaderOverridesCmd.h"
#include "pool/ShaderPool.h"

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(ShaderOverridesCmd, ExecuteAndUndo){
    Project project;
    const ShaderKey added = ShaderPool::getShaderKey(ShaderType::MESH, 1);
    const ShaderKey excluded = ShaderPool::getShaderKey(ShaderType::MESH, 2);
    ShaderOverrides before;
    before.exclusions.insert(added);
    project.getShaderOverrides() = before;

    ShaderOverrides after;
    after.additions.insert(added);
    after.exclusions.insert(excluded);
    ShaderOverridesCmd command(&project, before, after);
    EXPECT_TRUE(command.execute());
    EXPECT_EQ(project.getShaderOverrides().additions, after.additions);
    EXPECT_EQ(project.getShaderOverrides().exclusions, after.exclusions);

    command.undo();
    EXPECT_TRUE(project.getShaderOverrides().additions.empty());
    EXPECT_EQ(project.getShaderOverrides().exclusions, before.exclusions);
    EXPECT_STREQ(command.typeName(), "ShaderOverridesCmd");
    // a project without a folder (a test's): nothing written, nothing touched
    EXPECT_TRUE(command.touchedPaths(false).empty());
}
