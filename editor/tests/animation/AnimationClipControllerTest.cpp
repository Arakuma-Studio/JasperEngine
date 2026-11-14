// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's clip mode without its UI (animation clips spec 4.4, 9.R4-R7/R21):
// ClipRecordHooks' registries, the PreviewOwner registry and AnimationClipController on a
// headless project (AppSettings in a temporary config directory + Project::loadProject, the
// engine started on the editor's Platform as the other project tests do). An EditorHost that
// forwards to AnimationClipAppHooks stands in for the App, so saves, scene closes and project
// switches reach the controller the way they do in the editor.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Platform.h"
#include "Project.h"
#include "asset/AssetOps.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/ClipRecordHooks.h"
#include "animation/PreviewOwner.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/UndoTimeline.h"
#include "command/type/ClipSceneActions.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/CreatePrefabCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "command/type/PrefabOverrideCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "lattice/LatticeEditorOps.h"
#include "component/ActionComponent.h"
#include "component/Transform.h"
#include "component/AnimationComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/TranslateTracksComponent.h"
#include "util/EntityClipboard.h"
#include "window/InspectorWindowClips.h"
#include "animation/ClipEditorUtil.h"
#include "animation/ClipPropertyNames.h"
#include "animation/ClipTangents.h"
#include "animation/PropertyBindings.h"
#include "component/AudioSourceComponent.h"
#include "component/ButtonComponent.h"
#include "component/CameraComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/FogComponent.h"
#include "component/ImageComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RectTransformComponent.h"
#include "component/ScriptComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteComponent.h"
#include "component/TitleArtRendererComponent.h"
#include "component/UIComponent.h"
#include "util/Color.h"
#include "window/InspectorWindow.h"

#include "imgui.h"
#include "component/ModelComponent.h"
#include "object/Model.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseEditSession.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include "support/PosixCompat.h"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

// ---- ClipRecordHooks registries (no project) ------------------------------------------------

TEST(ClipRecordHooksRegistry, ChannelWriteListenersRunInOrderAndRemovalWaitsForTheCall){
    std::vector<std::string> calls;
    ClipRecordHooks::Id second = 0;
    const ClipRecordHooks::Id first = ClipRecordHooks::addBeforeChannelWrite([&](uint32_t sceneId, Entity entity, ClipChannel channel){
        calls.push_back("first " + std::to_string(sceneId) + " " + std::to_string(entity) + " " + anim::channelName(channel));
        // removing a later listener while running: it is not called in this round
        ClipRecordHooks::removeChannelWriteListener(second);
    });
    second = ClipRecordHooks::addBeforeChannelWrite([&](uint32_t, Entity, ClipChannel){ calls.push_back("second"); });
    const ClipRecordHooks::Id after = ClipRecordHooks::addAfterChannelWrite([&](uint32_t, Entity, ClipChannel channel){
        calls.push_back(std::string("after ") + anim::channelName(channel));
    });

    ClipRecordHooks::beforeChannelWrite(3, 7, ClipChannel::Rotation);
    ClipRecordHooks::afterChannelWrite(3, 7, ClipChannel::Rotation);
    EXPECT_EQ(calls, (std::vector<std::string>{"first 3 7 rotation", "after rotation"}));

    // a whole-Transform write notifies its three channels
    calls.clear();
    ClipRecordHooks::removeChannelWriteListener(first);
    ClipRecordHooks::afterComponentWrite(1, 2, ComponentType::Transform, "");
    EXPECT_EQ(calls, (std::vector<std::string>{"after position", "after rotation", "after scale"}));
    ClipRecordHooks::removeChannelWriteListener(after);

    calls.clear();
    ClipRecordHooks::beforeChannelWrite(3, 7, ClipChannel::Position);
    ClipRecordHooks::afterChannelWrite(3, 7, ClipChannel::Position);
    EXPECT_TRUE(calls.empty());
}

TEST(ClipRecordHooksRegistry, TheFirstClaimingListenerRecords){
    std::vector<std::string> recorded;
    ClipRecordListener positions;
    positions.claims = [](uint32_t, Entity, ClipChannel channel){ return channel == ClipChannel::Position; };
    positions.record = [&](uint32_t, Entity entity, const ClipRecordValue& value, const std::string& mergeKey){
        recorded.push_back("positions " + std::to_string(entity) + " " + std::to_string(value.value.size()) + " " + mergeKey);
    };
    ClipRecordListener everything;
    everything.claims = [](uint32_t, Entity, ClipChannel){ return true; };
    everything.record = [&](uint32_t, Entity, const ClipRecordValue&, const std::string&){ recorded.push_back("everything"); };
    const ClipRecordHooks::Id a = ClipRecordHooks::addRecordListener(positions);
    const ClipRecordHooks::Id b = ClipRecordHooks::addRecordListener(everything);

    EXPECT_TRUE(ClipRecordHooks::claims(1, 5, ClipChannel::Scale));
    const std::string drag = ClipRecordHooks::beginDrag();
    EXPECT_EQ(drag.rfind("record#", 0), 0u);
    EXPECT_EQ(ClipRecordHooks::currentMergeKey(), drag);
    ClipRecordValue value;
    value.channel = ClipChannel::Position;
    value.value = {1, 2, 3};
    EXPECT_TRUE(ClipRecordHooks::record(1, 5, value, ClipRecordHooks::currentMergeKey()));
    value.channel = ClipChannel::Scale;
    EXPECT_TRUE(ClipRecordHooks::record(1, 5, value, ""));
    ClipRecordHooks::endDrag();
    EXPECT_EQ(ClipRecordHooks::currentMergeKey(), "");
    // a new drag has a new merge key
    EXPECT_NE(ClipRecordHooks::beginDrag(), drag);
    ClipRecordHooks::endDrag();
    EXPECT_EQ(recorded, (std::vector<std::string>{"positions 5 3 " + drag, "everything"}));

    ClipRecordHooks::removeRecordListener(a);
    ClipRecordHooks::removeRecordListener(b);
    EXPECT_FALSE(ClipRecordHooks::claims(1, 5, ClipChannel::Position));
    EXPECT_FALSE(ClipRecordHooks::record(1, 5, value, ""));
}

// ---- PreviewOwner registry (no project) -----------------------------------------------------

TEST(PreviewOwnerRegistry, AcquiringStopsTheOwnersOfOverlappingRootsFirst){
    Scene scene;
    // top > middle > leaf, and a sibling of middle
    const Entity top = scene.createEntity();
    const Entity middle = scene.createEntity();
    const Entity leaf = scene.createEntity();
    const Entity sibling = scene.createEntity();
    for (Entity entity : {top, middle, leaf, sibling}) scene.addComponent<Transform>(entity, {});
    scene.addEntityChild(top, middle, false);
    scene.addEntityChild(middle, leaf, false);
    scene.addEntityChild(top, sibling, false);

    std::vector<std::string> stops;
    const PreviewOwner::Token pose = PreviewOwner::acquire(1, middle, &scene, "pose", [&](){ stops.push_back("pose"); });
    const PreviewOwner::Token side = PreviewOwner::acquire(1, sibling, &scene, "side", [&](){ stops.push_back("side"); });
    const PreviewOwner::Token otherScene = PreviewOwner::acquire(2, leaf, &scene, "other", [&](){ stops.push_back("other"); });
    EXPECT_TRUE(stops.empty()) << "siblings and other scenes do not overlap";
    EXPECT_EQ(PreviewOwner::ownerOf(1, middle).token, pose);
    EXPECT_EQ(PreviewOwner::ownerOf(1, middle).name, "pose");

    // a descendant of the pose's root: the pose stops (and may release itself while stopping)
    PreviewOwner::Token clip = 0;
    stops.clear();
    clip = PreviewOwner::acquire(1, leaf, &scene, "clip", [&](){ stops.push_back("clip"); });
    EXPECT_EQ(stops, std::vector<std::string>{"pose"});
    EXPECT_FALSE(PreviewOwner::owns(pose));
    EXPECT_TRUE(PreviewOwner::owns(clip));
    PreviewOwner::release(pose);    // stale: a no-op

    // an ancestor of both
    stops.clear();
    const PreviewOwner::Token whole = PreviewOwner::acquire(1, top, &scene, "whole", [&](){
        stops.push_back("whole");
    });
    EXPECT_EQ(stops, (std::vector<std::string>{"side", "clip"}));
    EXPECT_FALSE(PreviewOwner::owns(side));
    EXPECT_TRUE(PreviewOwner::owns(whole));
    EXPECT_TRUE(PreviewOwner::owns(otherScene));

    // keyed by an owner object: acquiring the same root again keeps it and stops nobody
    int poseObject = 0;
    stops.clear();
    const PreviewOwner::Token keyed = PreviewOwner::acquireFor(&poseObject, 1, top, &scene, "keyed", [&](){ stops.push_back("keyed"); });
    EXPECT_EQ(stops, std::vector<std::string>{"whole"});
    EXPECT_EQ(PreviewOwner::acquireFor(&poseObject, 1, top, &scene, "keyed", [&](){ stops.push_back("keyed"); }), keyed);
    EXPECT_EQ(PreviewOwner::tokenOf(&poseObject), keyed);
    EXPECT_EQ(stops, std::vector<std::string>{"whole"});
    PreviewOwner::releaseFor(&poseObject);
    EXPECT_FALSE(PreviewOwner::owns(keyed));
    EXPECT_EQ(PreviewOwner::tokenOf(&poseObject), 0);

    // a closed scene's owners are forgotten without being stopped
    stops.clear();
    PreviewOwner::releaseScene(2);
    EXPECT_FALSE(PreviewOwner::owns(otherScene));
    EXPECT_TRUE(stops.empty());
    EXPECT_FALSE(PreviewOwner::owns(0));
    for (const PreviewOwner::Owner& owner : PreviewOwner::owners()){
        EXPECT_NE(owner.sceneId, 2u);
    }
    PreviewOwner::releaseScene(1);
}

// ---- the controller on a headless project ---------------------------------------------------

namespace {

    // the App's part: what Project calls on the editor host reaches the clip hooks
    struct ClipHost : public EditorHost {
        void stopTransientPreviews() override {
            if (animationClipAppHooks().stopTransientPreviews) animationClipAppHooks().stopTransientPreviews();
        }
        void clearSceneWindowState(uint32_t sceneId) override {
            if (animationClipAppHooks().sceneClosed) animationClipAppHooks().sceneClosed(sceneId);
        }
        void prepareForProjectSwitch() override {
            if (animationClipAppHooks().projectClosed) animationClipAppHooks().projectClosed();
        }
    };

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-clip-controller-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            // the editor's own settings (last project, ...) stay out of the user's
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: clipcontroller\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
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

    bool headlessStarted = false;

    Project* project() {
        headlessStarted = true;
        return headless().project.get();
    }
    fs::path assets() { return project()->getAssetsPath(); }

    // the temporary project and config go when the run ends (the engine stays: other suites share it)
    struct TempProjectCleanup : public ::testing::Environment {
        void TearDown() override {
            if (!headlessStarted) return;
            std::error_code ec;
            fs::remove_all(headless().dir.parent_path(), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const tempProjectCleanup = ::testing::AddGlobalTestEnvironment(new TempProjectCleanup);

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    ClipKey key(float time, std::vector<float> value) {
        ClipKey out;
        out.time = time;
        out.value = std::move(value);
        return out;
    }

    // Arm moves up from (1, 0, 0) to (1, 2, 0) over a second, Arm/Hand grows to 2
    ClipData armClip() {
        ClipData data;
        data.wrap = ClipWrapMode::Loop;
        ClipTrackData position;
        position.path = "Arm";
        position.channel = ClipChannel::Position;
        position.keys = {key(0.0f, {1, 0, 0}), key(1.0f, {1, 2, 0})};
        ClipTrackData scale;
        scale.path = "Arm/Hand";
        scale.channel = ClipChannel::Scale;
        scale.keys = {key(0.0f, {1, 1, 1}), key(1.0f, {2, 2, 2})};
        data.tracks = {position, scale};
        return data;
    }

    // a clip file in the project (asset-relative path)
    void writeClip(const std::string& assetRelative, const ClipData& data) {
        writeAll(assets() / assetRelative, AnimationClipDocument::fileText(data));
    }

    // Hero (Animation Player listing anim/arm.anim) > Arm > Hand, and Prop beside it
    struct ClipScene {
        uint32_t id = 0;
        SceneProject* sceneProject = nullptr;
        Scene* scene = nullptr;
        Entity hero = NULL_ENTITY;
        Entity arm = NULL_ENTITY;
        Entity hand = NULL_ENTITY;
        Entity prop = NULL_ENTITY;

        Transform& transform(Entity entity) const { return scene->getComponent<Transform>(entity); }
    };

    Entity addObject(ClipScene& s, const std::string& name, Entity parent, const Vector3& position) {
        const Entity entity = s.scene->createUserEntity();
        s.scene->setEntityName(entity, name);
        Transform transform;
        transform.position = position;
        s.scene->addComponent<Transform>(entity, transform);
        if (parent != NULL_ENTITY) s.scene->addEntityChild(parent, entity, false);
        s.sceneProject->entities.push_back(entity);
        return entity;
    }

    ClipScene clipScene(const std::string& clip = "anim/arm.anim") {
        ClipScene s;
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Clips", SceneType::SCENE_3D);
        s.id = p->getSelectedSceneId();
        s.sceneProject = p->getScene(s.id);
        s.scene = s.sceneProject->scene;
        s.hero = addObject(s, "Hero", NULL_ENTITY, Vector3(0, 0, 0));
        s.arm = addObject(s, "Arm", s.hero, Vector3(5, 5, 5));
        s.hand = addObject(s, "Hand", s.arm, Vector3(0, 1, 0));
        s.prop = addObject(s, "Prop", NULL_ENTITY, Vector3(3, 0, 0));
        AnimationPlayerComponent player;
        AnimationPlayerClip entry;
        entry.clip = clip;
        player.clips = {entry};
        s.scene->addComponent<AnimationPlayerComponent>(s.hero, player);
        s.sceneProject->isModified = false;
        return s;
    }

    // what a frame does with the local values (world matrices), for the derived state checks
    void transformFrame(const ClipScene& s) {
        RenderSystem* render = s.scene->getSystem<RenderSystem>().get();
        for (Entity entity : {s.hero, s.arm, s.hand, s.prop}) render->updateTransform(s.transform(entity), entity);
    }

    void expectNear(const Vector3& actual, const Vector3& expected, const char* what) {
        EXPECT_NEAR(actual.x, expected.x, 1e-4f) << what;
        EXPECT_NEAR(actual.y, expected.y, 1e-4f) << what;
        EXPECT_NEAR(actual.z, expected.z, 1e-4f) << what;
    }

    // every test: the host forwarding to the clip hooks, a fresh clip file, no shared controller
    class AnimationClipControllerTest : public ::testing::Test {
    protected:
        ClipHost host;

        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            setEditorHost(&host);
            fs::remove_all(assets() / "anim");
            fs::remove_all(assets() / "pose");
            writeClip("anim/arm.anim", armClip());
            AnimationClipDocument::checkDiskChanges(project());
            // the headless project is shared by the tests: an earlier test's clip commits must
            // not be the newest undoable step of this one (the undo routing, R5b)
            project()->getProjectCommandHistory()->clear();
        }

        void TearDown() override {
            AnimationClipController::release(project());
            setEditorHost(nullptr);
        }

        AnimationClipController& controller() { return AnimationClipController::shared(project()); }
    };

}

TEST_F(AnimationClipControllerTest, BindsTheNearestPlayerRootAndItsDefaultClip) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hand, &error)) << error;
    EXPECT_EQ(c.getRoot(), s.hero) << "the nearest ancestor with an Animation Player";
    EXPECT_EQ(AnimationClipController::findPlayerRoot(s.scene, s.arm), s.hero);
    EXPECT_EQ(AnimationClipController::findPlayerRoot(s.scene, s.prop), NULL_ENTITY);
    EXPECT_TRUE(c.hasPlayer());
    EXPECT_EQ(c.getStateNames(), std::vector<std::string>{"arm"});
    EXPECT_EQ(c.getStateName(), "arm");
    ASSERT_TRUE(c.getDocument());
    EXPECT_EQ(c.getClipPath(), "assets/anim/arm.anim");
    EXPECT_FLOAT_EQ(c.getLength(), 1.0f);
    EXPECT_FALSE(c.isReadOnly());
    EXPECT_EQ(c.getBindings().tracks.size(), 2u);

    // an object without a player: the player's clip closes (the window's empty state)
    ASSERT_TRUE(c.bindRoot(s.id, s.prop, &error)) << error;
    EXPECT_EQ(c.getRoot(), s.prop);
    EXPECT_FALSE(c.hasPlayer());
    EXPECT_FALSE(c.getDocument());
    EXPECT_FALSE(c.openState("arm", &error));
    ASSERT_TRUE(c.bindRoot(s.id, NULL_ENTITY, &error));
    EXPECT_EQ(c.getRoot(), NULL_ENTITY);
}

