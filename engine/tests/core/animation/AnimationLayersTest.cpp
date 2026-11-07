// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Animation Player layers, masks and mixing transforms (upstream anim layers spec 14, tests 1-12;
// docs/specs/S21-animation.md "Upstream absorption"): the v1 fast path, Override and Additive
// layers with numeric pose checks, fractional masks, per-layer play / queue / stop, events gated
// by the layer weight, mixing transforms, humanoid masks on the T-pose rig, upstream's .mask
// format read as S21's version 2, layer runtime kept across reloads, the layered edit-mode
// preview, the Lua API, and the mobile performance budget (spec 13). The masks are S21's
// (AvatarMaskWeights.h) and the layers blend with S21's pose maths: a fractional rotation blend
// is an nlerp (anim::poseBlendRotation), where upstream slerped.

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"
#include "../pose/TestRigs.h"

#include "Engine.h"
#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorPose.h"
#include "animation/AvatarMaskWeights.h"
#include "object/AnimationPlayer.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "script/LuaBinding.h"
#include "subsystem/ActionSystem.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>

using namespace jasperengine;

namespace {

    constexpr double PI = 3.14159265358979323846;

    // (x, y, z, w) of a rotation of `degrees` about an axis
    std::vector<float> axisAngle(float degrees, float ax, float ay, float az){
        const double half = degrees * PI / 360.0;
        const float s = (float)std::sin(half);
        return {ax * s, ay * s, az * s, (float)std::cos(half)};
    }

    Quaternion rotation(float degrees, float ax, float ay, float az){
        std::vector<float> q = axisAngle(degrees, ax, ay, az);
        return quaternionFromXYZW(q.data());
    }

    ClipKey key(float time, std::vector<float> value){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        return k;
    }

    ClipTrackData track(const std::string& path, ClipChannel channel, std::vector<ClipKey> keys){
        ClipTrackData t;
        t.path = path;
        t.channel = channel;
        t.keys = std::move(keys);
        return t;
    }

    // walk: Leg x 0 -> 2 and Arm 0 -> 90 degrees about Z over 1 s
    ClipData walk(ClipWrapMode wrap = ClipWrapMode::Loop){
        ClipData data;
        data.wrap = wrap;
        data.tracks = {track("Arm", ClipChannel::Rotation, {key(0, axisAngle(0, 0, 0, 1)), key(1, axisAngle(90, 0, 0, 1))}),
                       track("Leg", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {2, 0, 0})})};
        return data;
    }

    // hold: Arm 45 degrees about X and Hand y = 1, held for `length` seconds
    ClipData hold(ClipWrapMode wrap = ClipWrapMode::Loop, float length = 1.0f){
        ClipData data;
        data.wrap = wrap;
        data.tracks = {track("Arm", ClipChannel::Rotation, {key(0, axisAngle(45, 1, 0, 0)), key(length, axisAngle(45, 1, 0, 0))}),
                       track("Arm/Hand", ClipChannel::Position, {key(0, {0, 1, 0}), key(length, {0, 1, 0})})};
        return data;
    }

    ClipEventData event(float time, const std::string& function){
        ClipEventData e;
        e.time = time;
        e.function = function;
        return e;
    }

    float quatDot(const Quaternion& a, const Quaternion& b){
        return std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    }

    class Layers : public ::testing::Test {
    protected:
        void SetUp() override {
            static int run = 0;
            prefix = "layers/r" + std::to_string(run++) + "/";
            AnimationClipPool::clear();
            AnimatorAssetPool::clear();
            root = scene.createEntity();
            scene.setEntityName(root, "Hero");
            scene.addComponent<Transform>(root, {});
            arm = child(root, "Arm", Vector3(5, 0, 0));
            hand = child(arm, "Hand", Vector3(0, 7, 0));
            leg = child(root, "Leg", Vector3(0, -3, 0));
            animtest::takeWarnings();
        }
        void TearDown() override {
            AnimationClipPool::clear();
            AnimatorAssetPool::clear();
        }

        Entity child(Entity parent, const std::string& name, Vector3 position = Vector3(0, 0, 0)){
            Entity entity = scene.createEntity();
            scene.setEntityName(entity, name);
            Transform transform;
            transform.position = position;
            scene.addComponent<Transform>(entity, transform);
            scene.addEntityChild(parent, entity, false);
            return entity;
        }

        std::string clip(const std::string& name, const ClipData& data){
            animtest::writeAsset(prefix + name + ".anim", anim::writeClip(data));
            return prefix + name + ".anim";
        }

        std::string mask(const std::string& name, const anim::AvatarMaskDef& data){
            animtest::writeAsset(prefix + name + ".mask", anim::writeMask(data));
            return prefix + name + ".mask";
        }

        anim::AvatarMaskDef armMask(float weight = 1.0f){
            anim::AvatarMaskDef data;
            data.defaultWeight = 0.0f;
            data.paths.push_back({"Arm", weight, true});
            return data;
        }

        // a player with entries (name, clip, layer) and layers
        AnimationPlayer player(const std::vector<std::tuple<std::string, ClipData, int>>& clips, const std::vector<AnimationPlayerLayer>& layers){
            AnimationPlayer object(&scene, root);
            AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
            for (const auto& entry : clips){
                AnimationPlayerClip clipEntry;
                clipEntry.clip = clip(std::get<0>(entry), std::get<1>(entry));
                clipEntry.name = std::get<0>(entry);
                clipEntry.layer = std::get<2>(entry);
                component.clips.push_back(clipEntry);
            }
            component.layers = layers;
            component.playAutomatically = false;
            component.needsLoad = true;
            return object;
        }

        AnimationPlayerLayer layer(const std::string& name, float weight = 1.0f, AnimationLayerBlend blend = AnimationLayerBlend::Override,
                                   const std::string& maskPath = ""){
            AnimationPlayerLayer l;
            l.name = name;
            l.weight = weight;
            l.blend = blend;
            l.mask = maskPath;
            return l;
        }

        void step(double dt, int count = 1){
            for (int i = 0; i < count; i++){
                scene.getSystem<ActionSystem>()->update(dt);
            }
        }

        Transform& transform(Entity entity){ return scene.getComponent<Transform>(entity); }

        Scene scene;
        std::string prefix;
        Entity root = NULL_ENTITY;
        Entity arm = NULL_ENTITY;
        Entity hand = NULL_ENTITY;
        Entity leg = NULL_ENTITY;
    };

    uint64_t fnv(uint64_t hash, const void* data, size_t size){
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; i++){
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

}

