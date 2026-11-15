// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Play and the project's scenes: a closed scene's scripts are read from its file once per version
// of it (Project::collectClosedSceneCppScripts, run on every Play's startup), the played scene is
// found by id however the scenes vector moves (Project::runtimeOf), and no scene leaves the project
// while a scene plays (closing a never-saved scene, removing one, deleting a listed .scene file).
// Play itself runs in the editor (tools/mcp/tests/test_play_startup_scenes.py).

#include <gtest/gtest.h>

#include "Project.h"
#include "command/type/DeleteFileCmd.h"
#include "window/HierarchyWindow.h"
#include "Engine.h"
#include "manager/SceneManager.h"
#include "manager/ObjectManager.h"
#include "component/Transform.h"

#include "support/PosixCompat.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace jasperengine::editor {

    // A play session as Project::preparePlayStartup leaves it, without starting the game
    struct PlaySessionTestAccess {
        static void beginSession(Project& project, uint32_t playedSceneId) {
            auto session = std::make_shared<Project::PlaySession>();
            session->mainSceneId = playedSceneId;
            Project::PlayRuntimeScene entry;
            entry.sourceSceneId = playedSceneId;
            entry.ownedRuntime = false;
            session->runtimeScenes.push_back(entry);
            project.activePlaySession = session;
        }

        static void setProjectPath(Project& project, const fs::path& path) {
            project.projectPath = path;
        }

        static void endSession(Project& project) {
            project.activePlaySession.reset();
        }

        static SceneProject* playedRuntime(Project& project) {
            return project.runtimeOf(project.activePlaySession->runtimeScenes.front());
        }

        static Scene* playedScene(Project& project) {
            return project.runtimeSceneOf(project.activePlaySession->runtimeScenes.front());
        }

        static SceneProject* sceneProjectOf(Project& project, Scene* scene) {
            return project.findSceneProjectByScene(scene);
        }

        // persistence: the session's DontDestroyOnLoad scene, the editor's side of a move
        static void markStarted(Project& project) {
            project.activePlaySession->startupSucceeded.store(true);
        }
        static Scene* createPersistent(Project& project) {
            return project.createPersistentRuntimeScene();
        }
        static void objectsMoved(Project& project, Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& moved) {
            project.onPlayObjectsMoved(from, to, moved);
        }
        static void setInspectedScene(Project& project, uint32_t sceneId) {
            project.selectedSceneForProperties = sceneId;
        }
        static uint32_t inspectedScene(Project& project) {
            return project.selectedSceneForProperties;
        }
        static void cleanup(Project& project) {
            project.cleanupPlaySession(project.activePlaySession);
            project.activePlaySession.reset();
        }
    };

    struct HierarchyTestAccess {
        static void rebuild(HierarchyWindow& window, SceneProject* sceneProject, TreeNode& root) {
            std::unordered_set<Entity> entities;
            window.rebuildEntityTree(sceneProject, root, entities);
        }
    };

}

namespace {

