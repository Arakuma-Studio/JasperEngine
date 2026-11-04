// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The animation clips contract (T0), editor side: every editor/animation header and the clip
// composite command compile and their stubs keep the API shape. The assertions hold for the
// stubs and for the implementations (T2b, T2d, T3a add the behaviour tests in their own files).

#include <gtest/gtest.h>
#include "unity/UnityYaml.h"
#include "animation/UpstreamAnimFormats.h"

#include "Catalog.h"
#include "Scene.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipInspectorHooks.h"
#include "animation/ClipConverters.h"
#include "animation/ClipRecordHooks.h"
#include "animation/ClipValidation.h"
#include "animation/PreviewOwner.h"
#include "command/type/CreateClipCmd.h"

#include <string>
#include <type_traits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    ClipData oneTrackClip(){
        ClipData data;
        ClipTrackData track;
        track.path = "Hips/Spine";
        track.channel = ClipChannel::Rotation;
        ClipKey key;
        key.value = {0, 0, 0, 1};
        track.keys.push_back(key);
        data.tracks.push_back(track);
        ClipEventData event;
        event.time = 0.0f;
        event.function = "onStep";
        data.events.push_back(event);
        return data;
    }

}

TEST(AnimationClipContract, CatalogKnowsTheAnimationPlayer){
    // within the types every loop runs over (S19's navigation components follow it)
    EXPECT_LE(static_cast<int>(ComponentType::AnimationPlayerComponent), static_cast<int>(LastComponentType));
    EXPECT_LT(static_cast<int>(ComponentType::AnimationPlayerComponent), ComponentTypeCount);
    // later features append after the Vertex Animator (prefab override masks use the ordinal):
    // upstream's Trail Renderer, Canvas Group, on-screen controls and Ragdoll, then the Camera
    // Shaker, the Camera Follow, the Text Effect, the Video Player and the Safe Area
    EXPECT_GT(static_cast<int>(LastComponentType), static_cast<int>(ComponentType::VertexAnimatorComponent));
    EXPECT_EQ(ComponentTypeCount, static_cast<int>(LastComponentType) + 1);
    // the Text Effect (ui-scaling-text), then the Video Player, the Safe Area and the Capsule Shadow Caster (S57);
    // later features append after them
    EXPECT_GE(static_cast<int>(LastComponentType), static_cast<int>(ComponentType::SafeAreaComponent));
    EXPECT_EQ(Catalog::getComponentType("animationplayer"), ComponentType::AnimationPlayerComponent);
    EXPECT_EQ(Catalog::getComponentType("AnimationPlayerComponent"), ComponentType::AnimationPlayerComponent);
    EXPECT_EQ(Catalog::getComponentName(ComponentType::AnimationPlayerComponent, false), "AnimationPlayerComponent");
    EXPECT_EQ(Catalog::getComponentName(ComponentType::AnimationPlayerComponent, true), "animationplayer");
    EXPECT_EQ(Catalog::getComponentDisplayName(ComponentType::AnimationPlayerComponent), "Animation Player");
    // the entity-sequence component keeps its names
    EXPECT_EQ(Catalog::getComponentType("animation"), ComponentType::AnimationComponent);
    EXPECT_EQ(Catalog::getComponentDisplayName(ComponentType::AnimationComponent), "Animation");

    Scene scene;
    EXPECT_EQ(Catalog::getComponentId(&scene, ComponentType::AnimationPlayerComponent),
              scene.getComponentId<AnimationPlayerComponent>());
}

// the hook structs (later tasks set the editor-wide instances at startup: only fresh structs
// are checked here)
TEST(AnimationClipContract, HookStructsStartUnset){
    static_assert(std::is_default_constructible<AnimationClipAppHooks>::value, "app hooks");
    static_assert(std::is_default_constructible<AnimationClipInspectorHooks>::value, "inspector hooks");
    AnimationClipAppHooks app;
    EXPECT_FALSE(app.stopTransientPreviews);
    EXPECT_FALSE(app.ownsUndo);
    EXPECT_FALSE(app.takesKeys);
    EXPECT_FALSE(app.sceneClosed);
    EXPECT_FALSE(app.projectClosed);
    EXPECT_FALSE(app.createClipAsset);
    EXPECT_FALSE(app.dropClips);

    AnimationClipInspectorHooks inspector;
    EXPECT_FALSE(inspector.openClip);
    EXPECT_FALSE(inspector.convertToClip);
    EXPECT_FALSE(inspector.useAnimationPlayer);
    (void)&animationClipAppHooks();
    (void)&animationClipInspectorHooks();
}

TEST(AnimationClipContract, ChannelsOfComponentWrites){
    using Channels = std::vector<ClipChannel>;
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::Transform, "position"), Channels{ClipChannel::Position});
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::Transform, "rotation"), Channels{ClipChannel::Rotation});
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::Transform, "scale"), Channels{ClipChannel::Scale});
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::Transform, ""),
              (Channels{ClipChannel::Position, ClipChannel::Rotation, ClipChannel::Scale}));
    EXPECT_TRUE(ClipRecordHooks::channelsOf(ComponentType::Transform, "parent").empty());
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::LatticeComponent, "offsets"), Channels{ClipChannel::Lattice});
    // a whole-component write (Reset, Paste Component Values) also writes its animatable
    // properties (anim-properties): the previewed ones are notified
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::MeshComponent, ""), (Channels{ClipChannel::Morph, ClipChannel::Property}));
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::CameraComponent, ""), Channels{ClipChannel::Property});
    EXPECT_EQ(ClipRecordHooks::channelsOf(ComponentType::ScriptComponent, "scripts[0].glow"), Channels{ClipChannel::Property});
    EXPECT_TRUE(ClipRecordHooks::channelsOf(ComponentType::ActionComponent, "").empty()) << "no animatable property";
}