TEST_F(AnimationClipControllerTest, PreviewWritesTheClipAndStopRestoresWithoutModifyingTheScene) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    EXPECT_EQ(c.getPreviewRoot(), s.hero);
    expectNear(s.transform(s.arm).position, Vector3(1, 1, 0), "Arm at 0.5 s");
    expectNear(s.transform(s.hand).scale, Vector3(1.5f, 1.5f, 1.5f), "Hand at 0.5 s");
    EXPECT_FALSE(s.sceneProject->isModified) << "a preview never marks the scene modified";
    EXPECT_TRUE(c.getUnresolvedPaths().empty());

    const std::vector<AnimationClipController::BoundTransform> bound = c.sampledBoundTransforms();
    ASSERT_EQ(bound.size(), 2u);
    EXPECT_EQ(bound[0].path, "Arm");
    EXPECT_EQ(bound[0].entity, s.arm);
    expectNear(bound[0].position, Vector3(1, 1, 0), "sampled Arm");
    EXPECT_EQ(c.sampledBoundTransforms(1).size(), 1u);

    // playback advances and loops
    c.play();
    c.update(0.25);
    EXPECT_NEAR(c.getTime(), 0.75f, 1e-5f);
    expectNear(s.transform(s.arm).position, Vector3(1, 1.5f, 0), "Arm at 0.75 s");
    c.update(0.5);
    EXPECT_NEAR(c.getTime(), 0.25f, 1e-5f);
    c.pause();

    c.stopPreview();
    EXPECT_FALSE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "Arm restored");
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "Hand restored");
    EXPECT_FALSE(s.sceneProject->isModified);
}

TEST_F(AnimationClipControllerTest, SaveDuringPreviewWritesTheUnpreviewedScene) {
    ClipScene s = clipScene();
    transformFrame(s);
    ASSERT_TRUE(project()->saveSceneToPath(s.id, "ClipPreview.scene"));
    const fs::path file = project()->getProjectPath() / "ClipPreview.scene";
    const std::string unpreviewed = readAll(file);
    ASSERT_FALSE(unpreviewed.empty());
    const Vector3 handWorld = s.transform(s.hand).worldPosition;

    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hand, &error)) << error;
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    // a frame ran: the world matrices show the preview too
    transformFrame(s);
    expectNear(s.transform(s.hand).worldPosition, Vector3(1, 2, 0), "Hand's previewed world position");

    // Save (Project::saveSceneFile asks the host to stop transient previews first)
    ASSERT_TRUE(project()->saveSceneToPath(s.id, "ClipPreview.scene"));
    EXPECT_FALSE(c.isPreviewing());
    EXPECT_EQ(readAll(file), unpreviewed) << "the saved file holds the scene without the preview";
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "Arm");
    expectNear(s.transform(s.hand).worldPosition, handWorld, "Hand's world position (derived state restored)");
    EXPECT_FALSE(s.sceneProject->isModified);
}

TEST_F(AnimationClipControllerTest, PlayStopsThePreviewAndAPreviewNeverStartsWhilePlaying) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(1.0f);
    ASSERT_TRUE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(1, 2, 0), "previewed");

    // Project::start asks the host to stop transient previews before it snapshots the scene
    getEditorHost().stopTransientPreviews();
    EXPECT_FALSE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "the play snapshot sees the unpreviewed scene");

    s.sceneProject->playState = ScenePlayState::PLAYING;
    EXPECT_FALSE(c.startPreview(&error));
    EXPECT_NE(error.find("edit mode"), std::string::npos) << error;
    c.seek(0.5f);
    EXPECT_FALSE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "nothing written while playing");

    // a preview running when play begins without the host's call stops on the next update
    s.sceneProject->playState = ScenePlayState::STOPPED;
    c.seek(1.0f);
    ASSERT_TRUE(c.isPreviewing());
    s.sceneProject->playState = ScenePlayState::LOADING;
    c.update(0.0);
    s.sceneProject->playState = ScenePlayState::STOPPED;
    EXPECT_FALSE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "restored");
}

TEST_F(AnimationClipControllerTest, SceneEditsAndUndoOfAPreviewedChannelUpdateTheSnapshot) {
    ClipScene s = clipScene();
    // the Hand's rotation is not animated: a write to it is kept as it is
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(1.0f);
    ASSERT_TRUE(c.isPreviewing());
    CommandHistory* history = CommandHandle::get(s.id);

    // an Inspector edit of the previewed Arm position: the command reads the rest
    ASSERT_TRUE(history->addCommand(new PropertyCmd<Vector3>(project(), s.id, s.arm, ComponentType::Transform, "position", Vector3(9, 9, 9))));
    expectNear(s.transform(s.arm).position, Vector3(9, 9, 9), "the edit shows until the preview applies again");
    c.update(0.0);
    expectNear(s.transform(s.arm).position, Vector3(1, 2, 0), "previewed again");

    // undo during the preview: the old value recorded is the rest, and it is the rest again
    ASSERT_TRUE(history->undo());
    c.update(0.0);
    expectNear(s.transform(s.arm).position, Vector3(1, 2, 0), "previewed after the undo");
    c.stopPreview();
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "the undone edit's rest");

    // redo during another preview: the edit's value is the snapshot, kept at stop
    c.seek(0.0f);
    ASSERT_TRUE(c.isPreviewing());
    ASSERT_TRUE(history->redo());
    c.update(0.0);
    expectNear(s.transform(s.arm).position, Vector3(1, 0, 0), "previewed after the redo");
    c.stopPreview();
    expectNear(s.transform(s.arm).position, Vector3(9, 9, 9), "the redone edit");
    // and undo outside the preview gives the original value: no preview value in the history
    ASSERT_TRUE(history->undo());
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "the original");
}

TEST_F(AnimationClipControllerTest, KeyingCommitsAreProjectStepsAndADragIsOneStep) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* projectHistory = project()->getProjectCommandHistory();
    CommandHistory* sceneHistory = CommandHandle::get(s.id);
    const size_t sceneSteps = sceneHistory->position();
    const size_t start = projectHistory->position();

    // Add Property: the Prop is not under the root; the Hand's position is (a key at the playhead)
    c.setTime(0.5f);
    EXPECT_FALSE(c.addProperty("Arm", ClipChannel::Position, nullptr, &error)) << "already animated";
    size_t track = 99;
    ASSERT_TRUE(c.addProperty("Arm/Hand", ClipChannel::Position, &track, &error)) << error;
    ASSERT_LT(track, c.getDocument()->data().tracks.size());
    const ClipTrackData& added = c.getDocument()->data().tracks[track];
    EXPECT_EQ(added.path, "Arm/Hand");
    ASSERT_EQ(added.keys.size(), 1u);
    EXPECT_FLOAT_EQ(added.keys[0].time, 0.5f);
    EXPECT_EQ(added.keys[0].value, (std::vector<float>{0, 1, 0}));
    EXPECT_EQ(projectHistory->position(), start + 1) << "one project step";
    EXPECT_EQ(sceneHistory->position(), sceneSteps) << "no scene step";
    EXPECT_FALSE(s.sceneProject->isModified);
    // R5b: the newest undoable step is the clip commit, whatever window has focus
    EXPECT_TRUE(c.newestStepIsClipEdit());
    EXPECT_TRUE(animationClipAppHooks().ownsUndo());

    // K with the Arm moved: keys every bound track at the playhead with the current values
    s.transform(s.arm).position = Vector3(4, 4, 4);
    ASSERT_TRUE(c.keyAll(0.5f, &error)) << error;
    const int arm = c.getDocument()->data().findTrack("Arm", ClipChannel::Position);
    ASSERT_GE(arm, 0);
    AnimationClipController::KeyRef middle;
    ASSERT_TRUE(AnimationClipController::findKey(c.getDocument()->data(), "Arm", ClipChannel::Position, 0.5f, middle));
    EXPECT_EQ(c.getDocument()->data().tracks[middle.track].keys[middle.key].value, (std::vector<float>{4, 4, 4}));
    EXPECT_EQ(projectHistory->position(), start + 2);
    // Shift+K: nothing differs from the clip any more
    EXPECT_FALSE(c.keyModified(0.5f, &error));

    // a drag of the first Arm key over the middle one and past it: one undo step, nothing lost
    AnimationClipController::KeyRef first;
    ASSERT_TRUE(AnimationClipController::findKey(c.getDocument()->data(), "Arm", ClipChannel::Position, 0.0f, first));
    ASSERT_TRUE(c.beginKeyDrag({first}, &error)) << error;
    std::vector<AnimationClipController::KeyRef> moved;
    ASSERT_TRUE(c.updateKeyDrag(0.25f, nullptr, &moved, &error)) << error;
    ASSERT_TRUE(c.updateKeyDrag(0.5f, nullptr, &moved, &error)) << error;
    ASSERT_TRUE(c.updateKeyDrag(0.75f, nullptr, &moved, &error)) << error;
    c.endKeyDrag();
    ASSERT_EQ(moved.size(), 1u);
    const ClipTrackData& armTrack = c.getDocument()->data().tracks[(size_t)c.getDocument()->data().findTrack("Arm", ClipChannel::Position)];
    ASSERT_EQ(armTrack.keys.size(), 3u) << "the key passed over is still there";
    EXPECT_NEAR(armTrack.keys[0].time, 0.5f, 1e-5f);
    EXPECT_NEAR(armTrack.keys[1].time, 0.75f, 1e-5f);
    EXPECT_EQ(armTrack.keys[1].value, (std::vector<float>{1, 0, 0}));
    EXPECT_EQ(moved[0].key, 1u);
    EXPECT_EQ(projectHistory->position(), start + 3) << "the drag merged into one step";
    EXPECT_EQ(sceneHistory->position(), sceneSteps);

    ASSERT_TRUE(projectHistory->undo());
    {
        const ClipTrackData& undone = c.getDocument()->data().tracks[(size_t)c.getDocument()->data().findTrack("Arm", ClipChannel::Position)];
        ASSERT_EQ(undone.keys.size(), 3u);
        EXPECT_NEAR(undone.keys[0].time, 0.0f, 1e-6f) << "undo puts the key back where the drag began";
    }

    // the file on disk is the document
    EXPECT_EQ(readAll(assets() / "anim/arm.anim"), AnimationClipDocument::fileText(c.getDocument()->data()));

    // discrete edits: move, value, ease, delete, retarget, remove
    AnimationClipController::KeyRef last;
    ASSERT_TRUE(AnimationClipController::findKey(c.getDocument()->data(), "Arm", ClipChannel::Position, 1.0f, last));
    ASSERT_TRUE(c.moveKeys({last}, 0.5f, "", &moved, &error)) << error;
    ASSERT_EQ(moved.size(), 1u);
    EXPECT_NEAR(c.getDocument()->data().tracks[moved[0].track].keys[moved[0].key].time, 1.5f, 1e-5f);
    EXPECT_FALSE(c.setKeyValue(moved[0], {1, 2}, "", &error)) << "a position holds three numbers";
    ASSERT_TRUE(c.setKeyValue(moved[0], {7, 7, 7}, "", &error)) << error;
    ASSERT_TRUE(c.setEase({moved[0]}, EaseType::CUBIC_IN_OUT, &error)) << error;
    EXPECT_EQ(c.getDocument()->data().tracks[moved[0].track].keys[moved[0].key].ease, EaseType::CUBIC_IN_OUT);
    const int hand = c.getDocument()->data().findTrack("Arm/Hand", ClipChannel::Position);
    ASSERT_GE(hand, 0);
    ASSERT_TRUE(c.deleteKeys({{(size_t)hand, 0}}, &error)) << error;
    EXPECT_LT(c.getDocument()->data().findTrack("Arm/Hand", ClipChannel::Position), 0) << "its last key went: the track too";
    const int scale = c.getDocument()->data().findTrack("Arm/Hand", ClipChannel::Scale);
    ASSERT_GE(scale, 0);
    EXPECT_FALSE(c.retargetTrack((size_t)scale, "Arm/", &error));
    ASSERT_TRUE(c.retargetTrack((size_t)scale, "Arm/Finger", &error)) << error;
    EXPECT_GE(c.getDocument()->data().findTrack("Arm/Finger", ClipChannel::Scale), 0);
    ASSERT_TRUE(c.removeTrack((size_t)c.getDocument()->data().findTrack("Arm/Finger", ClipChannel::Scale), &error)) << error;
    EXPECT_EQ(c.getDocument()->data().tracks.size(), 1u);
    EXPECT_EQ(sceneHistory->position(), sceneSteps) << "never a scene step";
    EXPECT_FALSE(s.sceneProject->isModified);
}

TEST_F(AnimationClipControllerTest, EditsDuringThePreviewShowAndRemovedTracksGoBackToRest) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(1.0f);
    expectNear(s.transform(s.hand).scale, Vector3(2, 2, 2), "previewed");
    AnimationClipController::KeyRef end;
    ASSERT_TRUE(AnimationClipController::findKey(c.getDocument()->data(), "Arm", ClipChannel::Position, 1.0f, end));
    ASSERT_TRUE(c.setKeyValue(end, {0, 3, 0}, "", &error)) << error;
    expectNear(s.transform(s.arm).position, Vector3(0, 3, 0), "the edited key shows at once");

    // the Hand's track goes: the Hand shows its rest
    ASSERT_TRUE(c.removeTrack((size_t)c.getDocument()->data().findTrack("Arm/Hand", ClipChannel::Scale), &error)) << error;
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "rest");
    // its undo: animated again
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    expectNear(s.transform(s.hand).scale, Vector3(2, 2, 2), "previewed again after the undo");
    c.stopPreview();
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "Arm");
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "Hand");
}

TEST_F(AnimationClipControllerTest, RecordKeysEditsOfTheBoundHierarchyAndRestoresAtStop) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.setTime(0.5f);
    c.setRecording(true);
    ASSERT_TRUE(c.isRecording());
    ASSERT_TRUE(c.isPreviewing()) << "Record previews";
    EXPECT_TRUE(ClipRecordHooks::claims(s.id, s.hand, ClipChannel::Rotation));
    EXPECT_FALSE(ClipRecordHooks::claims(s.id, s.prop, ClipChannel::Position)) << "not under the root";
    EXPECT_FALSE(ClipRecordHooks::claims(s.id + 1000, s.hand, ClipChannel::Position));
    EXPECT_FALSE(animationClipAppHooks().ownsUndo()) << "Record alone makes no step: the undo stays the scene's (R5b)";

    // a gizmo drag of the Hand: two record calls of one drag, one project step, no scene step
    const size_t start = project()->getProjectCommandHistory()->position();
    const size_t sceneSteps = CommandHandle::get(s.id)->position();
    const std::string drag = ClipRecordHooks::beginDrag();
    ClipRecordValue value;
    value.channel = ClipChannel::Position;
    value.value = {0, 3, 0};
    ASSERT_TRUE(ClipRecordHooks::record(s.id, s.hand, value, drag));
    value.value = {0, 4, 0};
    ASSERT_TRUE(ClipRecordHooks::record(s.id, s.hand, value, drag));
    ClipRecordHooks::endDrag();
    expectNear(s.transform(s.hand).position, Vector3(0, 4, 0), "the live value");
    EXPECT_TRUE(animationClipAppHooks().ownsUndo()) << "the recorded key is the newest step: the Scene view's undo goes to the project";
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), start + 1);
    EXPECT_EQ(CommandHandle::get(s.id)->position(), sceneSteps);
    AnimationClipController::KeyRef keyed;
    ASSERT_TRUE(AnimationClipController::findKey(c.getDocument()->data(), "Arm/Hand", ClipChannel::Position, 0.5f, keyed));
    EXPECT_EQ(c.getDocument()->data().tracks[keyed.track].keys[keyed.key].value, (std::vector<float>{0, 4, 0}));
    EXPECT_FALSE(s.sceneProject->isModified);

    // stopping the preview ends Record and leaves the scene as it was
    c.stopPreview();
    EXPECT_FALSE(c.isRecording());
    EXPECT_FALSE(ClipRecordHooks::claims(s.id, s.hand, ClipChannel::Position));
    expectNear(s.transform(s.hand).position, Vector3(0, 1, 0), "Hand restored");
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "Arm restored");
}

