// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Animation clips spec 4.3, 4.5, 9.R3/R4/R15/R23, editor side: ClipValidation (every outcome: OK,
// missing path with the nearest names, duplicate sibling name, missing component, lattice index
// outside the cage, the legacy layout with its fix, handlers found / not found / skipped for C++
// scripts), the converters (an entity Animation converted to a clip samples the same values; a
// model sub-clip extracted with its source; Use Animation Player), the composite scene step
// (CreateClipCmd: file written on execute/redo, deleted on undo only when unchanged, both
// mismatch orders), clips dropped onto an object and Bake to Animation Clip playing like the
// entity bake. A headless project with the pose-animation fixture's heroes.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Out.h"
#include "Project.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipInspectorHooks.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "animation/ClipConverters.h"
#include "animation/ClipValidation.h"
#include "animation/ModelSubClips.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/ClipSceneActions.h"
#include "command/type/CreateClipCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/ModelComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"
#include "component/TranslateTracksComponent.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseBaker.h"
#include "subsystem/ActionSystem.h"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        std::unique_ptr<editor::Platform> platform;
    };

    fs::path fixtureModel(const std::string& name) {
        return (fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / "tools" / "fixtures" / "pose-animation" /
                "assets" / "models" / name).lexically_normal();
    }

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-clip-converters-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets" / "models");
            std::ofstream(out.dir / "project.yaml") << "name: clipconverters\nassetsDir: assets\n";
            // T1c's animated hero (Walk, Idle, Animation 2, Wave, Wave 2) and the T-pose hero
            // (no animations: the legacy layout when loaded alone)
            fs::copy_file(fixtureModel("hero_animated.gltf"), out.dir / "assets" / "models" / "hero.gltf", fs::copy_options::overwrite_existing);
            fs::copy_file(fixtureModel("hero_tpose.gltf"), out.dir / "assets" / "models" / "tpose.gltf", fs::copy_options::overwrite_existing);
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            static char arg0[] = "jasperengine-editor-tests";
            static char* argv[] = {arg0, nullptr};
            Engine::systemInit(1, argv, out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            // the project's settings turn it on again: models load synchronously here
            Engine::setAsyncLoading(false);
            return out;
        }();
        return h;
    }

    Project* project() { return headless().project.get(); }
    const fs::path& root() { return headless().dir; }
    CommandHistory* projectHistory() { return project()->getProjectCommandHistory(); }
    CommandHistory* history(uint32_t sceneId) { return CommandHandle::get(sceneId); }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    // a fresh folder under assets per test
    std::string folder(const std::string& name) {
        fs::remove_all(root() / "assets" / name);
        fs::create_directories(root() / "assets" / name);
        return "assets/" + name;
    }

    uint32_t newScene() {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Clips", SceneType::SCENE_3D);
        return p->getSelectedSceneId();
    }

    Scene* sceneOf(uint32_t sceneId) { return project()->getScene(sceneId)->scene; }

    Entity create(uint32_t sceneId, const std::string& name, EntityCreationType type, Entity parent = NULL_ENTITY) {
        CreateEntityCmd* cmd = parent == NULL_ENTITY ? new CreateEntityCmd(project(), sceneId, name, type)
                                                     : new CreateEntityCmd(project(), sceneId, name, type, parent);
        cmd->setQuiet(true);
        history(sceneId)->addCommandNoMerge(cmd);
        return cmd->getEntity();
    }

    Entity gameObject(uint32_t sceneId, const std::string& name, Entity parent = NULL_ENTITY) {
        return create(sceneId, name, EntityCreationType::GAMEOBJECT, parent);
    }

    Entity loadModel(uint32_t sceneId, const std::string& assetsRelative) {
        const Entity entity = create(sceneId, "Hero", EntityCreationType::MODEL);
        // as the Scene view's drop stores it: relative to the assets folder
        const std::string path = assetsRelative;
        const uint64_t logs = Out::lastLogId();
        history(sceneId)->addCommandNoMerge(new ModelLoadCmd(project(), sceneId, entity, path));
        const ModelComponent& model = sceneOf(sceneId)->getComponent<ModelComponent>(entity);
        std::string log;
        for (const LogEntry& entry : Out::getLogsSince(logs)) log += entry.message + "\n";
        EXPECT_FALSE(model.loadedFilename.empty()) << "loaded " << path << " ('" << model.filename << "'): " << log;
        return entity;
    }

    ClipTrackData track(const std::string& path, ClipChannel channel, std::vector<std::vector<float>> values) {
        ClipTrackData out;
        out.path = path;
        out.channel = channel;
        for (size_t i = 0; i < values.size(); i++) {
            ClipKey key;
            key.time = (float)i;
            key.value = values[i];
            out.keys.push_back(key);
        }
        return out;
    }

    const TrackReport* reportOf(const ClipValidationReport& report, const std::string& path, ClipChannel channel) {
        for (const TrackReport& t : report.tracks) {
            if (t.path == path && t.channel == channel) return &t;
        }
        return nullptr;
    }

    // ---- sampling the two forms on the same entities

    struct Pose {
        Vector3 position;
        Quaternion rotation;
        Vector3 scale;
        std::vector<Vector3> offsets;
    };

    Pose poseOf(Scene* scene, Entity entity) {
        Pose pose;
        if (const Transform* t = scene->findComponent<Transform>(entity)) {
            pose.position = t->position;
            pose.rotation = t->rotation;
            pose.scale = t->scale;
        }
        if (const LatticeComponent* lattice = scene->findComponent<LatticeComponent>(entity)) pose.offsets = lattice->offsets;
        return pose;
    }

    // the entity Animation at `time` (its own preview path: started, then its time set)
    void sampleEntityAnimation(Scene* scene, Entity animation, float time) {
        std::shared_ptr<ActionSystem> actions = scene->getSystem<ActionSystem>();
        ActionComponent& action = scene->getComponent<ActionComponent>(animation);
        // a trigger the state does not take stays set (a stop of a stopped animation would stop
        // the next start)
        action.stopTrigger = action.state != ActionState::Stopped;
        action.startTrigger = true;
        actions->updateAnimationPreview(0.0, animation);
        scene->getComponent<ActionComponent>(animation).timecount = time;
        actions->updateAnimationPreview(0.0, animation);
    }

    void sampleClip(Scene* scene, Entity root, const std::shared_ptr<const AnimationClip>& clip, float time, anim::ClipPreviewSession& session) {
        ActionSystem::PlayerPreview preview;
        ActionSystem::PlayerPreviewState state;
        state.clip = clip;
        state.time = time;
        preview.states.push_back(state);
        ActionSystem::PlayerPreviewResult result = scene->getSystem<ActionSystem>()->previewPlayer(root, preview, session);
        EXPECT_TRUE(result.ready);
        EXPECT_TRUE(result.unresolved.empty()) << result.unresolved.size() << " unresolved, first "
                                               << (result.unresolved.empty() ? std::string() : result.unresolved[0]);
    }

    void expectSamePose(const Pose& a, const Pose& b, const std::string& what, float tolerance = 2e-4f) {
        EXPECT_NEAR(a.position.x, b.position.x, tolerance) << what;
        EXPECT_NEAR(a.position.y, b.position.y, tolerance) << what;
        EXPECT_NEAR(a.position.z, b.position.z, tolerance) << what;
        EXPECT_NEAR(a.scale.x, b.scale.x, tolerance) << what;
        EXPECT_NEAR(a.scale.y, b.scale.y, tolerance) << what;
        EXPECT_NEAR(a.scale.z, b.scale.z, tolerance) << what;
        const float dot = a.rotation.w * b.rotation.w + a.rotation.x * b.rotation.x + a.rotation.y * b.rotation.y + a.rotation.z * b.rotation.z;
        EXPECT_NEAR(std::fabs(dot), 1.0f, tolerance) << what << " rotation";
        ASSERT_EQ(a.offsets.size(), b.offsets.size()) << what;
        for (size_t i = 0; i < a.offsets.size(); i++) {
            EXPECT_NEAR(a.offsets[i].x, b.offsets[i].x, tolerance) << what << " offset " << i;
            EXPECT_NEAR(a.offsets[i].y, b.offsets[i].y, tolerance) << what << " offset " << i;
            EXPECT_NEAR(a.offsets[i].z, b.offsets[i].z, tolerance) << what << " offset " << i;
        }
    }

    ClipData readClipFile(const fs::path& file) {
        anim::ClipParseResult parsed = anim::parseClip(readAll(file));
        EXPECT_TRUE(parsed.ok()) << file << ": " << parsed.error.toString();
        return parsed.ok() ? *parsed.data : ClipData();
    }

    class ClipConverters_ : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(headless().ready) << "the headless project did not open";
        }
    };

    // An entity Animation on `rootEntity`'s children: a position track starting at 0.25 s with a
    // quad-out segment, a stepped rotation track at speed 2, a cubic scale track and a lattice
    // track, plus a tween (not converted)
    struct EntityAnimation {
        Entity root = NULL_ENTITY;
        Entity arm = NULL_ENTITY;
        Entity hand = NULL_ENTITY;
        Entity cage = NULL_ENTITY;
        Entity animation = NULL_ENTITY;
        std::vector<Entity> tracks;
        Entity tween = NULL_ENTITY;
    };

    EntityAnimation entityAnimation(uint32_t sceneId) {
        EntityAnimation out;
        Scene* scene = sceneOf(sceneId);
        out.root = gameObject(sceneId, "Hero");
        out.arm = gameObject(sceneId, "Arm", out.root);
        out.hand = gameObject(sceneId, "Hand", out.arm);
        out.cage = create(sceneId, "Cage", EntityCreationType::LATTICE, out.root);
        scene->getComponent<Transform>(out.arm).position = Vector3(3, 0, 0);
        out.animation = create(sceneId, "Wave", EntityCreationType::ANIMATION, out.root);

        const Entity position = create(sceneId, "Arm position", EntityCreationType::TRANSLATE_TRACKS, out.arm);
        {
            KeyframeTracksComponent& keys = scene->getComponent<KeyframeTracksComponent>(position);
            keys.times = {0.0f, 0.5f, 1.0f};
            keys.easings = {EaseType::QUAD_OUT, EaseType::LINEAR};
            scene->getComponent<TranslateTracksComponent>(position).values = {Vector3(0, 0, 0), Vector3(0, 2, 0), Vector3(1, 2, 0)};
        }
        const Entity rotation = create(sceneId, "Hand rotation", EntityCreationType::ROTATE_TRACKS, out.hand);
        {
            KeyframeTracksComponent& keys = scene->getComponent<KeyframeTracksComponent>(rotation);
            keys.times = {0.0f, 1.0f, 2.0f};
            keys.easings = {EaseType::STEP, EaseType::LINEAR};
            scene->getComponent<RotateTracksComponent>(rotation).values = {Quaternion(), Quaternion(90, Vector3(0, 1, 0)),
                                                                            Quaternion(180, Vector3(1, 0, 0))};
            scene->getComponent<ActionComponent>(rotation).speed = 2.0f;
        }
        const Entity scale = create(sceneId, "Hero scale", EntityCreationType::SCALE_TRACKS, out.root);
        {
            KeyframeTracksComponent& keys = scene->getComponent<KeyframeTracksComponent>(scale);
            keys.times = {0.0f, 1.2f};
            ScaleTracksComponent& values = scene->getComponent<ScaleTracksComponent>(scale);
            values.values = {Vector3(1, 1, 1), Vector3(2, 1, 0.5f)};
            values.inTangents = {Vector3(0, 0, 0), Vector3(1, 0, 0)};
            values.outTangents = {Vector3(0.5f, 0, 0), Vector3(0, 0, 0)};
        }
        const Entity lattice = create(sceneId, "Cage points", EntityCreationType::LATTICE_TRACKS, out.cage);
        {
            KeyframeTracksComponent& keys = scene->getComponent<KeyframeTracksComponent>(lattice);
            keys.times = {0.0f, 0.8f};
            LatticeTracksComponent& values = scene->getComponent<LatticeTracksComponent>(lattice);
            values.points = {0, 13};
            values.values = {{Vector3(0, 0, 0), Vector3(0, 0, 0)}, {Vector3(0.1f, 0, 0), Vector3(0, 0.3f, 0)}};
        }
        out.tween = create(sceneId, "Fade", EntityCreationType::POSITION_TWEEN, out.arm);
        out.tracks = {position, rotation, scale, lattice};

        AnimationComponent& animation = scene->getComponent<AnimationComponent>(out.animation);
        animation.actions = {{0.25f, 0.0f, position, 0}, {0.0f, 0.0f, rotation, 1}, {0.0f, 0.0f, scale, 2},
                             {0.1f, 0.0f, lattice, 3}, {0.0f, 0.5f, out.tween, 4}};
        animation.loop = false;
        return out;
    }

    std::string poseAnimationText() {
        auto pose = [](const std::string& name, float raise) {
            return "{\"name\": \"" + name + "\", \"hold\": 0.3, \"grounded\": true, \"lift\": 0, \"transition\": null, "
                   "\"offset\": {\"left\": 0, \"forward\": 0}, \"angles\": {\"left_upper_arm.raise\": " + std::to_string(raise) +
                   ", \"right_upper_leg.forward\": " + std::to_string(raise / 3) + "}}";
        };
        return "{\"format\": \"jasperengine.poseanim\", \"version\": 1, \"preview_model\": \"models/hero.gltf\", "
               "\"frame_rate\": 30, \"loop\": true, "
               "\"feel\": {\"transition\": {\"curve\": \"spring\", \"duration\": 0.2, \"overshoot\": 0.25, \"anticipation\": 0.06}, "
               "\"step_rate\": 0, \"overlap\": 0.05, \"looseness\": 0.2, \"weights\": {\"spine\": 1, \"head\": 1, \"arms\": 1, \"legs\": 1}}, "
               "\"humanize\": {\"enabled\": false, \"joint_limits\": true, \"moving_holds\": 0.08, \"inbetweens\": 0.4, "
               "\"life\": 0.6, \"seed\": 1, \"head_follow\": 0.5, \"torso_follow\": 0.4, \"springiness\": 0.5}, "
               "\"poses\": [" + pose("Rest", 0) + ", " + pose("Raise", 70) + "]}\n";
    }

}

