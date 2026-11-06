// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Editor scripts found by reading the files (S23 R18, R20, D6): Lua annotations under Editor/
// folders, C++ JE_MENU_ITEMs, the Unity path targets, and what stays a runtime script.

#include "Project.h"
#include "command/CommandHandle.h"
#include "util/EditorScripts.h"
#include "util/ExternalCodeEditor.h"

#include "TempDir.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace jasperengine::editor;

TEST(EditorScripts, ParsesLuaAnnotations) {
    const char* code = R"(local M = {}

---@menu Tools/Test/Raise priority=5 shortcut=Ctrl+Shift+K
function M.raise(ctx) end

---@menu GameObject/Test/Raise Selected
function M:raiseSelected(ctx) end

---@validate raiseSelected
function M.canRaise(ctx) return #ctx.selection > 0 end

---@toolbar Snap icon=fa-magnet tooltip=Snap the selection to the grid
M.snap = function(ctx) end

-- a plain function: no item
function M.helper() end
return M
)";
    const std::vector<EditorMenuItem> items = EditorScripts::parseLua(code, "Editor/Tools.lua");
    ASSERT_EQ(items.size(), 3u);

    EXPECT_EQ(items[0].path, "Tools/Test/Raise");
    EXPECT_EQ(items[0].kind, "menu");
    EXPECT_EQ(items[0].priority, 5);
    EXPECT_EQ(items[0].shortcut, "Ctrl+Shift+K");
    EXPECT_EQ(items[0].function, "raise");
    EXPECT_EQ(items[0].line, 3);
    EXPECT_EQ(items[0].id, "lua:Editor/Tools.lua#raise");
    EXPECT_EQ(items[0].targets(), std::vector<std::string>({"main"}));

    EXPECT_EQ(items[1].path, "GameObject/Test/Raise Selected");
    EXPECT_EQ(items[1].function, "raiseSelected");
    EXPECT_EQ(items[1].validateFunction, "canRaise");
    EXPECT_EQ(items[1].name(), "Raise Selected");
    EXPECT_EQ(items[1].targets(), std::vector<std::string>({"main", "gameobject"}));

    EXPECT_EQ(items[2].kind, "toolbar");
    EXPECT_EQ(items[2].path, "Snap");
    EXPECT_EQ(items[2].icon, "magnet");
    EXPECT_NE(items[2].iconGlyph, nullptr);
    EXPECT_EQ(items[2].tooltip, "Snap the selection to the grid");
    EXPECT_EQ(items[2].function, "snap");
    EXPECT_EQ(items[2].targets(), std::vector<std::string>({"toolbar"}));
}

TEST(EditorScripts, PathTargets) {
    EditorMenuItem item;
    item.kind = "menu";
    item.path = "CONTEXT/Transform/Reset Scale";
    EXPECT_EQ(item.targets(), std::vector<std::string>({"component"}));
    EXPECT_EQ(item.contextComponent(), "Transform");
    item.path = "SceneView/Drop Here";
    EXPECT_EQ(item.targets(), std::vector<std::string>({"sceneview"}));
    item.path = "Assets/Reimport Textures";
    EXPECT_EQ(item.targets(), std::vector<std::string>({"main", "assets"}));
    item.path = "MyStudio/Tools/Bake";
    EXPECT_EQ(item.targets(), std::vector<std::string>({"main"}));
}

TEST(EditorScripts, ParsesCppMenuItems) {
    const char* code = R"(#include "script/EditorMenu.h"

JE_MENU_ITEM("Tools/Bake Lights", 100)
static void bakeLights(const jasperengine::EditorMenuContext& ctx) {
    ctx.log("baking");
}

JE_MENU_ITEM("Tools/Quick") static void quick(const jasperengine::EditorMenuContext&) {}
)";
    const std::vector<EditorMenuItem> items = EditorScripts::parseCpp(code, "Editor/Bake.cpp");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].path, "Tools/Bake Lights");
    EXPECT_EQ(items[0].priority, 100);
    EXPECT_EQ(items[0].function, "bakeLights");
    EXPECT_EQ(items[0].language, "cpp");
    EXPECT_EQ(items[0].line, 3);
    EXPECT_EQ(items[1].function, "quick");
    EXPECT_EQ(items[1].priority, 1000);
}

