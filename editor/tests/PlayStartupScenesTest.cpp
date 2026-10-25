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

#include <unistd.h>

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

TEST(ClosedSceneScripts, ForgottenWhenTheSceneCloses) {
    const fs::path dir = tempDir("reopened");
    const fs::path file = dir / "Reopened.scene";
    writeLuaOnlyScene(file, "Reopened");

    Project project;
    SceneProject& listed = addScene(project, 511, "Reopened");
    listed.opened = false;
    listed.filepath = file;
    ASSERT_TRUE(project.collectClosedSceneCppScripts(listed));
    ASSERT_EQ(project.getClosedSceneScriptReads(), 1u);

    // opened, its scripts changed in memory (not saved), closed again
    SceneProject* reopened = findScene(project, 511);
    reopened->opened = true;
    reopened->scene = new Scene();
    reopened->cppScripts.push_back(SceneScriptSource{"scripts/Removed.cpp", "scripts/Removed.h", "Removed", {}});
    project.closeScene(511, true);

    SceneProject* closed = findScene(project, 511);
    ASSERT_NE(closed, nullptr);
    EXPECT_FALSE(closed->opened);
    EXPECT_EQ(closed->scene, nullptr);
    EXPECT_FALSE(closed->cppScriptsRead) << "what it had in memory is not its file's";
    EXPECT_TRUE(closed->cppScripts.empty());
    EXPECT_TRUE(project.collectClosedSceneCppScripts(*closed)) << "the next Play reads its file again";
    EXPECT_TRUE(closed->cppScripts.empty());
    EXPECT_EQ(project.getClosedSceneScriptReads(), 2u);

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

    // what deleting a path would take out of the project: a closed scene's entry
    EXPECT_TRUE(project.deletionRemovesScene(closedFile));
    EXPECT_TRUE(project.deletionRemovesScene(dir / "scenes")) << "a folder holding it";
    EXPECT_FALSE(project.deletionRemovesScene(openFile)) << "an open scene only loses its path";
    EXPECT_FALSE(project.deletionRemovesScene(dir / "scenes" / "Other.scene"));
    EXPECT_FALSE(project.deletionRemovesScene(playedFile)) << "open, and nothing plays";

    EXPECT_FALSE(project.refuseSceneRemovalWhilePlaying("remove a scene")) << "stopped: allowed";
    EXPECT_FALSE(project.refuseDeletionWhilePlaying({closedFile}));

    PlaySessionTestAccess::beginSession(project, 801);
    EXPECT_TRUE(project.deletionRemovesScene(playedFile)) << "the played scene's file";
    std::string reason;
    EXPECT_TRUE(project.refuseSceneRemovalWhilePlaying("remove a scene", &reason, false));
    EXPECT_NE(reason.find("while a scene is playing"), std::string::npos) << reason;

    project.closeScene(802, true);
    EXPECT_NE(findScene(project, 802), nullptr) << "closing a never-saved scene is refused while playing";
    project.closeScene(801, true);
    EXPECT_TRUE(findScene(project, 801)->opened) << "closing the played scene is refused";
    project.removeScene(805);
    EXPECT_NE(findScene(project, 805), nullptr) << "removing a scene is refused while playing";

    reason.clear();
    EXPECT_TRUE(project.refuseDeletionWhilePlaying({openFile, closedFile}, &reason, false));
    EXPECT_NE(reason.find("Listed.scene"), std::string::npos) << reason;

    DeleteFileCmd deleteClosed(&project, {closedFile}, dir);
    deleteClosed.setAlertOnRefusal(false);
    EXPECT_FALSE(deleteClosed.execute()) << "deleting a closed scene's file is refused while playing";
    EXPECT_NE(deleteClosed.getRefusal().find("while a scene is playing"), std::string::npos);
    EXPECT_TRUE(fs::exists(closedFile));
    EXPECT_NE(findScene(project, 803), nullptr);

    DeleteFileCmd deletePlayed(&project, {playedFile}, dir);
    deletePlayed.setAlertOnRefusal(false);
    EXPECT_FALSE(deletePlayed.execute()) << "the played scene's file too";
    EXPECT_TRUE(fs::exists(playedFile));

    DeleteFileCmd deleteOpen(&project, {openFile}, dir);
    EXPECT_TRUE(deleteOpen.execute()) << "an open scene's file may go: the scene stays, without a path";
    EXPECT_TRUE(deleteOpen.getRefusal().empty());
    EXPECT_FALSE(fs::exists(openFile));
    ASSERT_NE(findScene(project, 804), nullptr);
    EXPECT_TRUE(findScene(project, 804)->filepath.empty());

    PlaySessionTestAccess::endSession(project);
    EXPECT_FALSE(project.refuseSceneRemovalWhilePlaying("remove a scene"));
    DeleteFileCmd deleteStopped(&project, {closedFile}, dir);
    EXPECT_TRUE(deleteStopped.execute()) << "allowed once stopped";
    EXPECT_FALSE(fs::exists(closedFile));
    EXPECT_EQ(findScene(project, 803), nullptr) << "the closed scene left the project with its file";

    fs::remove_all(dir);
}