// ---- validation ---------------------------------------------------------------------------

TEST_F(ClipConverters_, ValidationOutcomes) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = gameObject(sceneId, "Hero");
    const Entity arm = gameObject(sceneId, "Arm", hero);
    gameObject(sceneId, "Hand", arm);
    scene->setEntityName(gameObject(sceneId, "Leg", hero), "Leg");
    scene->setEntityName(gameObject(sceneId, "Leg", hero), "Leg");
    const Entity cage = create(sceneId, "Cage", EntityCreationType::LATTICE, hero);
    const LatticeComponent& lattice = scene->getComponent<LatticeComponent>(cage);
    const int points = lattice.resolutionX * lattice.resolutionY * lattice.resolutionZ;

    ClipData data;
    data.tracks.push_back(track("", ClipChannel::Position, {{0, 0, 0}, {0, 1, 0}}));
    data.tracks.push_back(track("Arm/Hand", ClipChannel::Rotation, {{0, 0, 0, 1}}));
    data.tracks.push_back(track("Arm/Hnd", ClipChannel::Position, {{0, 0, 0}}));
    data.tracks.push_back(track("Leg", ClipChannel::Scale, {{1, 1, 1}}));
    data.tracks.push_back(track("Leg#1", ClipChannel::Scale, {{1, 1, 1}}));
    data.tracks.push_back(track("Arm", ClipChannel::Morph, {{0.5f}}));
    data.tracks.push_back(track("Arm", ClipChannel::Lattice, {{0, 0, 0}}));
    data.tracks.back().points = {0};
    ClipTrackData outside = track("Cage", ClipChannel::Lattice, {{0, 0, 0, 0, 1, 0}});
    outside.points = {13, points + 2};
    data.tracks.push_back(outside);
    ClipTrackData property;
    property.path = "Arm";
    property.channel = ClipChannel::Property;
    property.component = "Light";
    property.property = "intensity";
    data.tracks.push_back(property);

    // no root: structure only
    ClipValidationReport unchecked = ClipValidation::validate(data, nullptr, NULL_ENTITY);
    EXPECT_EQ(unchecked.total, 8u);
    EXPECT_EQ(unchecked.resolved, 0u);
    for (const TrackReport& t : unchecked.tracks) {
        EXPECT_EQ(t.status, t.channel == ClipChannel::Property ? TrackBindingStatus::NotSampled : TrackBindingStatus::Unchecked);
    }
    EXPECT_TRUE(unchecked.ok());

    ClipValidationReport report = ClipValidation::validateInScene(project(), project()->getScene(sceneId), data, hero);
    ASSERT_EQ(report.tracks.size(), data.tracks.size());
    EXPECT_EQ(report.root, hero);
    EXPECT_FALSE(report.ok());
    EXPECT_FALSE(report.legacyModelLayout);

    EXPECT_EQ(reportOf(report, "", ClipChannel::Position)->status, TrackBindingStatus::Ok);
    EXPECT_EQ(reportOf(report, "", ClipChannel::Position)->entity, hero);
    EXPECT_EQ(reportOf(report, "Arm/Hand", ClipChannel::Rotation)->status, TrackBindingStatus::Ok);

    const TrackReport* missing = reportOf(report, "Arm/Hnd", ClipChannel::Position);
    EXPECT_EQ(missing->status, TrackBindingStatus::MissingPath);
    ASSERT_FALSE(missing->nearest.empty());
    EXPECT_EQ(missing->nearest[0], "Arm/Hand");
    EXPECT_LE(missing->nearest.size(), 3u);
    EXPECT_NE(missing->message.find("did you mean 'Arm/Hand'"), std::string::npos) << missing->message;

    const TrackReport* duplicate = reportOf(report, "Leg", ClipChannel::Scale);
    EXPECT_EQ(duplicate->status, TrackBindingStatus::DuplicateSiblingName);
    EXPECT_NE(duplicate->entity, NULL_ENTITY);
    EXPECT_FALSE(duplicate->message.empty());
    EXPECT_EQ(reportOf(report, "Leg#1", ClipChannel::Scale)->status, TrackBindingStatus::Ok);
    EXPECT_NE(reportOf(report, "Leg#1", ClipChannel::Scale)->entity, duplicate->entity);

    EXPECT_EQ(reportOf(report, "Arm", ClipChannel::Morph)->status, TrackBindingStatus::MissingComponent);
    EXPECT_EQ(reportOf(report, "Arm", ClipChannel::Lattice)->status, TrackBindingStatus::MissingComponent);
    const TrackReport* cageReport = reportOf(report, "Cage", ClipChannel::Lattice);
    EXPECT_EQ(cageReport->status, TrackBindingStatus::LatticeIndexOutOfRange);
    EXPECT_EQ(cageReport->badPoints, std::vector<int>{points + 2});
    EXPECT_EQ(report.tracks.back().status, TrackBindingStatus::NotSampled);

    // resolved = Ok + duplicate; total = sampled tracks
    EXPECT_EQ(report.resolved, 4u);
    EXPECT_EQ(report.total, 8u);

    // nearest suggestions on their own; the same name under another parent first
    std::vector<std::string> nearest = ClipValidation::nearestPaths(scene, hero, "Hand", 2);
    ASSERT_FALSE(nearest.empty());
    EXPECT_EQ(nearest[0], "Arm/Hand");
    EXPECT_EQ(ClipValidation::trackLabel(data.tracks[1]), "Arm/Hand : rotation");

    // a name that can't be addressed is reported (its branch can't bind)
    const Entity bad = gameObject(sceneId, "Bone#2", hero);
    (void)bad;
    ClipValidationReport named = ClipValidation::validateInScene(project(), project()->getScene(sceneId), data, hero);
    bool warned = false;
    for (const ClipWarning& warning : named.warnings) warned = warned || warning.message.find("Bone#2") != std::string::npos;
    EXPECT_TRUE(warned);
}