TEST(EditorScripts, OnlyAnnotatedFilesInEditorFoldersAreEditorScripts) {
    EXPECT_TRUE(EditorScripts::isEditorFolderPath("Editor/Tools.lua"));
    EXPECT_TRUE(EditorScripts::isEditorFolderPath("lua/editor/sub/Tools.lua")) << "case-insensitive";
    EXPECT_FALSE(EditorScripts::isEditorFolderPath("lua/Tools.lua"));
    EXPECT_FALSE(EditorScripts::isEditorFolderPath("EditorTools/Tools.lua"));

    EXPECT_TRUE(EditorScripts::isLuaEditorScript("Editor/Tools.lua", "---@menu Tools/A\nfunction M.a() end"));
    // a runtime script that happens to live in an Editor folder stays one
    EXPECT_FALSE(EditorScripts::isLuaEditorScript("Editor/Player.lua", "local P = {}\nfunction P:init() end\nreturn P"));
    EXPECT_FALSE(EditorScripts::isLuaEditorScript("Tools.lua", "---@menu Tools/A\nfunction M.a() end"));

    jasperengine::test::TempDir dir;
    const auto root = dir.path();
    std::filesystem::create_directories(root / "Editor");
    std::ofstream(root / "Editor" / "Menu.lua") << "local M = {}\n---@menu Tools/X\nfunction M.x() end\nreturn M\n";
    std::ofstream(root / "Editor" / "Runtime.lua") << "return {}\n";
    std::ofstream(root / "Editor" / "Bake.cpp") << "JE_MENU_ITEM(\"Tools/B\")\nstatic void b(const jasperengine::EditorMenuContext&) {}\n";
    std::ofstream(root / "Editor" / "Helper.cpp") << "int helper() { return 1; }\n";
    EXPECT_TRUE(EditorScripts::isEditorOnlyScript(root / "Editor" / "Menu.lua", root));
    EXPECT_FALSE(EditorScripts::isEditorOnlyScript(root / "Editor" / "Runtime.lua", root));
    EXPECT_TRUE(EditorScripts::isEditorOnlyScript(root / "Editor" / "Bake.cpp", root));
    EXPECT_FALSE(EditorScripts::isEditorOnlyScript(root / "Editor" / "Helper.cpp", root));
}

TEST(EditorScripts, FontAwesomeNames) {
    EXPECT_NE(EditorScripts::fontAwesomeGlyph("magnet"), nullptr);
    EXPECT_NE(EditorScripts::fontAwesomeGlyph("fa-wand-magic-sparkles"), nullptr);
    EXPECT_EQ(EditorScripts::fontAwesomeGlyph("no-such-icon"), nullptr);
}

TEST(ExternalCodeEditor, CustomTemplateKeepsPathsWithSpacesWhole) {
    const std::vector<std::string> argv = ExternalCodeEditor::command("custom", "/home/jo doe/My Game/scripts/a.lua", 12, 3,
                                                                      "/home/jo doe/My Game", "myedit --goto \"{file}:{line}\" -p {project}");
    ASSERT_EQ(argv.size(), 5u);
    EXPECT_EQ(argv[0], "myedit");
    EXPECT_EQ(argv[1], "--goto");
    EXPECT_EQ(argv[2], "/home/jo doe/My Game/scripts/a.lua:12");
    EXPECT_EQ(argv[3], "-p");
    EXPECT_EQ(argv[4], "/home/jo doe/My Game") << "a placeholder fills one argument, spaces and all";

    std::string error;
    EXPECT_TRUE(ExternalCodeEditor::command("custom", "a.lua", 1, 1, "", "", &error).empty());
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ExternalCodeEditor::splitTemplate("a \"b c\" d"), std::vector<std::string>({"a", "b c", "d"}));
    EXPECT_EQ(ExternalCodeEditor::choices().front().id, std::string("builtin"));
}

TEST(EditorScripts, BracketedOptions) {
    const std::vector<EditorMenuItem> items = EditorScripts::parseLua(
        "---@menu Tools/Snap To Grid [priority=10] [shortcut=Ctrl+Shift+K]\nfunction M.snap() end\n"
        "---@toolbar Snap [icon=magnet] [tooltip=Snap it]\nfunction M.button() end\n", "Editor/Snap.lua");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].path, "Tools/Snap To Grid");
    EXPECT_EQ(items[0].priority, 10);
    EXPECT_EQ(items[0].shortcut, "Ctrl+Shift+K");
    EXPECT_EQ(items[1].icon, "magnet");
    EXPECT_EQ(items[1].tooltip, "Snap it");
}

// The C++ editor scripts link the editor's engine library as the project's scripts do: with its
// Jolt and Box2D macros (MSVC links Jolt's data, such as JPH::Allocate, only through dllimport)
TEST(EditorScripts, CppProjectCompilesWithTheEngineLibrarysMacros) {
    jasperengine::test::TestTempDir dir("editor-scripts-cmake");
    const std::filesystem::path root = dir / "project";
    std::filesystem::copy(std::filesystem::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", root,
                          std::filesystem::copy_options::recursive);
    jasperengine::test::TestTempDir::write(root / "Editor" / "Bake.cpp",
        "JE_MENU_ITEM(\"Tools/Bake\")\nstatic void bake(const jasperengine::EditorMenuContext&) {}\n");
    std::string cmake;
    {
        Project project;
        project.setSkipProjectLock(true);
        ASSERT_TRUE(project.loadProject(root, false));
        std::string libName;
        ASSERT_TRUE(EditorScripts::writeCppProject(&project, {root / "Editor" / "Bake.cpp"}, dir / "editor-scripts", libName));
        cmake = jasperengine::test::TestTempDir::read(dir / "editor-scripts" / "CMakeLists.txt");
        for (SceneProject& scene : project.getScenes()) CommandHandle::remove(scene.id);
        project.releaseScenes();
    }
    const size_t at = cmake.find("add_compile_definitions(");
    ASSERT_NE(at, std::string::npos) << cmake;
    const std::string line = cmake.substr(at, cmake.find('\n', at) - at);
    EXPECT_NE(line.find("JASPERENGINE_SHARED"), std::string::npos) << line;
    EXPECT_NE(line.find("JPH_SHARED_LIBRARY"), std::string::npos) << line;
    EXPECT_NE(line.find("JPH_OBJECT_LAYER_BITS="), std::string::npos) << line;
}