// Test 1: a player without layers keeps the v1 code path. On Linux x86-64 100 frames of a crossfade
// hash to the value the pre-layers engine produced (recorded by this same test, which uses only
// the v1 API, built against main before this feature); every target checks the frames below, the
// values of that run. Bytes hold on one target only: on arm64 macOS 17 of the 3000 floats differ by
// at most 2 ULP, 15 from Clang contracting a*b+c into a fused multiply-add (Quaternion's normalize)
// and 2 from one sinf argument that glibc rounds up and Apple's libm rounds down.
TEST_F(Layers, DefaultLayeringIsTheV1FastPath){
    struct Sample { int frame; float armW, armX, armZ, legX; };
    static constexpr Sample samples[] = {
        {0, 0.995990515f, -0.000109092565f, 0.0894587934f, 0.230555564f},
        {11, 0.987674475f, -0.0158358607f, 0.155718252f, 0.75f},
        {22, 0.990842938f, -0.0595754646f, 0.121165149f, 1.60555601f},
        {33, 0.990969777f, -0.132408008f, -0.021145314f, 2.7972219f},
        {44, 0.941164792f, -0.221361771f, -0.255358487f, 4.32499886f},
        {55, 0.882947564f, -0.281682968f, -0.375577331f, 5.59999847f},
        {66, 0.998134851f, -0.0366290249f, -0.0488386974f, 0.699997902f},
        {77, 0.987688422f, -0.0938605368f, -0.125147387f, 1.79999685f},
        {88, 0.968147695f, -0.15022783f, -0.200303778f, 2.8999958f},
        {99, 0.939692736f, -0.205211893f, -0.273615837f, 3.99999475f},
    };
    // 4 ULP at the largest value here (Leg x near 6): room for a target's rounding, far under any change of a blend
    constexpr float tolerance = 2e-6f;
    size_t nextSample = 0;
    ClipData run = walk();
    run.tracks[1].keys[1].value = {6, 0, 0};
    run.tracks[0].keys[1].value = axisAngle(-60, 0.6f, 0, 0.8f);
    AnimationPlayer object(&scene, root);
    object.addClip(clip("walk", walk()), "walk");
    object.addClip(clip("run", run), "run");
    object.setPlayAutomatically(false);
    object.play("walk");
    step(0.1);
    object.crossFade("run", 0.8f);
    uint64_t hash = 1469598103934665603ull;
    for (int frame = 0; frame < 100; frame++){
        step(1.0 / 60.0);
        for (Entity entity : {arm, leg, hand}){
            const Transform& t = transform(entity);
            const float values[] = {t.position.x, t.position.y, t.position.z, t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z,
                                    t.scale.x, t.scale.y, t.scale.z};
            hash = fnv(hash, values, sizeof(values));
        }
        if (nextSample < std::size(samples) && samples[nextSample].frame == frame){
            const Sample& expected = samples[nextSample++];
            SCOPED_TRACE("frame " + std::to_string(frame));
            const Transform& a = transform(arm);
            EXPECT_NEAR(a.rotation.w, expected.armW, tolerance);
            EXPECT_NEAR(a.rotation.x, expected.armX, tolerance);
            EXPECT_NEAR(a.rotation.y, 0.0f, tolerance);
            EXPECT_NEAR(a.rotation.z, expected.armZ, tolerance);
            EXPECT_NEAR(transform(leg).position.x, expected.legX, tolerance);
            // what no track of these clips animates stays at its rest
            EXPECT_NEAR(a.position.x, 5.0f, tolerance);
            EXPECT_NEAR(transform(leg).position.y, 0.0f, tolerance);
            EXPECT_NEAR(transform(hand).position.y, 7.0f, tolerance);
        }
    }
    EXPECT_EQ(nextSample, std::size(samples));
    EXPECT_TRUE(anim::isDefaultLayering(scene.getComponent<AnimationPlayerComponent>(root)));
#if defined(__linux__) && defined(__x86_64__)
    EXPECT_EQ(hash, 0x79e15195f729cd77ull) << std::hex << hash;
#endif
}

// Test 2: an Override layer with a transform mask on the arm
TEST_F(Layers, OverrideLayerWithAMaskOwnsTheMaskedBones){
    const std::string armPath = mask("arm", armMask());
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), layer("Holding", 1.0f, AnimationLayerBlend::Override, armPath)});
    object.play("walk");
    object.play("hold");
    step(0.25, 2);
    EXPECT_TRUE(object.isPlaying("walk")) << "a play on layer 1 does not stop layer 0";
    EXPECT_GT(quatDot(transform(arm).rotation, rotation(45, 1, 0, 0)), 1.0f - 1e-5f);
    EXPECT_NEAR(transform(leg).position.x, 1.0f, 1e-5f);
    EXPECT_NEAR(transform(hand).position.y, 1.0f, 1e-5f);
}

