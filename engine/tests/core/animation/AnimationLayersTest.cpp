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
#include "AnimatorTestUtil.h"
#include "../pose/TestRigs.h"
#include "support/AllocationCounter.h"

#include "Engine.h"
#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorInstance.h"
#include "animation/AnimatorPose.h"
#include "animation/AvatarMaskWeights.h"
#include "component/AnimatorComponent.h"
#include "object/AnimationPlayer.h"
#include "object/Animator.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "script/LuaBinding.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/AnimatorSystem.h"

#include "lua.hpp"
#include "LuaBridge.h"
#include "support/AllocationCounter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

using namespace jasperengine;
using namespace animatortest;

namespace {

    constexpr double ANIMATION_LAYERS_PI = 3.14159265358979323846;

    // (x, y, z, w) of a rotation of `degrees` about an axis
    std::vector<float> axisAngle(float degrees, float ax, float ay, float az){
        const double half = degrees * ANIMATION_LAYERS_PI / 360.0;
        const float s = (float)std::sin(half);
        return {ax * s, ay * s, az * s, (float)std::cos(half)};
    }

    Quaternion rotation(float degrees, float ax, float ay, float az){
        std::vector<float> q = axisAngle(degrees, ax, ay, az);
        return quaternionFromXYZW(q.data());
    }

    // walk: Leg x 0 -> 2 and Arm 0 -> 90 degrees about Z over 1 s
    ClipData animationLayersWalk(ClipWrapMode wrap = ClipWrapMode::Loop){
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

    float quatDot(const Quaternion& a, const Quaternion& b){
        return std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    }

    // ---- Mesh Space Rotation on the Animator (docs/specs/mesh-space-rotation.md) ----

    Quaternion yaw(float degrees){ return rotation(degrees, 0, 1, 0); }
    Quaternion tilt(float degrees){ return rotation(degrees, 1, 0, 0); }
    Quaternion roll(float degrees){ return rotation(degrees, 0, 0, 1); }

    std::vector<float> xyzw(const Quaternion& q){ return {q.x, q.y, q.z, q.w}; }

    // the angle between two rotations, degrees
    double degreesBetween(const Quaternion& a, const Quaternion& b){
        const double d = (double)a.w * b.w + (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
        const double n = std::sqrt(((double)a.w * a.w + (double)a.x * a.x + (double)a.y * a.y + (double)a.z * a.z) *
                                   ((double)b.w * b.w + (double)b.x * b.x + (double)b.y * b.y + (double)b.z * b.z));
        return 2.0 * std::acos(std::min(1.0, std::fabs(d) / n)) * 180.0 / ANIMATION_LAYERS_PI;
    }

    // R7's bound: the largest component difference, b on a's hemisphere
    float componentError(const Quaternion& a, const Quaternion& b){
        const float s = a.dot(b) < 0.0f ? -1.0f : 1.0f;
        return std::max(std::max(std::fabs(a.w - s * b.w), std::fabs(a.x - s * b.x)), std::max(std::fabs(a.y - s * b.y), std::fabs(a.z - s * b.z)));
    }

    // bones held at constant rotations (and positions) for `length` seconds
    ClipData still(const std::vector<std::pair<std::string, Quaternion>>& rotations, const std::vector<std::pair<std::string, Vector3>>& positions = {},
                   float length = 1.0f){
        ClipData data;
        data.wrap = ClipWrapMode::Loop;
        for (const auto& r : rotations) data.tracks.push_back(track(r.first, ClipChannel::Rotation, {key(0, xyzw(r.second)), key(length, xyzw(r.second))}));
        for (const auto& p : positions){
            const std::vector<float> v = {p.second.x, p.second.y, p.second.z};
            data.tracks.push_back(track(p.first, ClipChannel::Position, {key(0, v), key(length, v)}));
        }
        return data;
    }

    // the run: hips turned 90 degrees, the legs and spine in their own pose
    ClipData runPose(){
        return still({{"Hips", yaw(90)}, {"Hips/Spine", tilt(10)}, {"Hips/Thigh", tilt(-30)}, {"Hips/Thigh/Foot", tilt(20)}},
                     {{"Hips", Vector3(0, 0.95f, 0.1f)}});
    }

    // the attack: hips turned -20 degrees, its own spine, head and legs
    ClipData attackPose(){
        return still({{"Hips", yaw(-20)}, {"Hips/Spine", tilt(40)}, {"Hips/Spine/Head", roll(15)}, {"Hips/Thigh", tilt(50)},
                      {"Hips/Thigh/Foot", tilt(-10)}},
                     {{"Hips", Vector3(0.2f, 0.8f, 0)}, {"Hips/Spine", Vector3(0, 0.35f, 0.05f)}});
    }

    // a mask of the path's GameObject and everything under it
    anim::AvatarMaskDef pathMask(const std::string& path){
        anim::AvatarMaskDef data;
        data.defaultWeight = 0.0f;
        data.paths.push_back({path, 1.0f, true});
        return data;
    }

    anim::LayerDef animatorLayer(const std::string& name, const std::string& clip, const std::string& mask = "", bool meshSpace = false,
                                 float weight = 1.0f){
        anim::LayerDef layer;
        layer.name = name;
        layer.weight = weight;
        layer.mask.path = mask;
        layer.meshSpaceRotation = meshSpace;
        anim::StateDef state;
        state.name = name + " State";
        state.motion.clip.path = clip;
        layer.machine.states.push_back(state);
        return layer;
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

        // a player with entries (name, clip, layer) and layers (on `on`, the root by default)
        AnimationPlayer player(const std::vector<std::tuple<std::string, ClipData, int>>& clips, const std::vector<AnimationPlayerLayer>& layers,
                               Entity on = NULL_ENTITY){
            AnimationPlayer object(&scene, on == NULL_ENTITY ? root : on);
            AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(on == NULL_ENTITY ? root : on);
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

        // ---- the Animator's layers (Mesh Space Rotation) ----

        // Hips -> Spine -> Head and Hips -> Thigh -> Foot under `under`, in that order
        std::vector<Entity> body(Entity under){
            const Entity h = child(under, "Hips", Vector3(0, 1, 0));
            const Entity s = child(h, "Spine", Vector3(0, 0.3f, 0));
            const Entity hd = child(s, "Head", Vector3(0, 0.4f, 0));
            const Entity t = child(h, "Thigh", Vector3(0.1f, -0.5f, 0));
            const Entity f = child(t, "Foot", Vector3(0, -0.45f, 0));
            return {h, s, hd, t, f};
        }

        // the body under Hero
        void addBody(){
            const std::vector<Entity> bones = body(root);
            hips = bones[0];
            spine = bones[1];
            head = bones[2];
            thigh = bones[3];
            foot = bones[4];
        }

        std::string controllerFile(const std::string& name, const anim::ControllerDef& def){
            animtest::writeAsset(prefix + name + ".controller", anim::writeController(def));
            return prefix + name + ".controller";
        }

        AnimatorComponent& animator(const std::string& controllerPath, Entity on = NULL_ENTITY){
            const Entity entity = on == NULL_ENTITY ? root : on;
            if (!scene.findComponent<AnimatorComponent>(entity)) scene.addComponent<AnimatorComponent>(entity, {});
            AnimatorComponent& component = scene.getComponent<AnimatorComponent>(entity);
            component.controller = controllerPath;
            component.needsLoad = true;
            return component;
        }

        anim::AnimatorInstance& instance(Entity on = NULL_ENTITY){
            return *scene.getComponent<AnimatorComponent>(on == NULL_ENTITY ? root : on).runtime;
        }

        void animate(double dt, int count = 1){
            for (int i = 0; i < count; i++) scene.getSystem<AnimatorSystem>()->update(dt);
        }

        // a bone's rig-space rotation in the last evaluation
        Quaternion rig(const std::string& path, Entity on = NULL_ENTITY){
            const anim::AnimatorInstance& i = instance(on);
            const int bone = i.rig.findBone(path);
            EXPECT_GE(bone, 0) << path;
            return bone >= 0 ? i.rigRotations()[(size_t)bone] : Quaternion();
        }

        // the rig-space rotations of a pose of the instance's rig (poseToRigSpace: the reference)
        static std::vector<Quaternion> rigOf(const anim::AnimatorInstance& i, const anim::Pose& pose){
            const size_t n = i.rig.bones.size();
            std::vector<anim::Affine> world(n);
            std::vector<Vector3> positions(n), scales(n);
            std::vector<Quaternion> rotations(n);
            anim::poseToRigSpace(pose, i.rig.parents.data(), anim::RigSpace{world.data(), positions.data(), rotations.data(), scales.data()});
            return rotations;
        }

        // ---- the Animation Player's layers (Mesh Space Rotation) ----

        AnimationPlayerLayer meshSpaceLayer(const std::string& name, const std::string& maskPath, bool meshSpace = true, float weight = 1.0f){
            AnimationPlayerLayer l = layer(name, weight, AnimationLayerBlend::Override, maskPath);
            l.meshSpaceRotation = meshSpace;
            return l;
        }

        // a new GameObject at the scene's top level
        Entity topLevel(const std::string& name){
            const Entity entity = scene.createEntity();
            scene.setEntityName(entity, name);
            scene.addComponent<Transform>(entity, {});
            return entity;
        }

        // a GameObject's rotation relative to the Player's GameObject `under` (Hero by default):
        // the Player's rig space
        Quaternion playerRig(Entity entity, Entity under = NULL_ENTITY){
            if (under == NULL_ENTITY) under = root;
            Quaternion q = transform(entity).rotation;
            for (Entity p = transform(entity).parent; p != NULL_ENTITY && p != under; p = transform(p).parent) q = transform(p).rotation * q;
            return q;
        }

        // each GameObject's position, rotation (w x y z) and scale: 10 floats each
        std::vector<float> transformsOf(const std::vector<Entity>& entities){
            std::vector<float> out;
            for (Entity entity : entities){
                const Transform& t = transform(entity);
                for (float v : {t.position.x, t.position.y, t.position.z, t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z, t.scale.x,
                                t.scale.y, t.scale.z}){
                    out.push_back(v);
                }
            }
            return out;
        }

        // the T-pose humanoid (Mixamo names) under a new root at its rest pose: its bones' entities
        // and paths from the root
        struct Humanoid{
            Entity root = NULL_ENTITY;
            std::vector<Entity> bones;
            std::vector<std::string> paths;
            std::vector<Quaternion> rest;

            std::string path(const std::string& name) const{
                const posetest::TestRig& rig = posetest::tposeRig();
                for (size_t i = 0; i < rig.skeleton.bones.size(); i++){
                    if (rig.skeleton.bones[i].name == name) return paths[i];
                }
                ADD_FAILURE() << "no bone " << name;
                return std::string();
            }
        };

        Humanoid humanoid(const std::string& name){
            const posetest::TestRig& rig = posetest::tposeRig();
            Humanoid h;
            h.root = scene.createEntity();
            scene.setEntityName(h.root, name);
            scene.addComponent<Transform>(h.root, {});
            for (size_t i = 0; i < rig.skeleton.bones.size(); i++){
                const pose::SkeletonBone& b = rig.skeleton.bones[i];
                const Entity parent = b.parent >= 0 ? h.bones[(size_t)b.parent] : h.root;
                Entity bone = child(parent, b.name, b.position);
                transform(bone).rotation = b.rotation;
                transform(bone).scale = b.scale;
                h.bones.push_back(bone);
                h.paths.push_back(b.parent >= 0 ? h.paths[(size_t)b.parent] + "/" + b.name : b.name);
                h.rest.push_back(b.rotation);
            }
            return h;
        }

        // every humanoid bone keyed at its rest rotation, the named ones turned in their parent's
        // space (`turns`: bone name, rotation)
        static ClipData humanoidPose(const Humanoid& h, const std::map<std::string, Quaternion>& turns){
            const posetest::TestRig& rig = posetest::tposeRig();
            std::vector<std::pair<std::string, Quaternion>> rotations;
            for (size_t i = 0; i < rig.skeleton.bones.size(); i++){
                auto turn = turns.find(rig.skeleton.bones[i].name);
                rotations.push_back({h.paths[i], turn != turns.end() ? turn->second * h.rest[i] : h.rest[i]});
            }
            return still(rotations);
        }

        Scene scene;
        std::string prefix;
        Entity root = NULL_ENTITY;
        Entity arm = NULL_ENTITY;
        Entity hand = NULL_ENTITY;
        Entity leg = NULL_ENTITY;
        Entity hips = NULL_ENTITY, spine = NULL_ENTITY, head = NULL_ENTITY, thigh = NULL_ENTITY, foot = NULL_ENTITY;
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
    ClipData run = animationLayersWalk();
    run.tracks[1].keys[1].value = {6, 0, 0};
    run.tracks[0].keys[1].value = axisAngle(-60, 0.6f, 0, 0.8f);
    AnimationPlayer object(&scene, root);
    object.addClip(clip("walk", animationLayersWalk()), "walk");
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}},
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}},
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}},
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(ClipWrapMode::ClampForever), 0}, {"nod", nod, 1}},
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}, {"hold2", hold(), 1}},
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(ClipWrapMode::Once, 0.5f), 1}, {"hold2", hold(), 1}},
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
    ClipData walkEvents = animationLayersWalk();
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}}, {});
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