TEST_F(AnimationClipControllerTest, ATargetlessClipPreviewsOnItsPreviewModel) {
    ClipScene s = clipScene();
    // a second hierarchy: an instance of the clip's preview model with the same child names
    const Entity statue = addObject(s, "Statue", NULL_ENTITY, Vector3(0, 0, 0));
    const Entity statueArm = addObject(s, "Arm", statue, Vector3(8, 8, 8));
    addObject(s, "Hand", statueArm, Vector3(0, 1, 0));
    ModelComponent model;
    model.filename = "models/statue.gltf";
    s.scene->addComponent<ModelComponent>(statue, model);

    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.openClip("assets/anim/arm.anim", &error)) << error;
    EXPECT_EQ(c.getRoot(), NULL_ENTITY);
    EXPECT_FALSE(c.startPreview(&error)) << "no preview model yet";
    EXPECT_NE(error.find("preview.model"), std::string::npos) << error;

    ClipData data = c.getDocument()->data();
    data.previewModel = "models/statue.gltf";
    ASSERT_TRUE(c.commit(data, "Preview On", "", &error)) << error;
    EXPECT_EQ(c.getPreviewModel(), "models/statue.gltf");
    ASSERT_TRUE(c.startPreview(&error)) << error;
    EXPECT_EQ(c.getPreviewRoot(), statue);
    EXPECT_EQ(c.getPreviewSceneId(), s.id);
    c.seek(1.0f);
    expectNear(s.transform(statueArm).position, Vector3(1, 2, 0), "the statue's Arm");
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "the hero is not touched");
    EXPECT_EQ(c.getBindings().tracks.size(), 2u);
    c.stopPreview();
    expectNear(s.transform(statueArm).position, Vector3(8, 8, 8), "restored");
}

TEST_F(AnimationClipControllerTest, ReleasedOnProjectCloseAndDroppedWithItsScene) {
    ClipScene s = clipScene();
    {
        AnimationClipController& c = controller();
        std::string error;
        ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
        c.seek(1.0f);
        ASSERT_TRUE(c.isPreviewing());
    }
    // a project switch (or shutdown) restores the scene and drops the controller
    ASSERT_TRUE(animationClipAppHooks().projectClosed);
    getEditorHost().prepareForProjectSwitch();
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "restored at project close");
    EXPECT_TRUE(PreviewOwner::ownerOf(s.id, s.hero).token == 0);
    {
        AnimationClipController& fresh = controller();
        EXPECT_FALSE(fresh.isPreviewing());
        EXPECT_FALSE(fresh.getDocument());
        EXPECT_EQ(fresh.getRoot(), NULL_ENTITY);

        std::string error;
        ASSERT_TRUE(fresh.bindRoot(s.id, s.hero, &error)) << error;
        fresh.seek(1.0f);
        ASSERT_TRUE(fresh.isPreviewing());
        // the scene closes: nothing is written to it, the binding goes
        getEditorHost().clearSceneWindowState(s.id);
        EXPECT_FALSE(fresh.isPreviewing());
        EXPECT_EQ(fresh.getRoot(), NULL_ENTITY);
        expectNear(s.transform(s.arm).position, Vector3(1, 2, 0), "a closed scene is not written");
    }
}

// The Pose window and a clip preview on one model, started and stopped in every order: one
// owner at a time, each snapshot taken from the unpreviewed scene, so the scene ends as saved
TEST_F(AnimationClipControllerTest, PoseWindowAndClipPreviewInEveryOrderLeaveTheSavedScene) {
    fs::create_directories(assets() / "models");
    fs::copy_file(fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "pose-animation" / "assets" /
                  "models" / "hero_tpose.gltf", assets() / "models/hero.gltf", fs::copy_options::overwrite_existing);

    ClipScene s = clipScene();
    Model heroModel(s.scene);
    const Entity model = heroModel.getEntity();
    s.scene->setEntityName(model, "Character");
    s.sceneProject->entities.push_back(model);
    ASSERT_TRUE(s.scene->getSystem<MeshSystem>()->loadGLTF(model, (assets() / "models/hero.gltf").string(), false, false, false));
    s.scene->getComponent<ModelComponent>(model).filename = "models/hero.gltf";

    // a clip turning the left arm, on the model's own Animation Player
    Entity leftArm = NULL_ENTITY;
    std::string armPath;
    for (const auto& entry : anim::descendantPaths(s.scene, model)){
        const std::string name = s.scene->getEntityName(entry.second);
        if (name.size() >= 7 && name.compare(name.size() - 7, 7, "LeftArm") == 0){
            leftArm = entry.second;
            armPath = entry.first;
            break;
        }
    }
    ASSERT_NE(leftArm, NULL_ENTITY) << "the hero has a LeftArm bone";
    ClipData turn;
    ClipTrackData rotation;
    rotation.path = armPath;
    rotation.channel = ClipChannel::Rotation;
    std::vector<float> quarter(4);
    quaternionToXYZW(Quaternion(0.7071068f, 0.0f, 0.0f, 0.7071068f), quarter.data());
    rotation.keys = {key(0.0f, quarter), key(1.0f, quarter)};
    turn.tracks = {rotation};
    writeClip("anim/turn.anim", turn);
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = "anim/turn.anim";
    player.clips = {entry};
    s.scene->addComponent<AnimationPlayerComponent>(model, player);

    // a pose animation with its arms down
    pose::PoseAnimationData poseData;
    pose::PoseKey down;
    down.name = "Down";
    for (size_t i = 0; i < down.body.angles.size(); i++) down.body.angles[i] = (i % 3 == 0) ? 20.0f : 0.0f;
    poseData.poses = {down};
    std::string error;
    ASSERT_TRUE(PoseAnimationDocument::create(project(), "assets/pose/down.poseanim", poseData, "Create", nullptr, &error)) << error;

    s.sceneProject->isModified = false;
    transformFrame(s);
    ASSERT_TRUE(project()->saveSceneToPath(s.id, "PoseAndClip.scene"));
    const fs::path file = project()->getProjectPath() / "PoseAndClip.scene";
    const std::string saved = readAll(file);
    const Quaternion restRotation = s.transform(leftArm).rotation;

    PoseEditSession session(project());
    ASSERT_TRUE(session.bind(s.scene, s.id, model, &error)) << error;
    ASSERT_TRUE(session.open("assets/pose/down.poseanim", &error)) << error;
    AnimationClipController& c = controller();
    ASSERT_TRUE(c.bindRoot(s.id, model, &error)) << error;
    ASSERT_EQ(c.getStateName(), "turn");

    auto posed = [&](){ return session.bonesApplied(); };
    auto armAtRest = [&](){ return s.transform(leftArm).rotation == restRotation; };

    for (int order = 0; order < 4; order++){
        SCOPED_TRACE("order " + std::to_string(order));
        const bool poseFirst = order < 2;
        const bool stopClipFirst = order % 2 == 0;
        if (poseFirst){
            session.startPosing();
            ASSERT_TRUE(posed());
            EXPECT_FALSE(armAtRest()) << "posed";
            c.seek(0.5f);
            ASSERT_TRUE(c.isPreviewing());
            EXPECT_FALSE(posed()) << "the clip preview stopped the pose first";
            EXPECT_EQ(session.mode(), PoseEditSession::Mode::Idle);
        }else{
            c.seek(0.5f);
            ASSERT_TRUE(c.isPreviewing());
            EXPECT_FALSE(armAtRest()) << "previewed";
            session.startPosing();
            EXPECT_TRUE(posed());
            EXPECT_FALSE(c.isPreviewing()) << "posing stopped the clip preview first";
        }
        EXPECT_FALSE(armAtRest());
        if (stopClipFirst){
            c.stopPreview();
            session.goIdle();
        }else{
            session.goIdle();
            c.stopPreview();
        }
        EXPECT_TRUE(armAtRest());
        EXPECT_FALSE(s.sceneProject->isModified);
        ASSERT_TRUE(project()->saveSceneToPath(s.id, "PoseAndClip.scene"));
        EXPECT_EQ(readAll(file), saved) << "the scene equals the saved file";
    }
    EXPECT_TRUE(PreviewOwner::ownerOf(s.id, model).token == 0) << "both released their ownership";
    session.unbind();
}

// ---- review round 1 -------------------------------------------------------------------------

// A whole-Transform write of a previewed entity (the translate gizmo: the command carries the
// shown rotation and scale) changes only what the user changed; a redo after the preview
// writes no clip pose (C1)
TEST_F(AnimationClipControllerTest, AWholeTransformWriteDuringThePreviewKeepsTheUntouchedChannels) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.5f);
    expectNear(s.transform(s.hand).scale, Vector3(1.5f, 1.5f, 1.5f), "previewed scale");
    CommandHistory* history = CommandHandle::get(s.id);

    const Transform shown = s.transform(s.hand);
    ASSERT_TRUE(history->addCommand(new ObjectTransformCmd(project(), s.id, s.hand, Vector3(0, 3, 0), shown.rotation, shown.scale)));
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "the untouched scale keeps its rest");
    c.update(0.0);
    expectNear(s.transform(s.hand).scale, Vector3(1.5f, 1.5f, 1.5f), "previewed again");
    c.stopPreview();
    expectNear(s.transform(s.hand).position, Vector3(0, 3, 0), "the moved position");
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "the rest scale");

    // the matrix form the gizmo builds (decomposed again: not bit-equal)
    c.seek(1.0f);
    {
        Transform& hand = s.transform(s.hand);
        const Matrix4 m = Matrix4::translateMatrix(Vector3(0, 4, 0)) * hand.rotation.getRotationMatrix() * Matrix4::scaleMatrix(hand.scale);
        history->addCommandNoMerge(new ObjectTransformCmd(project(), s.id, s.hand, m));
    }
    c.stopPreview();
    expectNear(s.transform(s.hand).position, Vector3(0, 4, 0), "moved again");
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "the rest scale, not the previewed 2");

    // undo and redo with no preview: never the clip pose
    ASSERT_TRUE(history->undo());
    ASSERT_TRUE(history->undo());
    expectNear(s.transform(s.hand).position, Vector3(0, 1, 0), "undone");
    ASSERT_TRUE(history->redo());
    ASSERT_TRUE(history->redo());
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "a redo writes no previewed scale");
    expectNear(s.transform(s.hand).position, Vector3(0, 4, 0), "redone");
}

// The controller's own net: a whole-Transform write (the gizmo's) of exactly the previewed value
// is no edit of the rest (C1); a named property write of it (set_properties, an Inspector field)
// is the edit, the new rest (FX item 16)
TEST_F(AnimationClipControllerTest, WritingThePreviewedValueBackKeepsTheRest) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(1.0f);
    const Transform shown = s.transform(s.hand);
    const Vector3 previewed = shown.scale;
    ASSERT_TRUE(CommandHandle::get(s.id)->addCommand(
        new ObjectTransformCmd(project(), s.id, s.hand, shown.position, shown.rotation, previewed)));
    c.stopPreview();
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "the rest");

    c.seek(1.0f);
    CommandHandle::get(s.id)->addCommandNoMerge(
        new PropertyCmd<Vector3>(project(), s.id, s.hand, ComponentType::Transform, "scale", previewed));
    c.stopPreview();
    expectNear(s.transform(s.hand).scale, previewed, "the value set on purpose");
}

// Copying, duplicating and deleting the previewed hierarchy stop the preview first, so what
// is copied or kept for undo is the rest (C2: PoseEditSession::suspendActive* stop it)
TEST_F(AnimationClipControllerTest, CopiesDuplicatesAndDeletesTakeTheRestNotThePreview) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* history = CommandHandle::get(s.id);

    // Duplicate
    c.seek(1.0f);
    ASSERT_TRUE(c.isPreviewing());
    auto* duplicate = new DuplicateEntityCmd(project(), s.id, {s.hero});
    ASSERT_TRUE(history->addCommand(duplicate));
    EXPECT_FALSE(c.isPreviewing()) << "the duplicate stopped the preview first";
    Entity copyRoot = NULL_ENTITY;
    for (Entity created : duplicate->getCreatedEntities()){
        if (s.transform(created).parent == NULL_ENTITY) copyRoot = created;
    }
    ASSERT_NE(copyRoot, NULL_ENTITY);
    const Entity copyArm = anim::resolvePath(s.scene, copyRoot, "Arm");
    ASSERT_NE(copyArm, NULL_ENTITY);
    expectNear(s.transform(copyArm).position, Vector3(5, 5, 5), "the copy's Arm at rest");
    ASSERT_TRUE(history->undo());

    // Copy (the clipboard text goes through ImGui)
    ImGuiContext* imgui = ImGui::CreateContext();
    c.seek(1.0f);
    ASSERT_TRUE(c.isPreviewing());
    ASSERT_TRUE(EntityClipboard::copyGameObjects(project(), s.id, {s.arm}));
    EXPECT_FALSE(c.isPreviewing()) << "copying a previewed child stopped the preview";
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "copied at rest");

    // a copy elsewhere in the scene leaves the preview alone
    c.seek(1.0f);
    ASSERT_TRUE(EntityClipboard::copyGameObjects(project(), s.id, {s.prop}));
    EXPECT_TRUE(c.isPreviewing());
    ImGui::DestroyContext(imgui);

    // Delete the previewed root, then undo: it comes back at rest
    ASSERT_TRUE(history->addCommand(new DeleteEntityCmd(project(), s.id, s.hero)));
    EXPECT_FALSE(c.isPreviewing());
    c.update(0.0);
    ASSERT_TRUE(history->undo());
    const Entity hero = s.scene->findEntity("Hero");
    ASSERT_NE(hero, NULL_ENTITY);
    const Entity arm = anim::resolvePath(s.scene, hero, "Arm");
    ASSERT_NE(arm, NULL_ENTITY);
    expectNear(s.transform(arm).position, Vector3(5, 5, 5), "the undone delete brings the rest back");

    // the copy and prefab sites (copy, create, apply, revert) pass their GameObjects: only a preview
    // of, under or above them stops. suspendActive() is the pose's alone (final review M1): a copy of
    // another GameObject keeps the clip preview and Record going
    ASSERT_TRUE(c.bindRoot(s.id, hero, &error)) << error;
    c.seek(1.0f);
    ASSERT_TRUE(c.isPreviewing());
    PoseEditSession::suspendActiveFor(s.id, {s.prop});
    EXPECT_TRUE(c.isPreviewing()) << "not under the given roots";
    PoseEditSession::suspendActive();
    EXPECT_TRUE(c.isPreviewing()) << "suspendActive suspends the pose only";
    c.setRecording(true);
    ASSERT_TRUE(c.isRecording());
    PoseEditSession::suspendActive();
    ImGuiContext* clipboard = ImGui::CreateContext();
    ASSERT_TRUE(EntityClipboard::copyGameObjects(project(), s.id, {s.prop}));
    ImGui::DestroyContext(clipboard);
    EXPECT_TRUE(c.isRecording()) << "a copy of an unrelated GameObject keeps Record on";
    PoseEditSession::suspendActiveFor(s.id, {hero});
    EXPECT_FALSE(c.isPreviewing());
    EXPECT_FALSE(c.isRecording());
    expectNear(s.transform(arm).position, Vector3(5, 5, 5), "restored");
}

