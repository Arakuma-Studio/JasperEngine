// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's clip mode without its UI (animation clips spec 4.4, 9.R4-R7/R21):
// ClipRecordHooks' registries, the PreviewOwner registry and AnimationClipController on a
// headless project (AppSettings in a temporary config directory + Project::loadProject, the
// engine started on the editor's Platform as the other project tests do). An EditorHost that
// forwards to AnimationClipAppHooks stands in for the App, so saves, scene closes and project
// switches reach the controller the way they do in the editor.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "EditorHost.h"
#include "Engine.h"
#include "Platform.h"
#include "Project.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/ClipRecordHooks.h"
#include "animation/PreviewOwner.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "component/ModelComponent.h"
#include "object/Model.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseEditSession.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

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
        std::unique_ptr<editor::Platform> platform;
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
            out.platform.reset(new editor::Platform(out.project.get()));
            static char arg0[] = "jasperengine-editor-tests";
            static char* argv[] = {arg0, nullptr};
            Engine::systemInit(1, argv, out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
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
        if (parent != NULL_ENTITY) {
            s.scene->addEntityChild(parent, entity, false);
        } else {
            s.sceneProject->entities.push_back(entity);
        }
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
    EXPECT_TRUE(c.ownsUndo(false)) << "the window's last action was a clip edit";
    ClipUndoFocus focus;
    focus.animationWindow = true;
    EXPECT_TRUE(animationClipAppHooks().ownsUndo(focus));
    focus.animationWindow = false;
    focus.sceneView = true;
    EXPECT_FALSE(animationClipAppHooks().ownsUndo(focus)) << "only while Record is on";

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
    ClipUndoFocus focus;
    focus.sceneView = true;
    EXPECT_TRUE(animationClipAppHooks().ownsUndo(focus)) << "Record routes the Scene view's undo to the project";

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
    fs::copy_file(fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / "tools" / "fixtures" / "pose-animation" / "assets" /
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