// docs/specs/mesh-space-rotation.md R5: the boundary bones are where the mask starts (the weight
// rises from the parent's) and where it stops right under a start; elsewhere a bone follows its
// parent (Unity's rule). A child of the root (parent -1) is never one, a uniform mask has none
TEST(AvatarMaskBoundary, BonesWhereTheMaskStartsOrStopsUnderItsStart){
    // Hips (-1) > Spine > Chest > Neck, Hips > LeftUpLeg > LeftLeg, Chest > LeftArm
    const int parents[] = {-1, 0, 1, 2, 0, 4, 2};
    std::vector<uint16_t> boundary = {42};
    // a path mask from the spine
    const float spineUp[] = {0, 1, 1, 1, 0, 0, 1};
    anim::maskBoundaryBones(spineUp, parents, std::size(parents), -1, boundary);
    EXPECT_EQ(boundary, (std::vector<uint16_t>{1})) << "Spine only (and the list cleared first)";
    // a body mask with the torso on and the head and arms off: the mask starts at the hips (right
    // under the root), so it stops under them at the legs; the neck and the arm follow the chest
    const float body[] = {1, 1, 1, 0, 0, 0, 0};
    anim::maskBoundaryBones(body, parents, std::size(parents), -1, boundary);
    EXPECT_EQ(boundary, (std::vector<uint16_t>{4})) << "LeftUpLeg";
    // a lower-body mask: the hips and legs on, the spine off, stops right under its start
    const float lower[] = {1, 0, 0, 0, 1, 1, 0};
    anim::maskBoundaryBones(lower, parents, std::size(parents), -1, boundary);
    EXPECT_EQ(boundary, (std::vector<uint16_t>{1})) << "Spine";
    const float all[] = {1, 1, 1, 1, 1, 1, 1};
    anim::maskBoundaryBones(all, parents, std::size(parents), -1, boundary);
    EXPECT_TRUE(boundary.empty()) << "an unmasked layer has none";
    // fractional steps: every rise is a start, a drop counts right under one
    const float fractional[] = {0, 0.5f, 1, 0.25f, 0, 0, 1};
    anim::maskBoundaryBones(fractional, parents, std::size(parents), -1, boundary);
    EXPECT_EQ(boundary, (std::vector<uint16_t>{1, 2, 3})) << "Spine, Chest (rises), Neck (drops under the chest's rise)";
    const float rootOnly[] = {1, 0, 0, 0, 0, 0, 0};
    const int roots[] = {-1, -1, -1, -1, -1, -1, -1};
    anim::maskBoundaryBones(rootOnly, roots, std::size(roots), -1, boundary);
    EXPECT_TRUE(boundary.empty()) << "children of the root keep the root's space";
}

