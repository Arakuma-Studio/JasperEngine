// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation Player's plumbing that needs a project on disk (animation clips spec 5, 9.R8/R26/
// R41): set_properties / add_component values for clips (McpValues: the accepted forms, every
// refusal and its text, a clip list read and written back with a deleted clip in it, read-only
// GUIDs), the asset path visitor (a moved clip folder, the model part of "<model>#<clip>", a
// deletion keeping the reference) and the scene resources the generated scene source preloads.
// A headless project (AppSettings in a temporary config directory + Project::loadProject), the
// engine started on the editor's Platform as PlacementSessionTest does (model clip names are read
// through the ModelPool).

#include <gtest/gtest.h>
#include "component/AnimationPlayerComponent.h"
#include "component/Transform.h"

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Factory.h"
#include "Platform.h"
#include "Project.h"
#include "asset/AssetOps.h"
#include "command/type/MultiPropertyCmd.h"
#include "mcp/McpValues.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::editor::mcp::Json;
using jasperengine::editor::mcp::McpValues;
namespace fs = std::filesystem;

namespace {

    const char* const WALK_GUID = "0123456789abcdef0123456789abcdef";
    const char* const RUN_GUID = "fedcba9876543210fedcba9876543210";
    const char* const EXAMPLE = " (e.g. assets/anim/walk.anim, or assets/models/hero.gltf#Walk for a model's animation)";

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-player-project-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            // the editor's own settings (last project, ...) stay out of the user's
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            // an assets folder of its own, so project-relative ("assets/anim/walk.anim") and
            // stored ("anim/walk.anim") paths differ as in real projects
            std::ofstream(out.dir / "project.yaml") << "name: playerproject\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the model clip names are read through the ModelPool (File needs the System), as
            // PlacementSessionTest and the clip assets tests start it
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        // every test of the project (the paths end with each test)
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* project() { return headless().project.get(); }

    fs::path assets() { return project()->getAssetsPath(); }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeClip(const std::string& assetsRelative, const std::string& guid) {
        writeAll(assets() / assetsRelative,
                 "{\n  \"format\": \"jasperengine.anim\",\n  \"version\": 1,\n  \"guid\": \"" + guid + "\",\n"
                 "  \"frame_rate\": 30,\n  \"wrap\": \"loop\",\n  \"tracks\": []\n}\n");
    }

    // a fresh folder of assets per test
    void resetAssets() {
        fs::remove_all(assets() / "anim");
        fs::remove_all(assets() / "clips");
        fs::remove_all(assets() / "models");
        fs::remove_all(assets() / "characters");
        writeClip("anim/walk.anim", WALK_GUID);
        writeClip("anim/run.anim", RUN_GUID);
        // T1c's animated hero: Walk, Idle, Animation 2 (unnamed), Wave, Wave 2
        fs::create_directories(assets() / "models");
        fs::copy_file(fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "pose-animation" / "assets" /
                      "models" / "hero_animated.gltf", assets() / "models/hero.gltf", fs::copy_options::overwrite_existing);
    }

    // A new scene with one GameObject holding an Animation Player with `clips`
    struct PlayerScene {
        SceneProject* sceneProject = nullptr;
        Entity entity = NULL_ENTITY;

        AnimationPlayerComponent& player() const {
            return sceneProject->scene->getComponent<AnimationPlayerComponent>(entity);
        }
    };

    PlayerScene playerScene(const std::vector<AnimationPlayerClip>& clips) {
        PlayerScene out;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Players", SceneType::SCENE_3D);
        out.sceneProject = p->getScene(p->getSelectedSceneId());
        Scene* scene = out.sceneProject->scene;
        out.entity = scene->createUserEntity();
        scene->setEntityName(out.entity, "Hero");
        scene->addComponent<Transform>(out.entity, {});
        AnimationPlayerComponent player;
        player.clips = clips;
        scene->addComponent<AnimationPlayerComponent>(out.entity, player);
        out.sceneProject->entities.push_back(out.entity);
        return out;
    }

    AnimationPlayerClip entry(const std::string& clip, const std::string& guid = "") {
        AnimationPlayerClip e;
        e.clip = clip;
        e.guid = guid;
        return e;
    }

    // set_properties' path: the changes as one command, executed; the error otherwise
    bool apply(const PlayerScene& s, const Json& values, std::string& error) {
        std::vector<McpValues::Change> changes;
        if (!McpValues::parseChanges(values, changes, error)) return false;
        MultiPropertyCmd multi;
        if (!McpValues::appendChangeCommands(project(), s.sceneProject, {s.entity}, changes, multi, error)) return false;
        return multi.execute();
    }