    fs::path tempDir(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / ("jasper-play-scenes-test-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }

    // A scene whose only GameObject runs a Lua script: no C++ scripts to build
    void writeLuaOnlyScene(const fs::path& path, const std::string& entityName) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "id: 77\n"
                "name: Closed\n"
                "scene:\n"
                "  backgroundColor: [0.1, 0.1, 0.1, 1]\n"
                "sceneType: scene_3d\n"
                "mainCamera: 0\n"
                "entities:\n"
                "  - type: Entity\n"
                "    entity: 1001\n"
                "    name: " << entityName << "\n"
                "    components:\n"
                "      transform:\n"
                "        position: [0, 0, 0]\n"
                "        rotation: [1, 0, 0, 0]\n"
                "        scale: [1, 1, 1]\n"
                "        visible: true\n"
                "      script:\n"
                "        scripts:\n"
                "          - type: lua\n"
                "            path: Probe.lua\n"
                "            className: Probe\n"
                "            enabled: true\n";
    }

    SceneProject& addScene(Project& project, uint32_t id, const std::string& name) {
        SceneProject scene;
        scene.id = id;
        scene.name = name;
        project.getScenes().push_back(std::move(scene));
        return project.getScenes().back();
    }

    SceneProject* findScene(Project& project, uint32_t id) {
        for (SceneProject& scene : project.getScenes()) {
            if (scene.id == id) return &scene;
        }
        return nullptr;
    }

}

// ---- closed scenes' scripts ------------------------------------------------------------------

TEST(ClosedSceneScripts, ReadOncePerVersionOfTheFile) {
    const fs::path dir = tempDir("closed");
    const fs::path file = dir / "Closed.scene";
    writeLuaOnlyScene(file, "First");

    Project project;
    SceneProject& closed = addScene(project, 501, "Closed");
    closed.opened = false;
    closed.filepath = file;

    EXPECT_TRUE(project.collectClosedSceneCppScripts(closed)) << "the first Play reads the file";
    EXPECT_EQ(project.getClosedSceneScriptReads(), 1u);
    EXPECT_TRUE(closed.cppScripts.empty()) << "a Lua-only scene has no C++ scripts";
    EXPECT_TRUE(closed.cppScriptsRead);

    EXPECT_FALSE(project.collectClosedSceneCppScripts(closed)) << "the next Play does not decode it again";
    EXPECT_FALSE(project.collectClosedSceneCppScripts(closed));
    EXPECT_EQ(project.getClosedSceneScriptReads(), 1u);

    // the file changes (saved from elsewhere): read again, once
    writeLuaOnlyScene(file, "Second");
    fs::last_write_time(file, closed.cppScriptsReadStamp + std::chrono::seconds(5));
    EXPECT_TRUE(project.collectClosedSceneCppScripts(closed));
    EXPECT_FALSE(project.collectClosedSceneCppScripts(closed));
    EXPECT_EQ(project.getClosedSceneScriptReads(), 2u);

    // the same write time, another size (a copy keeping the time, two writes within one tick)
    writeLuaOnlyScene(file, "Third, with a longer name");
    fs::last_write_time(file, closed.cppScriptsReadStamp);
    EXPECT_TRUE(project.collectClosedSceneCppScripts(closed));
    EXPECT_FALSE(project.collectClosedSceneCppScripts(closed));
    EXPECT_EQ(project.getClosedSceneScriptReads(), 3u);

    // an open scene, or one with no file, is never read
    SceneProject& unsaved = addScene(project, 502, "Unsaved");
    unsaved.opened = false;
    EXPECT_FALSE(project.collectClosedSceneCppScripts(unsaved));
    EXPECT_EQ(project.getClosedSceneScriptReads(), 3u);

    fs::remove_all(dir);
}

TEST(ClosedSceneScripts, TheFileListAgainWhenTheSceneCloses) {
    const fs::path dir = tempDir("reopened");
    fs::create_directories(dir / "scripts");
    for (const char* name : {"Kept.cpp", "Kept.h", "Unsaved.cpp", "Unsaved.h"}) {
        std::ofstream(dir / "scripts" / name) << "// " << name << "\n";
    }
    // saved with one C++ script: its header lists it (what a closed scene gets at project load)
    {
        std::ofstream file(dir / "Reopened.scene", std::ios::binary | std::ios::trunc);
        file << "id: 78\n"
                "name: Reopened\n"
                "scene:\n"
                "  backgroundColor: [0.1, 0.1, 0.1, 1]\n"
                "sceneType: scene_3d\n"
                "mainCamera: 0\n"
                "cppScripts:\n"
                "  - path: scripts/Kept.cpp\n"
                "    headerPath: scripts/Kept.h\n"
                "    className: Kept\n"
                "entities: []\n";
    }
    writeLuaOnlyScene(dir / "LuaOnly.scene", "LuaOnly");

    Project project;
    PlaySessionTestAccess::setProjectPath(project, dir);
    SceneProject& cpp = addScene(project, 511, "Reopened");
    cpp.filepath = "Reopened.scene";
    cpp.scene = new Scene();
    // open, its scripts changed in memory and not saved
    cpp.cppScripts.push_back(SceneScriptSource{"scripts/Unsaved.cpp", "scripts/Unsaved.h", "Unsaved", {}});
    SceneProject& lua = addScene(project, 512, "LuaOnly");
    lua.filepath = "LuaOnly.scene";
    lua.scene = new Scene();
    lua.cppScriptsRead = true;  // read once before it was opened

    auto buildClasses = [&project]() {
        std::vector<std::string> classes;
        for (const SceneScriptSource& script : project.getBuildCppScripts()) classes.push_back(script.className);
        return classes;
    };
    EXPECT_EQ(buildClasses(), std::vector<std::string>{"Unsaved"}) << "an open scene builds what it has";

    project.closeScene(511, true);
    project.closeScene(512, true);

    SceneProject* closed = findScene(project, 511);
    ASSERT_NE(closed, nullptr);
    EXPECT_FALSE(closed->opened);
    EXPECT_EQ(closed->scene, nullptr);
    ASSERT_EQ(closed->cppScripts.size(), 1u) << "the file's list again, not what memory had";
    EXPECT_EQ(closed->cppScripts[0].className, "Kept");
    // exports (Exporter::copyCppScripts) and script builds (prepareScriptBuild) build this list
    EXPECT_EQ(buildClasses(), std::vector<std::string>{"Kept"});

    SceneProject* luaClosed = findScene(project, 512);
    ASSERT_NE(luaClosed, nullptr);
    EXPECT_TRUE(luaClosed->cppScripts.empty());
    EXPECT_FALSE(luaClosed->cppScriptsRead) << "the next Play reads its file again";
    EXPECT_TRUE(project.collectClosedSceneCppScripts(*luaClosed));
    EXPECT_FALSE(project.collectClosedSceneCppScripts(*luaClosed));
    EXPECT_EQ(project.getClosedSceneScriptReads(), 1u);

    fs::remove_all(dir);
}

// ---- the played scene and the scenes vector --------------------------------------------------

TEST(PlayedScene, FoundByIdWhenTheScenesVectorMoves) {
    Project project;
    addScene(project, 601, "Before");
    Scene* playedScene = new Scene();
    addScene(project, 602, "Played").scene = playedScene;
    PlaySessionTestAccess::beginSession(project, 602);
    ASSERT_EQ(PlaySessionTestAccess::playedRuntime(project), findScene(project, 602));

    // the scene in front goes (the played one moves down) and many come (the vector reallocates)
    std::vector<SceneProject>& scenes = project.getScenes();
    scenes.erase(scenes.begin());
    for (uint32_t id = 700; id < 764; id++) {
        addScene(project, id, "Added " + std::to_string(id));
    }

    SceneProject* played = PlaySessionTestAccess::playedRuntime(project);
    ASSERT_NE(played, nullptr);
    EXPECT_EQ(played, findScene(project, 602)) << "the element where it is now";
    EXPECT_EQ(played->name, "Played");
    EXPECT_EQ(PlaySessionTestAccess::playedScene(project), playedScene);
    EXPECT_EQ(PlaySessionTestAccess::sceneProjectOf(project, playedScene), played);

    PlaySessionTestAccess::endSession(project);
    findScene(project, 602)->scene = nullptr;
    delete playedScene;
}

TEST(PlayedScene, NoSceneLeavesTheProjectWhilePlaying) {
    const fs::path dir = tempDir("removal");
    fs::create_directories(dir / "scenes");
    const fs::path closedFile = dir / "scenes" / "Listed.scene";
    const fs::path openFile = dir / "scenes" / "Open.scene";
    const fs::path playedFile = dir / "Played.scene";
    writeLuaOnlyScene(closedFile, "Listed");
    writeLuaOnlyScene(openFile, "Open");
    writeLuaOnlyScene(playedFile, "Played");

    Project project;
    PlaySessionTestAccess::setProjectPath(project, dir);
    SceneProject& played = addScene(project, 801, "Played");
    played.filepath = "Played.scene";
    SceneProject& untitled = addScene(project, 802, "Untitled");
    untitled.opened = true;
    SceneProject& listed = addScene(project, 803, "Listed");
    listed.opened = false;
    listed.filepath = "scenes/Listed.scene";  // project-relative, as the project lists it
    SceneProject& open = addScene(project, 804, "Open");
    open.filepath = "scenes/Open.scene";
    addScene(project, 805, "Other");

    EXPECT_FALSE(project.refuseSceneRemovalWhilePlaying("remove a scene")) << "stopped: allowed";

    PlaySessionTestAccess::beginSession(project, 801);
    std::string reason;
    EXPECT_TRUE(project.refuseSceneRemovalWhilePlaying("remove a scene", &reason, false));
    EXPECT_NE(reason.find("while a scene is playing"), std::string::npos) << reason;

    project.closeScene(802, true);
    EXPECT_NE(findScene(project, 802), nullptr) << "closing a never-saved scene is refused while playing";
    project.closeScene(801, true);
    EXPECT_TRUE(findScene(project, 801)->opened) << "closing the played scene is refused";
    project.removeScene(805);
    EXPECT_NE(findScene(project, 805), nullptr) << "removing a scene is refused while playing";

    // Deleting a scene's file never takes its scene out of the project (S04 R15: the entry stays,
    // Missing until the file comes back), the closed one's and the played one's alike: no
    // deletion needs refusing while playing
    DeleteFileCmd deleteClosed(&project, {closedFile}, dir);
    EXPECT_TRUE(deleteClosed.execute());
    EXPECT_FALSE(fs::exists(closedFile));
    EXPECT_NE(findScene(project, 803), nullptr) << "the closed scene stays listed";
    DeleteFileCmd deletePlayed(&project, {playedFile}, dir);
    EXPECT_TRUE(deletePlayed.execute());
    ASSERT_NE(findScene(project, 801), nullptr) << "the played scene stays, run in place";
    deletePlayed.undo();
    deleteClosed.undo();
    EXPECT_TRUE(fs::exists(closedFile)) << "undo puts the files back";
    EXPECT_TRUE(fs::exists(playedFile));

    PlaySessionTestAccess::endSession(project);
    EXPECT_FALSE(project.refuseSceneRemovalWhilePlaying("remove a scene"));
    fs::remove_all(dir);
}

// ---- persistence: the DontDestroyOnLoad scene of a Play session ------------------------------

TEST(PersistentPlayScene, HierarchyRowAndSelectionFollowTheMove) {
    Project project;
    Scene* played = new Scene();
    SceneProject& menu = addScene(project, 910, "Menu");
    menu.scene = played;
    menu.sceneType = SceneType::SCENE_2D;
    const Entity manager = played->createEntity();
    played->addComponent<Transform>(manager, Transform());
    played->setEntityName(manager, "Manager");
    menu.entities = {manager};
    menu.selectedEntities = {manager};
    PlaySessionTestAccess::beginSession(project, 910);
    PlaySessionTestAccess::markStarted(project);
    findScene(project, 910)->playState = ScenePlayState::PLAYING;
    PlaySessionTestAccess::setInspectedScene(project, 910);
    const std::vector<Scene*> previousScenes = Engine::getSceneLayers();
    Engine::removeAllScenes();
    Engine::setScene(played);

    Scene* persistent = PlaySessionTestAccess::createPersistent(project);
    ASSERT_NE(persistent, nullptr);
    Engine::addSceneLayer(persistent);
    const uint32_t persistentId = project.getPersistentPlaySceneId();
    ASSERT_NE(persistentId, NULL_PROJECT_SCENE);
    EXPECT_TRUE(project.isPersistentPlayScene(persistentId));
    SceneProject* persistentProject = project.getScene(persistentId);
    ASSERT_NE(persistentProject, nullptr);
    EXPECT_EQ(persistentProject->name, "DontDestroyOnLoad");
    EXPECT_EQ(persistentProject->playState, ScenePlayState::PLAYING);

    ObjectManager::setMoveListener([&project](Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& moved) {
        PlaySessionTestAccess::objectsMoved(project, from, to, moved);
    });
    const std::vector<Entity> moved = ObjectManager::moveToScene(played, manager, persistent);
    ObjectManager::setMoveListener(nullptr);
    ASSERT_EQ(moved.size(), 1u);
    SceneProject* menuNow = findScene(project, 910);
    EXPECT_TRUE(menuNow->entities.empty()) << "it left the Menu's list";
    EXPECT_TRUE(menuNow->selectedEntities.empty());
    EXPECT_EQ(persistentProject->entities, moved) << "listed (editable) in the persistent scene";
    EXPECT_EQ(persistentProject->selectedEntities, moved) << "the selection follows";
    EXPECT_EQ(PlaySessionTestAccess::inspectedScene(project), persistentId) << "and the Inspector";

    const std::vector<SceneProject*> layers = project.getRunningRuntimeLayerScenes(910);
    ASSERT_FALSE(layers.empty());
    EXPECT_EQ(layers.back(), persistentProject);

    HierarchyWindow window(&project, nullptr);
    TreeNode root;
    HierarchyTestAccess::rebuild(window, menuNow, root);
    ASSERT_FALSE(root.children.empty());
    const TreeNode& row = root.children.back();
    EXPECT_TRUE(row.isPersistent) << "the last root row";
    EXPECT_EQ(row.name, "DontDestroyOnLoad");
    EXPECT_EQ(row.runtimeSceneId, persistentId);
    ASSERT_EQ(row.children.size(), 1u);
    EXPECT_EQ(row.children.front().name, "Manager");
    EXPECT_EQ(row.children.front().id, moved[0]);

    Engine::removeAllScenes();
    PlaySessionTestAccess::cleanup(project);
    findScene(project, 910)->scene = nullptr;
    delete played;
    for (size_t i = 0; i < previousScenes.size(); i++) {
        if (i == 0) Engine::setScene(previousScenes[i]);
        else Engine::addSceneLayer(previousScenes[i]);
    }
}