// D2: fingers left out of an arm mask follow the hand; a head left out of a torso mask follows the
// neck's parent; on a body-part mask on from the root, the body starts at the humanoid's hips
TEST(AvatarMaskBoundary, BonesUnderAContinuingMaskFollowTheirParent){
    // Armature (-1) > Hips > Spine > Chest > Shoulder > UpperArm > Hand > Finger1, Hand > Finger2,
    // Chest > Neck > Head, Hips > LeftUpLeg
    const int parents[] = {-1, 0, 1, 2, 3, 4, 5, 6, 6, 3, 9, 1};
    std::vector<uint16_t> boundary;
    const float arm[] = {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0};
    anim::maskBoundaryBones(arm, parents, std::size(parents), 1, boundary);
    EXPECT_EQ(boundary, (std::vector<uint16_t>{4})) << "the shoulder; the fingers follow the hand";
    const float torso[] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0};
    anim::maskBoundaryBones(torso, parents, std::size(parents), 1, boundary);
    EXPECT_EQ(boundary, (std::vector<uint16_t>{2})) << "the spine; the head follows the neck";
    // the hips and legs under an Armature the mask leaves out: the mask starts at the hips
    const float lower[] = {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    anim::maskBoundaryBones(lower, parents, std::size(parents), -1, boundary);
    EXPECT_EQ(boundary, (std::vector<uint16_t>{1, 2})) << "Hips (starts), Spine (stops under it)";
    // everything but the legs, on from the root: the hips start the body of a body-part mask
    const float noLegs[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
    anim::maskBoundaryBones(noLegs, parents, std::size(parents), -1, boundary);
    EXPECT_TRUE(boundary.empty()) << "a path mask: the mask starts at the Armature, the legs follow the hips";
    anim::maskBoundaryBones(noLegs, parents, std::size(parents), 1, boundary);
    EXPECT_EQ(boundary, (std::vector<uint16_t>{11})) << "with the hips: LeftUpLeg";
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}}, {layer(anim::BASE_LAYER_NAME), layer("Holding", 1.0f)});
    object.play("walk");
    object.play("hold");
    step(0.1);
    object.setLayerWeight("Holding", 0.3f);
    object.addClip(clip("extra", animationLayersWalk()), "extra");
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"arm", armOnly, 1}}, {layer(anim::BASE_LAYER_NAME), layer("Upper")});
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 0}}, {});
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}},
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}}, {layer(anim::BASE_LAYER_NAME), layer("Holding", 0.0f)});
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

// docs/specs/mesh-space-rotation.md R17: the Player's setter sets the authored field (by index or
// name, a reload follows), unlike the runtime weight / mask / blend setters
TEST_F(Layers, LuaMeshSpaceRotationSetsTheAuthoredLayer){
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}}, {layer(anim::BASE_LAYER_NAME), layer("Holding")});
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    step(0.1);
    ASSERT_FALSE(component.needsLoad);
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    luabridge::setGlobal(L, object, "meshSpacePlayer");
    ASSERT_EQ(luaL_dostring(L,
        "local p = meshSpacePlayer\n"
        "meshSpaceBefore = tostring(p:getLayerMeshSpaceRotation('Holding'))\n"
        "p:setLayerMeshSpaceRotation('Holding', true)\n"), LUA_OK) << lua_tostring(L, -1);
    EXPECT_TRUE(component.layers[1].meshSpaceRotation) << "the authored field";
    EXPECT_TRUE(component.needsLoad) << "the player reloads its layers";
    ASSERT_EQ(luaL_dostring(L, "meshSpaceAfter = tostring(meshSpacePlayer:getLayerMeshSpaceRotation(1))"), LUA_OK) << lua_tostring(L, -1);
    lua_getglobal(L, "meshSpaceBefore");
    const std::string before = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_getglobal(L, "meshSpaceAfter");
    const std::string after = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 2);
    EXPECT_EQ(before, "false");
    EXPECT_EQ(after, "true");
    EXPECT_FALSE(component.layers[0].meshSpaceRotation);
    step(0.1);
    EXPECT_TRUE(component.layers[1].meshSpaceRotation) << "kept through the reload";
    ASSERT_EQ(luaL_dostring(L, "meshSpacePlayer:setLayerMeshSpaceRotation(1, false)"), LUA_OK) << lua_tostring(L, -1);
    EXPECT_FALSE(component.layers[1].meshSpaceRotation);
    EXPECT_NE(luaL_dostring(L, "meshSpacePlayer:setLayerMeshSpaceRotation(1, 'yes')"), LUA_OK) << "enabled is a boolean";
    lua_pop(L, 1);
    // an unknown layer warns once and changes nothing
    ASSERT_EQ(luaL_dostring(L, "meshSpacePlayer:setLayerMeshSpaceRotation('Nope', true)"), LUA_OK) << lua_tostring(L, -1);
    EXPECT_FALSE(component.layers[1].meshSpaceRotation);
    const std::vector<std::string> warnings = animtest::takeWarnings();
    ASSERT_FALSE(warnings.empty());
    EXPECT_NE(warnings.back().find("no layer named 'Nope'"), std::string::npos) << warnings.back();
    luabridge::setGlobal(L, luabridge::LuaRef(L), "meshSpacePlayer");

    // a player with the default layering gets its list (one Base Layer, where it is kept and ignored)
    AnimationPlayerComponent& plain = component;
    plain.layers.clear();
    plain.needsLoad = true;
    step(0.1);
    object.setLayerMeshSpaceRotation(0, true);
    ASSERT_EQ(plain.layers.size(), 1u);
    EXPECT_EQ(plain.layers[0].name, anim::BASE_LAYER_NAME);
    EXPECT_TRUE(plain.layers[0].meshSpaceRotation);
    EXPECT_TRUE(object.getLayerMeshSpaceRotation(anim::BASE_LAYER_NAME));
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
    // the budget is the update's cost, not a loaded host's scheduling: enforced where
    // perfSlack() asks for it (the benchmark run), printed always
    if (const double slack = testsupport::perfSlack(); slack > 0.0){
        EXPECT_LT(ms, BUDGET_MS * slack) << ms << " ms per frame";
    }
}

TEST_F(Layers, RemovingALayerShiftsRuntimeMoves){
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 0}}, {layer(anim::BASE_LAYER_NAME), layer("A"), layer("B")});
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}, {"hold2", hold2, 1}},
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
    AnimationPlayer object = player({{"walk", animationLayersWalk(), 0}, {"hold", hold(), 1}},
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

// ---- Mesh Space Rotation on the Animator's layers (docs/specs/mesh-space-rotation.md) ----

// AC3: the boundary bones are where the mask weight changes from parent to child, listed for every
// masked layer above 0 (the option off too): a path mask from the spine gives the spine, a mask
// of everything but the legs (Body on) the legs' first bones, an unmasked layer none
TEST_F(Layers, MeshSpaceBoundaryBones){
    addBody();
    const std::string run = clip("run", runPose());
    const std::string attack = clip("attack", attackPose());
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", run), animatorLayer("Upper Body", attack, mask("spine", pathMask("Hips/Spine"))),
                  animatorLayer("Unmasked", attack, "", true, 0.0f)};
    animator(controllerFile("bones", def));
    animate(0.0);
    anim::AnimatorInstance& generic = instance();
    ASSERT_TRUE(generic.bound);
    EXPECT_EQ(generic.layerBoundaryBones(1), std::vector<uint16_t>{(uint16_t)generic.rig.findBone("Hips/Spine")});
    EXPECT_TRUE(generic.layerBoundaryBones(2).empty()) << "an unmasked layer has none";
    EXPECT_TRUE(generic.layerBoundaryBones(0).empty());
    EXPECT_TRUE(generic.layerBoundaryBones(7).empty());
    EXPECT_FALSE(generic.layerMeshSpaceApplies(1)) << "the option is off";
    EXPECT_FALSE(generic.layerMeshSpaceApplies(2)) << "on, but nothing to apply at";
    EXPECT_EQ(generic.meshSpaceScratchBytes(), 0u) << "no layer runs the pass: no scratch";
    Animator object(&scene, root);
    EXPECT_EQ(object.getLayerBoundaryBones(1), std::vector<Entity>{spine});

    const Humanoid human = humanoid("Granjero");
    anim::AvatarMaskDef noLegs;
    noLegs.bodyParts[(int)AvatarBodyPart::LeftLeg] = 0.0f;
    noLegs.bodyParts[(int)AvatarBodyPart::RightLeg] = 0.0f;
    const ClipData rest = humanoidPose(human, {});
    anim::ControllerDef humanDef;
    humanDef.layers = {animatorLayer("Base Layer", clip("rest", rest)), animatorLayer("Upper Body", clip("rest2", rest), mask("nolegs", noLegs)),
                       animatorLayer("Torso", clip("rest3", rest), mask("torso", pathMask(human.path("mixamorig:Spine"))))};
    animator(controllerFile("human", humanDef), human.root);
    animate(0.0);
    const anim::AnimatorInstance& i = instance(human.root);
    ASSERT_TRUE(i.bound);
    ASSERT_TRUE(i.humanoid.valid) << i.humanoid.error;
    const std::vector<uint16_t>& legs = i.layerBoundaryBones(1);
    ASSERT_EQ(legs.size(), 2u);
    EXPECT_EQ(i.humanoidSlotOf(legs[0]), (int)pose::HumanBone::LeftUpperLeg);
    EXPECT_EQ(i.humanoidSlotOf(legs[1]), (int)pose::HumanBone::RightUpperLeg);
    ASSERT_EQ(i.layerBoundaryBones(2).size(), 1u);
    EXPECT_EQ(i.humanoidSlotOf(i.layerBoundaryBones(2)[0]), (int)pose::HumanBone::Spine) << "what the editor's warning names";
    EXPECT_EQ(i.humanoidSlotOf(i.rig.findBone("Armature")), -1);
}