    void applyOk(const PlayerScene& s, const Json& values) {
        std::string error;
        EXPECT_TRUE(apply(s, values, error)) << values.dump() << ": " << error;
    }

    std::string refusal(const PlayerScene& s, const Json& values) {
        std::string error;
        EXPECT_FALSE(apply(s, values, error)) << values.dump();
        return error;
    }

    Json readClips(const PlayerScene& s) {
        McpValues::ReadContext ctx;
        ctx.project = project();
        ctx.scene = s.sceneProject;
        ctx.entity = s.entity;
        ctx.component = ComponentType::AnimationPlayerComponent;
        const PropertyData clips = Catalog::findProperty(s.sceneProject->scene, s.entity, ComponentType::AnimationPlayerComponent, "clips");
        return McpValues::readProperty(ctx, "clips", clips);
    }

    Json player(const Json& values) { return Json{{"animationplayer", values}}; }

}

TEST(AnimationPlayerProject, HeadlessProjectOpens) {
    ASSERT_TRUE(headless().ready);
    EXPECT_EQ(project()->getProjectPath(), headless().dir);
}

// ---- McpValues -----------------------------------------------------------------------------

TEST(AnimationPlayerProject, ClipPathsInBothFormsWithTheirGuids) {
    ASSERT_TRUE(headless().ready);
    resetAssets();
    PlayerScene s = playerScene({entry("anim/walk.anim", WALK_GUID)});

    // project-relative (as the tools name assets) and the stored form of an existing file
    applyOk(s, player({{"clips[0].clip", "assets/anim/run.anim"}}));
    EXPECT_EQ(s.player().clips[0].clip, "anim/run.anim");
    EXPECT_EQ(s.player().clips[0].guid, RUN_GUID);
    applyOk(s, player({{"clips[0].clip", "anim/walk.anim"}}));
    EXPECT_EQ(s.player().clips[0].clip, "anim/walk.anim");
    EXPECT_EQ(s.player().clips[0].guid, WALK_GUID);

    // a model's animation: path only, no GUID; an index is stored as the name (canonical form)
    applyOk(s, player({{"clips[0].clip", "assets/models/hero.gltf#2"}}));
    EXPECT_EQ(s.player().clips[0].clip, "models/hero.gltf#Animation 2");
    applyOk(s, player({{"clips[0].clip", "assets/models/hero.gltf#Walk"}}));
    EXPECT_EQ(s.player().clips[0].clip, "models/hero.gltf#Walk");
    EXPECT_EQ(s.player().clips[0].guid, "");

    // read back project-relative, the wrap by its name
    const Json clips = readClips(s);
    ASSERT_EQ(clips.size(), 1u);
    EXPECT_EQ(clips[0]["clip"], "assets/models/hero.gltf#Walk");
    EXPECT_EQ(clips[0]["wrap"], "default");
    EXPECT_FALSE(clips[0].contains("guid"));

    // an entry set as a list, a plain path, a wrap
    applyOk(s, player({{"clips", Json::array({Json{{"clip", "assets/anim/walk.anim"}, {"wrap", "loop"}, {"name", "Walk"}}, "anim/run.anim"})}}));
    ASSERT_EQ(s.player().clips.size(), 2u);
    EXPECT_EQ(s.player().clips[0].clip, "anim/walk.anim");
    EXPECT_EQ(s.player().clips[0].guid, WALK_GUID);
    EXPECT_EQ(s.player().clips[0].wrap, ClipWrapMode::Loop);
    EXPECT_EQ(s.player().clips[0].name, "Walk");
    EXPECT_EQ(s.player().clips[1].clip, "anim/run.anim");
    EXPECT_EQ(s.player().clips[1].guid, RUN_GUID);
    applyOk(s, player({{"clips[1].wrap", "ping_pong"}}));
    EXPECT_EQ(s.player().clips[1].wrap, ClipWrapMode::PingPong);
}

