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
    // version 3 (S40): clips with audio cues; version 2 (anim-retarget): humanoid clips; other
    // clips are still written as version 1
    EXPECT_EQ(anim::ANIM_VERSION, 3);

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
}

// the pure helpers T0 implemented: enum names, GUIDs, Unity YAML detection
TEST(AnimContract, AnimationClipIONamesAndGuids){
    const ClipChannel channels[] = {ClipChannel::Position, ClipChannel::Rotation, ClipChannel::Scale,
                                    ClipChannel::Morph, ClipChannel::Lattice, ClipChannel::Property};
    const char* channelNames[] = {"position", "rotation", "scale", "morph", "lattice", "property"};
    for (size_t i = 0; i < 6; i++){
        EXPECT_STREQ(anim::channelName(channels[i]), channelNames[i]);
        ClipChannel back;
        ASSERT_TRUE(anim::channelFromName(channelNames[i], back));
        EXPECT_EQ(back, channels[i]);
    }
    const ClipWrapMode wraps[] = {ClipWrapMode::Default, ClipWrapMode::Once, ClipWrapMode::Loop,
                                  ClipWrapMode::PingPong, ClipWrapMode::ClampForever};
    const char* wrapNames[] = {"default", "once", "loop", "ping_pong", "clamp_forever"};
    for (size_t i = 0; i < 5; i++){
        EXPECT_STREQ(anim::wrapName(wraps[i]), wrapNames[i]);
        ClipWrapMode back;
        ASSERT_TRUE(anim::wrapFromName(wrapNames[i], back));
        EXPECT_EQ(back, wraps[i]);
    }
    for (int e = (int)EaseType::LINEAR; e <= (int)EaseType::STEP; e++){
        EaseType back;
        ASSERT_TRUE(anim::easeFromName(anim::easeName((EaseType)e), back)) << e;
        EXPECT_EQ(back, (EaseType)e);
    }
    EXPECT_STREQ(anim::easeName(EaseType::QUAD_IN_OUT), "quad_in_out");
    EXPECT_STREQ(anim::easeName(EaseType::CUSTOM), "linear");

    std::string guid = anim::generateClipGuid();
    EXPECT_TRUE(anim::isClipGuid(guid));
    EXPECT_NE(guid, anim::generateClipGuid());
    EXPECT_FALSE(anim::isClipGuid("0123456789ABCDEF0123456789abcdef"));

    std::string text = "{\n  \"format\": \"jasperengine.anim\",\n  \"version\": 1,\n  \"frame_rate\": 30\n}\n";
    EXPECT_TRUE(anim::readClipGuid(text).empty());
    ASSERT_TRUE(anim::replaceClipGuid(text, guid));
    EXPECT_EQ(text, "{\n  \"format\": \"jasperengine.anim\",\n  \"version\": 1,\n  \"guid\": \"" + guid +
                    "\",\n  \"frame_rate\": 30\n}\n");
    EXPECT_EQ(anim::readClipGuid(text), guid);
    std::string other = anim::generateClipGuid();
    ASSERT_TRUE(anim::replaceClipGuid(text, other));
    EXPECT_EQ(anim::readClipGuid(text), other);
    // a nested "guid" is not the clip's
    EXPECT_TRUE(anim::readClipGuid("{\"version\": 1, \"source\": {\"guid\": \"" + guid + "\"}}").empty());
    std::string bad = text;
    EXPECT_FALSE(anim::replaceClipGuid(bad, "nope"));
    EXPECT_EQ(bad, text);

    EXPECT_TRUE(anim::isUnityYamlClip("%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!74 &7400000\nAnimationClip:\n"));
    EXPECT_TRUE(anim::isUnityYamlClip("--- !u!74 &7400000\nAnimationClip:\n  m_Name: Walk\n"));
    EXPECT_FALSE(anim::isUnityYamlClip(text));
}