// AC4, AC5: the base's hips turned 90 degrees, the layer's -20, a mask from the spine. With the
// option the spine (and what follows it) has the layer clip's rotation relative to the character
// and the legs the base's; without it the spine is 110 degrees off. Only the boundary bone's local
// rotation changes: translations, scales and every other rotation are poseBlend's, bit for bit.
TEST_F(Layers, MeshSpaceRotationKeepsTheLayerDirection){
    addBody();
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", runPose())),
                  animatorLayer("Upper Body", clip("attack", attackPose()), mask("spine", pathMask("Hips/Spine")), true)};
    animator(controllerFile("direction", def));
    animate(0.0);
    anim::AnimatorInstance& i = instance();
    ASSERT_TRUE(i.bound);
    ASSERT_TRUE(i.layerMeshSpaceApplies(1));
    EXPECT_EQ(i.meshSpaceScratchBytes(), 2 * i.rig.bones.size() * sizeof(Quaternion));
    const Quaternion layerSpine = yaw(-20) * tilt(40);
    EXPECT_LE(componentError(rig("Hips/Spine"), layerSpine), 1e-5f) << degreesBetween(rig("Hips/Spine"), layerSpine);
    EXPECT_LE(componentError(rig("Hips/Spine/Head"), layerSpine * roll(15)), 1e-5f) << "the head follows the corrected spine";
    EXPECT_LE(componentError(rig("Hips"), yaw(90)), 1e-5f) << "the hips are the base's";
    EXPECT_LE(componentError(rig("Hips/Thigh"), yaw(90) * tilt(-30)), 1e-5f) << "the legs are the base's";
    EXPECT_LE(componentError(rig("Hips/Thigh/Foot"), yaw(90) * tilt(-30) * tilt(20)), 1e-5f);
    const anim::Pose& pose = i.pose();
    std::vector<float> with(pose.raw(), pose.raw() + pose.floatCount());

    i.setLayerMeshSpaceRotation(1, false);
    EXPECT_EQ(i.meshSpaceScratchBytes(), 0u);
    animate(0.0);
    EXPECT_NEAR(degreesBetween(rig("Hips/Spine"), layerSpine), 110.0, 0.01) << "local blending: the spine inherits the base's hips";
    EXPECT_NEAR(degreesBetween(rig("Hips/Spine/Head"), layerSpine * roll(15)), 110.0, 0.01);
    EXPECT_LE(componentError(rig("Hips/Thigh"), yaw(90) * tilt(-30)), 1e-5f);
    const size_t stride = pose.stride();
    const size_t spineBone = (size_t)i.rig.findBone("Hips/Spine");
    for (int a = 0; a < anim::Pose::ARRAY_COUNT; a++){
        const bool rotation = a >= anim::Pose::QX && a <= anim::Pose::QW;
        for (size_t b = 0; b < stride; b++){
            if (rotation && b == spineBone) continue;
            EXPECT_EQ(std::memcmp(&with[(size_t)a * stride + b], pose.array((anim::Pose::Array)a) + b, sizeof(float)), 0)
                << "array " << a << ", bone " << b;
        }
    }
}

// AC5: a mask of everything but the legs takes the attack's hips; with the option the legs keep
// the run's rotation relative to the character (the other direction of the same problem). Every
// bone of mask weight 1 has the attack's rig rotation, every one of 0 the run's.
TEST_F(Layers, MeshSpaceRotationKeepsTheBaseLegsUnderABodyMask){
    const Humanoid human = humanoid("Granjero");
    const ClipData run = humanoidPose(human, {{"mixamorig:Hips", yaw(90)}, {"mixamorig:LeftUpLeg", tilt(-30)}, {"mixamorig:RightUpLeg", tilt(25)},
                                              {"mixamorig:Spine", tilt(10)}});
    const ClipData attack = humanoidPose(human, {{"mixamorig:Hips", yaw(-20)}, {"mixamorig:Spine", tilt(40)}, {"mixamorig:LeftUpLeg", tilt(50)},
                                                 {"mixamorig:RightArm", roll(60)}});
    anim::AvatarMaskDef noLegs;
    noLegs.bodyParts[(int)AvatarBodyPart::LeftLeg] = 0.0f;
    noLegs.bodyParts[(int)AvatarBodyPart::RightLeg] = 0.0f;
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", run)), animatorLayer("Upper Body", clip("attack", attack), mask("nolegs", noLegs), true)};
    animator(controllerFile("body", def), human.root);
    animate(0.0);
    anim::AnimatorInstance& i = instance(human.root);
    ASSERT_TRUE(i.bound);
    ASSERT_TRUE(i.humanoid.valid) << i.humanoid.error;
    ASSERT_TRUE(i.layerMeshSpaceApplies(1));
    const std::vector<Quaternion> base = rigOf(i, i.layerOutput(0));
    const std::vector<Quaternion> layer = rigOf(i, i.layerOutput(1));
    const std::vector<float>& weights = i.layers[1].mask;
    for (size_t b = 0; b < i.rig.bones.size(); b++){
        ASSERT_TRUE(weights[b] == 0.0f || weights[b] == 1.0f);
        const Quaternion& expected = weights[b] == 1.0f ? layer[b] : base[b];
        EXPECT_LE(componentError(i.rigRotations()[b], expected), 1e-5f) << i.rig.bones[b].path << " (mask " << weights[b] << ")";
    }
    const int leftLeg = i.humanoid.slotBones[(size_t)pose::HumanBone::LeftUpperLeg];
    ASSERT_GE(leftLeg, 0);
    const anim::Pose& pose = i.pose();
    std::vector<float> with(pose.raw(), pose.raw() + pose.floatCount());

    i.setLayerMeshSpaceRotation(1, false);
    animate(0.0);
    EXPECT_NEAR(degreesBetween(i.rigRotations()[(size_t)leftLeg], base[(size_t)leftLeg]), 110.0, 0.01)
        << "local blending: the run's legs inherit the attack's hips";
    const size_t stride = pose.stride();
    for (int a : {anim::Pose::TX, anim::Pose::TY, anim::Pose::TZ, anim::Pose::SX, anim::Pose::SY, anim::Pose::SZ}){
        EXPECT_EQ(std::memcmp(&with[(size_t)a * stride], pose.array((anim::Pose::Array)a), stride * sizeof(float)), 0) << "array " << a;
    }
}