// R5b: Ctrl+Z / Ctrl+Y go to the project history only while the newest undoable step is a clip
// commit (I1)
TEST_F(AnimationClipControllerTest, UndoGoesToTheClipOnlyWhileItsCommitIsTheNewestStep) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* history = CommandHandle::get(s.id);

    // a scene step made before: the scene's
    ASSERT_TRUE(history->addCommand(new PropertyCmd<Vector3>(project(), s.id, s.prop, ComponentType::Transform, "position", Vector3(1, 1, 1))));
    EXPECT_FALSE(animationClipAppHooks().ownsUndo());

    // Record on, the Hand recorded: the project's, from any window
    c.setTime(0.5f);
    c.setRecording(true);
    ClipRecordValue value;
    value.channel = ClipChannel::Position;
    value.value = {0, 2, 0};
    ASSERT_TRUE(ClipRecordHooks::record(s.id, s.hand, value, ClipRecordHooks::beginDrag()));
    ClipRecordHooks::endDrag();
    EXPECT_TRUE(animationClipAppHooks().ownsUndo());
    EXPECT_TRUE(c.newestStepIsClipEdit());

    // the Prop moved (not recorded: a scene step): the scene's again, even with Record on
    ASSERT_FALSE(ClipRecordHooks::claims(s.id, s.prop, ClipChannel::Position));
    ASSERT_TRUE(history->addCommand(new PropertyCmd<Vector3>(project(), s.id, s.prop, ComponentType::Transform, "position", Vector3(7, 7, 7))));
    EXPECT_FALSE(animationClipAppHooks().ownsUndo()) << "Record never steals a scene undo";
    EXPECT_FALSE(animationClipAppHooks().ownsUndo());

    // that move undone: the record commit is the newest undoable step again
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(animationClipAppHooks().ownsUndo());
    // and once the record commit is undone, the older scene step is the newest one
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FALSE(animationClipAppHooks().ownsUndo());
    // another project step that is no clip commit: not routed by the clip mode
    c.setRecording(false);
    c.stopPreview();
}

// A suspended Pose window (a save, a copy) keeps the model: a clip preview started then puts it
// to Idle instead of being stopped by the pose's next update (I3)
TEST_F(AnimationClipControllerTest, ASuspendedPoseWindowYieldsTheModelToAClipPreview) {
    fs::create_directories(assets() / "models");
    fs::copy_file(fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "pose-animation" / "assets" /
                  "models" / "hero_tpose.gltf", assets() / "models/hero.gltf", fs::copy_options::overwrite_existing);
    ClipScene s = clipScene();
    Model heroModel(s.scene);
    const Entity model = heroModel.getEntity();
    s.scene->setEntityName(model, "Character");
    s.sceneProject->entities.push_back(model);
    ASSERT_TRUE(s.scene->getSystem<MeshSystem>()->loadGLTF(model, (assets() / "models/hero.gltf").string(), false, false, false));
    s.scene->getComponent<ModelComponent>(model).filename = "models/hero.gltf";
    std::string armPath;
    for (const auto& entry : anim::descendantPaths(s.scene, model)){
        const std::string name = s.scene->getEntityName(entry.second);
        if (name.size() >= 7 && name.compare(name.size() - 7, 7, "LeftArm") == 0) armPath = entry.first;
    }
    ASSERT_FALSE(armPath.empty());
    ClipData turn;
    ClipTrackData rotation;
    rotation.path = armPath;
    rotation.channel = ClipChannel::Rotation;
    std::vector<float> quarter(4);
    quaternionToXYZW(Quaternion(0.7071068f, 0.0f, 0.0f, 0.7071068f), quarter.data());
    rotation.keys = {key(0.0f, quarter)};
    turn.tracks = {rotation};
    writeClip("anim/turn.anim", turn);
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = "anim/turn.anim";
    player.clips = {entry};
    s.scene->addComponent<AnimationPlayerComponent>(model, player);
    pose::PoseAnimationData poseData;
    pose::PoseKey down;
    for (size_t i = 0; i < down.body.angles.size(); i++) down.body.angles[i] = (i % 3 == 0) ? 20.0f : 0.0f;
    poseData.poses = {down};
    std::string error;
    fs::remove(assets() / "pose/down.poseanim");
    ASSERT_TRUE(PoseAnimationDocument::create(project(), "assets/pose/down.poseanim", poseData, "Create", nullptr, &error)) << error;

    PoseEditSession session(project());
    PoseEditSession::setActive(&session);
    ASSERT_TRUE(session.bind(s.scene, s.id, model, &error)) << error;
    ASSERT_TRUE(session.open("assets/pose/down.poseanim", &error)) << error;
    session.startPosing();
    ASSERT_TRUE(session.bonesApplied());
    PoseEditSession::suspendActive();
    EXPECT_FALSE(session.bonesApplied());
    EXPECT_EQ(PreviewOwner::ownerOf(s.id, model).name, "Pose Animation") << "suspended, still the owner";

    AnimationClipController& c = controller();
    ASSERT_TRUE(c.bindRoot(s.id, model, &error)) << error;
    c.seek(0.0f);
    ASSERT_TRUE(c.isPreviewing());
    EXPECT_EQ(session.mode(), PoseEditSession::Mode::Idle) << "the clip preview put the pose to Idle";
    session.update(0.016f);
    EXPECT_TRUE(c.isPreviewing()) << "the pose's next update does not take the model back";
    EXPECT_FALSE(session.bonesApplied());
    c.stopPreview();
    PoseEditSession::setActive(nullptr);
    session.unbind();
}

// A deferred stop while the scene is saved, a scene tab switch and a deleted root (I2)
TEST_F(AnimationClipControllerTest, StopsDeferredBySavingOnSceneSwitchAndOnADeletedRoot) {
    ClipScene other = clipScene();
    ASSERT_TRUE(project()->saveSceneToPath(other.id, "OtherClips.scene"));
    ClipScene s = clipScene();
    ASSERT_NE(s.id, other.id);
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;

    // a stop while a thread saves the scene waits for the save
    c.seek(1.0f);
    s.sceneProject->playState = ScenePlayState::SAVING;
    c.stopPreview();
    EXPECT_FALSE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(1, 2, 0), "nothing written under the save");
    c.update(0.0);
    expectNear(s.transform(s.arm).position, Vector3(1, 2, 0), "still saving");
    s.sceneProject->playState = ScenePlayState::STOPPED;
    c.update(0.0);
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "restored once saved");

    // another scene tab: the preview ends (also without update: the next write stops it)
    c.seek(1.0f);
    ASSERT_TRUE(c.isPreviewing());
    project()->setSelectedSceneId(other.id);
    c.seek(0.5f);
    EXPECT_FALSE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "restored on the switch");
    EXPECT_FALSE(c.startPreview(&error)) << "only in the selected scene";
    project()->setSelectedSceneId(s.id);

    // the previewed root destroyed without a command: the next update ends the preview
    c.seek(1.0f);
    ASSERT_TRUE(c.isPreviewing());
    s.scene->destroyEntity(s.hand);
    s.scene->destroyEntity(s.arm);
    s.scene->destroyEntity(s.hero);
    c.update(0.0);
    EXPECT_FALSE(c.isPreviewing());
    EXPECT_EQ(c.getRoot(), NULL_ENTITY);
}

// Sub-clips are read-only (I2)
TEST_F(AnimationClipControllerTest, AModelsAnimationIsReadOnly) {
    fs::create_directories(assets() / "models");
    fs::copy_file(fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "pose-animation" / "assets" /
                  "models" / "hero_animated.gltf", assets() / "models/hero_animated.gltf", fs::copy_options::overwrite_existing);
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    ASSERT_TRUE(c.openClip("assets/models/hero_animated.gltf#Walk", &error)) << error;
    EXPECT_TRUE(c.isReadOnly());
    const size_t steps = project()->getProjectCommandHistory()->position();
    EXPECT_FALSE(c.keyAll(0.0f, &error));
    EXPECT_NE(error.find("Extract to .anim"), std::string::npos) << error;
    EXPECT_FALSE(c.addProperty("Arm", ClipChannel::Rotation, nullptr, &error));
    EXPECT_FALSE(c.commit(c.getDocument()->data(), "Edit", "", &error));
    EXPECT_NE(error.find("read-only"), std::string::npos) << error;
    c.setRecording(true);
    EXPECT_FALSE(c.isRecording()) << "nothing to record into";
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), steps);
}

// Blend shapes and lattices: keyed, recorded, previewed and restored (I2)
TEST_F(AnimationClipControllerTest, BlendShapeAndLatticeChannels) {
    ClipScene s = clipScene();
    const Entity face = addObject(s, "Face", s.hero, Vector3(0, 0, 0));
    s.scene->addComponent<MeshComponent>(face, {});
    const Entity cage = addObject(s, "Cage", s.hero, Vector3(0, 0, 0));
    s.scene->addComponent<LatticeComponent>(cage, {});
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;

    // Add Property keys the current weights, as wide as they need (here: one zero weight)
    c.setTime(0.0f);
    size_t morph = 0;
    ASSERT_TRUE(c.addProperty("Face", ClipChannel::Morph, &morph, &error)) << error;
    EXPECT_EQ(c.getDocument()->data().tracks[morph].keys[0].value, std::vector<float>{0.0f});
    // a key at 1 s with the second weight up
    s.scene->getComponent<MeshComponent>(face).morphWeights[1] = 0.5f;
    std::vector<AnimationClipController::KeyRef> keyed;
    morph = (size_t)c.getDocument()->data().findTrack("Face", ClipChannel::Morph);
    ASSERT_TRUE(c.keyTracks({morph}, 1.0f, "", &keyed, &error)) << error;
    ASSERT_EQ(keyed.size(), 1u);
    EXPECT_EQ(c.getDocument()->data().tracks[keyed[0].track].keys[keyed[0].key].value, (std::vector<float>{0.0f, 0.5f}));
    s.scene->getComponent<MeshComponent>(face).morphWeights[1] = 0.0f;
    c.seek(0.5f);
    EXPECT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[1], 0.25f, 1e-5f) << "previewed weight";

    // Record a lattice point, then another: one track on the union of the points
    c.setRecording(true);
    ASSERT_TRUE(c.isRecording());
    ClipRecordValue point;
    point.channel = ClipChannel::Lattice;
    point.points = {4};
    point.value = {0, 1, 0};
    ASSERT_TRUE(ClipRecordHooks::record(s.id, cage, point, ""));
    point.points = {2};
    point.value = {1, 0, 0};
    ASSERT_TRUE(ClipRecordHooks::record(s.id, cage, point, ""));
    const int lattice = c.getDocument()->data().findTrack("Cage", ClipChannel::Lattice);
    ASSERT_GE(lattice, 0);
    const ClipTrackData& track = c.getDocument()->data().tracks[(size_t)lattice];
    EXPECT_EQ(track.points, (std::vector<int>{2, 4}));
    ASSERT_EQ(track.keys.size(), 1u);
    EXPECT_EQ(track.keys[0].value, (std::vector<float>{1, 0, 0, 0, 1, 0}));
    expectNear(s.scene->getComponent<LatticeComponent>(cage).offsets[4], Vector3(0, 1, 0), "the live point");

    c.stopPreview();
    EXPECT_FLOAT_EQ(s.scene->getComponent<MeshComponent>(face).morphWeights[1], 0.0f);
    expectNear(s.scene->getComponent<LatticeComponent>(cage).offsets[4], Vector3(0, 0, 0), "the cage at rest");
    expectNear(s.scene->getComponent<LatticeComponent>(cage).offsets[2], Vector3(0, 0, 0), "the cage at rest");
}

// Relative tracks key offsets; Shift+K keys only what differs; Record of an entity the preview
// never wrote; the inline (pink) value; a drag collapsing two keys (I2, M1, M3, M10)
TEST_F(AnimationClipControllerTest, RelativeKeysModifiedKeysPendingValuesAndCollapsedDrags) {
    ClipData relative = armClip();
    relative.tracks[0].relative = true;
    relative.tracks[0].keys = {key(0.0f, {0, 0, 0}), key(1.0f, {0, 1, 0})};
    writeClip("anim/arm.anim", relative);
    AnimationClipDocument::checkDiskChanges(project());
    ClipScene s = clipScene();
    const Entity tail = addObject(s, "Tail", s.hero, Vector3(0, -1, 0));
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    ASSERT_TRUE(c.getDocument()->data().tracks[0].relative);
    c.seek(1.0f);
    expectNear(s.transform(s.arm).position, Vector3(5, 6, 5), "rest + offset");

    // Shift+K with the Arm moved: only the Arm's track, keyed as an offset from the rest
    s.transform(s.arm).position = Vector3(5, 8, 5);
    std::vector<AnimationClipController::KeyRef> keyed;
    ASSERT_TRUE(c.keyModified(1.0f, &keyed, &error)) << error;
    ASSERT_EQ(keyed.size(), 1u);
    const ClipTrackData& arm = c.getDocument()->data().tracks[keyed[0].track];
    EXPECT_EQ(arm.path, "Arm");
    const std::vector<float>& offset = arm.keys[keyed[0].key].value;
    ASSERT_EQ(offset.size(), 3u);
    EXPECT_NEAR(offset[1], 3.0f, 1e-5f);
    EXPECT_FALSE(c.keyModified(1.0f, &keyed, &error)) << "nothing differs now";

    // the inline value: shown until the playhead moves, keyed by K
    const size_t hand = (size_t)c.getDocument()->data().findTrack("Arm/Hand", ClipChannel::Scale);
    ASSERT_FALSE(c.setPendingValue(hand, {1, 2}, &error)) << "three numbers";
    ASSERT_TRUE(c.setPendingValue(hand, {3, 3, 3}, &error)) << error;
    EXPECT_TRUE(c.hasPendingValue(hand));
    c.update(0.0);
    expectNear(s.transform(s.hand).scale, Vector3(3, 3, 3), "pending over the clip");
    ASSERT_TRUE(c.keyTracks({hand}, 1.0f, "", &keyed, &error)) << error;
    EXPECT_FALSE(c.hasPendingValue(hand));
    EXPECT_EQ(c.getDocument()->data().tracks[keyed[0].track].keys[keyed[0].key].value, (std::vector<float>{3, 3, 3}));
    ASSERT_TRUE(c.setPendingValue(hand, {4, 4, 4}, &error)) << error;
    c.seek(0.0f);
    EXPECT_FALSE(c.hasPendingValue(hand)) << "the playhead moved";
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "the clip at 0");

    // Record of an entity no track had: its rest is the value before the live write
    c.setRecording(true);
    ClipRecordValue value;
    value.channel = ClipChannel::Rotation;
    value.value = {0, 0.7071068f, 0, 0.7071068f};
    ASSERT_TRUE(ClipRecordHooks::record(s.id, tail, value, ""));
    EXPECT_GE(c.getDocument()->data().findTrack("Tail", ClipChannel::Rotation), 0);

    // two keys of one track dragged onto one time: one key, one ref, a warning
    const int armTrack = c.getDocument()->data().findTrack("Arm", ClipChannel::Position);
    ASSERT_GE(armTrack, 0);
    ASSERT_EQ(c.getDocument()->data().tracks[(size_t)armTrack].keys.size(), 2u);
    ASSERT_TRUE(c.beginKeyDrag({{(size_t)armTrack, 0}}, &error)) << error;
    std::vector<AnimationClipController::KeyRef> moved;
    ASSERT_TRUE(c.updateKeyDrag(1.0f, nullptr, &moved, &error)) << error;
    c.endKeyDrag();
    EXPECT_EQ(c.getDocument()->data().tracks[(size_t)c.getDocument()->data().findTrack("Arm", ClipChannel::Position)].keys.size(), 1u);
    EXPECT_EQ(moved.size(), 1u);
    EXPECT_FALSE(c.getLastWarnings().empty()) << "the collapse is reported";

    // a vertical drag of a rotation key keeps it a unit quaternion
    const int tailTrack = c.getDocument()->data().findTrack("Tail", ClipChannel::Rotation);
    ASSERT_TRUE(c.beginKeyDrag({{(size_t)tailTrack, 0}}, &error)) << error;
    const std::vector<float> lift = {0.5f, 0.0f, 0.0f, 0.0f};
    ASSERT_TRUE(c.updateKeyDrag(0.0f, &lift, &moved, &error)) << error;
    c.endKeyDrag();
    const std::vector<float>& q = c.getDocument()->data().tracks[(size_t)c.getDocument()->data().findTrack("Tail", ClipChannel::Rotation)].keys[0].value;
    EXPECT_NEAR(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0f, 1e-4f);

    c.stopPreview();
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "the Arm at its rest (the direct write was no scene edit)");
    EXPECT_TRUE(s.transform(tail).rotation == Quaternion()) << "the recorded Tail back at its rest";
}

