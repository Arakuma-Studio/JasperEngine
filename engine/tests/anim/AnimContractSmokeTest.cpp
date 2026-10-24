// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The animation clips contract (T0): one smoke test per public engine header, exercising the
// API shape. The assertions hold for the stubs and for the real implementations (T1a/T1b/T1c
// add the behaviour tests in their own files).

#include <gtest/gtest.h>

#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationEvent.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "component/AnimationPlayerComponent.h"
#include "object/AnimationPlayer.h"
#include "pool/AnimationClipPool.h"
#include "registry/EntityRegistry.h"
#include "subsystem/ActionSystem.h"

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    ClipData twoKeyClip(){
        ClipData data;
        ClipTrackData track;
        track.path = "Arm/Hand";
        track.channel = ClipChannel::Position;
        ClipKey a;
        a.time = 0.0f;
        a.value = {0, 0, 0};
        ClipKey b;
        b.time = 1.5f;
        b.value = {0, 1, 0};
        b.ease = EaseType::QUAD_OUT;
        track.keys = {a, b};
        data.tracks.push_back(track);
        ClipEventData event;
        event.time = 0.5f;
        event.function = "onFootstep";
        event.args = {AnimationEventArg(std::string("left")), AnimationEventArg(1.0)};
        data.events.push_back(event);
        return data;
    }

}

TEST(AnimContract, AnimationClipData){
    ClipData data;
    EXPECT_FLOAT_EQ(data.frameRate, 30.0f);
    EXPECT_EQ(data.wrap, ClipWrapMode::Once);
    EXPECT_FALSE(data.hasExplicitLength());
    EXPECT_FLOAT_EQ(data.effectiveLength(), 0.0f);

    data = twoKeyClip();
    EXPECT_FLOAT_EQ(data.effectiveLength(), 1.5f);
    data.length = 2.0f;
    EXPECT_FLOAT_EQ(data.effectiveLength(), 2.0f);
    EXPECT_EQ(data.findTrack("Arm/Hand", ClipChannel::Position), 0);
    EXPECT_EQ(data.findTrack("Arm/Hand", ClipChannel::Rotation), -1);

    EXPECT_EQ(ClipData::valueWidth(ClipChannel::Position), 3u);
    EXPECT_EQ(ClipData::valueWidth(ClipChannel::Rotation), 4u);
    EXPECT_EQ(ClipData::valueWidth(ClipChannel::Morph, 5), 5u);
    EXPECT_EQ(ClipData::valueWidth(ClipChannel::Lattice, 0, 2), 6u);
    EXPECT_EQ(ClipData::valueWidth(ClipChannel::Property), 0u);

    std::vector<ClipWarning> warnings;
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data, &warnings);
    ASSERT_TRUE(clip != nullptr);
    EXPECT_GE(clip->getLength(), 0.0f);
    EXPECT_EQ(clip->findTrack("Missing", ClipChannel::Scale), -1);
    for (const AnimationClip::Track& track : clip->getTracks()){
        EXPECT_FALSE(track.path.empty() && track.keyCount > 0 && track.times == nullptr);
    }
    ClipData back = clip->toData();
    (void)back;
}

TEST(AnimContract, AnimationClipIO){
    EXPECT_FALSE(anim::parseClip("").ok());
    EXPECT_FALSE(anim::parseClip("not json").ok());
    EXPECT_FALSE(anim::parseClip("[1, 2]").ok());
    anim::ClipParseResult result = anim::parseClip("{\"format\": \"something.else\", \"version\": 1}");
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());

    EXPECT_NE(std::string(anim::unityYamlRefusal()).find("Unity .anim (YAML)"), std::string::npos);
    EXPECT_STREQ(anim::ANIM_FORMAT, "jasperengine.anim");
    EXPECT_EQ(anim::ANIM_VERSION, 1);

    anim::ClipParseError error;
    error.line = 3;
    error.column = 7;
    error.message = "expected ','";
    EXPECT_EQ(error.toString(), "3:7: expected ','");

    ClipData data = twoKeyClip();
    std::string text = anim::writeClip(data);
    anim::normalize(data);
    (void)text;
    (void)anim::isUnityYamlClip("%YAML 1.1");
    (void)anim::generateClipGuid();
    EXPECT_FALSE(anim::isClipGuid("not a guid"));
    EXPECT_TRUE(anim::readClipGuid("{}").empty());
    std::string notAClip = "plain text";
    EXPECT_FALSE(anim::replaceClipGuid(notAClip, "0123456789abcdef0123456789abcdef"));
    EXPECT_EQ(notAClip, "plain text");

    ClipChannel channel;
    ClipWrapMode wrap;
    EaseType ease;
    EXPECT_FALSE(anim::channelFromName("no_such_channel", channel));
    EXPECT_FALSE(anim::wrapFromName("no_such_wrap", wrap));
    EXPECT_FALSE(anim::easeFromName("no_such_ease", ease));
    (void)anim::channelName(ClipChannel::Lattice);
    (void)anim::wrapName(ClipWrapMode::PingPong);
    (void)anim::easeName(EaseType::BOUNCE_IN_OUT);
}