// D2: fingers an arm mask leaves out follow the hand (Unity's rule): the mask starts at the
// shoulder only, so the fingers keep the base's local rotation under the layer's hand instead of
// the base's rotation relative to the character. The legs of a body-part mask on from the root
// (everything but the legs) still keep the base's direction: its body starts at the hips.
TEST_F(Layers, MeshSpaceRotationFingersLeftOutOfAnArmMaskFollowTheHand){
    const Humanoid human = humanoid("Granjero");
    const ClipData run = humanoidPose(human, {{"mixamorig:Hips", yaw(90)}, {"mixamorig:Spine", tilt(10)}, {"mixamorig:LeftHandIndex1", tilt(25)}});
    const ClipData attack = humanoidPose(human, {{"mixamorig:Hips", yaw(-20)}, {"mixamorig:LeftArm", roll(60)}, {"mixamorig:LeftHand", tilt(30)},
                                                 {"mixamorig:LeftHandIndex1", tilt(-40)}});
    anim::AvatarMaskDef leftArm;
    leftArm.defaultWeight = 0.0f;
    leftArm.bodyParts[(int)AvatarBodyPart::LeftArm] = 1.0f;
    leftArm.bodyParts[(int)AvatarBodyPart::LeftHand] = 1.0f;
    leftArm.bodyParts[(int)AvatarBodyPart::LeftFingers] = 0.0f;
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", run)), animatorLayer("Left Arm", clip("attack", attack), mask("leftarm", leftArm), true)};
    animator(controllerFile("fingers", def), human.root);
    animate(0.0);
    anim::AnimatorInstance& i = instance(human.root);
    ASSERT_TRUE(i.bound);
    ASSERT_TRUE(i.humanoid.valid) << i.humanoid.error;
    ASSERT_TRUE(i.layerMeshSpaceApplies(1));
    ASSERT_EQ(i.layerBoundaryBones(1).size(), 1u);
    EXPECT_EQ(i.humanoidSlotOf(i.layerBoundaryBones(1)[0]), (int)pose::HumanBone::LeftShoulder) << "the mask starts there; nothing else";
    const int handBone = i.humanoid.slotBones[(size_t)pose::HumanBone::LeftHand];
    ASSERT_GE(handBone, 0);
    const std::vector<Quaternion> layerRig = rigOf(i, i.layerOutput(1));
    EXPECT_LE(componentError(i.rigRotations()[(size_t)handBone], layerRig[(size_t)handBone]), 1e-5f) << "the hand is the layer's";
    size_t fingers = 0;
    for (size_t b = 0; b < i.rig.bones.size(); b++){
        if (i.rig.bones[b].path.find("mixamorig:LeftHand/") == std::string::npos) continue;
        fingers++;
        ASSERT_EQ(i.layers[1].mask[b], 0.0f) << i.rig.bones[b].path;
        EXPECT_LE(componentError(i.pose().rotation(b), i.layerOutput(0).rotation(b)), 1e-6f)
            << i.rig.bones[b].path << ": the base's local rotation, under the layer's hand";
    }
    EXPECT_GE(fingers, 3u);

    // the Animation Player (no Model, so no humanoid: the same weights as paths): the same rule
    const Humanoid twin = humanoid("Twin");
    const posetest::TestRig& rig = posetest::tposeRig();
    anim::AvatarMaskDef armPaths;
    armPaths.defaultWeight = 0.0f;
    armPaths.paths.push_back({twin.path("mixamorig:LeftShoulder"), 1.0f, true});
    for (size_t b = 0; b < rig.skeleton.bones.size(); b++){
        const int parent = rig.skeleton.bones[b].parent;
        if (parent >= 0 && rig.skeleton.bones[(size_t)parent].name == "mixamorig:LeftHand") armPaths.paths.push_back({twin.paths[b], 0.0f, true});
    }
    const ClipData twinRun = humanoidPose(twin, {{"mixamorig:Hips", yaw(90)}, {"mixamorig:Spine", tilt(10)}, {"mixamorig:LeftHandIndex1", tilt(25)}});
    const ClipData twinAttack = humanoidPose(twin, {{"mixamorig:Hips", yaw(-20)}, {"mixamorig:LeftArm", roll(60)}, {"mixamorig:LeftHand", tilt(30)},
                                                    {"mixamorig:LeftHandIndex1", tilt(-40)}});
    AnimationPlayer object = player({{"run", twinRun, 0}, {"attack", twinAttack, 1}},
                                    {layer(anim::BASE_LAYER_NAME), meshSpaceLayer("Left Arm", mask("armpaths", armPaths))}, twin.root);
    object.play("run");
    object.play("attack");
    step(0.1);
    const anim::PlayerPoseScratch& scratch = scene.getComponent<AnimationPlayerComponent>(twin.root).runtime->scratch;
    ASSERT_EQ(scratch.meshBoundary.size(), 2u);
    EXPECT_EQ(scratch.meshBoundary[1].size(), 1u) << "the shoulder";
    for (size_t b = 0; b < rig.skeleton.bones.size(); b++){
        if (twin.paths[b].find("mixamorig:LeftHand/") == std::string::npos) continue;
        const Quaternion expected = rig.skeleton.bones[b].name == "mixamorig:LeftHandIndex1" ? tilt(25) * twin.rest[b] : twin.rest[b];
        EXPECT_LE(componentError(transform(twin.bones[b]).rotation, expected), 1e-5f) << twin.paths[b] << ": the run's local rotation";
    }
}

// The lower-body mask (the hips and legs on, the spine off): the mask starts at the hips and stops
// right under them, so the torso keeps the base's direction, not the layer's hips', on the
// Animator and on the Animation Player
TEST_F(Layers, MeshSpaceRotationLowerBodyMaskKeepsTheBaseTorso){
    addBody();
    anim::AvatarMaskDef lower;
    lower.defaultWeight = 0.0f;
    lower.paths = {{"Hips", 1.0f, true}, {"Hips/Spine", 0.0f, true}};
    const std::string lowerMask = mask("lower", lower);
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", runPose())), animatorLayer("Lower Body", clip("attack", attackPose()), lowerMask, true)};
    animator(controllerFile("lower", def));
    animate(0.0);
    anim::AnimatorInstance& i = instance();
    ASSERT_TRUE(i.bound);
    EXPECT_EQ(i.layerBoundaryBones(1), std::vector<uint16_t>{(uint16_t)i.rig.findBone("Hips/Spine")});
    EXPECT_LE(componentError(rig("Hips"), yaw(-20)), 1e-5f) << "the hips are the layer's";
    EXPECT_LE(componentError(rig("Hips/Thigh"), yaw(-20) * tilt(50)), 1e-5f) << "the legs follow them";
    EXPECT_LE(componentError(rig("Hips/Spine"), yaw(90) * tilt(10)), 1e-5f) << "the torso is the base's: " << degreesBetween(rig("Hips/Spine"), yaw(90) * tilt(10));

    const Entity twin = topLevel("Twin");
    const std::vector<Entity> twinBones = body(twin);
    AnimationPlayer object = player({{"run", runPose(), 0}, {"attack", attackPose(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), meshSpaceLayer("Lower Body", lowerMask)}, twin);
    object.play("run");
    object.play("attack");
    step(0.1);
    EXPECT_LE(componentError(playerRig(twinBones[1], twin), yaw(90) * tilt(10)), 1e-5f) << "the Player's torso is the base's";
    EXPECT_LE(componentError(transform(twinBones[1]).rotation, transform(spine).rotation), 1e-5f) << "R18: the Animator's";
}

// AC6: a flagged layer of weight 0 changes nothing, bit for bit
TEST_F(Layers, MeshSpaceRotationOfAWeightZeroLayerChangesNothing){
    addBody();
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", runPose())),
                  animatorLayer("Upper Body", clip("attack", attackPose()), mask("spine", pathMask("Hips/Spine")), true, 0.0f)};
    animator(controllerFile("zero", def));
    animate(0.0);
    anim::AnimatorInstance& i = instance();
    ASSERT_TRUE(i.layerMeshSpaceApplies(1));
    animate(0.1);
    const anim::Pose& pose = i.pose();
    std::vector<float> flagged(pose.raw(), pose.raw() + pose.floatCount());
    i.setLayerMeshSpaceRotation(1, false);
    i.play("Base Layer State", 0, 0.0f);
    i.play("Upper Body State", 1, 0.0f);
    animate(0.1);
    ASSERT_EQ(flagged.size(), pose.floatCount());
    EXPECT_EQ(std::memcmp(flagged.data(), pose.raw(), flagged.size() * sizeof(float)), 0);
    EXPECT_LE(componentError(rig("Hips/Spine"), yaw(90) * tilt(10)), 1e-5f) << "the base's pose";
}