// Test 3: layer weight 0.5 blends the arm halfway (an nlerp: at 0.5 the slerp's midpoint); the
// leg is the base's
TEST_F(Layers, LayerWeightBlendsWithTheLayersBelow){
    const std::string armPath = mask("arm", armMask());
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), layer("Holding", 0.5f, AnimationLayerBlend::Override, armPath)});
    object.play("walk");
    object.play("hold");
    step(0.25, 2);
    const Quaternion expected = anim::poseBlendRotation(rotation(45, 0, 0, 1), rotation(45, 1, 0, 0), 0.5f);
    EXPECT_GT(quatDot(transform(arm).rotation, expected), 1.0f - 1e-5f);
    EXPECT_GT(quatDot(expected, Quaternion::slerp(0.5f, rotation(45, 0, 0, 1), rotation(45, 1, 0, 0), true)), 1.0f - 1e-6f);
    EXPECT_NEAR(transform(leg).position.x, 1.0f, 1e-5f);
    // Hand: rest y 7, only the layer animates it: lerp(7, 1, 0.5)
    EXPECT_NEAR(transform(hand).position.y, 4.0f, 1e-5f);
    EXPECT_NEAR(object.getLayerWeight("Holding"), 0.5f, 1e-6f);
    EXPECT_NEAR(object.getLayerWeight(0), 1.0f, 1e-6f);
}

// Test 4: a fractional mask weight is a partial override (S21's nlerp at weight * mask)
TEST_F(Layers, FractionalMaskWeightIsAPartialOverride){
    const std::string armPath = mask("arm", armMask(0.25f));
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), layer("Holding", 1.0f, AnimationLayerBlend::Override, armPath)});
    object.play("walk");
    object.play("hold");
    step(0.25, 2);
    const Quaternion expected = anim::poseBlendRotation(rotation(45, 0, 0, 1), rotation(45, 1, 0, 0), 0.25f);
    EXPECT_GT(quatDot(transform(arm).rotation, expected), 1.0f - 1e-5f);
    EXPECT_NEAR(transform(leg).position.x, 1.0f, 1e-5f);
}

// Test 5: additive layers add the delta from the clip's additive_reference
TEST_F(Layers, AdditiveLayersAddTheirDelta){
    ClipData nod;
    nod.wrap = ClipWrapMode::ClampForever;
    nod.tracks = {track("Arm", ClipChannel::Rotation, {key(0, axisAngle(0, 1, 0, 0)), key(1, axisAngle(30, 1, 0, 0))}),
                  track("Arm/Hand", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 2, 0})}),
                  track("Leg", ClipChannel::Scale, {key(0, {1, 1, 1}), key(1, {2, 3, 4})})};
    AnimationPlayer object = player({{"walk", walk(ClipWrapMode::ClampForever), 0}, {"nod", nod, 1}},
                                    {layer(anim::BASE_LAYER_NAME), layer("Hit", 1.0f, AnimationLayerBlend::Additive)});
    object.play("walk");
    object.play("nod");
    step(0.5, 2);
    EXPECT_GT(quatDot(transform(arm).rotation, rotation(90, 0, 0, 1) * rotation(30, 1, 0, 0)), 1.0f - 1e-5f);
    // L3: over nothing the delta adds to the rest (Hand y 7 + 2, Leg scale 1 * (2, 3, 4))
    EXPECT_NEAR(transform(hand).position.y, 9.0f, 1e-5f);
    EXPECT_NEAR(transform(leg).scale.x, 2.0f, 1e-5f);
    EXPECT_NEAR(transform(leg).scale.z, 4.0f, 1e-5f);
    EXPECT_NEAR(transform(leg).position.x, 2.0f, 1e-5f) << "the base's channels pass through";

    object.setLayerWeight("Hit", 0.5f);
    step(0.0);
    EXPECT_GT(quatDot(transform(arm).rotation, rotation(90, 0, 0, 1) * rotation(15, 1, 0, 0)), 1.0f - 1e-5f);
    EXPECT_NEAR(transform(hand).position.y, 8.0f, 1e-5f);
    EXPECT_NEAR(transform(leg).scale.y, 2.0f, 1e-5f) << "scale: 1 + 0.5 * (3 - 1)";

    // the reference time moves the zero of the delta
    nod.additiveReference = 1.0f;
    AnimationClipPool::invalidate(clip("nod", nod));
    object.setLayerWeight("Hit", 1.0f);
    step(0.0);
    EXPECT_GT(quatDot(transform(arm).rotation, rotation(90, 0, 0, 1)), 1.0f - 1e-5f);
    EXPECT_NEAR(transform(hand).position.y, 7.0f, 1e-5f);
}

// Test 6: play stops the same layer, PlayMode StopAll every layer
TEST_F(Layers, PlayStopsTheSameLayerOrAll){
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 1}, {"hold2", hold(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), layer("Holding")});
    object.play("walk");
    object.play("hold");
    step(0.1);
    EXPECT_TRUE(object.isPlaying("walk"));
    EXPECT_TRUE(object.isPlaying("hold"));
    object.play("hold2");
    EXPECT_FALSE(object.isPlaying("hold"));
    EXPECT_TRUE(object.isPlaying("walk"));
    object.play("walk", 0.0f, PlayMode::StopAll);
    EXPECT_TRUE(object.isPlaying("walk"));
    EXPECT_FALSE(object.isPlaying("hold2"));
    object.crossFade("hold", 0.5f);
    step(0.1);
    EXPECT_TRUE(object.isPlaying("walk")) << "a crossfade on layer 1 leaves layer 0 alone";
    object.stopLayer("Holding");
    EXPECT_FALSE(object.isPlaying("hold"));
    EXPECT_TRUE(object.isPlaying("walk"));
}