TEST(AnimContract, KeyframeSampling){
    const float times[] = {0.0f, 1.0f};
    const EaseType easings[] = {EaseType::LINEAR, EaseType::LINEAR};
    anim::KeyCursor cursor;
    anim::KeyLocation at = anim::locateKey(times, 2, easings, 2, 0.0f, &cursor);
    EXPECT_EQ(at.index, 0u);
    EXPECT_TRUE(anim::keyIndexValid(at, 2));
    EXPECT_FALSE(anim::keyIndexValid(at, 0));

    const Vector3 vectors[] = {Vector3(1, 2, 3), Vector3(4, 5, 6)};
    const Quaternion rotations[] = {Quaternion(), Quaternion()};
    (void)anim::sampleVector3(at, vectors, nullptr, nullptr);
    (void)anim::sampleQuaternion(at, rotations, nullptr, nullptr);
    float weights[2] = {9, 9};
    const float row0[] = {0, 1};
    const float row1[] = {1, 0};
    anim::sampleFloatRow(at, row0, row1, 2, nullptr, nullptr, weights);
    Vector3 offsets[2];
    anim::sampleVector3Row(at, vectors, vectors, 2, nullptr, nullptr, offsets);
    (void)anim::applyEase(EaseType::QUAD_IN, 0.5f);
    (void)anim::wrapClipTime(1.5f, 1.0f, ClipWrapMode::Loop);

    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(twoKeyClip());
    std::vector<anim::TrackSample> samples;
    anim::sampleClipTracks(*clip, 0.25f, samples);
    EXPECT_EQ(samples.size(), clip->getTracks().size());
    std::vector<size_t> crossed;
    anim::collectCrossedEvents(*clip, ClipWrapMode::Once, 0.0f, 0.25f, true, crossed);
    for (size_t index : crossed) EXPECT_LT(index, clip->getEvents().size());
}

TEST(AnimContract, ClipBinding){
    std::vector<anim::PathSegment> segments;
    anim::parsePath("", segments);
    EXPECT_TRUE(segments.empty());
    EXPECT_FALSE(anim::parsePath("Arm//Hand", segments));
    (void)anim::formatPath(segments);
    (void)anim::formatSegment("Arm", 2);
    EXPECT_FALSE(anim::isAddressableName("a/b"));

    EntityRegistry registry;
    anim::PathResolution info;
    EXPECT_EQ(anim::resolvePath(&registry, NULL_ENTITY, "Arm", &info), NULL_ENTITY);
    std::string path;
    EXPECT_FALSE(anim::relativePath(&registry, NULL_ENTITY, NULL_ENTITY, path));
    EXPECT_TRUE(anim::getChildren(&registry, NULL_ENTITY).empty());
    (void)anim::descendantPaths(&registry, NULL_ENTITY);
}

TEST(AnimContract, AnimationEvent){
    AnimationEvent event;
    event.functionName = "onFootstep";
    event.args = {AnimationEventArg(std::string("left")), AnimationEventArg(2.5), AnimationEventArg(true)};
    EXPECT_EQ(event.getArgCount(), 3u);
    EXPECT_EQ(event.getArgType(0), "string");
    EXPECT_EQ(event.getArgType(1), "number");
    EXPECT_EQ(event.getArgType(2), "boolean");
    EXPECT_EQ(event.getArgType(3), "");
    EXPECT_EQ(event.getStringArg(0), "left");
    EXPECT_DOUBLE_EQ(event.getNumberArg(1), 2.5);
    EXPECT_TRUE(event.getBoolArg(2));
    EXPECT_DOUBLE_EQ(event.getNumberArg(0), 0.0);
    EXPECT_EQ(event.getStringArg(9), "");
    EXPECT_EQ(MAX_ANIMATION_EVENT_ARGS, 8u);
}

TEST(AnimContract, ModelSubClips){
    EXPECT_FALSE(anim::isSubClipReference("assets/anim/walk.anim"));
    anim::SubClipReference reference;
    (void)anim::parseSubClipReference("assets/models/hero.gltf#Walk", reference);
    (void)anim::formatSubClipReference("assets/models/hero.gltf", "Walk");
    (void)anim::isModelFilePath("hero.GLB");
    ClipData data;
    std::vector<ClipWarning> warnings;
    std::string error;
    EXPECT_FALSE(anim::buildModelSubClip("assets/models/missing.gltf#Walk", data, warnings, &error));
}