// AC7: two flagged layers: the second blends over the first's corrected pose (its head, at weight
// 0.5, starts from the attack's direction, not from the run's hips)
TEST_F(Layers, MeshSpaceRotationLayersStackInOrder){
    addBody();
    const ClipData look = still({{"Hips", yaw(30)}, {"Hips/Spine", tilt(-15)}, {"Hips/Spine/Head", roll(-40)}});
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", runPose())),
                  animatorLayer("Upper Body", clip("attack", attackPose()), mask("spine", pathMask("Hips/Spine")), true),
                  animatorLayer("Look", clip("look", look), mask("head", pathMask("Hips/Spine/Head")), true, 0.5f)};
    animator(controllerFile("stack", def));
    animate(0.0);
    anim::AnimatorInstance& i = instance();
    ASSERT_TRUE(i.layerMeshSpaceApplies(1));
    ASSERT_TRUE(i.layerMeshSpaceApplies(2));
    const Quaternion attackHead = yaw(-20) * tilt(40) * roll(15);
    const Quaternion lookHead = yaw(30) * tilt(-15) * roll(-40);
    const Quaternion expected = anim::poseBlendRotation(attackHead, lookHead, 0.5f);
    EXPECT_LE(componentError(rig("Hips/Spine"), yaw(-20) * tilt(40)), 1e-5f) << "the first layer's spine, kept under the second";
    EXPECT_LE(componentError(rig("Hips/Spine/Head"), expected), 1e-5f) << degreesBetween(rig("Hips/Spine/Head"), expected);
    const Quaternion fromTheRun = anim::poseBlendRotation(yaw(90) * tilt(40) * roll(15), lookHead, 0.5f);
    EXPECT_GT(degreesBetween(expected, fromTheRun), 10.0) << "the test tells the two apart";
}

// AC7: an upper-body montage on a flagged slot layer is corrected with nothing more
TEST_F(Layers, MeshSpaceRotationCorrectsAMontage){
    addBody();
    const std::string run = clip("run", runPose());
    anim::MontageDef montage;
    montage.clip.path = clip("attack", attackPose());
    montage.slot = "UpperBody";
    montage.blendIn.time = 0.0f;
    montage.blendOut.time = 0.0f;
    montage.sections = {{"Attack", 0.0f, ""}};
    animtest::writeAsset(prefix + "attack.montage", anim::writeMontage(montage));
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", run), animatorLayer("Upper Body", run, mask("spine", pathMask("Hips/Spine")), true)};
    def.layers[1].slot = "UpperBody";
    animator(controllerFile("montage", def));
    animate(0.0);
    Animator object(&scene, root);
    EXPECT_LE(componentError(rig("Hips/Spine"), yaw(90) * tilt(10)), 1e-5f) << "no montage: the layer plays the run";
    ASSERT_TRUE(object.playMontage(prefix + "attack.montage"));
    animate(0.0);
    animate(0.1);
    ASSERT_TRUE(object.isMontagePlaying());
    EXPECT_LE(componentError(rig("Hips/Spine"), yaw(-20) * tilt(40)), 1e-5f) << degreesBetween(rig("Hips/Spine"), yaw(-20) * tilt(40));
    EXPECT_LE(componentError(rig("Hips/Thigh"), yaw(90) * tilt(-30)), 1e-5f);
}

// AC8: with root motion the layer clip's turning root is taken out first (its heading becomes root
// motion), so the reference is the Animator, not the clip's root bone path
TEST_F(Layers, MeshSpaceRotationReferenceIsTheAnimatorUnderRootMotion){
    addBody();
    ClipData turning = still({{"Hips/Spine", tilt(40)}});
    ClipTrackData hipsTrack;
    hipsTrack.path = "Hips";
    hipsTrack.channel = ClipChannel::Rotation;
    for (int k = 0; k <= 10; k++) hipsTrack.keys.push_back(key((float)k * 0.1f, xyzw(yaw(-40.0f * (float)k * 0.1f) * tilt(10))));
    turning.tracks.push_back(hipsTrack);
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", runPose())),
                  animatorLayer("Upper Body", clip("turning", turning), mask("spine", pathMask("Hips/Spine")), true)};
    AnimatorComponent& component = animator(controllerFile("rootmotion", def));
    component.applyRootMotion = true;
    component.rootMotionBone = "Hips";
    animate(0.0);
    animate(0.5);
    ASSERT_TRUE(instance().layerMeshSpaceApplies(1));
    EXPECT_LE(componentError(rig("Hips/Spine"), tilt(10) * tilt(40)), 1e-5f) << degreesBetween(rig("Hips/Spine"), tilt(50));
    EXPECT_GT(degreesBetween(rig("Hips/Spine"), yaw(-20) * tilt(50)), 19.9) << "not the clip's turned root";
}

// AC10: over a 0.3 s weight fade the spine moves along the blend with no step larger than the
// fade's own (no pop at its start or end), from the run's spine to the attack's
TEST_F(Layers, MeshSpaceRotationFadesWithTheLayerWeight){
    addBody();
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", runPose())),
                  animatorLayer("Upper Body", clip("attack", attackPose()), mask("spine", pathMask("Hips/Spine")), true, 0.0f)};
    animator(controllerFile("fade", def));
    animate(0.0);
    const Quaternion from = yaw(90) * tilt(10);
    const Quaternion to = yaw(-20) * tilt(40);
    EXPECT_LE(componentError(rig("Hips/Spine"), from), 1e-5f);
    const double total = degreesBetween(from, to);
    const float dt = 1.0f / 60.0f;
    // an nlerp turns at most ~1.1 x its mean rate (at its middle, for this angle)
    const double bound = total * (dt / 0.3) * 1.25;
    instance().setLayerWeight(1, 1.0f, 0.3f);
    Quaternion previous = rig("Hips/Spine");
    double largest = 0.0;
    for (int frame = 0; frame < 30; frame++){
        animate(dt);
        const Quaternion now = rig("Hips/Spine");
        const double step = degreesBetween(previous, now);
        EXPECT_LE(step, bound) << "frame " << frame;
        largest = std::max(largest, step);
        previous = now;
    }
    EXPECT_GT(largest, 0.0);
    EXPECT_LE(componentError(rig("Hips/Spine"), to), 1e-5f) << "the attack's spine once the fade ends";
}

// AC13: Lua sets and gets the option by index and by name, an unknown layer is warned once and
// changes nothing (as on the Animation Player), the controller file is not written and a reload of
// the controller clears the override
TEST_F(Layers, MeshSpaceRotationFromLua){
    addBody();
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", runPose())),
                  animatorLayer("Upper Body", clip("attack", attackPose()), mask("spine", pathMask("Hips/Spine")))};
    const std::string path = controllerFile("lua", def);
    const std::string bytes = anim::writeController(def);
    animator(path);
    animate(0.0);
    Animator object(&scene, root);
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    luabridge::setGlobal(L, object, "msrAnimator");
    ASSERT_EQ(luaL_dostring(L,
        "local a = msrAnimator\n"
        "msrResult = tostring(a:getLayerMeshSpaceRotation('Upper Body'))\n"
        "a:setLayerMeshSpaceRotation(1, true)\n"
        "msrResult = msrResult .. '|' .. tostring(a:getLayerMeshSpaceRotation(1)) .. '|' .. tostring(a:getLayerMeshSpaceRotation('Upper Body'))\n"
        "a:setLayerMeshSpaceRotation('Nope', true)\n"
        "a:setLayerMeshSpaceRotation('Nope', true)\n"
        "msrUnknown = tostring(a:getLayerMeshSpaceRotation(7))\n"
        "local ok3, err3 = pcall(function() a:setLayerMeshSpaceRotation(1) end)\n"
        "msrError3 = tostring(ok3) .. ' ' .. tostring(err3)\n"), LUA_OK) << lua_tostring(L, -1);
    auto global = [&](const char* name){
        lua_getglobal(L, name);
        const std::string value = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        return value;
    };
    EXPECT_EQ(global("msrResult"), "false|true|true");
    EXPECT_EQ(global("msrUnknown"), "false");
    const std::vector<std::string> warnings = animtest::takeWarnings();
    EXPECT_EQ(std::count_if(warnings.begin(), warnings.end(), [](const std::string& line){
        return line.find("unknown layer \"Nope\"") != std::string::npos; }), 1) << "warned once";
    EXPECT_EQ(std::count_if(warnings.begin(), warnings.end(), [](const std::string& line){
        return line.find("unknown layer \"7\"") != std::string::npos; }), 1);
    EXPECT_EQ(global("msrError3").rfind("false ", 0), 0u) << "a missing enabled is an error, as on the Animation Player: " << global("msrError3");
    EXPECT_NE(global("msrError3").find("boolean expected"), std::string::npos) << global("msrError3");
    ASSERT_TRUE(instance().layerMeshSpaceApplies(1));
    animate(0.0);
    EXPECT_LE(componentError(rig("Hips/Spine"), yaw(-20) * tilt(40)), 1e-5f) << "the override drives the pass";
    std::ifstream file(animtest::assetDir() + "/" + path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()), bytes)
        << "a runtime override: the controller is not written";

    ASSERT_EQ(luaL_dostring(L, "msrAnimator:setLayerMeshSpaceRotation('Upper Body', false)"), LUA_OK) << lua_tostring(L, -1);
    EXPECT_FALSE(object.getLayerMeshSpaceRotation(1));
    ASSERT_EQ(luaL_dostring(L, "msrAnimator:setLayerMeshSpaceRotation('Upper Body', true)"), LUA_OK) << lua_tostring(L, -1);
    EXPECT_TRUE(object.getLayerMeshSpaceRotation(1));
    // the same controller loaded again: the override is gone, the asset's value (off) is back
    AnimatorAssetPool::invalidate(path);
    animate(0.0);
    EXPECT_FALSE(object.getLayerMeshSpaceRotation(1));
    EXPECT_FALSE(instance().layerMeshSpaceApplies(1));
    luabridge::setGlobal(L, luabridge::LuaRef(L), "msrAnimator");
}