// ---- review round 2: redo routed by the newest redoable step (R5b) -------------------------

namespace {
    // the App's routing (App::undoRedo): one step of S24's timeline of the selected scene, which
    // takes the newer of the scene's and the project's top steps (a clip commit is a project step)
    // and redoes in the reverse order of the undos; the history that took it
    struct Routing {
        Project* project;
        uint32_t sceneId;

        std::string undo() {
            std::vector<UndoTimeline::Step> steps;
            if (!UndoTimeline::undo(sceneId, nullptr, &steps) || steps.empty()) return "nothing";
            return steps.front().project ? "project" : "scene";
        }
        std::string redo() {
            std::vector<UndoTimeline::Step> steps;
            if (!UndoTimeline::redo(sceneId, nullptr, &steps) || steps.empty()) return "nothing";
            return steps.front().project ? "project" : "scene";
        }
    };
}

// Final review M3: while a Physics Placement gesture runs or settles in the selected scene, Ctrl+Z
// cancels the gesture even when a clip commit is the newest step, and redo waits: the timeline asks
// CommandHistory::beforeChange first (PhysicsPlacementWindow::beforeHistoryChange). The App keeps the
// Animation window's key focus off while the gesture runs (clipUndoGoesToProject).
TEST_F(AnimationClipControllerTest, APlacementGestureKeepsUndoOnTheScene) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    AnimationClipController::KeyRef end;
    ASSERT_TRUE(AnimationClipController::findKey(c.getDocument()->data(), "Arm", ClipChannel::Position, 1.0f, end));
    ASSERT_TRUE(c.setKeyValue(end, {0, 4, 0}, "", &error)) << error;
    ASSERT_TRUE(c.newestStepIsClipEdit());
    EXPECT_FALSE(clipUndoGoesToProject(true)) << "the gesture's undo comes first";
    EXPECT_TRUE(clipUndoGoesToProject(false));
    Routing routing{project(), s.id};

    // a gesture running: undo cancels it (the clip commit stays), redo waits
    int cancels = 0;
    const auto before = CommandHistory::beforeChange;
    CommandHistory::beforeChange = [&](size_t, CommandHistory::Change change){
        if (change == CommandHistory::Change::Undo) cancels++;
        return change == CommandHistory::Change::Add;
    };
    EXPECT_EQ(routing.undo(), "nothing");
    EXPECT_EQ(cancels, 1);
    EXPECT_TRUE(c.newestStepIsClipEdit()) << "the clip commit is not undone while the gesture runs";
    CommandHistory::beforeChange = before;

    EXPECT_EQ(routing.undo(), "project");
    ASSERT_TRUE(c.newestRedoIsClipEdit());
    CommandHistory::beforeChange = [](size_t, CommandHistory::Change change){ return change != CommandHistory::Change::Redo; };
    EXPECT_EQ(routing.redo(), "nothing") << "redo waits for the gesture";
    EXPECT_TRUE(c.newestRedoIsClipEdit());
    CommandHistory::beforeChange = before;
    EXPECT_EQ(routing.redo(), "project");
}

TEST_F(AnimationClipControllerTest, RedoGoesWhereTheStepUndoneLastWas) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* scene = CommandHandle::get(s.id);
    Routing routing{project(), s.id};
    auto move = [&](const Vector3& to){
        // one step per move (no merge with the move before)
        scene->addCommandNoMerge(new PropertyCmd<Vector3>(project(), s.id, s.prop, ComponentType::Transform, "position", to));
    };
    auto armKeyAtOne = [&](){
        AnimationClipController::KeyRef ref;
        return AnimationClipController::findKey(c.getDocument()->data(), "Arm", ClipChannel::Position, 1.0f, ref)
            ? c.getDocument()->data().tracks[ref.track].keys[ref.key].value : std::vector<float>();
    };
    AnimationClipController::KeyRef end;
    ASSERT_TRUE(AnimationClipController::findKey(c.getDocument()->data(), "Arm", ClipChannel::Position, 1.0f, end));

    // a clip commit undone, then redone: back to it
    move(Vector3(1, 0, 0));
    ASSERT_TRUE(c.setKeyValue(end, {0, 9, 0}, "", &error)) << error;
    EXPECT_EQ(routing.undo(), "project");
    EXPECT_EQ(armKeyAtOne(), (std::vector<float>{1, 2, 0}));
    EXPECT_TRUE(c.newestRedoIsClipEdit());
    EXPECT_FALSE(c.newestStepIsClipEdit()) << "the next undo is the scene's";
    EXPECT_EQ(routing.redo(), "project") << "redo returns to the clip commit";
    EXPECT_EQ(armKeyAtOne(), (std::vector<float>{0, 9, 0}));
    EXPECT_FALSE(c.newestRedoIsClipEdit());

    // a scene step undone, then redone: stays in the scene
    move(Vector3(2, 0, 0));
    EXPECT_EQ(routing.undo(), "scene");
    expectNear(s.transform(s.prop).position, Vector3(1, 0, 0), "the move undone");
    EXPECT_FALSE(c.newestRedoIsClipEdit());
    EXPECT_EQ(routing.redo(), "scene");
    expectNear(s.transform(s.prop).position, Vector3(2, 0, 0), "the move redone");

    // interleaved: undone newest first, redone in reverse order of the undos
    EXPECT_EQ(routing.undo(), "scene");          // the move to 2
    EXPECT_EQ(routing.undo(), "project");        // the key (0, 9, 0)
    EXPECT_EQ(routing.undo(), "scene");          // the move to 1
    EXPECT_EQ(routing.redo(), "scene");          // undone last: the move to 1
    EXPECT_EQ(routing.redo(), "project");        // then the key
    EXPECT_EQ(armKeyAtOne(), (std::vector<float>{0, 9, 0}));
    EXPECT_EQ(routing.redo(), "scene");          // then the move to 2
    expectNear(s.transform(s.prop).position, Vector3(2, 0, 0), "all redone");
    EXPECT_FALSE(c.newestRedoIsClipEdit());

    // two clip commits undone around a scene undo
    ASSERT_TRUE(c.setKeyValue(end, {0, 7, 0}, "", &error)) << error;
    move(Vector3(3, 0, 0));
    ASSERT_TRUE(c.setKeyValue(end, {0, 5, 0}, "", &error)) << error;
    EXPECT_EQ(routing.undo(), "project");        // (0, 5, 0)
    EXPECT_EQ(routing.undo(), "scene");          // the move to 3
    EXPECT_EQ(routing.undo(), "project");        // (0, 7, 0)
    EXPECT_EQ(armKeyAtOne(), (std::vector<float>{0, 9, 0}));
    EXPECT_EQ(routing.redo(), "project");        // (0, 7, 0)
    EXPECT_EQ(routing.redo(), "scene");          // the move to 3
    EXPECT_EQ(routing.redo(), "project");        // (0, 5, 0)
    EXPECT_EQ(armKeyAtOne(), (std::vector<float>{0, 5, 0}));
    expectNear(s.transform(s.prop).position, Vector3(3, 0, 0), "redone");

    // a clip commit undone, then a new scene edit: that edit is the newest action, Ctrl+Y does
    // not jump back to the clip, and Ctrl+Z takes the new edit first
    EXPECT_EQ(routing.undo(), "project");        // (0, 5, 0)
    move(Vector3(4, 0, 0));
    EXPECT_FALSE(c.newestRedoIsClipEdit());
    EXPECT_EQ(routing.redo(), "nothing") << "the timeline (S24): the new edit ended the redo";
    EXPECT_EQ(armKeyAtOne(), (std::vector<float>{0, 7, 0}));
    EXPECT_EQ(routing.undo(), "scene");
    expectNear(s.transform(s.prop).position, Vector3(3, 0, 0), "the move to 4 undone");
    EXPECT_EQ(routing.redo(), "scene");          // undone last
    expectNear(s.transform(s.prop).position, Vector3(4, 0, 0), "the move to 4 redone");
}

// ---- final editor review fixes (FX2) --------------------------------------------------------

namespace {

    Entity createQuiet(ClipScene& s, const std::string& name, EntityCreationType type, Entity parent) {
        CreateEntityCmd* cmd = new CreateEntityCmd(project(), s.id, name, type, parent);
        cmd->setQuiet(true);
        CommandHandle::get(s.id)->addCommandNoMerge(cmd);
        return cmd->getEntity();
    }

}

// I1: an action that changes a scene stops the previews of what it changes before it reads
// anything: a converted hold key and the scene keep the rest, never the previewed value
TEST_F(AnimationClipControllerTest, SceneChangingClipActionsStopThePreviewFirst) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* history = CommandHandle::get(s.id);

    // an entity Animation whose Arm track starts at 0.5 s: Convert holds the Arm's value before it
    const Entity animation = createQuiet(s, "Wave", EntityCreationType::ANIMATION, s.hero);
    const Entity track = createQuiet(s, "Arm position", EntityCreationType::TRANSLATE_TRACKS, s.arm);
    s.scene->getComponent<KeyframeTracksComponent>(track).times = {0.0f, 1.0f};
    s.scene->getComponent<TranslateTracksComponent>(track).values = {Vector3(9, 9, 9), Vector3(9, 10, 9)};
    s.scene->getComponent<AnimationComponent>(animation).actions = {{0.5f, 0.0f, track, 0}};
    const size_t steps = history->position();

    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(1, 1, 0), "previewed");
    ClipActionResult result;
    ASSERT_TRUE(ClipSceneActions::convertToClip(project(), s.id, animation, s.hero, "assets/anim/converted.anim", false, result, error)) << error;
    EXPECT_FALSE(c.isPreviewing()) << "Convert stopped the preview before reading the scene";
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "the Arm at rest");
    const anim::ClipParseResult parsed = anim::parseClip(readAll(assets() / "anim" / "converted.anim"));
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    const int armTrack = parsed.data->findTrack("Arm", ClipChannel::Position);
    ASSERT_GE(armTrack, 0);
    const ClipTrackData& converted = parsed.data->tracks[(size_t)armTrack];
    ASSERT_FALSE(converted.keys.empty());
    EXPECT_FLOAT_EQ(converted.keys[0].time, 0.0f);
    EXPECT_EQ(converted.keys[0].value, (std::vector<float>{5, 5, 5})) << "the hold key is the rest, not the previewed value";
    ASSERT_TRUE(history->undo());
    ASSERT_EQ(history->position(), steps);

    // Create New Clip on a previewed child, and a drop on one: stopped first, the rest stays
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    ASSERT_TRUE(ClipSceneActions::createNewClip(project(), s.id, s.arm, "assets/anim/arm_new.anim", result, error)) << error;
    EXPECT_FALSE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "created on the rest");
    ASSERT_TRUE(history->undo());
    c.seek(0.5f);
    ASSERT_TRUE(ClipSceneActions::addClipsToPlayer(project(), s.id, s.hand, {"assets/anim/arm.anim"}, result, error)) << error;
    EXPECT_FALSE(c.isPreviewing());
    expectNear(s.transform(s.hand).scale, Vector3(1, 1, 1), "the Hand at rest");
    ASSERT_TRUE(history->undo());
    // elsewhere in the scene: the preview goes on
    c.seek(0.5f);
    ASSERT_TRUE(ClipSceneActions::createNewClip(project(), s.id, s.prop, "assets/anim/prop.anim", result, error)) << error;
    EXPECT_TRUE(c.isPreviewing());
    c.stopPreview();
    ASSERT_TRUE(history->undo());
}

// I8: a Hierarchy move during the preview keeps the object where its rest puts it (from the
// rest, not the previewed pose), and its undo too
TEST_F(AnimationClipControllerTest, AReparentDuringThePreviewMovesTheRest) {
    ClipScene s = clipScene();
    transformFrame(s);
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* history = CommandHandle::get(s.id);
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(1, 1, 0), "previewed");

    ASSERT_TRUE(history->addCommand(new MoveEntityOrderCmd(project(), s.id, s.arm, s.prop, InsertionType::INTO)));
    EXPECT_FALSE(c.isPreviewing()) << "the move stopped the preview first";
    EXPECT_EQ(s.transform(s.arm).parent, s.prop);
    // the Prop is at (3, 0, 0): the Arm's rest world position (5, 5, 5) is kept
    expectNear(s.transform(s.arm).position, Vector3(2, 5, 5), "local under the Prop, from the rest");
    c.stopPreview();
    expectNear(s.transform(s.arm).position, Vector3(2, 5, 5), "nothing written back by a stop");

    // the undo during a preview of the new parent's hierarchy: from the rest again
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    transformFrame(s);
    c.seek(0.5f);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(s.transform(s.arm).parent, s.hero);
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "back under the Hero at rest");
    c.stopPreview();
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "still at rest");
}

// Merge of main (pose suspendActiveForMove, a prop held by a posed hand) with 9.R7: a GameObject
// the clip does not animate, put under a previewed bone during the preview (so not in its
// snapshot, which restores the root's subtree as it was when the preview started), keeps its rest
// world when it moves out: MoveEntityOrderCmd::prepareReparent stops the preview, then recomputes
// the moved GameObject's chain from the restored rest before the move reads its world. A reorder
// among the same siblings keeps the preview.
TEST_F(AnimationClipControllerTest, AHeldPropMovedOutDuringThePreviewKeepsItsRestWorld) {
    ClipScene s = clipScene();
    transformFrame(s);
    RenderSystem* render = s.scene->getSystem<RenderSystem>().get();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* history = CommandHandle::get(s.id);
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    // a frame ran: Arm at (1, 1, 0), Hand scaled 1.5; Held added under the Hand now, its world from
    // the preview
    transformFrame(s);
    const Entity held = addObject(s, "Held", s.hand, Vector3(0, 0, 1));
    render->updateTransform(s.transform(held), held);
    expectNear(s.transform(held).worldPosition, Vector3(1, 2, 1.5f), "previewed world");

    // out of the Hand, next to the Prop (the scene root): its world is kept, the rest's: Hero (0) >
    // Arm (5, 5, 5) > Hand (0, 1, 0) > Held (0, 0, 1)
    ASSERT_TRUE(history->addCommand(new MoveEntityOrderCmd(project(), s.id, held, s.prop, InsertionType::AFTER)));
    EXPECT_FALSE(c.isPreviewing()) << "the move stopped the preview first";
    EXPECT_EQ(s.transform(held).parent, NULL_ENTITY);
    expectNear(s.transform(held).position, Vector3(5, 6, 6), "the rest world as its new local");
    expectNear(s.transform(held).scale, Vector3(1, 1, 1), "not the previewed scale");

    // back under the previewed Hand by the undo: from the rest again
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    transformFrame(s);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(s.transform(held).parent, s.hand);
    expectNear(s.transform(held).position, Vector3(0, 0, 1), "its local under the Hand");
    expectNear(s.transform(held).scale, Vector3(1, 1, 1), "unscaled");
    c.stopPreview();

    // a reorder among the same siblings keeps the preview (nothing is recomputed)
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    const Entity other = addObject(s, "Other", s.hand, Vector3(0, 0, -1));
    ASSERT_TRUE(history->addCommand(new MoveEntityOrderCmd(project(), s.id, other, held, InsertionType::BEFORE)));
    EXPECT_TRUE(c.isPreviewing()) << "a reorder under the Hand";
    c.stopPreview();
}