// Test 7: a queue per layer: hold2 starts when hold (once, 0.5 s) ends while walk loops on
TEST_F(Layers, QueuesRunPerLayer){
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(ClipWrapMode::Once, 0.5f), 1}, {"hold2", hold(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), layer("Holding")});
    object.play("walk");
    object.play("hold");
    object.queue("hold2");
    EXPECT_FALSE(object.isPlaying("hold2"));
    EXPECT_TRUE(object.getStateInfo("hold2").queued);
    step(0.2, 3);
    EXPECT_TRUE(object.isPlaying("hold2"));
    EXPECT_TRUE(object.isPlaying("walk")) << "the queue of layer 1 does not wait for layer 0";
    EXPECT_NEAR(object.getState("hold2").getTime(), 0.1f, 1e-4f);
    // a queue on the base waits for the base only
    object.queue("walk");
    EXPECT_TRUE(object.getStateInfo("walk").queued);
    object.stop("walk");
    EXPECT_FALSE(object.getStateInfo("walk").queued) << "stop clears the state's layer queue";
}

// Test 8: events fire only on layers with weight
TEST_F(Layers, EventsNeedALayerWeight){
    ClipData holdEvents = hold();
    holdEvents.events = {event(0.25f, "onHold")};
    ClipData walkEvents = walk();
    walkEvents.events = {event(0.25f, "onStep")};
    anim::AvatarMaskDef none;
    none.defaultWeight = 0.0f;
    const std::string nonePath = mask("none", none);
    AnimationPlayer object = player({{"walk", walkEvents, 0}, {"hold", holdEvents, 1}},
                                    {layer(anim::BASE_LAYER_NAME, 1.0f, AnimationLayerBlend::Override, nonePath), layer("Holding", 0.0f)});
    std::vector<std::string> fired;
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){ fired.push_back(e.functionName); }));
    object.play("walk");
    object.play("hold");
    step(0.5);
    ASSERT_EQ(fired.size(), 1u);
    EXPECT_EQ(fired[0], "onStep") << "the base fires (its mask hides every bone), the zero-weight layer does not";
    EXPECT_NEAR(object.getState("hold").getTime(), 0.5f, 1e-5f) << "L2: a zero-weight layer keeps advancing";
    object.setLayerWeight("Holding", 0.01f);
    step(1.0);
    EXPECT_EQ(std::count(fired.begin(), fired.end(), "onHold"), 1);
    object.getOnEvent().clear();
}

// Test 9: mixing transforms: the state animates only the given paths
TEST_F(Layers, MixingTransformsLimitAState){
    AnimationPlayer object = player({{"walk", walk(), 0}}, {});
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    object.play("walk");
    step(0.25, 2);
    EXPECT_NEAR(transform(leg).position.x, 1.0f, 1e-5f);
    object.getState("walk").addMixingTransform("Arm");
    EXPECT_FALSE(anim::isDefaultLayering(component));
    step(0.25);
    EXPECT_NEAR(transform(leg).position.x, 0.0f, 1e-5f) << "the leg goes back to its rest";
    EXPECT_NEAR(transform(leg).position.y, -3.0f, 1e-5f);
    EXPECT_GT(quatDot(transform(arm).rotation, rotation(67.5f, 0, 0, 1)), 1.0f - 1e-5f) << "the arm animates";
    object.getState("walk").clearMixingTransforms();
    EXPECT_TRUE(anim::isDefaultLayering(component));
    step(0.125);
    EXPECT_NEAR(transform(leg).position.x, 1.75f, 1e-5f);
}

// Test 10: a humanoid mask on the T-pose rig
TEST_F(Layers, HumanoidMasksCoverBodyParts){
    using namespace jasperengine::pose;
    const posetest::TestRig& rig = posetest::tposeRig();
    Scene human;
    Entity character = human.createEntity();
    human.setEntityName(character, "Granjero");
    human.addComponent<Transform>(character, {});
    std::vector<Entity> bones(rig.skeleton.bones.size(), NULL_ENTITY);
    for (size_t i = 0; i < rig.skeleton.bones.size(); i++){
        Entity bone = human.createEntity();
        human.setEntityName(bone, rig.skeleton.bones[i].name);
        human.addComponent<Transform>(bone, {});
        const int parent = rig.skeleton.bones[i].parent;
        human.addEntityChild(parent >= 0 ? bones[(size_t)parent] : character, bone, false);
        bones[i] = bone;
    }
    auto bone = [&](const std::string& name) -> Entity {
        for (size_t i = 0; i < rig.skeleton.bones.size(); i++){
            if (rig.skeleton.bones[i].name == name) return bones[i];
        }
        return NULL_ENTITY;
    };
    Entity twist = human.createEntity();
    human.setEntityName(twist, "RightArmTwist");
    human.addComponent<Transform>(twist, {});
    human.addEntityChild(bone("mixamorig:RightArm"), twist, false);

    anim::HumanoidSlots slots = anim::humanoidSlotsByName(&human, character, rig.expected.bones);
    ASSERT_TRUE(slots.valid);
    // upstream's right_arm (which held the hand bone) is right_arm + right_hand here
    anim::AvatarMaskDef data;
    data.defaultWeight = 0.0f;
    data.bodyParts[(int)AvatarBodyPart::RightArm] = 1.0f;
    data.bodyParts[(int)AvatarBodyPart::RightHand] = 1.0f;
    auto weight = [&](Entity e){ return anim::maskWeightOf(data, &human, character, e, &slots); };
    for (const char* name : {"mixamorig:RightShoulder", "mixamorig:RightArm", "mixamorig:RightForeArm", "mixamorig:RightHand"}){
        EXPECT_EQ(weight(bone(name)), 1.0f) << name;
    }
    EXPECT_EQ(weight(bone("mixamorig:RightHandIndex1")), 0.0f) << "fingers are right_fingers (unset: default)";
    EXPECT_EQ(weight(bone("mixamorig:RightHandIndex3")), 0.0f);
    EXPECT_EQ(weight(twist), 1.0f) << "a twist bone takes its slot ancestor's part";
    EXPECT_EQ(weight(bone("mixamorig:LeftHand")), 0.0f);
    EXPECT_EQ(weight(bone("mixamorig:Spine")), 0.0f);

    anim::AvatarMaskDef rootOnly;
    rootOnly.defaultWeight = 0.0f;
    rootOnly.rootHierarchy = true;
    rootOnly.bodyParts[(int)AvatarBodyPart::Root] = 1.0f;
    rootOnly.bodyParts[(int)AvatarBodyPart::RightFingers] = 0.5f;
    const anim::MaskWeight armature = anim::maskWeightOf(rootOnly, &human, character, bone("Armature"), {}, &slots);
    EXPECT_EQ(armature.weight, 1.0f) << "root (root_hierarchy) covers the glTF root node above the hips";
    EXPECT_EQ(armature.ruleName(), "body_part:root");
    EXPECT_EQ(anim::maskWeightOf(rootOnly, &human, character, character, &slots), 1.0f);
    EXPECT_EQ(anim::maskWeightOf(rootOnly, &human, character, bone("mixamorig:Hips"), &slots), 0.0f);
    EXPECT_EQ(anim::maskWeightOf(rootOnly, &human, character, bone("mixamorig:RightHandThumb2"), &slots), 0.5f);
    rootOnly.rootHierarchy = false;
    EXPECT_EQ(anim::maskWeightOf(rootOnly, &human, character, bone("Armature"), &slots), 0.0f) << "S21's rule: above the hips is the default";
    EXPECT_EQ(anim::maskRootMotionWeight(rootOnly), 1.0f) << "the root part weighs root motion";

    // a path entry wins over the body part
    data.paths.push_back({"Armature/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1/mixamorig:Spine2/mixamorig:RightShoulder/mixamorig:RightArm", 0.3f, false});
    const anim::MaskWeight upper = anim::maskWeightOf(data, &human, character, bone("mixamorig:RightArm"), anim::bindMaskPaths(data, &human, character), &slots);
    EXPECT_FLOAT_EQ(upper.weight, 0.3f);
    EXPECT_EQ(upper.ruleName(), "path");
    EXPECT_EQ(weight(twist), 1.0f) << "children: false: the twist bone falls back to the body part";

    // L9: without a rig, the humanoid part is skipped
    anim::HumanoidSlots none;
    EXPECT_EQ(anim::maskWeightOf(data, &human, character, bone("mixamorig:RightHand"), &none), 0.0f);
}