// ---- Mesh Space Rotation on the Animation Player's layers (docs/specs/mesh-space-rotation.md R16, R18) ----

// AC14: the AC4 case on the Player. The base's hips turned 90 degrees, the layer's -20, a mask from
// the spine: with the option the spine (and the head under it) has the layer clip's rotation
// relative to the Player's GameObject and the legs the base's; without it the spine is 110 degrees
// off. Only the spine's local rotation changes: every position, scale and other rotation is the
// local blend's, bit for bit.
TEST_F(Layers, PlayerMeshSpaceRotationKeepsTheLayerDirection){
    addBody();
    AnimationPlayer object = player({{"run", runPose(), 0}, {"attack", attackPose(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), meshSpaceLayer("Upper Body", mask("spine", pathMask("Hips/Spine")))});
    object.play("run");
    object.play("attack");
    step(0.1);
    const anim::PlayerPoseScratch& scratch = scene.getComponent<AnimationPlayerComponent>(root).runtime->scratch;
    ASSERT_EQ(scratch.meshBoundary.size(), 2u);
    EXPECT_EQ(scratch.meshBoundary[1].size(), 1u) << "the spine";
    EXPECT_EQ(scratch.meshNodes.size(), 2u) << "the spine and the hips";
    const Quaternion layerSpine = yaw(-20) * tilt(40);
    EXPECT_LE(componentError(playerRig(spine), layerSpine), 1e-5f) << degreesBetween(playerRig(spine), layerSpine);
    EXPECT_LE(componentError(playerRig(head), layerSpine * roll(15)), 1e-5f) << "the head follows the corrected spine";
    EXPECT_LE(componentError(playerRig(hips), yaw(90)), 1e-5f) << "the hips are the base's";
    EXPECT_LE(componentError(playerRig(thigh), yaw(90) * tilt(-30)), 1e-5f) << "the legs are the base's";
    EXPECT_LE(componentError(playerRig(foot), yaw(90) * tilt(-30) * tilt(20)), 1e-5f);
    const std::vector<Entity> bones = {hips, spine, head, thigh, foot};
    const std::vector<float> with = transformsOf(bones);

    // the same clip time (the sampler's last bit differs between times)
    object.setLayerMeshSpaceRotation(1, false);
    step(0.0);
    ASSERT_TRUE(object.isPlaying("attack"));
    const anim::AnimationPlayerRuntime& runtime = *scene.getComponent<AnimationPlayerComponent>(root).runtime;
    EXPECT_EQ(runtime.scratch.meshNodes.capacity() + runtime.scratch.meshSums.capacity() + runtime.scratch.meshBelow.capacity() +
                  runtime.scratch.meshRig.capacity() + runtime.scratch.meshBoundary.capacity() + runtime.meshSpaceStamp.capacity(), 0u)
        << "R21: no layer runs the pass, no scratch";
    for (const anim::PlayerStateRuntime& state : runtime.states) EXPECT_EQ(state.meshSources.capacity(), 0u) << state.name;
    EXPECT_NEAR(degreesBetween(playerRig(spine), layerSpine), 110.0, 0.01) << "local blending: the spine inherits the base's hips";
    EXPECT_NEAR(degreesBetween(playerRig(head), layerSpine * roll(15)), 110.0, 0.01);
    EXPECT_LE(componentError(playerRig(thigh), yaw(90) * tilt(-30)), 1e-5f);
    const std::vector<float> without = transformsOf(bones);
    ASSERT_EQ(with.size(), without.size());
    for (size_t i = 0; i < with.size(); i++){
        const bool spineRotation = i >= 10 + 3 && i < 10 + 7;
        if (!spineRotation){
            EXPECT_EQ(std::memcmp(&with[i], &without[i], sizeof(float)), 0) << "float " << i;
        }
    }
}

// AC14 with the mask's other edge: a mask of the hips and everything above the legs takes the
// attack's hips; with the option the legs keep the run's rotation relative to the Player's
// GameObject, without it they inherit the attack's hips (110 degrees off)
TEST_F(Layers, PlayerMeshSpaceRotationKeepsTheBaseLegs){
    addBody();
    anim::AvatarMaskDef noLegs;
    noLegs.defaultWeight = 0.0f;
    noLegs.paths = {{"Hips", 1.0f, true}, {"Hips/Thigh", 0.0f, true}};
    AnimationPlayer object = player({{"run", runPose(), 0}, {"attack", attackPose(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), meshSpaceLayer("Upper Body", mask("nolegs", noLegs))});
    object.play("run");
    object.play("attack");
    step(0.1);
    EXPECT_LE(componentError(playerRig(hips), yaw(-20)), 1e-5f) << "the hips are the attack's";
    EXPECT_LE(componentError(playerRig(spine), yaw(-20) * tilt(40)), 1e-5f);
    EXPECT_LE(componentError(playerRig(head), yaw(-20) * tilt(40) * roll(15)), 1e-5f);
    EXPECT_LE(componentError(playerRig(thigh), yaw(90) * tilt(-30)), 1e-5f) << degreesBetween(playerRig(thigh), yaw(90) * tilt(-30));
    EXPECT_LE(componentError(playerRig(foot), yaw(90) * tilt(-30) * tilt(20)), 1e-5f) << "the foot follows the corrected thigh";
    EXPECT_NEAR(transform(hips).position.x, 0.2f, 1e-6f) << "translation stays local: the attack's hips";

    object.setLayerMeshSpaceRotation(1, false);
    step(0.1);
    EXPECT_NEAR(degreesBetween(playerRig(thigh), yaw(90) * tilt(-30)), 110.0, 0.01) << "local blending: the run's legs inherit the attack's hips";
}

// R18: the Animation Player and the Animator give the same pose, within 1e-5, for the same rig,
// clips, masks and weights with the option on: two flagged layers at fractional weights, the
// second over the first's corrected pose
TEST_F(Layers, PlayerMeshSpaceRotationMatchesTheAnimator){
    addBody();
    const ClipData look = still({{"Hips", yaw(30)}, {"Hips/Spine", tilt(-15)}, {"Hips/Spine/Head", roll(-40)}});
    const std::string spineMask = mask("spine", pathMask("Hips/Spine"));
    const std::string headMask = mask("head", pathMask("Hips/Spine/Head"));
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", runPose())), animatorLayer("Upper Body", clip("attack", attackPose()), spineMask, true, 0.6f),
                  animatorLayer("Look", clip("look", look), headMask, true, 0.5f)};
    animator(controllerFile("parity", def));
    animate(0.0);
    ASSERT_TRUE(instance().layerMeshSpaceApplies(1));
    ASSERT_TRUE(instance().layerMeshSpaceApplies(2));

    const Entity twin = topLevel("Twin");
    const std::vector<Entity> twinBones = body(twin);
    AnimationPlayer object = player({{"run", runPose(), 0}, {"attack", attackPose(), 1}, {"look", look, 2}},
                                    {layer(anim::BASE_LAYER_NAME), meshSpaceLayer("Upper Body", spineMask, true, 0.6f),
                                     meshSpaceLayer("Look", headMask, true, 0.5f)},
                                    twin);
    object.play("run");
    object.play("attack");
    object.play("look");
    step(0.1);
    const std::vector<Entity> bones = {hips, spine, head, thigh, foot};
    for (size_t b = 0; b < bones.size(); b++){
        const Transform& a = transform(bones[b]);
        const Transform& p = transform(twinBones[b]);
        EXPECT_LE(componentError(a.rotation, p.rotation), 1e-5f) << scene.getEntityName(bones[b]) << ": " << degreesBetween(a.rotation, p.rotation);
        EXPECT_LE((a.position - p.position).length(), 1e-5f) << scene.getEntityName(bones[b]);
        EXPECT_LE((a.scale - p.scale).length(), 1e-5f) << scene.getEntityName(bones[b]);
    }
}

// R16: a boundary bone no clip animates still gets its rotation relative to the Player's GameObject
// (the layer clip's hips over the bone's rest), and the Animator's for the same setup
TEST_F(Layers, PlayerMeshSpaceRotationCorrectsABoneNoClipAnimates){
    addBody();
    const ClipData run = still({{"Hips", yaw(90)}, {"Hips/Thigh", tilt(-30)}});
    const ClipData attack = still({{"Hips", yaw(-20)}, {"Hips/Spine/Head", roll(15)}, {"Hips/Thigh", tilt(50)}});
    AnimationPlayer object = player({{"run", run, 0}, {"attack", attack, 1}},
                                    {layer(anim::BASE_LAYER_NAME), meshSpaceLayer("Upper Body", mask("spine", pathMask("Hips/Spine")))});
    object.play("run");
    object.play("attack");
    step(0.1);
    EXPECT_LE(componentError(playerRig(spine), yaw(-20)), 1e-5f) << degreesBetween(playerRig(spine), yaw(-20));
    EXPECT_LE(componentError(playerRig(head), yaw(-20) * roll(15)), 1e-5f);
    EXPECT_LE(componentError(playerRig(thigh), yaw(90) * tilt(-30)), 1e-5f);

    const Entity twin = topLevel("Twin");
    const std::vector<Entity> twinBones = body(twin);
    anim::ControllerDef def;
    def.layers = {animatorLayer("Base Layer", clip("run", run)), animatorLayer("Upper Body", clip("attack", attack), mask("spine", pathMask("Hips/Spine")), true)};
    animator(controllerFile("untracked", def), twin);
    animate(0.0);
    ASSERT_TRUE(instance(twin).layerMeshSpaceApplies(1));
    EXPECT_LE(componentError(transform(twinBones[1]).rotation, transform(spine).rotation), 1e-5f) << "R18: the Animator's spine";
}

// R16, R21: the option turned off releases the table; the boundary bone it gave a Rotation slot of
// its own (no clip rotates it) goes back to its rest once, instead of keeping the pass's last
// rotation, and stays there
TEST_F(Layers, PlayerMeshSpaceRotationOffPutsABoneNoClipAnimatesBack){
    addBody();
    const Quaternion rest = transform(spine).rotation;
    const ClipData run = still({{"Hips", yaw(90)}, {"Hips/Thigh", tilt(-30)}});
    const ClipData attack = still({{"Hips", yaw(-20)}, {"Hips/Spine/Head", roll(15)}, {"Hips/Thigh", tilt(50)}});
    AnimationPlayer object = player({{"run", run, 0}, {"attack", attack, 1}},
                                    {layer(anim::BASE_LAYER_NAME), meshSpaceLayer("Upper Body", mask("spine", pathMask("Hips/Spine")))});
    object.play("run");
    object.play("attack");
    step(0.1);
    ASSERT_GT(degreesBetween(transform(spine).rotation, rest), 100.0) << "the pass turned the spine (the layer's -20 over the run's 90)";

    object.setLayerMeshSpaceRotation(1, false);
    step(0.1);
    EXPECT_LE(componentError(transform(spine).rotation, rest), 1e-6f) << degreesBetween(transform(spine).rotation, rest);
    EXPECT_LE(componentError(playerRig(spine), yaw(90)), 1e-5f) << "local again: the spine follows the run's hips";
    transform(spine).rotation = roll(5);
    step(0.1);
    EXPECT_LE(componentError(transform(spine).rotation, roll(5)), 1e-6f) << "once: nothing writes the bone after that";
}

// R8 on the Player: where the option does not apply it changes nothing, bit for bit: a flagged
// layer at weight 0, a flagged Additive layer and a flagged layer without a mask give the pose of
// the same layer without the option
TEST_F(Layers, PlayerMeshSpaceRotationChangesNothingWhereItDoesNotApply){
    const std::string spineMask = mask("spine", pathMask("Hips/Spine"));
    AnimationPlayerLayer additive = meshSpaceLayer("Additive", spineMask);
    additive.blend = AnimationLayerBlend::Additive;
    const AnimationPlayerLayer cases[] = {meshSpaceLayer("Weight Zero", spineMask, true, 0.0f), additive, meshSpaceLayer("Unmasked", "")};
    for (const AnimationPlayerLayer& flagged : cases){
        const Entity character = topLevel(flagged.name);
        const std::vector<Entity> bones = body(character);
        AnimationPlayer object = player({{"run", runPose(), 0}, {"attack", attackPose(), 1}}, {layer(anim::BASE_LAYER_NAME), flagged}, character);
        object.play("run");
        object.play("attack");
        step(0.1);
        const anim::PlayerPoseScratch& scratch = scene.getComponent<AnimationPlayerComponent>(character).runtime->scratch;
        // R22: the scratch is the bind's, whatever the weight; R21: no pass, none
        EXPECT_EQ(scratch.meshNodes.empty(), flagged.weight > 0.0f) << flagged.name;
        const std::vector<float> with = transformsOf(bones);
        object.setLayerMeshSpaceRotation(1, false);
        step(0.0);
        ASSERT_TRUE(object.isPlaying("attack")) << flagged.name;
        const std::vector<float> without = transformsOf(bones);
        ASSERT_EQ(with.size(), without.size());
        EXPECT_EQ(std::memcmp(with.data(), without.data(), with.size() * sizeof(float)), 0) << flagged.name;
    }
}

// R16, R20 on the Player: with the option on, the steady state allocates nothing per frame
TEST_F(Layers, PlayerMeshSpaceRotationAllocatesNothingPerFrame){
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this build cannot count allocations";
    addBody();
    AnimationPlayer object = player({{"run", runPose(), 0}, {"attack", attackPose(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), meshSpaceLayer("Upper Body", mask("spine", pathMask("Hips/Spine")), true, 0.7f)});
    object.play("run");
    object.play("attack");
    step(1.0 / 60.0, 3);
    testsupport::AllocationCountScope scope;
    step(1.0 / 60.0, 300);
    EXPECT_EQ(scope.count(), 0u);
}

// R16 in the layered edit-mode preview (the Animation window's Preview over): a layer with the
// option composes as it plays
TEST_F(Layers, PlayerMeshSpaceRotationInTheLayeredPreview){
    addBody();
    const std::string spineMask = mask("spine", pathMask("Hips/Spine"));
    AnimationPlayer object = player({{"run", runPose(), 0}, {"attack", attackPose(), 1}},
                                    {layer(anim::BASE_LAYER_NAME), meshSpaceLayer("Upper Body", spineMask)});
    ActionSystem* system = scene.getSystem<ActionSystem>().get();
    ActionSystem::PlayerPreview preview;
    preview.layered = true;
    preview.layers = {ActionSystem::PlayerPreviewLayer(), ActionSystem::PlayerPreviewLayer()};
    preview.layers[1].mask = spineMask;
    preview.layers[1].meshSpaceRotation = true;
    ActionSystem::PlayerPreviewState base;
    base.state = "run";
    ActionSystem::PlayerPreviewState edited;
    edited.state = "attack";
    edited.layer = 1;
    preview.states = {base, edited};
    const Quaternion layerSpine = yaw(-20) * tilt(40);
    {
        anim::ClipPreviewSession session;
        EXPECT_TRUE(system->previewPlayer(root, preview, session).ready);
        EXPECT_LE(componentError(playerRig(spine), layerSpine), 1e-5f) << degreesBetween(playerRig(spine), layerSpine);
        EXPECT_LE(componentError(playerRig(thigh), yaw(90) * tilt(-30)), 1e-5f);
        system->restorePlayerPreview(session);
    }
    EXPECT_LE(componentError(transform(spine).rotation, Quaternion()), 1e-6f) << "the preview put the spine back";
    preview.layers[1].meshSpaceRotation = false;
    anim::ClipPreviewSession session;
    EXPECT_TRUE(system->previewPlayer(root, preview, session).ready);
    EXPECT_NEAR(degreesBetween(playerRig(spine), layerSpine), 110.0, 0.01);
    system->restorePlayerPreview(session);
    (void)object;
}