TEST(AnimationPlayerProject, RefusalsNameTheProblemAndGiveAnExample) {
    ASSERT_TRUE(headless().ready);
    resetAssets();
    PlayerScene s = playerScene({entry("anim/walk.anim", WALK_GUID), entry("anim/run.anim", RUN_GUID)});
    const std::vector<AnimationPlayerClip> before = s.player().clips;
    const std::string on = "animationplayer.clips[0].clip on GameObject " + std::to_string(s.entity) + " \"Hero\": ";

    EXPECT_EQ(refusal(s, player({{"clips[0].clip", "assets/anim/missing.anim"}})),
              on + "clip: assets/anim/missing.anim does not exist: create it first with edit_animation {clip, create: {}}" + EXAMPLE + ".");
    EXPECT_EQ(refusal(s, player({{"clips[0].clip", "assets/models/hero.gltf"}})),
              on + "clip must be a .anim file or <model>#<animation>" + EXAMPLE + ".");
    EXPECT_EQ(refusal(s, player({{"clips[0].clip", "assets/models/hero.gltf#Run"}})),
              on + "clip: assets/models/hero.gltf has no animation \"Run\" (it has: Walk, Idle, Animation 2, Wave, Wave 2).");
    EXPECT_EQ(refusal(s, player({{"clips[0].clip", "assets/models/none.gltf#Walk"}})),
              on + "clip: the model assets/models/none.gltf does not exist" + EXAMPLE + ".");
    EXPECT_EQ(refusal(s, player({{"clips[0].clip", "../x.anim"}})),
              on + "clip: \"../x.anim\" must be a project-relative path (no '..', not absolute)." + EXAMPLE);
    EXPECT_EQ(refusal(s, player({{"clips[1].wrap", "sideways"}})),
              "animationplayer.clips[1].wrap on GameObject " + std::to_string(s.entity) +
              " \"Hero\": Property clips[1].wrap is a wrap mode: default, once, loop, ping_pong, clamp_forever.");
    EXPECT_EQ(refusal(s, player({{"clips[0].guid", RUN_GUID}})),
              "animationplayer.clips[0].guid on GameObject " + std::to_string(s.entity) +
              " \"Hero\": clips[0].guid is read-only: set clips[0].clip, its GUID follows the file.");

    const std::string list = "animationplayer on GameObject " + std::to_string(s.entity) + " \"Hero\": ";
    EXPECT_EQ(refusal(s, player({{"clips", Json::array({Json{{"clip", "assets/anim/walk.anim"}, {"wrap", "sideways"}}})}})),
              list + "clips[0].wrap \"sideways\" is not a wrap mode (default, once, loop, ping_pong, clamp_forever).");
    EXPECT_EQ(refusal(s, player({{"clips", Json::array({Json{{"clip", "assets/anim/walk.anim"}, {"loop", true}}})}})),
              list + "clips[0]: unknown key \"loop\" (an entry is {clip, name, wrap, layer, retarget, mirror, footIK}; its guid follows the clip).");
    EXPECT_EQ(refusal(s, player({{"clips", Json::array({Json{{"clip", "assets/anim/nope.anim"}}})}})),
              list + "clips[0].clip: assets/anim/nope.anim does not exist: create it first with edit_animation {clip, create: {}}" +
              EXAMPLE + ".");
    EXPECT_EQ(refusal(s, player({{"clips", "anim/walk.anim"}})),
              list + "clips must be a list of {clip, name, wrap}, e.g. [{\"clip\": \"assets/anim/walk.anim\", \"wrap\": \"loop\"}].");
    EXPECT_EQ(refusal(s, player({{"defaultFadeTime", -1}})),
              "animationplayer.defaultFadeTime on GameObject " + std::to_string(s.entity) +
              " \"Hero\": defaultFadeTime is in seconds and must be 0 or more (0.25 by default).");

    // nothing changed
    EXPECT_EQ(s.player().clips, before);
}

TEST(AnimationPlayerProject, AClipListReadAndWrittenBackKeepsItsMissingClips) {
    ASSERT_TRUE(headless().ready);
    resetAssets();
    PlayerScene s = playerScene({entry("anim/walk.anim", WALK_GUID), entry("anim/run.anim", RUN_GUID)});

    // walk.anim is deleted: its reference stays (R8); the list as read, one entry appended
    fs::remove(assets() / "anim/walk.anim");
    Json clips = readClips(s);
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_EQ(clips[0]["clip"], "assets/anim/walk.anim");
    clips.push_back(Json{{"clip", "assets/models/hero.gltf#Idle"}});
    applyOk(s, player({{"clips", clips}}));

    ASSERT_EQ(s.player().clips.size(), 3u);
    // the held entries keep their stored text and GUIDs, without being checked again
    EXPECT_EQ(s.player().clips[0], entry("anim/walk.anim", WALK_GUID));
    EXPECT_EQ(s.player().clips[1], entry("anim/run.anim", RUN_GUID));
    EXPECT_EQ(s.player().clips[2], entry("models/hero.gltf#Idle"));

    // a GUID given with an entry is never taken: the held one's, else the file's
    applyOk(s, player({{"clips", Json::array({Json{{"clip", "assets/anim/run.anim"}, {"guid", WALK_GUID}},
                                              Json{{"clip", "assets/models/hero.gltf#Idle"}, {"guid", RUN_GUID}}})}}));
    ASSERT_EQ(s.player().clips.size(), 2u);
    EXPECT_EQ(s.player().clips[0].guid, RUN_GUID);
    EXPECT_EQ(s.player().clips[1].guid, "");
    writeClip("anim/walk.anim", WALK_GUID);
    applyOk(s, player({{"clips", Json::array({Json{{"clip", "assets/anim/walk.anim"}, {"guid", RUN_GUID}}})}}));
    EXPECT_EQ(s.player().clips[0].guid, WALK_GUID);
}