// Test 11: upstream's .mask format (version 1), read as S21's version 2 (UpstreamAnimFormats.h)
TEST(AvatarMaskIO, UpstreamMaskReadsAsVersion2LenientAndRefusesUnityYaml){
    const std::string text =
        "{\"format\": \"jasperengine.mask\", \"version\": 1, \"default\": false,\n"
        " \"humanoid\": {\"right_arm\": true, \"right_fingers\": 0.5, \"left_hand_ik\": 1, \"tail\": 1},\n"
        " \"transforms\": [{\"path\": \"Hero/Can\", \"weight\": 2}, {\"weight\": 1}, {\"path\": \"A\", \"children\": false}],\n"
        " \"preview\": {\"model\": \"models/hero.glb\"}, \"custom\": {\"x\": 1}}";
    anim::MaskParseResult parsed = anim::parseMask(text);
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    EXPECT_EQ(parsed.dialect, anim::AnimatorFileDialect::UpstreamVersion1);
    const anim::AvatarMaskDef& data = parsed.data;
    EXPECT_EQ(data.defaultWeight, 0.0f);
    EXPECT_EQ(data.partWeight(AvatarBodyPart::RightArm), 1.0f);
    EXPECT_EQ(data.partWeight(AvatarBodyPart::RightHand), 1.0f) << "upstream's arm held the hand bone";
    EXPECT_EQ(data.partWeight(AvatarBodyPart::RightFingers), 0.5f);
    EXPECT_EQ(data.partWeight(AvatarBodyPart::Head), 0.0f) << "unset parts take default";
    EXPECT_FALSE(data.hasPart(AvatarBodyPart::Head));
    EXPECT_TRUE(data.rootHierarchy) << "upstream's root part covered the hierarchy above the hips";
    ASSERT_EQ(data.paths.size(), 2u);
    EXPECT_EQ(data.paths[0].path, "A");
    EXPECT_FALSE(data.paths[0].children);
    EXPECT_TRUE(data.paths[1].children) << "upstream's default: children";
    EXPECT_EQ(data.paths[1].weight, 1.0f) << "clamped";
    EXPECT_EQ(data.previewModel, "models/hero.glb");
    auto warned = [&](const std::string& path){
        for (const ClipWarning& warning : parsed.warnings){
            if (warning.path == path) return true;
        }
        return false;
    };
    EXPECT_TRUE(warned("transforms[1].path")) << "the entry without a path is dropped with its JSON path";
    EXPECT_TRUE(warned("humanoid.left_hand_ik"));
    EXPECT_TRUE(warned("humanoid.tail"));
    EXPECT_TRUE(warned("transforms[0].weight"));

    // saved as version 2, and canonical from there
    const std::string canonical = anim::writeMask(data);
    EXPECT_NE(canonical.find("\"version\": 2"), std::string::npos) << canonical;
    anim::MaskParseResult again = anim::parseMask(canonical);
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(again.dialect, anim::AnimatorFileDialect::Current);
    EXPECT_EQ(anim::writeMask(again.data), canonical) << "canonical round trip";
    EXPECT_NE(canonical.find("\"custom\": {\"x\":1}"), std::string::npos) << canonical;

    anim::MaskParseResult unity = anim::parseMask("%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!319 &31900000\nAvatarMask:\n");
    EXPECT_FALSE(unity.ok);
    EXPECT_EQ(unity.error.message, anim::unityYamlMaskRefusal());
    anim::MaskParseResult broken = anim::parseMask("{\"format\": \"jasperengine.mask\",\n \"version\": 1,,}");
    EXPECT_FALSE(broken.ok);
    EXPECT_EQ(broken.error.line, 2);
    EXPECT_FALSE(anim::parseMask("{\"format\": \"jasperengine.mask\", \"version\": 3}").ok) << "a newer version";
    EXPECT_FALSE(anim::parseMask("{\"format\": \"jasperengine.anim\", \"version\": 1}").ok);
}