// I7: a prefab override's Revert (and its undo) while the instance is previewed: the preview
// restores first, so its stop never writes the old value back over the reverted one
TEST_F(AnimationClipControllerTest, PrefabRevertAndItsUndoDuringThePreviewKeepTheirValues) {
    ClipScene s = clipScene();
    CommandHistory* history = CommandHandle::get(s.id);
    fs::create_directories(assets() / "prefabs");
    std::error_code ec;
    fs::remove(assets() / "prefabs" / "ArmHero.prefab", ec);
    history->addCommandNoMerge(new CreatePrefabCmd(project(), s.id, fs::path("assets") / "prefabs" / "ArmHero.prefab",
                                                   Stream::encodeEntity(s.hero, s.scene, project(), s.sceneProject)));
    ASSERT_TRUE(project()->isPrefabInstanceRoot(s.id, s.hero));
    // an override of the Arm's position (6, 6, 6) over the prefab's (5, 5, 5)
    ASSERT_TRUE(history->addCommand(new PropertyCmd<Vector3>(project(), s.id, s.arm, ComponentType::Transform, "position", Vector3(6, 6, 6))));
    ASSERT_TRUE(project()->getOverriddenProperties(s.id, s.arm, ComponentType::Transform).count("position"));

    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    ASSERT_TRUE(history->addCommand(new PrefabOverrideCmd(project(), s.id, s.arm, PrefabOverrideCmd::Operation::RevertProperty,
                                                          ComponentType::Transform, {"position"})));
    EXPECT_FALSE(c.isPreviewing()) << "the revert stopped the preview first";
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "reverted to the prefab's value");
    EXPECT_FALSE(project()->getOverriddenProperties(s.id, s.arm, ComponentType::Transform).count("position"));
    c.stopPreview();
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "a stop writes nothing back");

    // its undo while previewing: the override value comes back and stays after the stop
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(c.isPreviewing()) << "the undo stopped the preview first";
    expectNear(s.transform(s.arm).position, Vector3(6, 6, 6), "the override is back");
    EXPECT_TRUE(project()->getOverriddenProperties(s.id, s.arm, ComponentType::Transform).count("position"));
    c.stopPreview();
    expectNear(s.transform(s.arm).position, Vector3(6, 6, 6), "and stays");
}

// I3 (and Minor 6): the Inspector's document of a clip reference is the file the reference names:
// after a rename the old path names a new file (Create again), not the renamed one; the controller
// reports where its open clip is now
TEST_F(AnimationClipControllerTest, TheInspectorDocumentCacheFollowsTheFileAfterARename) {
    writeClip("anim/New Animation.anim", armClip());
    AnimationClipDocument::checkDiskChanges(project());
    inspectorclips::ClipDocumentCache cache;
    std::string error;
    const std::string original = "assets/anim/New Animation.anim";
    std::shared_ptr<AnimationClipDocument> first = cache.find(project(), original, 0.0, 1.0, &error);
    ASSERT_TRUE(first) << error;
    AnimationClipController& c = controller();
    ASSERT_TRUE(c.openClip(original, &error)) << error;

    // the inline rename (RenameFileCmd: AssetOps::move)
    const fs::path from = assets() / "anim" / "New Animation.anim";
    const fs::path to = assets() / "anim" / "Walk.anim";
    ASSERT_TRUE(AssetOps::move(*project(), from, to).ok);
    EXPECT_EQ(first->path(), "assets/anim/Walk.anim") << "the document followed its file";
    EXPECT_EQ(c.getClipPath(), "assets/anim/Walk.anim");

    // Create again: a new file at the old path
    ClipData other = armClip();
    other.frameRate = 12.0f;
    writeClip("anim/New Animation.anim", other);
    std::shared_ptr<AnimationClipDocument> second = cache.find(project(), original, 0.5, 1.0, &error);
    ASSERT_TRUE(second) << error;
    EXPECT_NE(second.get(), first.get()) << "not the renamed clip's document";
    EXPECT_EQ(second->path(), original);
    EXPECT_FLOAT_EQ(second->data().frameRate, 12.0f);
    EXPECT_EQ(cache.find(project(), "assets/anim/Walk.anim", 0.5, 1.0, &error).get(), first.get());
    c.closeClip();
}

// I6 / R5c: a clip commit the editor UI makes outside the controller (the Inspector's .anim asset
// section, Extract or Create file writes) routes Ctrl+Z to the project history, with or without a
// controller; an unmarked commit (MCP) does not
TEST_F(AnimationClipControllerTest, MarkedClipCommitsRouteUndoWithoutAController) {
    ClipScene s = clipScene();
    AnimationClipController::release(project());
    CommandHistory* history = CommandHandle::get(s.id);
    ASSERT_TRUE(history->addCommand(new PropertyCmd<Vector3>(project(), s.id, s.prop, ComponentType::Transform, "position", Vector3(1, 1, 1))));
    std::string error;
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project(), "assets/anim/arm.anim", &error);
    ASSERT_TRUE(document) << error;

    // an MCP edit: not marked
    ClipData data = document->data();
    data.frameRate = 24.0f;
    ASSERT_TRUE(document->commit(data, "Frame Rate", "", &error)) << error;
    EXPECT_FALSE(animationClipAppHooks().ownsUndo());
    // the Inspector's: marked
    data.frameRate = 25.0f;
    const uint64_t clock = AnimationClipController::clipCommitClock(project());
    ASSERT_TRUE(document->commit(data, "Frame Rate", "", &error)) << error;
    AnimationClipController::markClipCommit(clock);
    EXPECT_TRUE(animationClipAppHooks().ownsUndo()) << "Ctrl+Z undoes the Inspector's clip edit";
    // a newer scene step is undone first
    ASSERT_TRUE(history->addCommand(new PropertyCmd<Vector3>(project(), s.id, s.prop, ComponentType::Transform, "position", Vector3(2, 2, 2))));
    EXPECT_FALSE(animationClipAppHooks().ownsUndo());
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(animationClipAppHooks().ownsUndo());
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FLOAT_EQ(document->data().frameRate, 24.0f);
    EXPECT_FALSE(animationClipAppHooks().ownsUndo());
}

// I5: a scene undo giving an entry back its old path hint (the clip was renamed since) points it
// at the clip by GUID again, so the player finds it without a reopen
TEST_F(AnimationClipControllerTest, SceneUndoReResolvesStaleClipPathsByGuid) {
    ClipScene s = clipScene();
    controller();   // (the history timeline, which the App installs at startup)
    std::string error;
    ClipData data = armClip();
    data.guid = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
    writeClip("anim/run.anim", data);
    project()->refreshAssetDatabase(true);
    AnimationClipDocument::checkDiskChanges(project());
    CommandHistory* history = CommandHandle::get(s.id);
    AnimationPlayerClip entry;
    entry.clip = "anim/run.anim";
    entry.guid = data.guid;
    std::vector<AnimationPlayerClip> clips = s.scene->getComponent<AnimationPlayerComponent>(s.hero).clips;
    clips.push_back(entry);
    history->addCommandNoMerge(new PropertyCmd<std::vector<AnimationPlayerClip>>(project(), s.id, s.hero,
                                                                                      ComponentType::AnimationPlayerComponent, "clips", clips));
    // renamed (the Project window's rename): the entry follows
    const fs::path from = assets() / "anim" / "run.anim";
    const fs::path to = assets() / "anim" / "sprint.anim";
    ASSERT_TRUE(AssetOps::move(*project(), from, to).ok);
    ASSERT_EQ(s.scene->getComponent<AnimationPlayerComponent>(s.hero).clips.back().clip, "anim/sprint.anim");

    // a scene step removing it, undone: the recorded value has the old hint, re-resolved
    std::vector<AnimationPlayerClip> fewer = s.scene->getComponent<AnimationPlayerComponent>(s.hero).clips;
    fewer.pop_back();
    history->addCommandNoMerge(new PropertyCmd<std::vector<AnimationPlayerClip>>(project(), s.id, s.hero,
                                                                                      ComponentType::AnimationPlayerComponent, "clips", fewer));
    ASSERT_TRUE(history->undo());
    ASSERT_TRUE(history->undo());
    ASSERT_TRUE(history->redo());
    const AnimationPlayerClip& back = s.scene->getComponent<AnimationPlayerComponent>(s.hero).clips.back();
    EXPECT_EQ(back.guid, data.guid);
    EXPECT_EQ(back.clip, "anim/sprint.anim") << "the redo's stale hint follows the GUID";
}

// Minors 10 / 11: a lattice resolution change and a Remove Component of a previewed mesh restore
// the preview first: the resampled offsets and the removed (then restored) weights are the rest
TEST_F(AnimationClipControllerTest, ResolutionChangesAndComponentRemovalsTakeTheRest) {
    ClipScene s = clipScene();
    const Entity face = addObject(s, "Face", s.hero, Vector3(0, 0, 0));
    s.scene->addComponent<MeshComponent>(face, {});
    const Entity cage = addObject(s, "Cage", s.hero, Vector3(0, 0, 0));
    s.scene->addComponent<LatticeComponent>(cage, {});
    s.scene->getComponent<LatticeComponent>(cage).offsets.assign(27, Vector3(0, 0, 0));
    ClipData data;
    ClipTrackData weights;
    weights.path = "Face";
    weights.channel = ClipChannel::Morph;
    weights.keys = {key(0.0f, {0}), key(1.0f, {1})};
    ClipTrackData points;
    points.path = "Cage";
    points.channel = ClipChannel::Lattice;
    points.points = {0};
    points.keys = {key(0.0f, {0, 0, 0}), key(1.0f, {0, 1, 0})};
    data.tracks = {weights, points};
    writeClip("anim/arm.anim", data);
    AnimationClipDocument::checkDiskChanges(project());
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* history = CommandHandle::get(s.id);

    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    expectNear(s.scene->getComponent<LatticeComponent>(cage).offsets[0], Vector3(0, 0.5f, 0), "previewed point");
    ASSERT_TRUE(LatticeEditorOps::setResolution(project(), s.id, cage, 4, 3, 3, &error)) << error;
    EXPECT_FALSE(c.isPreviewing()) << "the resolution change stopped the preview first";
    const LatticeComponent& resampled = s.scene->getComponent<LatticeComponent>(cage);
    ASSERT_EQ(resampled.offsets.size(), 36u);
    for (const Vector3& offset : resampled.offsets) expectNear(offset, Vector3(0, 0, 0), "resampled from the rest");
    ASSERT_TRUE(history->undo());

    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    EXPECT_NEAR(s.scene->getComponent<MeshComponent>(face).morphWeights[0], 0.5f, 1e-5f) << "previewed weight";
    ASSERT_TRUE(history->addCommand(new RemoveComponentCmd(project(), s.id, face, ComponentType::MeshComponent)));
    EXPECT_FALSE(c.isPreviewing()) << "the removal stopped the preview first";
    ASSERT_TRUE(history->undo());
    ASSERT_TRUE(s.scene->findComponent<MeshComponent>(face));
    EXPECT_FLOAT_EQ(s.scene->getComponent<MeshComponent>(face).morphWeights[0], 0.0f) << "the undo brings the rest back";
}

// ---- verification round fixes (FX5) ---------------------------------------------------------

// N2: a first model load on an entity whose own values a preview shows stops that preview first
// (its Transform is the load's oldTransform: the undo would write the previewed pose back); a
// first load of something new under the previewed root leaves the preview and Record running
TEST_F(AnimationClipControllerTest, AFirstModelLoadStopsOnlyAPreviewOfTheEntityItself) {
    fs::create_directories(assets() / "models");
    fs::copy_file(fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "pose-animation" / "assets" /
                  "models" / "hero_tpose.gltf", assets() / "models/hero.gltf", fs::copy_options::overwrite_existing);
    Engine::setAsyncLoading(false);
    ClipScene s = clipScene();
    const Entity gadget = addObject(s, "Gadget", s.hero, Vector3(0, 0, 2));
    for (Entity entity : {s.arm, gadget}){
        s.scene->addComponent<MeshComponent>(entity, {});
        s.scene->addComponent<ModelComponent>(entity, {});
    }
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* history = CommandHandle::get(s.id);

    // a model loaded on Gadget (nothing of it previewed): the preview and Record go on
    c.seek(0.5f);
    c.setRecording(true);
    ASSERT_TRUE(c.isPreviewing() && c.isRecording());
    ASSERT_TRUE(history->addCommand(new ModelLoadCmd(project(), s.id, gadget, "models/hero.gltf")));
    EXPECT_FALSE(s.scene->getComponent<ModelComponent>(gadget).loadedFilename.empty()) << "loaded";
    EXPECT_TRUE(c.isPreviewing()) << "a new model under the previewed root";
    EXPECT_TRUE(c.isRecording());
    c.setRecording(false);
    // (its undo deletes the model's nodes: DeleteEntityCmd restores the previews above them first)
    ASSERT_TRUE(history->undo());
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());

    // a model loaded on the previewed Arm: stopped first, the load keeps the Arm's rest
    expectNear(s.transform(s.arm).position, Vector3(1, 1, 0), "previewed");
    ASSERT_TRUE(history->addCommand(new ModelLoadCmd(project(), s.id, s.arm, "models/hero.gltf")));
    EXPECT_FALSE(c.isPreviewing()) << "the Arm's own values were previewed";
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "loaded at rest");
    // its undo during a new preview: the rest comes back, never the previewed pose
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(c.isPreviewing());
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "the undo writes the rest");
    c.stopPreview();
    expectNear(s.transform(s.arm).position, Vector3(5, 5, 5), "and keeps it");
}

// N5: steps that would stop a preview for nothing leave it (and Record) running: a reorder among
// the same siblings, a Create refused by its checks, an invalid or unchanged lattice resolution
TEST_F(AnimationClipControllerTest, RefusedOrNoOpStepsLeaveThePreviewAndRecordRunning) {
    ClipScene s = clipScene();
    const Entity leg = addObject(s, "Leg", s.hero, Vector3(0, -1, 0));
    const Entity cage = addObject(s, "Cage", s.hero, Vector3(0, 0, 0));
    s.scene->addComponent<LatticeComponent>(cage, {});
    s.scene->getComponent<LatticeComponent>(cage).offsets.assign(27, Vector3(0, 0, 0));
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    CommandHistory* history = CommandHandle::get(s.id);
    c.seek(0.5f);
    c.setRecording(true);
    ASSERT_TRUE(c.isPreviewing() && c.isRecording());

    ASSERT_TRUE(history->addCommand(new MoveEntityOrderCmd(project(), s.id, leg, s.arm, InsertionType::BEFORE)));
    EXPECT_TRUE(c.isRecording()) << "a reorder among the Hero's children";
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(c.isRecording()) << "and its undo";

    ClipActionResult result;
    EXPECT_FALSE(ClipSceneActions::createNewClip(project(), s.id, s.arm, "assets/anim/arm.anim", result, error)) << "it exists";
    EXPECT_TRUE(c.isRecording()) << "a refused Create";
    EXPECT_FALSE(LatticeEditorOps::setResolution(project(), s.id, cage, 1, 3, 3, &error));
    EXPECT_TRUE(c.isRecording()) << "an invalid resolution";
    EXPECT_TRUE(LatticeEditorOps::setResolution(project(), s.id, cage, 3, 3, 3, &error)) << error;
    EXPECT_TRUE(c.isRecording()) << "the same resolution";

    // a reparent still stops
    ASSERT_TRUE(history->addCommand(new MoveEntityOrderCmd(project(), s.id, leg, s.arm, InsertionType::INTO)));
    EXPECT_FALSE(c.isPreviewing());
    ASSERT_TRUE(history->undo());
}