// ---- the asset path visitor ----------------------------------------------------------------

TEST(AnimationPlayerProject, MovesRemapClipsAndTheModelPartOfSubClips) {
    ASSERT_TRUE(headless().ready);
    resetAssets();
    // the files the test wrote reach the asset database as an outside change would
    project()->onAssetFilesChanged(project()->getAssetDatabase().refresh());
    PlayerScene s = playerScene({entry("anim/walk.anim", WALK_GUID), entry("models/hero.gltf#Walk"), entry("anim/run.anim", RUN_GUID)});
    s.player().needsLoad = false;

    // the clips' folder moved (RenameFileCmd: AssetOps::move): the entries follow the GUIDs
    ASSERT_TRUE(AssetOps::move(*project(), assets() / "anim", assets() / "clips").ok);
    EXPECT_EQ(s.player().clips[0], entry("clips/walk.anim", WALK_GUID));
    EXPECT_EQ(s.player().clips[2], entry("clips/run.anim", RUN_GUID));
    EXPECT_TRUE(s.player().needsLoad);
    EXPECT_TRUE(s.sceneProject->isModified);

    // the model moved: the model part of the sub-clip follows (R26), the .anim entries stay
    fs::create_directories(assets() / "characters");
    ASSERT_TRUE(AssetOps::move(*project(), assets() / "models/hero.gltf", assets() / "characters/hero.gltf").ok);
    EXPECT_EQ(s.player().clips[1].clip, "characters/hero.gltf#Walk");
    EXPECT_EQ(s.player().clips[0].clip, "clips/walk.anim");

    // a deletion keeps the references (R8: Missing, and undoing the delete relinks them)
    s.player().needsLoad = false;
    fs::path trashedClips, trashedModel;
    ASSERT_TRUE(AssetOps::remove(*project(), assets() / "clips", trashedClips).ok);
    ASSERT_TRUE(AssetOps::remove(*project(), assets() / "characters/hero.gltf", trashedModel).ok);
    EXPECT_EQ(s.player().clips[0].clip, "clips/walk.anim");
    EXPECT_EQ(s.player().clips[1].clip, "characters/hero.gltf#Walk");
    EXPECT_EQ(s.player().clips[2].clip, "clips/run.anim");
}

// ---- scene resources -----------------------------------------------------------------------

TEST(AnimationPlayerProject, TheGeneratedSceneSourcePreloadsTheClipsAndTheirModels) {
    ASSERT_TRUE(headless().ready);
    resetAssets();
    PlayerScene s = playerScene({entry("anim/walk.anim", WALK_GUID), entry(""), entry("models/hero.gltf#Walk"),
                                 entry("anim/walk.anim", WALK_GUID)});
    ASSERT_TRUE(project()->saveSceneToPath(s.sceneProject->id, "Players.scene"));

    const fs::path source = project()->getProjectInternalPath() / "generated" /
                            (Factory::sceneToFileName(s.sceneProject->id, s.sceneProject->name) + ".cpp");
    ASSERT_TRUE(fs::exists(source)) << source;
    const std::string code = readAll(source);
    // each file once, the empty entry left out; a sub-clip's model is preloaded with it
    const std::string resources = code.substr(code.find("SceneResources resources_"));
    EXPECT_NE(resources.find("    resources.models.push_back(\"models/hero.gltf\");\n"
                             "    resources.clips.push_back(\"anim/walk.anim\");\n"
                             "    resources.clips.push_back(\"models/hero.gltf#Walk\");\n"
                             "    return resources;"), std::string::npos) << resources.substr(0, 600);
    EXPECT_NE(code.find("animationplayer.clips[2].clip = \"models/hero.gltf#Walk\";"), std::string::npos);

    // the scene file stores the GUIDs, the generated code does not
    EXPECT_NE(readAll(project()->getProjectPath() / "Players.scene").find(WALK_GUID), std::string::npos);
    EXPECT_EQ(code.find(WALK_GUID), std::string::npos);
}