TEST_F(ClipConverters_, ValidationEventHandlersAndCppScripts) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = gameObject(sceneId, "Hero");
    const fs::path script = project()->resolveLuaPath("scripts/hero_events.lua");
    writeAll(script, "local Hero = {}\n\nfunction Hero:init()\nend\n\nfunction Hero:onStep(side, n)\nend\n\n"
                     "Hero.onJump = function(self)\nend\n\nreturn Hero\n");
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.path = "scripts/hero_events.lua";
    entry.className = "Hero";
    entry.enabled = true;
    scripts.scripts.push_back(entry);
    scene->addComponent<ScriptComponent>(hero, scripts);

    const std::vector<std::string> functions = ClipValidation::scriptFunctions(project(), project()->getScene(sceneId), hero);
    EXPECT_NE(std::find(functions.begin(), functions.end(), "onStep"), functions.end());
    EXPECT_NE(std::find(functions.begin(), functions.end(), "onJump"), functions.end());
    EXPECT_NE(std::find(functions.begin(), functions.end(), "init"), functions.end());

    ClipData data;
    data.tracks.push_back(track("", ClipChannel::Position, {{0, 0, 0}}));
    ClipEventData step;
    step.time = 0.5f;
    step.function = "onStep";
    ClipEventData land;
    land.time = 0.75f;
    land.function = "onLand";
    data.events = {step, land};

    ClipValidationReport report = ClipValidation::validateInScene(project(), project()->getScene(sceneId), data, hero);
    ASSERT_EQ(report.events.size(), 2u);
    EXPECT_EQ(report.events[0].status, EventHandlerStatus::Found);
    EXPECT_EQ(report.events[1].status, EventHandlerStatus::NotFound);
    EXPECT_NE(report.events[1].message.find("onLand"), std::string::npos);
    EXPECT_FALSE(report.ok());

    // a C++ script subscribes in code: handlers are not checked (R23)
    ScriptEntry cpp;
    cpp.type = ScriptType::CPP;
    cpp.path = "scripts/Hero.cpp";
    cpp.className = "HeroScript";
    scene->getComponent<ScriptComponent>(hero).scripts.push_back(cpp);
    ClipValidationReport skipped = ClipValidation::validateInScene(project(), project()->getScene(sceneId), data, hero);
    EXPECT_EQ(skipped.events[0].status, EventHandlerStatus::Unchecked);
    EXPECT_EQ(skipped.events[1].status, EventHandlerStatus::Unchecked);
    EXPECT_TRUE(skipped.ok());

    // no root: unchecked
    EXPECT_EQ(ClipValidation::validate(data, nullptr, NULL_ENTITY).events[1].status, EventHandlerStatus::Unchecked);
}

