// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// One set of script templates (project-window spec D8): the Project window, the Inspector's
// dialog and create_script write the same files

#include "util/ScriptTemplates.h"
#include "window/dialog/ScriptCreateDialog.h"

#include "TempDir.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(ScriptTemplates, LuaTemplateDeclaresTheModuleClass) {
    const std::string text = ScriptTemplates::lua("Goblin");
    EXPECT_NE(text.find("---@class Goblin : ScriptInstance"), std::string::npos);
    EXPECT_NE(text.find("local Goblin = {"), std::string::npos);
    EXPECT_NE(text.find("return Goblin\n"), std::string::npos);
}

TEST(ScriptTemplates, CppScriptPairNamesTheClass) {
    const ScriptTemplates::Files files = ScriptTemplates::cppScript("Goblin", "Goblin.h");
    EXPECT_NE(files.header.find("class Goblin : public jasperengine::ScriptBase"), std::string::npos);
    EXPECT_NE(files.source.find("#include \"Goblin.h\""), std::string::npos);
    EXPECT_NE(files.source.find("Goblin::Goblin(Scene* scene, Entity entity): ScriptBase(scene, entity)"), std::string::npos);
}

TEST(ScriptTemplates, LoneFiles) {
    EXPECT_EQ(ScriptTemplates::cppHeader("Math"), "#pragma once\n\n// Math.h\n");
    EXPECT_NE(ScriptTemplates::cppSource("Math", true).find("#include \"Math.h\""), std::string::npos);
    EXPECT_EQ(ScriptTemplates::cppSource("Math", false).find("#include \"Math.h\""), std::string::npos);
}

TEST(ScriptTemplates, WriteTemplateFilesWritesTheTemplateStrings) {
    jasperengine::test::TempDir dir;
    ScriptCreateDialog::writeTemplateFiles(nullptr, NULL_ENTITY, fs::path(), dir.path() / "Goblin.lua", "Goblin", ScriptCreateDialog::CreationKind::LUA);
    EXPECT_EQ(dir.read("Goblin.lua"), ScriptTemplates::lua("Goblin"));
    ScriptCreateDialog::writeTemplateFiles(nullptr, NULL_ENTITY, dir.path() / "Orc.h", dir.path() / "Orc.cpp", "Orc",
                                           ScriptCreateDialog::CreationKind::CPP_SCRIPT_CLASS);
    const ScriptTemplates::Files files = ScriptTemplates::cppScript("Orc", "Orc.h");
    EXPECT_EQ(dir.read("Orc.h"), files.header);
    EXPECT_EQ(dir.read("Orc.cpp"), files.source);
}