// Integration review I1: a Hermite (CUBICSPLINE) rotation key edited with the same rotation of
// the other sign (an Euler entry) keeps its side: the segments around it sample as before, no
// flip; a key added (Record, K) takes the side of the key before it; a key without tangents
// in a track with some (linear tangents made for it) is aligned before they are made
TEST_F(AnimationClipControllerTest, HermiteRotationEditsKeepTheKeySideAndDoNotFlip) {
    ClipData data;
    ClipTrackData turn;
    turn.path = "Arm";
    turn.channel = ClipChannel::Rotation;
    auto hermite = [](float time, std::vector<float> value){
        ClipKey k = key(time, std::move(value));
        k.inTangent.assign(4, 0.0f);
        k.outTangent.assign(4, 0.0f);
        return k;
    };
    // a half turn past 0.5 s stored with w < 0, as exporters write it
    turn.keys = {hermite(0.0f, {0, 0, 0, 1}), hermite(1.0f, {0, -0.6f, 0, -0.8f}), hermite(2.0f, {0, -0.8f, 0, -0.6f})};
    data.tracks = {turn};
    writeClip("anim/arm.anim", data);
    AnimationClipDocument::checkDiskChanges(project());
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    auto rotationAt = [&](float time){
        c.seek(time);
        return s.transform(s.arm).rotation;
    };
    std::vector<Quaternion> before;
    for (float t : {0.25f, 0.5f, 0.75f, 1.25f, 1.5f, 1.75f}) before.push_back(rotationAt(t));

    // the same rotation as +q (what an Euler value gives): stored on the old key's side
    AnimationClipController::KeyRef middle{0, 1};
    ASSERT_TRUE(c.setKeyValue(middle, {0, 0.6f, 0, 0.8f}, "", &error)) << error;
    const std::vector<float>& stored = c.getDocument()->data().tracks[0].keys[1].value;
    ASSERT_EQ(stored.size(), 4u);
    EXPECT_NEAR(stored[1], -0.6f, 1e-5f) << "on the old value's side";
    EXPECT_NEAR(stored[3], -0.8f, 1e-5f);
    size_t i = 0;
    for (float t : {0.25f, 0.5f, 0.75f, 1.25f, 1.5f, 1.75f}){
        const Quaternion now = rotationAt(t);
        EXPECT_NEAR(std::fabs(now.dot(before[i])), 1.0f, 1e-5f) << "no flip at " << t;
        i++;
    }
    c.stopPreview();

    // a new key keyed from the scene's +q value at 1.5 s: on the previous key's side
    s.transform(s.arm).rotation = quaternionFromXYZW(std::vector<float>{0, 0.7071068f, 0, 0.7071068f}.data());
    std::vector<AnimationClipController::KeyRef> keyed;
    ASSERT_TRUE(c.keyTracks({0}, 1.5f, "", &keyed, &error)) << error;
    const std::vector<float>& added = c.getDocument()->data().tracks[0].keys[2].value;
    EXPECT_LT(added[3], 0.0f) << "w on the side of the key at 1 s";

    // normalize: a key without tangents among keys with them is aligned before it gets them
    ClipData mixed;
    ClipTrackData partial = turn;
    partial.keys[2] = key(2.0f, {0, 0.8f, 0, 0.6f});
    mixed.tracks = {partial};
    anim::normalize(mixed, nullptr);
    const std::vector<float>& completed = mixed.tracks[0].keys[2].value;
    ASSERT_EQ(completed.size(), 4u);
    EXPECT_NEAR(completed[1], -0.8f, 1e-5f) << "aligned with the key before it";
    EXPECT_NEAR(completed[3], -0.6f, 1e-5f);
    EXPECT_TRUE(mixed.tracks[0].keys[2].hasTangents());
    // (FX6) a first key without tangents takes the side of the key after it (stored tangents)
    ClipData leading;
    ClipTrackData front = turn;
    front.keys[0] = key(0.0f, {0, 0.6f, 0, 0.8f});     // the stored key 1 is [0,-0.6,0,-0.8]
    leading.tracks = {front};
    anim::normalize(leading, nullptr);
    EXPECT_NEAR(leading.tracks[0].keys[0].value[1], -0.6f, 1e-5f) << "key 0 aligned with key 1";
    EXPECT_NEAR(leading.tracks[0].keys[0].value[3], -0.8f, 1e-5f);

    // the helper the edit paths share
    std::vector<float> flipped = {0, 0.6f, 0, 0.8f};
    anim::alignQuaternionKey(flipped, {0, -0.6f, 0, -0.8f});
    EXPECT_EQ(flipped, (std::vector<float>{0, -0.6f, 0, -0.8f}));
    EXPECT_EQ(clipui::alignedRotationKeyValue(turn, 1.0f, {0, 0.6f, 0, 0.8f}), (std::vector<float>{0, -0.6f, 0, -0.8f}));
}

// ---- property tracks in the editor (anim-properties spec 15, editor tests) -----------------

namespace {

    bool compatibleType(PropertyKind kind, PropertyType type){
        switch (kind){
            case PropertyKind::Bool: return type == PropertyType::Bool;
            case PropertyKind::Int: return type == PropertyType::Int || type == PropertyType::UInt;
            case PropertyKind::Float: return type == PropertyType::Float || type == PropertyType::Double;
            case PropertyKind::Vector2: return type == PropertyType::Vector2;
            case PropertyKind::Vector3:
            case PropertyKind::Color3: return type == PropertyType::Vector3;
            case PropertyKind::Vector4:
            case PropertyKind::Color4: return type == PropertyType::Vector4;
            default: return false;
        }
    }

    // a UI hierarchy under the clip scene's Hero: Panel (Image) > Label (Text)
    Entity addImage(ClipScene& s, const std::string& name, Entity parent, const Vector4& srgb){
        const Entity entity = addObject(s, name, parent, Vector3(0, 0, 0));
        UIComponent ui;
        ui.color = Color::sRGBToLinear(srgb);
        s.scene->addComponent<UIComponent>(entity, ui);
        s.scene->addComponent<RectTransformComponent>(entity, {});
        s.scene->addComponent<ImageComponent>(entity, {});
        return entity;
    }

}

// decision 2: every registry row is a Catalog property of a compatible type, by the same name
TEST(PropertyRegistryCatalog, EveryRowIsACatalogPropertyOfACompatibleType){
    Scene scene;
    const Entity e = scene.createEntity();
    scene.addComponent<Transform>(e, {});
    scene.addComponent<UIComponent>(e, {});
    scene.addComponent<ButtonComponent>(e, {});
    scene.addComponent<RectTransformComponent>(e, {});
    scene.addComponent<CanvasGroupComponent>(e, {});
    scene.addComponent<ProgressbarComponent>(e, {});
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    // the submesh itself: a read of one the mesh does not hold names nothing (mesh-memory)
    mesh.submeshes.resize(1);
    scene.addComponent<MeshComponent>(e, mesh);
    scene.addComponent<SpriteComponent>(e, {});
    scene.addComponent<LightComponent>(e, {});
    scene.addComponent<Light2DComponent>(e, {});
    scene.addComponent<CameraComponent>(e, {});
    scene.addComponent<AudioSourceComponent>(e, {});
    scene.addComponent<FogComponent>(e, {});
    scene.addComponent<SkyboxComponent>(e, {});
    // S81 R40's rows: the Title Art Renderer's live values
    scene.addComponent<TitleArtRendererComponent>(e, {});
    size_t checked = 0;
    for (const anim::AnimatableProperty& row : anim::animatableProperties()){
        const std::string component = row.component;
        std::string property = row.property;
        // pseudo rows (GameObject active, the sprite frame) and shader members (from manifests)
        if (component == "gameobject" || (component == "sprite" && property == "frame") || row.kind == PropertyKind::None) continue;
        const size_t n = property.find("[N]");
        if (n != std::string::npos) property.replace(n, 3, "[0]");
        ComponentType type = ComponentType::Transform;
        bool found = false;
        for (int i = 0; i < ComponentTypeCount && !found; i++){
            if (Catalog::getComponentName(static_cast<ComponentType>(i), true) == component){
                type = static_cast<ComponentType>(i);
                found = true;
            }
        }
        ASSERT_TRUE(found) << "a Catalog component key: " << component;
        if (property == "enabled"){
            EXPECT_TRUE(Catalog::hasEnabledProperty(type)) << component;
            EXPECT_NE(type, ComponentType::AnimationPlayerComponent);
            checked++;
            continue;
        }
        // (the canvas group's and the shadow casters' own rows are checked where they live)
        if (type == ComponentType::CanvasGroupComponent && property != "alpha" && property != "interactable") continue;
        const PropertyData typed = Catalog::findProperty(&scene, e, type, property);
        ASSERT_TRUE(typed.ref) << "the Catalog has " << component << "." << property;
        EXPECT_TRUE(compatibleType(row.kind, typed.type)) << component << "." << property << " is a " << anim::kindName(row.kind);
        checked++;
    }
    EXPECT_GT(checked, 60u);
    // and the other way: every component with the Inspector's enabled checkbox has its row (the
    // Animation Player's own is not animatable)
    for (int i = 0; i < ComponentTypeCount; i++){
        const ComponentType type = static_cast<ComponentType>(i);
        if (!Catalog::hasEnabledProperty(type) || type == ComponentType::AnimationPlayerComponent) continue;
        const std::string component = Catalog::getComponentName(type, true);
        EXPECT_TRUE(anim::findAnimatableProperty(component, "enabled")) << component << " has an enabled checkbox but no enabled row";
    }
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::UIComponent, "color"), (std::vector<ClipChannel>{ClipChannel::Property}));
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::LightComponent, "intensity"), (std::vector<ClipChannel>{ClipChannel::Property}));
    EXPECT_TRUE(ClipRecordHooks::channelsOf(ComponentType::MeshComponent, "castShadows").empty()) << "not animatable";
    EXPECT_TRUE(ClipRecordHooks::channelsOf(ComponentType::AnimationPlayerComponent, "enabled").empty());
}

// test 13: Clamped Auto, and the tangent edits of the curves view
TEST(ClipTangents, ClampedAutoModesAndSides){
    EXPECT_FLOAT_EQ(cliptangents::clampedAutoSlope(0, 0, 1, 1, 2, 0.5f, true, true), 0.0f) << "a local extremum is flat";
    EXPECT_FLOAT_EQ(cliptangents::clampedAutoSlope(0, 0, 1, 1, 2, 2, true, true), 1.0f);
    EXPECT_FLOAT_EQ(cliptangents::clampedAutoSlope(0, 0, 1, 1, 2, 2, false, true), 0.0f) << "an end key is flat";
    EXPECT_FLOAT_EQ(cliptangents::clampedAutoSlope(0, 0, 1, 0.1f, 1.1f, 1.1f, true, true), 0.3f) << "limited: no overshoot";

    ClipTrackData track;
    track.channel = ClipChannel::Property;
    track.component = "light";
    track.property = "intensity";
    track.keys = {key(0, {0}), key(1, {1}), key(2, {2})};
    ClipData data;
    data.tracks = {track};
    anim::normalize(data, nullptr);
    ClipTrackData& t = data.tracks[0];
    ASSERT_TRUE(cliptangents::editable(t));
    cliptangents::setMode(t, 1, ClipTangentMode::Auto);
    for (const ClipKey& k : t.keys) ASSERT_TRUE(k.hasTangents()) << "the track's other keys got linear tangents";
    EXPECT_FLOAT_EQ(t.keys[1].inTangent[0], 1.0f);
    EXPECT_FLOAT_EQ(t.keys[1].outTangent[0], 1.0f);
    EXPECT_EQ(t.keys[1].tangentMode, ClipTangentMode::Auto);
    // Auto follows when a neighbour moves
    t.keys[2].value = {1};
    cliptangents::refresh(t);
    EXPECT_FLOAT_EQ(t.keys[1].outTangent[0], 0.0f) << "now a plateau: flat";
    // Broken keeps the sides independent; a handle of a smooth key is mirrored
    cliptangents::setMode(t, 1, ClipTangentMode::Free);
    cliptangents::setSlope(t, 1, 0, cliptangents::Side::Right, 2.0f);
    EXPECT_FLOAT_EQ(t.keys[1].inTangent[0], 2.0f) << "mirrored";
    cliptangents::setMode(t, 1, ClipTangentMode::Broken);
    cliptangents::setSlope(t, 1, 0, cliptangents::Side::Left, -1.0f);
    EXPECT_FLOAT_EQ(t.keys[1].inTangent[0], -1.0f);
    EXPECT_FLOAT_EQ(t.keys[1].outTangent[0], 2.0f) << "broken: the other side stays";
    // Constant: the right side is the key's step, the left the previous key's
    cliptangents::setSide(t, 1, cliptangents::Side::Right, cliptangents::SideType::Constant);
    EXPECT_EQ(t.keys[1].ease, EaseType::STEP);
    cliptangents::setSide(t, 1, cliptangents::Side::Left, cliptangents::SideType::Constant);
    EXPECT_EQ(t.keys[0].ease, EaseType::STEP);
    cliptangents::setSide(t, 1, cliptangents::Side::Both, cliptangents::SideType::Linear);
    EXPECT_EQ(t.keys[1].tangentMode, ClipTangentMode::Linear);
    EXPECT_EQ(t.keys[1].ease, EaseType::LINEAR);
    EXPECT_EQ(t.keys[0].ease, EaseType::LINEAR);
    // a step with tangents survives the format (a Constant segment)
    cliptangents::setSide(t, 0, cliptangents::Side::Right, cliptangents::SideType::Constant);
    anim::ClipParseResult reread = anim::parseClip(anim::writeClip(data));
    ASSERT_TRUE(reread.ok());
    EXPECT_EQ(reread.data->tracks[0].keys[0].ease, EaseType::STEP);
    EXPECT_TRUE(reread.data->tracks[0].keys[0].hasTangents());
    // rotations and stepped properties are not tangent curves
    ClipTrackData rotation;
    rotation.channel = ClipChannel::Rotation;
    EXPECT_FALSE(cliptangents::editable(rotation));
    ClipTrackData active;
    active.channel = ClipChannel::Property;
    active.known = true;
    active.discrete = true;
    EXPECT_FALSE(cliptangents::editable(active));
}

TEST_F(AnimationClipControllerTest, AddPropertyListsAndNamesUiProperties) {
    ClipScene s = clipScene();
    const Entity panel = addImage(s, "Panel", s.hero, Vector4(1, 0.5f, 0.2f, 1));
    const std::vector<anim::AnimatablePropertyInfo> listed = anim::listAnimatableProperties(s.scene, panel);
    auto find = [&](const std::string& component, const std::string& property) -> const anim::AnimatablePropertyInfo* {
        for (const anim::AnimatablePropertyInfo& info : listed){
            if (info.component == component && info.property == property) return &info;
        }
        return nullptr;
    };
    ASSERT_TRUE(find("gameobject", "active")) << "GameObject : Active";
    const anim::AnimatablePropertyInfo* colour = find("ui", "color");
    ASSERT_TRUE(colour);
    EXPECT_EQ(colour->label, "Color");
    EXPECT_EQ(colour->kind, PropertyKind::Color4) << "the popup offers its Alpha only (color.a)";
    EXPECT_TRUE(find("ui", "enabled"));
    EXPECT_TRUE(find("recttransform", "positionOffset"));
    EXPECT_FALSE(find("light", "intensity")) << "only the components it has";
    EXPECT_EQ(ClipPropertyNames::groupLabel(s.scene, panel, "ui"), "Image");
    EXPECT_EQ(ClipPropertyNames::propertyLabel("ui", "color.a"), "Color (Alpha)");
    EXPECT_EQ(ClipPropertyNames::propertyLabel("recttransform", "positionOffset"), "Anchored Position");
    std::string component, property, error;
    ASSERT_TRUE(ClipPropertyNames::resolve("Image", "color", component, property, error)) << error;
    EXPECT_EQ(component, "ui") << "an Inspector name resolves to the Catalog key";
    // the key itself: a sprite's colour is its mesh material colour (Lua's Sprite.color)
    ASSERT_TRUE(ClipPropertyNames::resolve("sprite", "color", component, property, error)) << error;
    EXPECT_EQ(component, "mesh");
    EXPECT_EQ(property, "submeshes[0].material.baseColorFactor");
    ASSERT_TRUE(ClipPropertyNames::resolve("sprite", "color.a", component, property, error)) << error;
    EXPECT_EQ(property, "submeshes[0].material.baseColorFactor.a");
    ASSERT_TRUE(ClipPropertyNames::resolve("sprite", "frame", component, property, error)) << error;
    EXPECT_EQ(component, "sprite") << "its own rows stay";
    EXPECT_FALSE(ClipPropertyNames::resolve("light", "fooBar", component, property, error));
    EXPECT_NE(error.find("did you mean"), std::string::npos) << error;
    EXPECT_NE(error.find("intensity"), std::string::npos) << "the component's animatable properties are listed: " << error;

    // Add Property keys the current value (sRGB) at the playhead
    AnimationClipController& c = controller();
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.setTime(0.5f);
    size_t track = 0;
    ASSERT_TRUE(c.addPropertyTrack("Panel", "ui", "color.a", &track, &error)) << error;
    const ClipTrackData& added = c.getDocument()->data().tracks[track];
    EXPECT_EQ(added.property, "color.a");
    ASSERT_EQ(added.keys.size(), 1u);
    EXPECT_NEAR(added.keys[0].value[0], 1.0f, 1e-5f);
    EXPECT_FALSE(c.addPropertyTrack("Panel", "ui", "color.a", nullptr, &error)) << "already animated";
    EXPECT_FALSE(c.addPropertyTrack("Panel", "light", "intensity", nullptr, &error)) << "no Light on Panel";
    EXPECT_FALSE(s.sceneProject->isModified);
}