TEST(ClipAdditiveReference, ReadWrittenAndClamped){
    ClipData data;
    data.tracks = {track("A", ClipChannel::Position, {key(0, {0, 0, 0}), key(2, {1, 0, 0})})};
    EXPECT_EQ(anim::writeClip(data).find("additive_reference"), std::string::npos) << "written only when not 0";
    data.additiveReference = 0.5f;
    const std::string text = anim::writeClip(data);
    EXPECT_NE(text.find("\"additive_reference\": 0.5"), std::string::npos) << text;
    anim::ClipParseResult parsed = anim::parseClip(text);
    ASSERT_TRUE(parsed.ok());
    EXPECT_FLOAT_EQ(parsed.data->additiveReference, 0.5f);
    EXPECT_FLOAT_EQ(AnimationClip::build(parsed)->getAdditiveReference(), 0.5f);
    data.additiveReference = 5.0f;
    std::vector<ClipWarning> warnings;
    anim::normalize(data, &warnings);
    EXPECT_FLOAT_EQ(data.additiveReference, 2.0f) << "clamped to the length";
    EXPECT_FALSE(warnings.empty());
}

// Test 12: the layers' runtime weights survive a clip list edit
TEST_F(Layers, LayerRuntimeSurvivesAReload){
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 1}}, {layer(anim::BASE_LAYER_NAME), layer("Holding", 1.0f)});
    object.play("walk");
    object.play("hold");
    step(0.1);
    object.setLayerWeight("Holding", 0.3f);
    object.addClip(clip("extra", walk()), "extra");
    step(0.1);
    EXPECT_NEAR(object.getLayerWeight("Holding"), 0.3f, 1e-6f);
    EXPECT_TRUE(object.isPlaying("hold"));
    EXPECT_EQ(object.getState("hold").getLayer(), 1);
    // an authored change of the weight is taken on the reload
    scene.getComponent<AnimationPlayerComponent>(root).layers[1].weight = 0.8f;
    scene.getComponent<AnimationPlayerComponent>(root).needsLoad = true;
    step(0.0);
    EXPECT_NEAR(object.getLayerWeight("Holding"), 0.8f, 1e-6f);
    // fades are linear in the player's time
    object.setLayerWeight(1, 0.0f, 0.4f);
    step(0.1);
    EXPECT_NEAR(object.getLayerWeight(1), 0.6f, 1e-5f);
    step(0.4);
    EXPECT_NEAR(object.getLayerWeight(1), 0.0f, 1e-6f);
    object.setLayerWeight(0, 0.5f);
    EXPECT_NEAR(object.getLayerWeight(0), 1.0f, 1e-6f) << "the base layer is always 1";
}

// L5: a higher Override layer without a mask leaves the channels it does not animate
TEST_F(Layers, LayersWriteChannelsIndependently){
    ClipData armOnly;
    armOnly.wrap = ClipWrapMode::Loop;
    armOnly.tracks = {track("Arm", ClipChannel::Rotation, {key(0, axisAngle(45, 1, 0, 0)), key(1, axisAngle(45, 1, 0, 0))})};
    AnimationPlayer object = player({{"walk", walk(), 0}, {"arm", armOnly, 1}}, {layer(anim::BASE_LAYER_NAME), layer("Upper")});
    object.play("walk");
    object.play("arm");
    step(0.25, 2);
    EXPECT_NEAR(transform(leg).position.x, 1.0f, 1e-5f);
    EXPECT_GT(quatDot(transform(arm).rotation, rotation(45, 1, 0, 0)), 1.0f - 1e-5f);
    // a missing mask makes the layer contribute nothing (spec 5.7, L6)
    object.setLayerMask("Upper", prefix + "missing.mask");
    step(0.25);
    EXPECT_GT(quatDot(transform(arm).rotation, rotation(67.5f, 0, 0, 1)), 1.0f - 1e-5f);
    EXPECT_EQ(object.getLayerMask("Upper"), prefix + "missing.mask");
}

TEST_F(Layers, AuthoringLayersFromCode){
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 0}}, {});
    EXPECT_EQ(object.getLayerCount(), 1);
    EXPECT_EQ(object.getLayerName(0), anim::BASE_LAYER_NAME);
    EXPECT_EQ(object.addLayer("Holding", 0.5f, "additive"), 1);
    EXPECT_EQ(object.addLayer("Holding"), -1) << "names are unique";
    EXPECT_EQ(object.getLayerCount(), 2);
    EXPECT_EQ(object.getLayerBlendMode("Holding"), "additive");
    EXPECT_NEAR(object.getLayerWeight("Holding"), 0.5f, 1e-6f);
    object.getState("hold").setLayer(1);
    EXPECT_EQ(object.getState("hold").getLayer(), 1);
    EXPECT_EQ(object.getLayerIndex("nope"), -1);
    EXPECT_EQ(object.getLayerWeight("nope"), 0.0f);
    scene.getComponent<AnimationPlayerComponent>(root).clips[1].layer = 1;
    EXPECT_TRUE(object.removeLayer("Holding"));
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(root).clips[1].layer, 0) << "its entries move to the base layer";
    EXPECT_FALSE(object.removeLayer(0));
    EXPECT_EQ(object.getLayerCount(), 1);
    EXPECT_TRUE(scene.getComponent<AnimationPlayerComponent>(root).layers.empty()) << "back to the default";
}