TEST(AnimContract, QuaternionLayout){
    const float xyzw[] = {0.1f, 0.2f, 0.3f, 0.9f};
    Quaternion q = quaternionFromXYZW(xyzw);
    EXPECT_FLOAT_EQ(q.x, 0.1f);
    EXPECT_FLOAT_EQ(q.y, 0.2f);
    EXPECT_FLOAT_EQ(q.z, 0.3f);
    EXPECT_FLOAT_EQ(q.w, 0.9f);
    float back[4];
    quaternionToXYZW(q, back);
    for (int i = 0; i < 4; i++) EXPECT_FLOAT_EQ(back[i], xyzw[i]);
}

TEST(AnimContract, StateNames){
    AnimationPlayerClip walk;
    walk.clip = "anim/walk.anim";
    AnimationPlayerClip sub;
    sub.clip = "models/hero.gltf#Run";
    AnimationPlayerClip named;
    named.clip = "anim/other.anim";
    named.name = "Idle";
    AnimationPlayerClip walkAgain;
    walkAgain.clip = "other/walk.anim";
    EXPECT_EQ(anim::defaultStateName(walk), "walk");
    EXPECT_EQ(anim::defaultStateName(sub), "Run");
    EXPECT_EQ(anim::defaultStateName(named), "Idle");
    std::vector<std::string> names = anim::playerStateNames({walk, sub, named, walkAgain, walk});
    ASSERT_EQ(names.size(), 5u);
    EXPECT_EQ(names[0], "walk");
    EXPECT_EQ(names[1], "Run");
    EXPECT_EQ(names[2], "Idle");
    EXPECT_EQ(names[3], "walk 2");
    EXPECT_EQ(names[4], "walk 3");
}