// The T-pose hero alone loads in the legacy layout; a player added by a path that does not
// switch it (here: the component written directly, as a duplicate would) leaves it so: the
// report says so instead of N missing paths, and its fix reloads it as one scene step
TEST_F(ClipConverters_, ValidationLegacyLayoutAndItsFix) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = loadModel(sceneId, "models/tpose.gltf");
    ASSERT_TRUE(scene->getComponent<ModelComponent>(hero).nodesIdMapping.empty()) << "legacy layout";

    // the animated hero's Walk: hierarchy paths of the same rig
    ClipData walk;
    std::vector<ClipWarning> warnings;
    std::string error;
    ASSERT_TRUE(ClipConverters::extractModelSubClip("models/hero.gltf#Walk", walk, warnings, &error)) << error;
    ASSERT_FALSE(walk.tracks.empty());

    ClipValidationReport legacy = ClipValidation::validateInScene(project(), project()->getScene(sceneId), walk, hero);
    EXPECT_TRUE(legacy.legacyModelLayout);
    ASSERT_EQ(legacy.legacyModels.size(), 1u);
    EXPECT_EQ(legacy.legacyModels[0], hero);
    size_t legacyTracks = 0;
    for (const TrackReport& t : legacy.tracks) {
        EXPECT_NE(t.status, TrackBindingStatus::MissingPath) << t.path;
        if (t.status == TrackBindingStatus::LegacyModelLayout) legacyTracks++;
    }
    EXPECT_GT(legacyTracks, 0u);
    ASSERT_FALSE(legacy.warnings.empty());
    EXPECT_NE(legacy.warnings[0].message.find("legacy layout"), std::string::npos);

    // the fix needs a player that applies; without one there is nothing to reload
    ClipActionResult result;
    EXPECT_FALSE(ClipSceneActions::reloadModelsForPlayer(project(), sceneId, hero, result, error));
    scene->addComponent<AnimationPlayerComponent>(hero, AnimationPlayerComponent());
    const size_t steps = history(sceneId)->position();
    ASSERT_TRUE(ClipSceneActions::reloadModelsForPlayer(project(), sceneId, hero, result, error)) << error;
    EXPECT_EQ(result.reloadedModels, 1u);
    EXPECT_EQ(history(sceneId)->position(), steps + 1) << "one scene step";
    EXPECT_FALSE(scene->getComponent<ModelComponent>(hero).nodesIdMapping.empty()) << "node hierarchy";
    ClipValidationReport fixed = ClipValidation::validateInScene(project(), project()->getScene(sceneId), walk, hero);
    EXPECT_FALSE(fixed.legacyModelLayout);
    EXPECT_EQ(fixed.resolved, fixed.total) << "every Walk track binds on the T-pose hero";

    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(scene->getComponent<ModelComponent>(hero).nodesIdMapping.empty()) << "legacy again";
    ASSERT_TRUE(history(sceneId)->redo());
    EXPECT_FALSE(scene->getComponent<ModelComponent>(hero).nodesIdMapping.empty());
}

// ---- Convert to Clip ----------------------------------------------------------------------

TEST_F(ClipConverters_, ConvertSamplesTheEntityAnimationsValues) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    EntityAnimation e = entityAnimation(sceneId);

    ClipData data;
    ClipConvertReport report;
    std::string error;
    ASSERT_TRUE(ClipConverters::convertEntityAnimation(scene, e.animation, NULL_ENTITY, data, report, &error)) << error;
    EXPECT_EQ(report.convertedTracks, 4u);
    EXPECT_EQ(report.convertedTrackEntities, e.tracks);
    ASSERT_EQ(report.notConverted.size(), 1u);
    EXPECT_NE(report.notConverted[0].find("position tween"), std::string::npos) << report.notConverted[0];
    EXPECT_EQ(data.wrap, ClipWrapMode::Once);
    EXPECT_EQ(data.source.kind, "entity");
    EXPECT_EQ(data.source.name, "Wave");
    ASSERT_GE(data.findTrack("Arm", ClipChannel::Position), 0);
    ASSERT_GE(data.findTrack("Arm/Hand", ClipChannel::Rotation), 0);
    ASSERT_GE(data.findTrack("", ClipChannel::Scale), 0);
    ASSERT_GE(data.findTrack("Cage", ClipChannel::Lattice), 0);
    const ClipTrackData& armTrack = data.tracks[data.findTrack("Arm", ClipChannel::Position)];
    EXPECT_FLOAT_EQ(armTrack.keys.front().time, 0.0f) << "the hold key before the frame starts";
    EXPECT_FLOAT_EQ(armTrack.keys[1].time, 0.25f) << "shifted by the frame's start";
    const ClipTrackData& handTrack = data.tracks[data.findTrack("Arm/Hand", ClipChannel::Rotation)];
    EXPECT_FLOAT_EQ(handTrack.keys.back().time, 1.0f) << "speed 2 halves the times";
    // the text round trip is the clip the file holds
    anim::ClipParseResult parsed = anim::parseClip(anim::writeClip(data));
    ASSERT_TRUE(parsed.ok());
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(*parsed.data);
    const float length = clip->getLength();
    EXPECT_NEAR(length, 1.25f, 1e-5f) << "the position track ends last";

    // the tween (not in the clip) also moves the Arm: out of the comparison
    std::vector<ActionFrame>& frames = scene->getComponent<AnimationComponent>(e.animation).actions;
    ASSERT_EQ(frames.back().action, e.tween);
    frames.pop_back();

    const std::vector<Entity> targets = {e.root, e.arm, e.hand, e.cage};
    anim::ClipPreviewSession session;
    for (int i = 0; i < 50; i++) {
        const float t = length * (float)i / 49.0f;
        sampleEntityAnimation(scene, e.animation, t);
        std::vector<Pose> expected;
        for (Entity target : targets) expected.push_back(poseOf(scene, target));
        sampleClip(scene, e.root, clip, t, session);
        for (size_t k = 0; k < targets.size(); k++) {
            expectSamePose(poseOf(scene, targets[k]), expected[k], scene->getEntityName(targets[k]) + " at " + std::to_string(t));
        }
    }
    scene->getSystem<ActionSystem>()->restorePlayerPreview(session);
}