TEST_F(Layers, LayeredEditModePreview){
    const std::string armPath = mask("arm", armMask());
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), layer("Holding", 0.2f, AnimationLayerBlend::Override, armPath)});
    ActionSystem* system = scene.getSystem<ActionSystem>().get();
    anim::ClipPreviewSession session;
    ActionSystem::PlayerPreview preview;
    preview.layered = true;
    preview.layers = {ActionSystem::PlayerPreviewLayer(), ActionSystem::PlayerPreviewLayer()};
    preview.layers[1].mask = armPath;
    ActionSystem::PlayerPreviewState base;
    base.state = "walk";
    base.time = 0.5f;
    ActionSystem::PlayerPreviewState edited;
    edited.state = "hold";
    edited.time = 0.0f;
    edited.layer = 1;
    preview.states = {base, edited};
    ActionSystem::PlayerPreviewResult result = system->previewPlayer(root, preview, session);
    EXPECT_TRUE(result.ready);
    EXPECT_GT(quatDot(transform(arm).rotation, rotation(45, 1, 0, 0)), 1.0f - 1e-5f) << "the edited clip at weight 1 on its layer";
    EXPECT_NEAR(transform(leg).position.x, 1.0f, 1e-5f);
    system->restorePlayerPreview(session);
    EXPECT_NEAR(transform(leg).position.x, 0.0f, 1e-5f);
    EXPECT_NEAR(transform(hand).position.y, 7.0f, 1e-5f);
    (void)object;
}

TEST_F(Layers, LuaApi){
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 1}}, {layer(anim::BASE_LAYER_NAME), layer("Holding", 0.0f)});
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    luabridge::setGlobal(L, object, "layersPlayer");
    ASSERT_EQ(luaL_dostring(L,
        "local p = layersPlayer\n"
        "p:play('walk')\n"
        "p:play('hold', 0, PlayMode.STOP_SAME_LAYER)\n"
        "p:setLayerWeight('Holding', 0.25)\n"
        "p:getState('hold'):addMixingTransform('Arm', true)\n"
        "layersResult = tostring(p:getLayerCount()) .. '|' .. p:getLayerName(1) .. '|' .. tostring(p:getLayerIndex('Holding')) .. '|' ..\n"
        "  tostring(p:getLayerWeight(1)) .. '|' .. p:getLayerBlendMode('Holding') .. '|' .. tostring(p:getState('hold').layer) .. '|' ..\n"
        "  tostring(p:isPlaying('walk'))\n"
        "p:play('walk', 0, PlayMode.STOP_ALL)\n"
        "layersResult = layersResult .. '|' .. tostring(p:isPlaying('hold'))\n"), LUA_OK) << lua_tostring(L, -1);
    lua_getglobal(L, "layersResult");
    const std::string result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "2|Holding|1|0.25|override|1|true|false");
    luabridge::setGlobal(L, luabridge::LuaRef(L), "layersPlayer");
}

// Spec 13 (host check): 40 characters x 65 bones, each with two player layers (the upper one an
// Override layer masked to half the bones) and a crossfade on the base: under 0.6 ms per frame
// for the whole ActionSystem update (Release). (Upstream measured its controller on the player;
// controllers play on S21's Animator, whose budgets are je-bench-anim's.)
TEST_F(Layers, PerformanceBudget){
    // Release: the budget; Debug and sanitizer builds (5-50x slower): a short run against a loose
    // bound (as SchedulerTest), so the workload still runs there
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(NDEBUG)
    constexpr double BUDGET_MS = 60.0;
    constexpr int BATCHES = 1;
    constexpr int FRAMES = 5;
#else
    constexpr double BUDGET_MS = 0.6;
    constexpr int BATCHES = 8;
    constexpr int FRAMES = 30;
#endif
    constexpr int CHARACTERS = 40;
    constexpr int BONES = 65;
    ClipData walkData, runData, holdData;
    walkData.wrap = runData.wrap = holdData.wrap = ClipWrapMode::Loop;
    anim::AvatarMaskDef maskData;
    maskData.defaultWeight = 0.0f;
    std::string chain;
    for (int b = 0; b < BONES; b++){
        chain += (b ? "/" : "") + std::string("B") + std::to_string(b);
        walkData.tracks.push_back(track(chain, ClipChannel::Rotation, {key(0, axisAngle(0, 0, 0, 1)), key(1, axisAngle(40, 0, 0, 1))}));
        runData.tracks.push_back(track(chain, ClipChannel::Rotation, {key(0, axisAngle(10, 1, 0, 0)), key(0.5f, axisAngle(30, 1, 0, 0)), key(1, axisAngle(10, 1, 0, 0))}));
        if (b == 0) runData.tracks.push_back(track(chain, ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 1, 0})}));
        if (b >= 32){
            holdData.tracks.push_back(track(chain, ClipChannel::Rotation, {key(0, axisAngle(20, 0, 1, 0)), key(1, axisAngle(25, 0, 1, 0))}));
        }
        if (b == 32) maskData.paths.push_back({chain, 1.0f, true});
    }
    const std::string walkPath = clip("pwalk", walkData), runPath = clip("prun", runData), holdPath = clip("phold", holdData);
    const std::string maskPath = mask("pmask", maskData);
    std::vector<AnimationPlayer> players;
    for (int c = 0; c < CHARACTERS; c++){
        Entity character = scene.createEntity();
        scene.addComponent<Transform>(character, {});
        Entity parent = character;
        for (int b = 0; b < BONES; b++) parent = child(parent, "B" + std::to_string(b));
        AnimationPlayer object(&scene, character);
        AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(character);
        AnimationPlayerClip walkEntry, runEntry, holdEntry;
        walkEntry.clip = walkPath;
        walkEntry.name = "Walk";
        runEntry.clip = runPath;
        runEntry.name = "Run";
        holdEntry.clip = holdPath;
        holdEntry.name = "Hold";
        holdEntry.layer = 1;
        component.clips = {walkEntry, runEntry, holdEntry};
        component.layers = {layer(anim::BASE_LAYER_NAME), layer("Holding", 1.0f, AnimationLayerBlend::Override, maskPath)};
        component.playAutomatically = false;
        component.needsLoad = true;
        players.push_back(object);
    }
    step(1.0 / 60.0, 3);
    for (AnimationPlayer& object : players){
        object.play("Walk");
        object.play("Hold");
        ASSERT_EQ(object.getLayerCount(), 2);
    }
    step(1.0 / 60.0, 3);
    for (AnimationPlayer& object : players) object.crossFade("Run", 100.0f);
    step(1.0 / 60.0, 3);
    ASSERT_GT(players[0].getStateInfo("Run").weight, 0.0f);
    ASSERT_LT(players[0].getStateInfo("Run").weight, 1.0f);
    // the best of a few batches: the host is shared with other builds
    double ms = 1e9;
    for (int batch = 0; batch < BATCHES; batch++){
        const int frames = FRAMES;
        const auto start = std::chrono::steady_clock::now();
        step(1.0 / 60.0, frames);
        ms = std::min(ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames);
    }
    ASSERT_LT(players[0].getStateInfo("Run").weight, 1.0f);
    std::printf("[ perf ] 40 characters x 65 bones, masked layer, crossfade: %.3f ms per frame\n", ms);
    // the budget is the update's cost, not an overloaded host's scheduling (as CanvasGroupTest):
    // over it on an overloaded host says nothing (skipped); under it passes whatever the load