TEST(AnimContract, ChannelValues){
    Scene scene;
    Entity entity = scene.createEntity();
    scene.addComponent<Transform>(entity, {});
    std::vector<float> value;
    ASSERT_TRUE(anim::writeChannelValue(&scene, entity, ClipChannel::Position, {}, {1, 2, 3}));
    ASSERT_TRUE(anim::readChannelValue(&scene, entity, ClipChannel::Position, {}, value));
    EXPECT_EQ(value, (std::vector<float>{1, 2, 3}));
    ASSERT_TRUE(anim::writeChannelValue(&scene, entity, ClipChannel::Rotation, {}, {0, 0.6f, 0, 0.8f}));
    ASSERT_TRUE(anim::readChannelValue(&scene, entity, ClipChannel::Rotation, {}, value));
    EXPECT_EQ(value, (std::vector<float>{0, 0.6f, 0, 0.8f}));
    EXPECT_TRUE(scene.getComponent<Transform>(entity).needUpdate);
    EXPECT_FALSE(anim::readChannelValue(&scene, entity, ClipChannel::Morph, {}, value));
    EXPECT_FALSE(anim::readChannelValue(&scene, entity, ClipChannel::Lattice, {}, value));

    Entity cage = scene.createEntity();
    scene.addComponent<LatticeComponent>(cage, {});
    uint32_t revision = scene.getComponent<LatticeComponent>(cage).revision;
    ASSERT_TRUE(anim::writeChannelValue(&scene, cage, ClipChannel::Lattice, {13}, {0, 0.2f, 0}));
    EXPECT_EQ(scene.getComponent<LatticeComponent>(cage).revision, revision + 1);
    ASSERT_TRUE(anim::readChannelValue(&scene, cage, ClipChannel::Lattice, {13}, value));
    EXPECT_EQ(value, (std::vector<float>{0, 0.2f, 0}));
    ASSERT_TRUE(anim::readChannelValue(&scene, cage, ClipChannel::Lattice, {}, value));
    EXPECT_EQ(value.size(), 27u * 3u);
    EXPECT_FALSE(anim::readChannelValue(&scene, cage, ClipChannel::Lattice, {27}, value));
    EXPECT_FALSE(anim::writeChannelValue(&scene, cage, ClipChannel::Lattice, {27}, {0, 0, 0}));
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
    std::vector<anim::CrossedEvent> crossed;
    anim::collectCrossedEvents(*clip, ClipWrapMode::Once, 0.0f, 0.25f, true, crossed);
    for (const anim::CrossedEvent& event : crossed) EXPECT_LT(event.index, clip->getEvents().size());
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
    event.args = {AnimationEvent::stringArg("left"), AnimationEvent::numberArg(2.5), AnimationEvent::boolArg(true)};
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
    EXPECT_FALSE(anim::isSubClipReference("models/hero.gltf#"));
    anim::SubClipReference reference;
    ASSERT_TRUE(anim::parseSubClipReference("models/hero.gltf#Walk", reference));
    EXPECT_EQ(reference.model, "models/hero.gltf");
    EXPECT_EQ(reference.clip, "Walk");
    // split at the first '#' after a model extension: file names may contain '#'
    ASSERT_TRUE(anim::parseSubClipReference("models/take#2/hero.GLB#Run #2", reference));
    EXPECT_EQ(reference.model, "models/take#2/hero.GLB");
    EXPECT_EQ(reference.clip, "Run #2");
    EXPECT_EQ(anim::formatSubClipReference("models/hero.gltf", "Walk"), "models/hero.gltf#Walk");
    EXPECT_TRUE(anim::isModelFilePath("hero.GLB"));
    EXPECT_TRUE(anim::isModelFilePath("hero.fbx"));
    EXPECT_FALSE(anim::isModelFilePath("hero.anim"));
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

    AnimationStateInfo info;
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

    // the authored-field accessors (implemented in T0)
    EXPECT_EQ(player.getClipCount(), 0);
    EXPECT_TRUE(player.addClip("anim/walk.anim"));
    EXPECT_FALSE(player.addClip("other/walk.anim"));         // "walk" is taken
    EXPECT_TRUE(player.addClip("other/walk.anim", "Walk Fast"));
    EXPECT_EQ(player.getClipCount(), 2);
    EXPECT_EQ(player.getClipName(0), "walk");
    EXPECT_EQ(player.getClipName(1), "Walk Fast");
    EXPECT_EQ(player.getClipName(2), "");
    EXPECT_TRUE(player.removeClip("walk"));
    EXPECT_FALSE(player.removeClip("walk"));
    EXPECT_EQ(player.getClipCount(), 1);
    player.setSpeed(2.0f);
    EXPECT_FLOAT_EQ(player.getSpeed(), 2.0f);
    player.setDefaultClip("Walk Fast");
    EXPECT_EQ(player.getDefaultClip(), "Walk Fast");
    player.setDefaultFadeTime(0.5f);
    EXPECT_FLOAT_EQ(player.getDefaultFadeTime(), 0.5f);
    player.setPlayAutomatically(false);
    EXPECT_FALSE(player.isPlayAutomatically());
    player.setEnabled(false);
    EXPECT_FALSE(player.isEnabled());
    EXPECT_TRUE(scene.getComponent<AnimationPlayerComponent>(root).needsLoad);
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
    for (const auto& written : result.channels) EXPECT_NE(written.first, NULL_ENTITY);

    // restorePlayerPreview writes the rest back to what the session wrote, then resets it
    scene.getComponent<Transform>(root).position = Vector3(5, 5, 5);
    anim::ChannelRest rest;
    rest.hasTransform = true;
    rest.position = Vector3(1, 2, 3);
    session.root = root;
    session.setRest(root, rest);
    session.written.push_back({root, ClipChannel::Position});
    actions->restorePlayerPreview(session);
    EXPECT_EQ(scene.getComponent<Transform>(root).position, Vector3(1, 2, 3));
    EXPECT_TRUE(session.written.empty());
    EXPECT_EQ(session.root, NULL_ENTITY);
    std::vector<anim::TrackSample> samples;
    ActionSystem::sampleClip(*previewState.clip, 0.5f, samples);
    EXPECT_EQ(samples.size(), previewState.clip->getTracks().size());
    actions->resetPlayers();
}