TEST_F(ClipConverters_, ConvertToClipIsOneSceneStep) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    EntityAnimation e = entityAnimation(sceneId);
    const std::string dir = folder("convert");
    const fs::path file = root() / dir / "wave.anim";
    project()->getScene(sceneId)->isModified = false;

    const size_t steps = history(sceneId)->position();
    ClipActionResult result;
    std::string error;
    ASSERT_TRUE(ClipSceneActions::convertToClip(project(), sceneId, e.animation, NULL_ENTITY, dir + "/wave.anim", true, result, error)) << error;
    EXPECT_EQ(history(sceneId)->position(), steps + 1) << "one scene step";
    EXPECT_EQ(result.clip, dir + "/wave.anim");
    EXPECT_TRUE(result.addedPlayer);
    EXPECT_EQ(result.removedEntities, 4u);
    EXPECT_TRUE(project()->getScene(sceneId)->isModified);
    ASSERT_TRUE(fs::exists(file));
    const std::string text = readAll(file);
    const std::string guid = anim::readClipGuid(text);
    EXPECT_TRUE(anim::isClipGuid(guid));
    EXPECT_EQ(project()->getAssetDatabase().getGuid(file), guid) << "registered";
    ASSERT_TRUE(scene->findComponent<AnimationPlayerComponent>(e.root));
    const AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(e.root);
    ASSERT_EQ(player.clips.size(), 1u);
    EXPECT_EQ(player.clips[0].clip, "convert/wave.anim") << "asset-relative";
    EXPECT_EQ(player.clips[0].guid, guid);
    for (Entity t : e.tracks) EXPECT_FALSE(scene->isEntityCreated(t)) << "converted tracks removed";
    EXPECT_TRUE(scene->isEntityCreated(e.tween)) << "the tween stays";
    ASSERT_EQ(scene->getComponent<AnimationComponent>(e.animation).actions.size(), 1u);

    // undo: the file is gone, the tracks and frames are back, no player
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_FALSE(fs::exists(file));
    EXPECT_TRUE(project()->getAssetDatabase().getGuid(file).empty());
    EXPECT_FALSE(scene->findComponent<AnimationPlayerComponent>(e.root));
    for (Entity t : e.tracks) EXPECT_TRUE(scene->isEntityCreated(t));
    EXPECT_EQ(scene->getComponent<AnimationComponent>(e.animation).actions.size(), 5u);
    EXPECT_FALSE(project()->getScene(sceneId)->isModified);

    // redo: the same bytes (same GUID) again
    ASSERT_TRUE(history(sceneId)->redo());
    EXPECT_EQ(readAll(file), text);
    EXPECT_TRUE(scene->findComponent<AnimationPlayerComponent>(e.root));

    // an existing file or a path outside the assets is refused, nothing changes
    const size_t after = history(sceneId)->position();
    EXPECT_FALSE(ClipSceneActions::convertToClip(project(), sceneId, e.animation, NULL_ENTITY, dir + "/wave.anim", false, result, error));
    EXPECT_NE(error.find("already exists"), std::string::npos) << error;
    EXPECT_FALSE(ClipSceneActions::convertToClip(project(), sceneId, e.animation, NULL_ENTITY, "../outside.anim", false, result, error));
    EXPECT_EQ(history(sceneId)->position(), after);
}

// ---- Extract to .anim -----------------------------------------------------------------------

TEST_F(ClipConverters_, ExtractKeepsTheSourceAndSwitchesThePlayer) {
    const std::string dir = folder("extract");
    ClipData data;
    std::vector<ClipWarning> warnings;
    std::string error;
    ASSERT_TRUE(ClipConverters::extractModelSubClip("models/hero.gltf#Walk", data, warnings, &error)) << error;
    EXPECT_EQ(data.source.kind, "model");
    EXPECT_EQ(data.source.path, "models/hero.gltf");
    EXPECT_EQ(data.source.name, "Walk");
    EXPECT_TRUE(anim::isClipGuid(data.guid));
    EXPECT_FALSE(ClipConverters::extractModelSubClip("models/hero.gltf#Nope", data, warnings, &error));
    EXPECT_FALSE(ClipConverters::extractModelSubClip("models/hero.gltf", data, warnings, &error));

    // the file alone: a project step
    ClipActionResult result;
    const size_t projectSteps = projectHistory()->position();
    ASSERT_TRUE(ClipSceneActions::extractSubClip(project(), 0, NULL_ENTITY, "models/hero.gltf#Walk", dir + "/walk.anim", false, result, error)) << error;
    EXPECT_TRUE(result.projectStep);
    EXPECT_EQ(projectHistory()->position(), projectSteps + 1);
    const ClipData written = readClipFile(root() / dir / "walk.anim");
    EXPECT_EQ(written.source.kind, "model");
    EXPECT_EQ(written.source.name, "Walk");
    EXPECT_EQ(written.previewModel, "models/hero.gltf");
    ClipData sub;
    ASSERT_TRUE(anim::buildModelSubClip("models/hero.gltf#Walk", sub, warnings, &error));
    ASSERT_EQ(written.tracks.size(), sub.tracks.size());
    for (size_t i = 0; i < sub.tracks.size(); i++) {
        EXPECT_EQ(written.tracks[i].path, sub.tracks[i].path);
        EXPECT_EQ(written.tracks[i].keys.size(), sub.tracks[i].keys.size());
    }
    ASSERT_TRUE(projectHistory()->undo());
    EXPECT_FALSE(fs::exists(root() / dir / "walk.anim"));

    // from a model whose player plays the sub-clip: the entry switches to the file, keeping its state name
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = gameObject(sceneId, "Hero");
    AnimationPlayerComponent player;
    AnimationPlayerClip walk;
    walk.clip = "models/hero.gltf#Walk";
    AnimationPlayerClip idle;
    idle.clip = "models/hero.gltf#Idle";
    player.clips = {idle, walk};
    scene->addComponent<AnimationPlayerComponent>(hero, player);
    const size_t steps = history(sceneId)->position();
    ASSERT_TRUE(ClipSceneActions::extractSubClip(project(), sceneId, hero, "assets/models/hero.gltf#Walk", dir + "/walk_edit.anim", true,
                                                 result, error)) << error;
    EXPECT_FALSE(result.projectStep);
    EXPECT_EQ(history(sceneId)->position(), steps + 1);
    const AnimationPlayerComponent& switched = scene->getComponent<AnimationPlayerComponent>(hero);
    ASSERT_EQ(switched.clips.size(), 2u);
    EXPECT_EQ(switched.clips[0].clip, "models/hero.gltf#Idle");
    EXPECT_EQ(switched.clips[1].clip, "extract/walk_edit.anim");
    EXPECT_EQ(switched.clips[1].name, "Walk") << "the state keeps its name";
    EXPECT_EQ(switched.clips[1].guid, anim::readClipGuid(readAll(root() / dir / "walk_edit.anim")));
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips[1].clip, "models/hero.gltf#Walk");
    EXPECT_FALSE(fs::exists(root() / dir / "walk_edit.anim"));
}