TEST_F(AnimationClipControllerTest, RecordKeysPropertiesInKeySpaceAndThePreviewRestores) {
    ClipScene s = clipScene();
    const Entity panel = addImage(s, "Panel", s.hero, Vector4(1, 0.5f, 0.2f, 1));
    const Vector4 restColour = s.scene->getComponent<UIComponent>(panel).color;
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.setTime(0.5f);
    c.setRecording(true);
    ASSERT_TRUE(c.isRecording());
    const ClipPropertyKey colourKey{"ui", "color"};
    EXPECT_TRUE(ClipRecordHooks::claimsProperty(s.id, panel, colourKey));
    EXPECT_FALSE(ClipRecordHooks::claimsProperty(s.id, s.prop, ClipRecordHooks::activeProperty())) << "not under the root";
    EXPECT_FALSE(ClipRecordHooks::animatesProperty(s.id, panel, colourKey)) << "Record would start keying it";

    // an Inspector colour edit: the row's command (a linear value), recorded instead of run
    const size_t projectSteps = project()->getProjectCommandHistory()->position();
    const size_t sceneSteps = CommandHandle::get(s.id)->position();
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    {
        InspectorWindow inspector(project());
        PropertyCmd<Vector4> command(project(), s.id, panel, ComponentType::UIComponent, "color", Color::sRGBToLinear(Vector4(0.2f, 0.4f, 0.6f, 0.5f)));
        ASSERT_TRUE(inspector.recordPropertyCommand(s.sceneProject, ComponentType::UIComponent, "color", &command));
        inspector.finishRecordRow();
        // the GameObject's active checkbox keys a bool
        ASSERT_TRUE(inspector.recordPropertyToggle(s.sceneProject, {panel}, ComponentType::Transform, ClipRecordHooks::activeProperty(), false));
    }
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    EXPECT_EQ(CommandHandle::get(s.id)->position(), sceneSteps) << "no scene command";
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), projectSteps + 2) << "project steps";
    EXPECT_FALSE(s.sceneProject->isModified);
    const ClipData& data = c.getDocument()->data();
    const int colourTrack = data.findPropertyTrack("Panel", "ui", "color");
    ASSERT_GE(colourTrack, 0);
    const ClipKey& colourKeyed = data.tracks[(size_t)colourTrack].keys[0];
    EXPECT_NEAR(colourKeyed.time, 0.5f, 1e-6f) << "at the playhead";
    EXPECT_NEAR(colourKeyed.value[0], 0.2f, 1e-4f) << "keyed in sRGB";
    EXPECT_NEAR(colourKeyed.value[2], 0.6f, 1e-4f);
    EXPECT_NEAR(colourKeyed.value[3], 0.5f, 1e-5f);
    const int activeTrack = data.findPropertyTrack("Panel", "gameobject", "active");
    ASSERT_GE(activeTrack, 0);
    EXPECT_EQ(data.tracks[(size_t)activeTrack].kind, PropertyKind::Bool);
    EXPECT_EQ(data.tracks[(size_t)activeTrack].keys[0].value, std::vector<float>{0.0f});
    EXPECT_FALSE(s.scene->isEntityActive(panel)) << "the live value shows";
    EXPECT_TRUE(ClipRecordHooks::animatesProperty(s.id, panel, colourKey));
    // undo is a project step
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_LT(c.getDocument()->data().findPropertyTrack("Panel", "gameobject", "active"), 0) << "the active key undone";

    // stop: the scene is back (active and colour), unmodified
    c.stopPreview();
    EXPECT_TRUE(s.scene->isEntityActive(panel));
    const Vector4 now = s.scene->getComponent<UIComponent>(panel).color;
    EXPECT_FLOAT_EQ(now.x, restColour.x);
    EXPECT_FLOAT_EQ(now.w, restColour.w);
    EXPECT_FALSE(s.sceneProject->isModified);
}

TEST_F(AnimationClipControllerTest, TangentEditsAreClipStepsAndAHandleDragIsOne) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    // the Arm's position: keys at 0 and 1 (linear)
    const int arm = c.getDocument()->data().findTrack("Arm", ClipChannel::Position);
    ASSERT_GE(arm, 0);
    const size_t start = project()->getProjectCommandHistory()->position();
    ASSERT_TRUE(c.setTangentMode({{(size_t)arm, 1}}, ClipTangentMode::Auto, &error)) << error;
    const ClipTrackData& track = c.getDocument()->data().tracks[(size_t)arm];
    ASSERT_TRUE(track.keys[1].hasTangents());
    EXPECT_EQ(track.keys[1].tangentMode, ClipTangentMode::Auto);
    EXPECT_FLOAT_EQ(track.keys[1].outTangent[1], 0.0f) << "an end key's Auto is flat";
    EXPECT_TRUE(track.keys[0].hasTangents()) << "the other key got its linear slope";
    EXPECT_FLOAT_EQ(track.keys[0].outTangent[1], 2.0f);
    // a handle drag: several updates, one undo step
    const std::string merge = "tangent#test";
    ASSERT_TRUE(c.setTangentSlope({(size_t)arm, 0}, 1, cliptangents::Side::Right, 1.0f, merge, &error)) << error;
    ASSERT_TRUE(c.setTangentSlope({(size_t)arm, 0}, 1, cliptangents::Side::Right, 3.0f, merge, &error)) << error;
    EXPECT_FLOAT_EQ(c.getDocument()->data().tracks[(size_t)arm].keys[0].outTangent[1], 3.0f);
    EXPECT_EQ(project()->getProjectCommandHistory()->position(), start + 2) << "the mode, then one step for the drag";
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FLOAT_EQ(c.getDocument()->data().tracks[(size_t)arm].keys[0].outTangent[1], 2.0f) << "the whole drag undone";
    // a rotation track has no tangent edits
    const int hand = c.getDocument()->data().findTrack("Arm/Hand", ClipChannel::Scale);
    ASSERT_GE(hand, 0);
    EXPECT_TRUE(c.setTangentMode({{(size_t)hand, 0}}, ClipTangentMode::Flat, &error)) << "scale is a scalar curve: " << error;
    EXPECT_FALSE(s.sceneProject->isModified);
}

// H1: scene commands during a property preview edit the rest: a Lua script field row (the
// Inspector's and set_script_properties' PropertyCmd on "scripts[N].<field>") and a whole-component
// Reset. Stop keeps what they wrote, and undo puts the rest back, never the previewed value
TEST_F(AnimationClipControllerTest, SceneEditsDuringAPropertyPreviewEditTheRest) {
    ClipScene s = clipScene("anim/glow.anim");
    const Entity panel = addImage(s, "Panel", s.hero, Vector4(1, 0.5f, 0.2f, 1));
    const Vector4 restColour = s.scene->getComponent<UIComponent>(panel).color;
    ScriptComponent scripts;
    ScriptEntry torch;
    torch.type = ScriptType::LUA;
    torch.enabled = true;
    torch.className = "Torch";
    ScriptProperty glow;
    glow.name = "glow";
    glow.type = ScriptPropertyType::Float;
    glow.value = 0.2f;
    torch.properties = {glow};
    scripts.scripts.push_back(torch);
    s.scene->addComponent<ScriptComponent>(panel, scripts);
    auto glowOf = [&](){ return std::get<float>(s.scene->getComponent<ScriptComponent>(panel).scripts[0].properties[0].value); };

    ClipData data;
    ClipTrackData glowTrack;
    glowTrack.path = "Panel";
    glowTrack.channel = ClipChannel::Property;
    glowTrack.component = "script:Torch";
    glowTrack.property = "glow";
    glowTrack.keys = {key(0, {1}), key(1, {1})};
    ClipTrackData colourTrack = glowTrack;
    colourTrack.component = "ui";
    colourTrack.property = "color";
    colourTrack.keys = {key(0, {0, 0, 0, 1}), key(1, {0, 0, 0, 1})};
    data.tracks = {glowTrack, colourTrack};
    writeClip("anim/glow.anim", data);
    AnimationClipDocument::checkDiskChanges(project());

    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    EXPECT_FLOAT_EQ(glowOf(), 1.0f) << "previewed";
    EXPECT_FLOAT_EQ(s.scene->getComponent<UIComponent>(panel).color.x, 0.0f) << "previewed black";
    CommandHistory* history = CommandHandle::get(s.id);

    // the script row: glow = 3 is the new rest
    ASSERT_TRUE(history->addCommand(new PropertyCmd<float>(project(), s.id, panel, ComponentType::ScriptComponent, "scripts[0].glow", 3.0f)));
    // Reset of the Image's UI values (white): the colour's new rest
    Command* reset = ComponentValuesCmd::buildReset(project(), s.id, {panel}, ComponentType::UIComponent, &error);
    ASSERT_TRUE(reset) << error;
    ASSERT_TRUE(history->addCommand(reset));
    const Vector4 resetColour = s.scene->getComponent<UIComponent>(panel).color;
    c.update(0.0);
    EXPECT_FLOAT_EQ(glowOf(), 1.0f) << "previewed again";
    c.stopPreview();
    EXPECT_FLOAT_EQ(glowOf(), 3.0f) << "stop keeps the edit, not the rest before it";
    const Vector4 kept = s.scene->getComponent<UIComponent>(panel).color;
    EXPECT_EQ(kept.x, resetColour.x) << "stop keeps the Reset";
    EXPECT_NE(kept.x, 0.0f);
    ASSERT_TRUE(history->undo());
    const Vector4 undone = s.scene->getComponent<UIComponent>(panel).color;
    // (the command's own copy of the component, through its YAML)
    EXPECT_FLOAT_EQ(undone.x, restColour.x) << "undoing the Reset writes the rest, not the previewed black";
    EXPECT_FLOAT_EQ(undone.y, restColour.y);
    ASSERT_TRUE(history->undo());
    EXPECT_FLOAT_EQ(glowOf(), 0.2f) << "undo writes the rest, not the previewed 1";
}

// the curves view says when a track's first tangents turn its eases linear
TEST_F(AnimationClipControllerTest, TangentEditsSayWhichEasesBecameLinear) {
    ClipScene s = clipScene();
    AnimationClipController& c = controller();
    std::string error, notice;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    const int arm = c.getDocument()->data().findTrack("Arm", ClipChannel::Position);
    ASSERT_GE(arm, 0);
    ASSERT_TRUE(c.setEase({{(size_t)arm, 0}}, EaseType::CUBIC_IN_OUT, &error)) << error;
    ASSERT_TRUE(c.setTangentMode({{(size_t)arm, 1}}, ClipTangentMode::Auto, &error, &notice)) << error;
    EXPECT_NE(notice.find("1 key's ease became linear tangents"), std::string::npos) << notice;
    ASSERT_TRUE(c.setTangentMode({{(size_t)arm, 0}}, ClipTangentMode::Flat, &error, &notice)) << error;
    EXPECT_TRUE(notice.empty()) << "nothing more to replace: " << notice;
}

namespace {
    // a clip of one property track on `path`, keyed `value` over a second
    void writePropertyClip(const std::function<void(const std::string&, const ClipData&)>& write, const std::string& file,
                           const std::string& path, const std::string& component, const std::string& property, std::vector<float> value){
        ClipData data;
        ClipTrackData track;
        track.path = path;
        track.channel = ClipChannel::Property;
        track.component = component;
        track.property = property;
        track.keys = {key(0, value), key(1, value)};
        data.tracks = {track};
        write(file, data);
    }
}

// Paste Component Values onto a previewed component: the pasted values are the new rest
TEST_F(AnimationClipControllerTest, PasteComponentValuesDuringAPreviewEditsTheRest) {
    ClipScene s = clipScene("anim/paste.anim");
    const Entity panel = addImage(s, "Panel", s.hero, Vector4(1, 0.5f, 0.2f, 1));
    const Entity donor = addImage(s, "Donor", NULL_ENTITY, Vector4(0, 1, 0, 1));
    const Vector4 restColour = s.scene->getComponent<UIComponent>(panel).color;
    const Vector4 donorColour = s.scene->getComponent<UIComponent>(donor).color;
    writePropertyClip([&](const std::string& f, const ClipData& d){ writeClip(f, d); }, "anim/paste.anim", "Panel", "ui", "color", {0, 0, 0, 1});
    AnimationClipDocument::checkDiskChanges(project());
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    ASSERT_EQ(s.scene->getComponent<UIComponent>(panel).color.x, 0.0f) << "previewed black";
    auto clip = std::make_shared<ComponentClip>();
    clip->project = EntityClipboard::projectKey(project());
    clip->sceneId = s.id;
    clip->type = ComponentType::UIComponent;
    clip->entity = donor;
    clip->value = EntityClipboard::encodeComponent(s.scene, donor, ComponentType::UIComponent);
    Command* paste = ComponentValuesCmd::buildPasteValues(project(), s.id, {panel}, ComponentType::UIComponent, clip, &error);
    ASSERT_TRUE(paste) << error;
    CommandHistory* history = CommandHandle::get(s.id);
    ASSERT_TRUE(history->addCommand(paste));
    c.update(0.0);
    EXPECT_EQ(s.scene->getComponent<UIComponent>(panel).color.x, 0.0f) << "previewed again";
    c.stopPreview();
    EXPECT_FLOAT_EQ(s.scene->getComponent<UIComponent>(panel).color.y, donorColour.y) << "stop keeps the pasted colour";
    EXPECT_FLOAT_EQ(s.scene->getComponent<UIComponent>(panel).color.x, donorColour.x);
    ASSERT_TRUE(history->undo());
    EXPECT_FLOAT_EQ(s.scene->getComponent<UIComponent>(panel).color.x, restColour.x) << "undo writes the rest, not black";
}

// Reset of a whole Mesh: its submesh rows are previewed properties of it (before: the command
// recorded the previewed colour, and its undo wrote it into the scene)
TEST_F(AnimationClipControllerTest, MeshResetDuringAPreviewEditsTheSubmeshRest) {
    ClipScene s = clipScene("anim/mesh.anim");
    const Entity cube = addObject(s, "Cube", s.hero, Vector3(0, 0, 0));
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].material.baseColorFactor = Color::sRGBToLinear(Vector4(1, 0.5f, 0.2f, 1));
    s.scene->addComponent<MeshComponent>(cube, mesh);
    const Vector4 restColour = s.scene->getComponent<MeshComponent>(cube).submeshes[0].material.baseColorFactor;
    writePropertyClip([&](const std::string& f, const ClipData& d){ writeClip(f, d); }, "anim/mesh.anim", "Cube", "mesh",
                      "submeshes[0].material.baseColorFactor", {0, 0, 0, 1});
    AnimationClipDocument::checkDiskChanges(project());
    AnimationClipController& c = controller();
    std::string error;
    ASSERT_TRUE(c.bindRoot(s.id, s.hero, &error)) << error;
    c.seek(0.5f);
    ASSERT_TRUE(c.isPreviewing());
    auto colour = [&](){ return s.scene->getComponent<MeshComponent>(cube).submeshes[0].material.baseColorFactor; };
    ASSERT_EQ(colour().y, 0.0f) << "previewed black";
    Command* reset = ComponentValuesCmd::buildReset(project(), s.id, {cube}, ComponentType::MeshComponent, &error);
    ASSERT_TRUE(reset) << error;
    CommandHistory* history = CommandHandle::get(s.id);
    ASSERT_TRUE(history->addCommand(reset));
    const Vector4 resetColour = colour();
    c.update(0.0);
    EXPECT_EQ(colour().y, 0.0f) << "previewed again";
    c.stopPreview();
    // (a Mesh's Reset keeps its geometry and materials: the colour is the rest the command saw)
    EXPECT_EQ(colour().y, resetColour.y) << "stop keeps what the Reset left";
    EXPECT_EQ(resetColour.y, restColour.y) << "the Reset was recorded on the rest, not on the previewed black";
    ASSERT_TRUE(history->undo());
    EXPECT_FLOAT_EQ(colour().y, restColour.y) << "undo writes the rest, not the previewed black";
}
