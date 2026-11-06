// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scripting catch-up's editor side, on a headless project whose Lua directory is "lua":
// - script properties: a saved script without a `properties` table has none (Doriax 3d7a91e4e),
//   code being edited keeps them until saved, errors keep the last ones; the code editor's live
//   refresh finds Lua entries under that Lua directory; a scene opened after its scripts changed
//   shows their current fields; one refresh pass runs each file once;
// - ScriptAttach, the one attach path of the Inspector, the Hierarchy and MCP attach_script;
// - ModelLoadCmd carrying the mesh settings of a model's parts across a rebuild.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/TwoPartGltf.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/ModelLoadCmd.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"
#include "util/ScriptAttach.h"
#include "util/ScriptPropertyCache.h"
#include "window/CodeEditor.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-scripting-editor-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            fs::create_directories(out.dir / "lua");
            fs::create_directories(out.dir / "scripts");
            std::ofstream(out.dir / "project.yaml") << "name: scripting\nassetsDir: assets\nluaDir: lua\nscriptDirs:\n  - scripts\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    bool headlessStarted = false;
    Project* project() {
        headlessStarted = true;
        return headless().project.get();
    }

    struct Cleanup : public ::testing::Environment {
        void TearDown() override {
            if (!headlessStarted) return;
            std::error_code ec;
            fs::remove_all(headless().dir.parent_path(), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const cleanup = ::testing::AddGlobalTestEnvironment(new Cleanup);

    void writeFile(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    }

    const char* kSpeedScript = "return { properties = { { name = 'speed', type = 'float', default = 1 } } }\n";
    const char* kSpeedJumpScript =
        "return { properties = { { name = 'speed', type = 'float', default = 1 }, { name = 'jump', type = 'float', default = 2 } } }\n";

    std::vector<std::string> names(const std::vector<ScriptProperty>& properties) {
        std::vector<std::string> out;
        for (const ScriptProperty& property : properties) out.push_back(property.name);
        return out;
    }

    ScriptEntry luaEntry(const std::string& file) {
        ScriptEntry entry;
        entry.type = ScriptType::LUA;
        entry.enabled = true;
        entry.path = file;
        entry.className = fs::path(file).stem().string();
        return entry;
    }

    // a new scene with one GameObject (Transform) per name
    SceneProject* newScene(const std::string& name, std::vector<Entity>& entities, size_t count) {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        const uint32_t id = p->createNewScene(name, SceneType::SCENE_3D);
        SceneProject* sceneProject = p->getScene(id);
        for (size_t i = 0; i < count; i++) {
            const Entity entity = sceneProject->scene->createUserEntity();
            sceneProject->scene->setEntityName(entity, name + std::to_string(i));
            sceneProject->scene->addComponent<Transform>(entity, Transform());
            sceneProject->entities.push_back(entity);
            entities.push_back(entity);
        }
        return sceneProject;
    }

    class ScriptingEditor : public ::testing::Test {
    protected:
        EditorHost host;
        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            setEditorHost(&host);
        }
        void TearDown() override {
            setEditorHost(nullptr);
        }
        fs::path lua(const std::string& name) { return project()->getProjectPath() / "lua" / name; }
    };

}

TEST_F(ScriptingEditor, SavedFileWithoutTableClears) {
    writeFile(lua("clears.lua"), kSpeedScript);
    std::vector<Entity> entities;
    SceneProject* scene = newScene("Clears", entities, 1);
    std::vector<ScriptEntry> scripts = {luaEntry("clears.lua")};
    project()->updateScriptProperties(scene, entities[0], scripts);
    ASSERT_EQ(names(scripts[0].properties), std::vector<std::string>{"speed"});

    writeFile(lua("clears.lua"), "return { update = function() end }\n");
    EXPECT_TRUE(project()->updateScriptProperties(scene, entities[0], scripts));
    EXPECT_TRUE(scripts[0].properties.empty());
}

TEST_F(ScriptingEditor, LiveBufferWithoutTableKeeps) {
    writeFile(lua("live.lua"), kSpeedScript);
    std::vector<Entity> entities;
    SceneProject* scene = newScene("Live", entities, 1);
    std::vector<ScriptEntry> scripts = {luaEntry("live.lua")};
    project()->updateScriptProperties(scene, entities[0], scripts);

    // the table cut from the buffer (to paste it back): kept until the file is saved
    EXPECT_FALSE(project()->updateScriptProperties(scene, entities[0], scripts, "return {}\n", lua("live.lua").string()));
    EXPECT_EQ(names(scripts[0].properties), std::vector<std::string>{"speed"});
    // an empty table is a script without properties, edited or saved
    EXPECT_TRUE(project()->updateScriptProperties(scene, entities[0], scripts, "return { properties = {} }\n", lua("live.lua").string()));
    EXPECT_TRUE(scripts[0].properties.empty());
}

TEST_F(ScriptingEditor, ErrorsKeepLastProperties) {
    writeFile(lua("errors.lua"), kSpeedScript);
    std::vector<Entity> entities;
    SceneProject* scene = newScene("Errors", entities, 1);
    std::vector<ScriptEntry> scripts = {luaEntry("errors.lua")};
    project()->updateScriptProperties(scene, entities[0], scripts);
    for (const char* broken : {"return {\n", "error('at load')\n", "return 5\n"}) {
        writeFile(lua("errors.lua"), broken);
        EXPECT_FALSE(project()->updateScriptProperties(scene, entities[0], scripts)) << broken;
        EXPECT_EQ(names(scripts[0].properties), std::vector<std::string>{"speed"}) << broken;
    }
}

TEST_F(ScriptingEditor, CustomLuaRootMatchesLiveEdit) {
    writeFile(lua("enemy.lua"), kSpeedScript);
    std::vector<Entity> entities;
    SceneProject* scene = newScene("Enemy", entities, 1);
    ScriptComponent component;
    component.scripts = {luaEntry("enemy.lua")};
    scene->scene->addComponent<ScriptComponent>(entities[0], component);
    project()->updateScriptProperties(scene, entities[0], scene->scene->getComponent<ScriptComponent>(entities[0]).scripts);

    const ScriptEntry& entry = scene->scene->getComponent<ScriptComponent>(entities[0]).scripts[0];
    EXPECT_TRUE(project()->scriptEntryUsesFile(entry, lua("enemy.lua")));
    EXPECT_EQ(project()->scriptPropertyFile(entry), lua("enemy.lua").lexically_normal());

    // the code editor's live refresh: its path is project-relative, the entry's Lua-root-relative
    CodeEditor codeEditor(project());
    codeEditor.updateScriptPropertiesForPath("lua/enemy.lua", kSpeedJumpScript);
    EXPECT_EQ(names(scene->scene->getComponent<ScriptComponent>(entities[0]).scripts[0].properties),
              (std::vector<std::string>{"speed", "jump"}));
}

TEST_F(ScriptingEditor, SceneOpenRefreshes) {
    writeFile(lua("opened.lua"), kSpeedScript);
    std::vector<Entity> entities;
    SceneProject* scene = newScene("Opened", entities, 1);
    ScriptComponent component;
    component.scripts = {luaEntry("opened.lua")};
    scene->scene->addComponent<ScriptComponent>(entities[0], component);
    project()->updateScriptProperties(scene, entities[0], scene->scene->getComponent<ScriptComponent>(entities[0]).scripts);
    const fs::path path = project()->getProjectPath() / "Opened.scene";
    ASSERT_TRUE(project()->saveSceneToPath(scene->id, path));
    std::string error;
    ASSERT_TRUE(project()->closeSceneNow(scene->id, UnsavedChoice::Discard, &error)) << error;

    // changed while the scene was closed
    writeFile(lua("opened.lua"), kSpeedJumpScript);
    const uint32_t id = project()->openSceneNow(path, false, UnsavedChoice::Discard, &error);
    ASSERT_NE(id, NULL_PROJECT_SCENE) << error;
    SceneProject* opened = project()->getScene(id);
    bool found = false;
    for (Entity entity : opened->entities) {
        if (const ScriptComponent* scripts = opened->scene->findComponent<ScriptComponent>(entity)) {
            ASSERT_EQ(scripts->scripts.size(), 1u);
            EXPECT_EQ(names(scripts->scripts[0].properties), (std::vector<std::string>{"speed", "jump"}));
            found = true;
        }
    }
    EXPECT_TRUE(found);
    EXPECT_FALSE(opened->isModified);
}

TEST_F(ScriptingEditor, CacheRunsEachFileOnce) {
    writeFile(lua("shared.lua"), kSpeedScript);
    std::vector<Entity> entities;
    SceneProject* scene = newScene("Shared", entities, 50);
    for (Entity entity : entities) {
        ScriptComponent component;
        component.scripts = {luaEntry("shared.lua")};
        scene->scene->addComponent<ScriptComponent>(entity, component);
    }
    const int before = ScriptPropertyCache::runsForTest();
    project()->updateAllScriptsProperties(scene->id);
    EXPECT_EQ(ScriptPropertyCache::runsForTest() - before, 1);
    for (Entity entity : entities) {
        EXPECT_EQ(names(scene->scene->getComponent<ScriptComponent>(entity).scripts[0].properties), std::vector<std::string>{"speed"});
    }
}

TEST_F(ScriptingEditor, ScriptAttachMakesEntries) {
    writeFile(lua("ai/patrol.lua"), kSpeedScript);
    writeFile(project()->getProjectPath() / "outside.lua", kSpeedScript);
    writeFile(project()->getProjectPath() / "scripts" / "Mover.h",
              "#pragma once\n#include \"ScriptBase.h\"\nclass Mover : public jasperengine::ScriptBase {\npublic:\n    void update();\n};\n");
    writeFile(project()->getProjectPath() / "scripts" / "Mover.cpp", "#include \"Mover.h\"\nvoid Mover::update() {}\n");
    writeFile(project()->getProjectPath() / "scripts" / "Spinner.h",
              "#pragma once\n#include \"Mesh.h\"\nclass Spinner : public jasperengine::Mesh {\npublic:\n    void update();\n};\n");
    writeFile(project()->getProjectPath() / "scripts" / "Spinner.cpp", "#include \"Spinner.h\"\nvoid Spinner::update() {}\n");

    ScriptEntry entry;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(ScriptAttach::makeEntry(project(), lua("ai/patrol.lua"), "", entry, warnings, error)) << error;
    EXPECT_EQ(entry.type, ScriptType::LUA);
    EXPECT_EQ(entry.path.str(), "ai/patrol.lua");
    EXPECT_EQ(entry.className, "patrol");

    EXPECT_FALSE(ScriptAttach::makeEntry(project(), project()->getProjectPath() / "outside.lua", "", entry, warnings, error));
    EXPECT_NE(error.find("Lua directory"), std::string::npos) << error;

    ASSERT_TRUE(ScriptAttach::makeEntry(project(), project()->getProjectPath() / "scripts" / "Mover.cpp", "", entry, warnings, error)) << error;
    EXPECT_EQ(entry.type, ScriptType::CPP);
    EXPECT_EQ(entry.className, "Mover");
    EXPECT_EQ(entry.headerPath.str(), "scripts/Mover.h");

    const std::vector<ScriptCandidate> scripts = ScriptAttach::findProjectScripts(project());
    auto find = [&](const std::string& name) {
        return std::find_if(scripts.begin(), scripts.end(), [&](const ScriptCandidate& c) { return c.name == name; });
    };
    ASSERT_NE(find("patrol"), scripts.end());
    EXPECT_EQ(find("patrol")->type, ScriptType::LUA);
    EXPECT_EQ(find("outside"), scripts.end());  // not in the Lua directory
    ASSERT_NE(find("Mover"), scripts.end());
    EXPECT_FALSE(find("Mover")->subclass);
    ASSERT_NE(find("Spinner"), scripts.end());
    EXPECT_TRUE(find("Spinner")->subclass);
    EXPECT_EQ(find("Spinner")->baseClass, "Mesh");
}

TEST_F(ScriptingEditor, ScriptAttachIsOneUndoStepAndSkipsDuplicates) {
    writeFile(lua("guard.lua"), kSpeedScript);
    std::vector<Entity> entities;
    SceneProject* scene = newScene("Guards", entities, 2);
    ScriptEntry entry;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(ScriptAttach::makeEntry(project(), lua("guard.lua"), "", entry, warnings, error)) << error;

    std::vector<Entity> skipped;
    ASSERT_TRUE(ScriptAttach::attach(project(), scene, entities, entry, -1, skipped, error)) << error;
    EXPECT_TRUE(skipped.empty());
    for (Entity entity : entities) {
        const ScriptComponent* component = scene->scene->findComponent<ScriptComponent>(entity);
        ASSERT_NE(component, nullptr);
        ASSERT_EQ(component->scripts.size(), 1u);
        EXPECT_EQ(names(component->scripts[0].properties), std::vector<std::string>{"speed"});
    }

    // attached again: both skipped, nothing done
    EXPECT_FALSE(ScriptAttach::attach(project(), scene, entities, entry, -1, skipped, error));
    EXPECT_EQ(skipped.size(), 2u);

    // one undo step takes it off both
    ASSERT_TRUE(CommandHandle::get(scene->id)->undo());
    for (Entity entity : entities) {
        EXPECT_EQ(scene->scene->findComponent<ScriptComponent>(entity), nullptr);
    }
}

TEST_F(ScriptingEditor, PartMeshSettingsSurviveRebuild) {
    const fs::path assets = project()->getAssetsPath();
    editor::test::writeTwoPartGltf(assets / "models", "parts");
    const std::string file = "models/parts.gltf";

    Project* p = project();
    if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
    const uint32_t id = p->createNewScene("Parts", SceneType::SCENE_3D);
    SceneProject* sceneProject = p->getScene(id);
    Scene* scene = sceneProject->scene;
    const Entity model = scene->createUserEntity();
    scene->setEntityName(model, "Parts");
    scene->addComponent<Transform>(model, Transform());
    scene->addComponent<MeshComponent>(model, {});
    scene->addComponent<ModelComponent>(model, {});
    sceneProject->entities.push_back(model);
    Engine::setAsyncLoading(false);
    ASSERT_TRUE(CommandHandle::get(id)->addCommand(new ModelLoadCmd(p, id, model, file)));

    auto eyes = [&]() {
        const ModelComponent& component = scene->getComponent<ModelComponent>(model);
        auto node = component.meshNodesMapping.find(1);
        return node == component.meshNodesMapping.end() ? NULL_ENTITY : node->second;
    };
    const Entity oldEyes = eyes();
    ASSERT_NE(oldEyes, NULL_ENTITY);
    scene->getComponent<MeshComponent>(oldEyes).castShadows = false;
    scene->getComponent<MeshComponent>(oldEyes).receiveLights = false;

    // the same file again: the parts are made anew
    ASSERT_TRUE(CommandHandle::get(id)->addCommand(new ModelLoadCmd(p, id, model, file)));
    const Entity newEyes = eyes();
    ASSERT_NE(newEyes, NULL_ENTITY);
    EXPECT_NE(newEyes, oldEyes) << "a rebuild";
    EXPECT_FALSE(scene->getComponent<MeshComponent>(newEyes).castShadows);
    EXPECT_FALSE(scene->getComponent<MeshComponent>(newEyes).receiveLights);
    EXPECT_TRUE(scene->getComponent<MeshComponent>(scene->getComponent<ModelComponent>(model).meshNodesMapping.at(0)).castShadows);

    ASSERT_TRUE(CommandHandle::get(id)->undo());
    ASSERT_NE(eyes(), NULL_ENTITY);
    EXPECT_FALSE(scene->getComponent<MeshComponent>(eyes()).castShadows);
}