// ---- Use Animation Player -------------------------------------------------------------------

TEST_F(ClipConverters_, UseAnimationPlayer) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    const std::vector<Entity> animations = scene->getComponent<ModelComponent>(hero).animations;
    ASSERT_EQ(animations.size(), 5u) << "the loader's entity animations";
    std::vector<Entity> tracks;
    for (Entity animation : animations) {
        for (const ActionFrame& frame : scene->getComponent<AnimationComponent>(animation).actions) tracks.push_back(frame.action);
    }
    ASSERT_FALSE(tracks.empty());

    UseAnimationPlayerPlan plan;
    std::string error;
    ASSERT_TRUE(ClipConverters::planUseAnimationPlayer(scene, hero, plan, &error)) << error;
    EXPECT_TRUE(plan.addPlayer);
    EXPECT_FALSE(plan.reloadModel) << "an animated model loads in the node hierarchy";
    ASSERT_EQ(plan.clips.size(), 5u);
    EXPECT_EQ(plan.clips[0].clip, "models/hero.gltf#Walk");
    EXPECT_EQ(plan.clips[4].clip, "models/hero.gltf#Wave 2");
    EXPECT_EQ(plan.removeEntities.size(), animations.size() + tracks.size());

    project()->getScene(sceneId)->isModified = false;
    const size_t steps = history(sceneId)->position();
    ClipActionResult result;
    ASSERT_TRUE(ClipSceneActions::useAnimationPlayer(project(), sceneId, hero, result, error)) << error;
    EXPECT_EQ(history(sceneId)->position(), steps + 1);
    auto checkUsed = [&](Scene* s, Entity model) {
        const ModelComponent& component = s->getComponent<ModelComponent>(model);
        EXPECT_FALSE(component.animationEntities);
        EXPECT_TRUE(component.animations.empty());
        EXPECT_EQ(s->getComponentArray<AnimationComponent>()->size(), 0u) << "no entity animation left";
        ASSERT_TRUE(s->findComponent<AnimationPlayerComponent>(model));
        const AnimationPlayerComponent& player = s->getComponent<AnimationPlayerComponent>(model);
        ASSERT_EQ(player.clips.size(), 5u);
        EXPECT_EQ(player.clips[1].clip, "models/hero.gltf#Idle");
    };
    checkUsed(scene, hero);
    for (Entity entity : animations) EXPECT_FALSE(scene->isEntityCreated(entity));
    for (Entity entity : tracks) EXPECT_FALSE(scene->isEntityCreated(entity));
    // again: nothing left to do
    ClipActionResult again;
    EXPECT_FALSE(ClipSceneActions::useAnimationPlayer(project(), sceneId, hero, again, error));

    // a reload of the model (same file) makes no entity animations again
    history(sceneId)->addCommandNoMerge(new ModelLoadCmd(project(), sceneId, hero, "models/hero.gltf"));
    checkUsed(scene, hero);
    ASSERT_TRUE(history(sceneId)->undo());

    // undo restores everything
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(scene->getComponent<ModelComponent>(hero).animationEntities);
    EXPECT_EQ(scene->getComponent<ModelComponent>(hero).animations, animations);
    for (Entity entity : animations) EXPECT_TRUE(scene->isEntityCreated(entity) && scene->findComponent<AnimationComponent>(entity));
    for (Entity entity : tracks) EXPECT_TRUE(scene->isEntityCreated(entity));
    EXPECT_FALSE(scene->findComponent<AnimationPlayerComponent>(hero));
    EXPECT_FALSE(project()->getScene(sceneId)->isModified);
    ASSERT_TRUE(history(sceneId)->redo());
    checkUsed(scene, hero);

    // saved and opened again
    const std::string dir = folder("useplayer");
    const fs::path sceneFile = root() / dir / "Level.scene";
    ASSERT_TRUE(project()->saveSceneToPath(sceneId, sceneFile));
    project()->getScene(sceneId)->isModified = false;
    project()->closeScene(sceneId, true);
    project()->openScene(sceneFile, false);
    SceneProject* opened = nullptr;
    for (SceneProject& candidate : project()->getScenes()) {
        if (candidate.scene && (root() / candidate.filepath).lexically_normal() == sceneFile.lexically_normal()) opened = &candidate;
    }
    ASSERT_TRUE(opened);
    auto models = opened->scene->getComponentArray<ModelComponent>();
    ASSERT_EQ(models->size(), 1u);
    checkUsed(opened->scene, models->getEntity(0));
    opened->isModified = false;
}

// ---- R4: the composite and the two mismatch orders ------------------------------------------