TEST(AnimContract, AnimationClipPool){
    EXPECT_TRUE(AnimationClipPool::get("never/loaded.anim") == nullptr);
    EXPECT_EQ(AnimationClipPool::getGeneration("never/loaded.anim"), 0u);
    (void)AnimationClipPool::getKey("assets/anim/walk.anim");
    (void)AnimationClipPool::revision();
    AnimationClipPool::invalidate("never/loaded.anim");
    AnimationClipPool::invalidateModel("never/loaded.gltf");
    AnimationClipPool::clearUnused();
    AnimationClipPool::clear();
    EXPECT_EQ(AnimationClipPool::size(), 0u);
}

TEST(AnimContract, AnimationPlayerComponent){
    AnimationPlayerComponent player;
    EXPECT_TRUE(player.enabled);
    EXPECT_TRUE(player.playAutomatically);
    EXPECT_FLOAT_EQ(player.speed, 1.0f);
    EXPECT_FLOAT_EQ(player.defaultFadeTime, 0.25f);
    EXPECT_TRUE(player.clips.empty());
    EXPECT_TRUE(player.needsLoad);

    AnimationPlayerClip entry;
    entry.clip = "anim/walk.anim";
    EXPECT_EQ(entry.wrap, ClipWrapMode::Default);
    AnimationPlayerClip other = entry;
    EXPECT_TRUE(entry == other);
    other.wrap = ClipWrapMode::Loop;
    EXPECT_TRUE(entry != other);

    // registered: one more component in the 64-bit signature
    EntityRegistry registry;
    Entity entity = registry.createEntity();
    registry.addComponent<AnimationPlayerComponent>(entity, {});
    ASSERT_TRUE(registry.findComponent<AnimationPlayerComponent>(entity) != nullptr);
    EXPECT_LT((size_t)registry.getComponentId<AnimationPlayerComponent>(), Signature().size());
    EXPECT_EQ(registry.hierarchyRevision(), registry.hierarchyRevision());

    std::string received;
    registry.getComponent<AnimationPlayerComponent>(entity).onEvent.add("test", std::function<void(const AnimationEvent&)>([&received](const AnimationEvent& event){
        received = event.functionName;
    }));
    AnimationEvent event;
    event.functionName = "onFootstep";
    registry.getComponent<AnimationPlayerComponent>(entity).onEvent.call(event);
    EXPECT_EQ(received, "onFootstep");
}

TEST(AnimContract, AnimationPlayerRuntime){
    anim::AnimationPlayerRuntime runtime;
    anim::PlayerStateRuntime state;
    state.name = "Walk";
    runtime.states.push_back(state);
    EXPECT_EQ(runtime.findState("Walk"), 0);
    EXPECT_EQ(runtime.findState("Run"), -1);

    anim::ClipPreviewSession session;
    session.root = 5;
    session.setRest(7, anim::ChannelRest());
    EXPECT_EQ(session.rest.size(), 1u);
    session.reset();
    EXPECT_EQ(session.root, NULL_ENTITY);
    EXPECT_TRUE(session.rest.empty());

    anim::AnimationStateInfo info;
    EXPECT_FALSE(info.loaded);
}

TEST(AnimContract, AnimationPlayerObjectAndPreview){
    Scene scene;
    Entity root = scene.createEntity();
    scene.addComponent<Transform>(root, {});
    AnimationPlayer player(&scene, root);
    ASSERT_TRUE(scene.findComponent<AnimationPlayerComponent>(root) != nullptr);
    player.play("Walk");
    player.crossFade("Walk", 0.2f);
    player.blend("Walk", 0.5f, 0.1f);
    player.queue("Walk");
    player.stop();
    player.rewind("Walk");
    player.sample();
    EXPECT_FALSE(player.isPlaying("NoSuchState"));
    AnimationState state = player.getState("NoSuchState");
    EXPECT_FALSE(state.isValid());
    (void)player.getStateInfo("NoSuchState");
    (void)player.getClipCount();
    (void)player.getOnEvent();
    (void)player.getOnFinish();

    ActionSystem* actions = scene.getSystem<ActionSystem>().get();
    ASSERT_TRUE(actions != nullptr);
    anim::ClipPreviewSession session;
    ActionSystem::PlayerPreview preview;
    ActionSystem::PlayerPreviewState previewState;
    previewState.clip = AnimationClip::build(twoKeyClip());
    previewState.time = 0.5f;
    preview.states.push_back(previewState);
    ActionSystem::PlayerPreviewResult result = actions->previewPlayer(root, preview, session);
    for (Entity written : result.written) EXPECT_NE(written, NULL_ENTITY);
    std::vector<anim::TrackSample> samples;
    ActionSystem::sampleClip(*previewState.clip, 0.5f, samples);
    EXPECT_EQ(samples.size(), previewState.clip->getTracks().size());
    actions->resetPlayers();
}