TEST(AnimationClipContract, Document){
    std::string error;
    EXPECT_TRUE(AnimationClipDocument::open(nullptr, "assets/anim/walk.anim", &error) == nullptr);
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_TRUE(AnimationClipDocument::create(nullptr, "assets/anim/walk.anim", oneTrackClip(), "Create Clip", &error) == nullptr);
    EXPECT_FALSE(error.empty());
    (void)AnimationClipDocument::fileText(oneTrackClip());
    (void)AnimationClipDocument::openDocuments();
    AnimationClipDocument::filesChanged({});
    AnimationClipDocument::checkDiskChanges();
}

TEST(AnimationClipContract, Validation){
    ClipData data = oneTrackClip();
    ClipValidationReport report = ClipValidation::validate(data, nullptr, NULL_ENTITY);
    ASSERT_EQ(report.tracks.size(), data.tracks.size());
    EXPECT_EQ(report.tracks[0].status, TrackBindingStatus::Unchecked);
    EXPECT_EQ(report.tracks[0].path, "Hips/Spine");
    ASSERT_EQ(report.events.size(), data.events.size());
    EXPECT_EQ(report.events[0].status, EventHandlerStatus::Unchecked);
    EXPECT_TRUE(report.ok());

    ClipValidationOptions options;
    EXPECT_EQ(options.maxNearest, 3u);
    (void)ClipValidation::nearestPaths(nullptr, NULL_ENTITY, "Hips", 3);
}

TEST(AnimationClipContract, Converters){
    Scene scene;
    Entity entity = scene.createEntity();
    ClipData out;
    ClipConvertReport report;
    std::string error;
    // not an entity Animation
    EXPECT_FALSE(ClipConverters::convertEntityAnimation(&scene, entity, NULL_ENTITY, out, report, &error));
    // (a missing model file is refused too: ClipConvertersTest, which starts the engine the model
    // lookup needs; this binary may run this test alone)
    std::vector<ClipWarning> warnings;
    EXPECT_FALSE(ClipConverters::extractModelSubClip("assets/models/walk.anim", out, warnings, &error));
    UseAnimationPlayerPlan plan;
    EXPECT_FALSE(ClipConverters::planUseAnimationPlayer(&scene, entity, plan, &error));
    BakedChannel channel;
    EXPECT_EQ(channel.channel, ClipChannel::Rotation);
}

TEST(AnimationClipContract, CreateClipCommand){
    CreateClipCmd command(nullptr, 0, {}, {});
    EXPECT_FALSE(command.affectsStructure());
    EXPECT_TRUE(command.getFailure().empty());
}

TEST(AnimationClipContract, RecordHooksWithoutListeners){
    EXPECT_FALSE(ClipRecordHooks::claims(1, 2, ClipChannel::Position));
    ClipRecordValue value;
    value.value = {1, 2, 3};
    EXPECT_FALSE(ClipRecordHooks::record(1, 2, value, "record#1"));
    ClipRecordHooks::beforeChannelWrite(1, 2, ClipChannel::Rotation);
    ClipRecordHooks::afterChannelWrite(1, 2, ClipChannel::Rotation);
    ClipRecordHooks::removeRecordListener(0);
    ClipRecordHooks::removeChannelWriteListener(0);
}

TEST(AnimationClipContract, PreviewOwner){
    PreviewOwner::release(0);
    EXPECT_FALSE(PreviewOwner::owns(0));
    PreviewOwner::Token token = PreviewOwner::acquire(1, 2, nullptr, "test", [](){});
    EXPECT_GT(token, 0);
    EXPECT_TRUE(PreviewOwner::owns(token));
    PreviewOwner::release(token);
    EXPECT_FALSE(PreviewOwner::owns(token));
    PreviewOwner::releaseScene(1);
}

TEST(AnimationClipContract, Controller){
    AnimationClipController controller(nullptr);
    EXPECT_EQ(controller.getRoot(), NULL_ENTITY);
    EXPECT_FALSE(controller.isPreviewing());
    EXPECT_FALSE(controller.isPlaying());
    EXPECT_FALSE(controller.isRecording());
    EXPECT_TRUE(controller.getDocument() == nullptr);
    EXPECT_TRUE(controller.sampledBoundTransforms().empty());
    EXPECT_FALSE(controller.ownsUndo());
    controller.stopTransientPreviews();
    controller.stopPreview();
    std::string error;
    EXPECT_FALSE(controller.openClip("assets/anim/missing.anim", &error));
    EXPECT_FALSE(controller.isDraggingKeys());

    ClipData data = oneTrackClip();
    AnimationClipController::KeyRef ref;
    ASSERT_TRUE(AnimationClipController::findKey(data, "Hips/Spine", ClipChannel::Rotation, 0.0f, ref));
    EXPECT_EQ(ref.track, 0u);
    EXPECT_EQ(ref.key, 0u);
    EXPECT_FALSE(AnimationClipController::findKey(data, "Hips/Spine", ClipChannel::Rotation, 0.5f, ref));
    EXPECT_FALSE(AnimationClipController::findKey(data, "Hips", ClipChannel::Rotation, 0.0f, ref));

    AnimationClipController::shared(nullptr);
    AnimationClipController::release(nullptr);
    AnimationClipController::release(nullptr);
}

TEST(AnimationClipContract, ClipUsersWithoutProject){
    EXPECT_TRUE(findClipUsers(nullptr, "anim/walk.anim").empty());
}