TEST_F(ClipConverters_, CreateNewClipAndBothMismatchOrders) {
    const std::string dir = folder("mismatch");
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = gameObject(sceneId, "Hero");
    const fs::path file = root() / dir / "jump.anim";

    // 1. the clip edited after the step (a project step), then the scene step undone: the
    //    component goes, the file stays as it is now (an orphan, harmless)
    ClipActionResult result;
    std::string error;
    ASSERT_TRUE(ClipSceneActions::createNewClip(project(), sceneId, hero, dir + "/jump.anim", result, error)) << error;
    ASSERT_TRUE(fs::exists(file));
    EXPECT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips.size(), 1u);
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project(), dir + "/jump.anim", &error);
    ASSERT_TRUE(document) << error;
    ClipData edited = document->data();
    edited.tracks.push_back(track("", ClipChannel::Position, {{0, 0, 0}, {0, 2, 0}}));
    ASSERT_TRUE(document->commit(edited, "Key", "", &error)) << error;
    const std::string editedText = readAll(file);
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_FALSE(scene->findComponent<AnimationPlayerComponent>(hero));
    ASSERT_TRUE(fs::exists(file)) << "changed since: kept";
    EXPECT_EQ(readAll(file), editedText);
    // the project edit still undoes on its own history
    ASSERT_TRUE(projectHistory()->undo());
    EXPECT_TRUE(document->data().tracks.empty());
    history(sceneId)->dropRedo();
    fs::remove(file);
    AnimationClipDocument::filesChanged({file}, project());

    // 2. the clip deleted (a project step) while the component keeps it: "Missing", nothing
    //    breaks; undoing the scene step then finds no file to remove; the delete undone brings
    //    the file back as an orphan
    ASSERT_TRUE(ClipSceneActions::createNewClip(project(), sceneId, hero, dir + "/jump.anim", result, error)) << error;
    const std::string created = readAll(file);
    ASSERT_TRUE(projectHistory()->addCommand(new DeleteFileCmd(project(), {file}, project()->getProjectPath())));
    EXPECT_FALSE(fs::exists(file));
    const AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(hero);
    ASSERT_EQ(player.clips.size(), 1u) << "the reference is kept (Missing)";
    EXPECT_FALSE(AnimationClipDocument::open(project(), dir + "/jump.anim", &error));
    ClipValidationReport none = ClipValidation::validate(ClipData(), scene, hero);
    EXPECT_TRUE(none.ok());
    // the runtime skips it quietly
    scene->getSystem<ActionSystem>()->update(0.016);
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_FALSE(scene->findComponent<AnimationPlayerComponent>(hero));
    EXPECT_FALSE(fs::exists(file));
    ASSERT_TRUE(projectHistory()->undo());
    EXPECT_EQ(readAll(file), created) << "the delete undone: an orphan file";
    // redo of the scene step writes it again (what is there now is what its undo gives back)
    ASSERT_TRUE(history(sceneId)->redo());
    EXPECT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips.size(), 1u);
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_EQ(readAll(file), created) << "the file that was there before the redo stays";
}

TEST_F(ClipConverters_, CreateClipCmdIsAllOrNothing) {
    const std::string dir = folder("allornothing");
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = gameObject(sceneId, "Hero");
    // a directory where the second file goes: it can't be written
    fs::create_directories(root() / dir / "blocked.anim");
    std::vector<Command*> commands = {new AddComponentCmd(project(), sceneId, hero, ComponentType::AnimationPlayerComponent)};
    std::vector<CreateClipCmd::File> files = {{root() / dir / "first.anim", "{}\n"}, {root() / dir / "blocked.anim", "{}\n"}};
    auto* command = new CreateClipCmd(project(), sceneId, files, commands);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    const size_t steps = history(sceneId)->position();
    EXPECT_FALSE(history(sceneId)->addCommand(command));
    EXPECT_EQ(history(sceneId)->position(), steps);
    EXPECT_NE(failure->find("blocked.anim"), std::string::npos) << *failure;
    EXPECT_FALSE(fs::exists(root() / dir / "first.anim")) << "the first file is taken back";
    EXPECT_FALSE(scene->findComponent<AnimationPlayerComponent>(hero)) << "no scene command ran";
}

// ---- drops ----------------------------------------------------------------------------------

TEST_F(ClipConverters_, DroppedClipsAddOrReuseAPlayer) {
    const std::string dir = folder("drop");
    std::string error;
    ASSERT_TRUE(AnimationClipDocument::create(project(), dir + "/a.anim", ClipData(), "Create", &error)) << error;
    ASSERT_TRUE(AnimationClipDocument::create(project(), dir + "/b.anim", ClipData(), "Create", &error)) << error;
    const std::string guidA = project()->getAssetDatabase().getGuid(root() / dir / "a.anim");
    ASSERT_FALSE(guidA.empty());

    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = gameObject(sceneId, "Hero");

    // the App hook is installed (the Scene view and Hierarchy drop targets call it)
    ASSERT_TRUE(animationClipAppHooks().dropClips);
    ASSERT_TRUE(animationClipInspectorHooks().convertToClip);
    ASSERT_TRUE(animationClipInspectorHooks().useAnimationPlayer);
    ASSERT_TRUE(animationClipInspectorHooks().extractSubClip);
    ASSERT_TRUE(animationClipInspectorHooks().addToAnimationPlayer);
    ASSERT_TRUE(animationClipInspectorHooks().createNewClip);
    const size_t steps = history(sceneId)->position();
    EXPECT_TRUE(animationClipAppHooks().dropClips(project(), sceneId, hero, {(root() / dir / "a.anim").string()}));
    EXPECT_EQ(history(sceneId)->position(), steps + 1);
    ASSERT_TRUE(scene->findComponent<AnimationPlayerComponent>(hero));
    ASSERT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips.size(), 1u);
    EXPECT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips[0].clip, "drop/a.anim");
    EXPECT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips[0].guid, guidA);

    // the same again with another: one appended, one reused, one step
    ClipActionResult result;
    ASSERT_TRUE(ClipSceneActions::addClipsToPlayer(project(), sceneId, hero, {dir + "/b.anim", "drop/a.anim"}, result, error)) << error;
    EXPECT_FALSE(result.addedPlayer);
    EXPECT_EQ(result.appended, 1u);
    EXPECT_EQ(result.entries, (std::vector<size_t>{1, 0}));
    EXPECT_EQ(history(sceneId)->position(), steps + 2);
    ASSERT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips.size(), 2u);
    // a model file: its sub-clips
    ASSERT_TRUE(ClipSceneActions::addClipsToPlayer(project(), sceneId, hero, {(root() / "assets/models/hero.gltf").string()}, result, error)) << error;
    EXPECT_EQ(result.appended, 5u);
    EXPECT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips[2].clip, "models/hero.gltf#Walk");
    // refused: missing files, other types
    EXPECT_FALSE(ClipSceneActions::addClipsToPlayer(project(), sceneId, hero, {dir + "/none.anim"}, result, error));
    EXPECT_FALSE(ClipSceneActions::addClipsToPlayer(project(), sceneId, hero, {"models/hero.gltf#Nope"}, result, error));
    EXPECT_FALSE(ClipSceneActions::addClipsToPlayer(project(), sceneId, hero, {dir + "/a.txt"}, result, error));

    ASSERT_TRUE(history(sceneId)->undo());
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips.size(), 1u);
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_FALSE(scene->findComponent<AnimationPlayerComponent>(hero));

    // onto a legacy-layout model: the drop reloads it in its node hierarchy, same step
    const Entity tpose = loadModel(sceneId, "models/tpose.gltf");
    ASSERT_TRUE(scene->getComponent<ModelComponent>(tpose).nodesIdMapping.empty());
    ASSERT_TRUE(ClipSceneActions::addClipsToPlayer(project(), sceneId, tpose, {"models/hero.gltf#Walk"}, result, error)) << error;
    EXPECT_FALSE(scene->getComponent<ModelComponent>(tpose).nodesIdMapping.empty());
    EXPECT_EQ(result.reloadedModels, 1u);
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(scene->getComponent<ModelComponent>(tpose).nodesIdMapping.empty());
    EXPECT_FALSE(scene->findComponent<AnimationPlayerComponent>(tpose));
}

// ---- Bake to Animation Clip -----------------------------------------------------------------