#if defined(__unix__) || defined(__APPLE__)
    double load[1] = {0.0};
    if (ms >= BUDGET_MS && getloadavg(load, 1) == 1 && load[0] > (double)std::thread::hardware_concurrency()){
        GTEST_SKIP() << "load average " << load[0] << " above " << std::thread::hardware_concurrency() << " cores (measured " << ms << " ms)";
    }
#endif
    EXPECT_LT(ms, BUDGET_MS) << ms << " ms per frame";
}

TEST_F(Layers, RemovingALayerShiftsRuntimeMoves){
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 0}}, {layer(anim::BASE_LAYER_NAME), layer("A"), layer("B")});
    step(0.0);
    object.getState("hold").setLayer(2);
    EXPECT_TRUE(object.removeLayer("A"));
    step(0.0);
    EXPECT_EQ(object.getState("hold").getLayer(), 1) << "it stays on B";
    EXPECT_EQ(object.getLayerName(1), "B");
}

// independent review: skipping the bones a full Override layer replaces gives exactly the pose of
// composing them (fractional state weights inside the layer included); a loading or failed mask
// makes its layer contribute nothing
TEST_F(Layers, OcclusionMatchesTheFullComposition){
    ClipData hold2 = hold();
    hold2.tracks[0].keys[0].value = axisAngle(-30, 0, 1, 0);
    hold2.tracks[0].keys[1].value = axisAngle(-30, 0, 1, 0);
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 1}, {"hold2", hold2, 1}},
                                    {layer(anim::BASE_LAYER_NAME), layer("Upper", 1.0f)});
    object.play("walk");
    object.play("hold");
    object.blend("hold2", 0.4f, 0.0f);
    object.getState("hold").setWeight(0.6f);
    step(0.25, 2);
    const Quaternion occluded = transform(arm).rotation;
    // the same frame with the upper layer just under 1: no occlusion, the full composition
    object.setLayerWeight("Upper", 0.9999999f);
    object.getState("walk").setTime(0.5f);
    object.getState("hold").setTime(0.5f);
    object.getState("hold2").setTime(0.5f);
    object.sample();
    const Quaternion composed = transform(arm).rotation;
    EXPECT_GT(quatDot(occluded, composed), 1.0f - 1e-5f);
    const Quaternion expected = Quaternion::slerp(0.4f, rotation(45, 1, 0, 0), rotation(-30, 0, 1, 0), true);
    EXPECT_GT(quatDot(occluded, anim::poseBlendRotation(rotation(45, 1, 0, 0), rotation(-30, 0, 1, 0), 0.4f)), 1.0f - 1e-5f)
        << "the states' weights inside a layer are a normalized sum: S21's nlerp";
    EXPECT_GT(quatDot(occluded, expected), 1.0f - 1e-3f) << "weights 0.6 / 0.4 inside the layer (normalized sum ~ slerp)";
    EXPECT_NEAR(transform(leg).position.x, 1.0f, 1e-5f) << "the base still owns the leg";
}

TEST_F(Layers, ALayerWithoutItsMaskContributesNothing){
    const std::string broken = prefix + "broken.mask";
    animtest::writeAsset(broken, "{\"format\": \"jasperengine.mask\", \"version\": 1,,}");
    AnimationPlayer object = player({{"walk", walk(), 0}, {"hold", hold(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), layer("Upper", 1.0f, AnimationLayerBlend::Override, broken)});
    object.play("walk");
    object.play("hold");
    step(0.25, 2);
    EXPECT_GT(quatDot(transform(arm).rotation, rotation(45, 0, 0, 1)), 1.0f - 1e-5f) << "a mask that does not parse: the walk shows";
    EXPECT_NEAR(transform(hand).position.y, 7.0f, 1e-5f);
    // fixed on disk: the layer takes effect after the retry (1 s) once the pool reloads it
    animtest::writeAsset(broken, anim::writeMask(armMask()));
    AnimatorAssetPool::invalidate(broken);
    step(0.6, 2);
    EXPECT_NEAR(transform(hand).position.y, 1.0f, 1e-5f);
}