TEST_F(ClipConverters_, BakeToClipPlaysLikeTheEntityBake) {
    const std::string dir = folder("bake");
    writeAll(root() / dir / "raise.poseanim", poseAnimationText());
    std::string error;
    std::shared_ptr<PoseAnimationDocument> document = PoseAnimationDocument::open(project(), dir + "/raise.poseanim", &error);
    ASSERT_TRUE(document) << error;

    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = loadModel(sceneId, "models/hero.gltf");

    PoseBakeResult entityBake;
    ASSERT_TRUE(PoseBaker::bake(project(), sceneId, hero, *document, "raise", entityBake, error)) << error;
    PoseClipBakeResult clipBake;
    const size_t steps = history(sceneId)->position();
    ASSERT_TRUE(PoseBaker::bakeToClip(project(), sceneId, hero, *document, "", "", clipBake, error)) << error;
    EXPECT_EQ(history(sceneId)->position(), steps + 1) << "one scene step";
    EXPECT_EQ(clipBake.clip, dir + "/raise.anim");
    EXPECT_TRUE(clipBake.created);
    EXPECT_TRUE(clipBake.addedPlayer);
    EXPECT_EQ(clipBake.name, "raise");
    EXPECT_EQ(clipBake.tracks, entityBake.tracks);
    EXPECT_EQ(clipBake.frames, entityBake.frames);
    const fs::path file = root() / clipBake.clip;
    const ClipData data = readClipFile(file);
    EXPECT_EQ(data.source.kind, "pose");
    EXPECT_EQ(data.source.path, dir + "/raise.poseanim");
    EXPECT_EQ(data.wrap, ClipWrapMode::Loop);
    EXPECT_EQ((int)data.tracks.size(), entityBake.tracks);
    EXPECT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips[0].clip, "bake/raise.anim");
    ClipValidationReport report = ClipValidation::validateInScene(project(), project()->getScene(sceneId), data, hero);
    EXPECT_EQ(report.resolved, report.total);
    EXPECT_GT(report.total, 10u);

    // the same bones at the same times
    std::vector<Entity> bones;
    for (const ActionFrame& frame : scene->getComponent<AnimationComponent>(entityBake.animation).actions) {
        bones.push_back(scene->getComponent<ActionComponent>(frame.action).target);
    }
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    anim::ClipPreviewSession session;
    for (int i = 0; i < 50; i++) {
        const float t = entityBake.length * (float)i / 50.0f;
        sampleEntityAnimation(scene, entityBake.animation, t);
        std::vector<Pose> expected;
        for (Entity bone : bones) expected.push_back(poseOf(scene, bone));
        sampleClip(scene, hero, clip, t, session);
        for (size_t k = 0; k < bones.size(); k++) {
            expectSamePose(poseOf(scene, bones[k]), expected[k], scene->getEntityName(bones[k]) + " at " + std::to_string(t));
        }
    }
    scene->getSystem<ActionSystem>()->restorePlayerPreview(session);

    // baking again rewrites the same file (its GUID kept), no second entry
    const std::string guid = anim::readClipGuid(readAll(file));
    ASSERT_TRUE(PoseBaker::bakeToClip(project(), sceneId, hero, *document, "", "", clipBake, error)) << error;
    EXPECT_FALSE(clipBake.created);
    EXPECT_EQ(clipBake.clip, dir + "/raise.anim");
    EXPECT_EQ(anim::readClipGuid(readAll(file)), guid);
    EXPECT_EQ(scene->getComponent<AnimationPlayerComponent>(hero).clips.size(), 1u);
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(fs::exists(file)) << "the rewrite's undo gives the first bake back";
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_FALSE(fs::exists(file));
    EXPECT_FALSE(scene->findComponent<AnimationPlayerComponent>(hero));
}

// The T-pose hero alone is in the legacy layout: the bake's paths are those of the node
// hierarchy the player reloads it in (from the actual bones), all bound afterwards
TEST_F(ClipConverters_, BakeToClipOnALegacyLayoutModel) {
    const std::string dir = folder("bakelegacy");
    writeAll(root() / dir / "raise.poseanim", poseAnimationText());
    std::string error;
    std::shared_ptr<PoseAnimationDocument> document = PoseAnimationDocument::open(project(), dir + "/raise.poseanim", &error);
    ASSERT_TRUE(document) << error;
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = loadModel(sceneId, "models/tpose.gltf");
    ASSERT_TRUE(scene->getComponent<ModelComponent>(hero).nodesIdMapping.empty());

    PoseClipBakeResult clipBake;
    ASSERT_TRUE(PoseBaker::bakeToClip(project(), sceneId, hero, *document, "lift", "", clipBake, error)) << error;
    EXPECT_EQ(clipBake.clip, dir + "/lift.anim");
    EXPECT_FALSE(scene->getComponent<ModelComponent>(hero).nodesIdMapping.empty()) << "reloaded in the node hierarchy";
    EXPECT_EQ(clipBake.reloadedModels, 1u);
    const ClipData data = readClipFile(root() / clipBake.clip);
    ClipValidationReport report = ClipValidation::validateInScene(project(), project()->getScene(sceneId), data, hero);
    EXPECT_FALSE(report.legacyModelLayout);
    EXPECT_EQ(report.resolved, report.total);
    EXPECT_EQ((int)report.total, clipBake.tracks);
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(scene->getComponent<ModelComponent>(hero).nodesIdMapping.empty()) << "legacy again";
    EXPECT_FALSE(fs::exists(root() / clipBake.clip));

    // an explicit path that exists and was not baked from it is refused
    writeAll(root() / dir / "other.anim", "{\"format\": \"jasperengine.anim\", \"version\": 1}\n");
    EXPECT_FALSE(PoseBaker::bakeToClip(project(), sceneId, hero, *document, "", dir + "/other.anim", clipBake, error));
    EXPECT_NE(error.find("was not baked"), std::string::npos) << error;
}

// ---- helpers ----------------------------------------------------------------------------------

TEST_F(ClipConverters_, PathsAndDirectories) {
    const std::string dir = folder("paths");
    writeAll(root() / dir / "Walk.anim", "{}");
    EXPECT_EQ(ClipConverters::uniqueClipPath(project(), dir, "Walk"), dir + "/Walk 2.anim");
    EXPECT_EQ(ClipConverters::uniqueClipPath(project(), dir + "/", "Run"), dir + "/Run.anim");
    EXPECT_EQ(ClipConverters::uniqueClipPath(project(), dir, "a/b:c"), dir + "/a_b_c.anim");

    const uint32_t sceneId = newScene();
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    EXPECT_EQ(ClipConverters::defaultClipDirectory(project(), sceneId, hero), "assets/models");
    const Entity empty = gameObject(sceneId, "Empty");
    EXPECT_EQ(ClipConverters::defaultClipDirectory(project(), sceneId, empty), "assets");

    AnimationPlayerClip entry = ClipConverters::playerEntry(project(), "assets/models/hero.gltf#Walk");
    EXPECT_EQ(entry.clip, "models/hero.gltf#Walk");
    EXPECT_TRUE(entry.guid.empty());
}
