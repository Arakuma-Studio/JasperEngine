// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// pose/ModelRig on a synthetic scene (spec: pose animation phase 2, section 3.1): the skeleton
// of a Model's bone entities with non-bone nodes folded in (bridges), and the Pose Animator
// system giving the bones their base pose back when its component goes

#include "TestUtil.h"

#include "Scene.h"
#include "component/BoneComponent.h"
#include "component/ModelComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/Transform.h"
#include "pose/ModelRig.h"
#include "object/PoseAnimator.h"
#include "subsystem/PoseAnimatorSystem.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>

using namespace posetest;

namespace {

    Entity node(Scene& scene, Entity parent, const std::string& name, const Vector3& position, const Quaternion& rotation){
        Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        scene.setEntityName(entity, name);
        Transform& transform = scene.getComponent<Transform>(entity);
        transform.position = position;
        transform.rotation = rotation;
        transform.scale = Vector3(1, 1, 1);
        if (parent != NULL_ENTITY) scene.addEntityChild(parent, entity, false);
        return entity;
    }

    Entity bone(Scene& scene, Entity model, Entity parent, const std::string& name, const Vector3& position, const Quaternion& rotation){
        Entity entity = node(scene, parent, name, position, rotation);
        BoneComponent component;
        component.model = model;
        component.index = 0;
        component.bindPosition = position;
        component.bindRotation = rotation;
        component.bindScale = Vector3(1, 1, 1);
        scene.addComponent<BoneComponent>(entity, component);
        return entity;
    }

    Quaternion aboutY(float degrees){
        const float half = degrees * 3.14159265f / 360.0f;
        return Quaternion(std::cos(half), 0, std::sin(half), 0);
    }

    struct Fixture {
        std::unique_ptr<Scene> scene = std::make_unique<Scene>();
        Entity model, root, hips, mid, spine;

        Fixture(){
            Scene& s = *scene;
            model = node(s, NULL_ENTITY, "Model", Vector3(5, 0, 0), Quaternion());
            s.addComponent<ModelComponent>(model, {});
            // glTF scene root and an armature-like node between two bones: not bones
            root = node(s, model, "SceneRoot", Vector3(0, 1, 0), aboutY(90));
            hips = bone(s, model, root, "Hips", Vector3(0, 0.5f, 0), Quaternion());
            mid = node(s, hips, "Helper", Vector3(0, 0.1f, 0), aboutY(-30));
            spine = bone(s, model, mid, "Spine", Vector3(0, 0.2f, 0), aboutY(10));
            ModelComponent& m = s.getComponent<ModelComponent>(model);
            m.bonesIdMapping[1] = hips;
            m.bonesIdMapping[3] = spine;
        }
    };

}

TEST(PoseModelRig, BuildSkeletonFoldsNonBoneNodes){
    Fixture f;
    Skeleton skeleton;
    std::vector<Entity> entities;
    std::vector<Matrix4> bridges;
    ASSERT_TRUE(buildSkeleton(f.scene.get(), f.model, skeleton, entities, &bridges));
    ASSERT_EQ(skeleton.bones.size(), 2u);
    ASSERT_EQ(entities.size(), 2u);
    EXPECT_EQ(entities[0], f.hips);
    EXPECT_EQ(entities[1], f.spine);
    EXPECT_EQ(skeleton.bones[0].name, "Hips");
    EXPECT_EQ(skeleton.bones[0].parent, -1);
    EXPECT_EQ(skeleton.bones[1].parent, 0);

    // hips: the scene root folded in (model space), spine: the helper folded in
    EXPECT_NEAR((skeleton.bones[0].position - Vector3(0, 1.5f, 0)).length(), 0.0f, 1e-5f);
    EXPECT_LT(rotationError(skeleton.bones[0].rotation, aboutY(90)), 1e-5);
    EXPECT_NEAR((skeleton.bones[1].position - Vector3(0, 0.1f, 0) - aboutY(-30) * Vector3(0, 0.2f, 0)).length(), 0.0f, 1e-5f);
    EXPECT_LT(rotationError(skeleton.bones[1].rotation, aboutY(-20)), 1e-5);

    // skeleton local = bridge * Transform local
    ASSERT_EQ(bridges.size(), 2u);
    Vector3 p, s;
    Quaternion r;
    bridges[0].decompose(p, s, r);
    EXPECT_NEAR((p - Vector3(0, 1, 0)).length(), 0.0f, 1e-5f);
    EXPECT_LT(rotationError(r, aboutY(90)), 1e-5);
    bridges[1].decompose(p, s, r);
    EXPECT_NEAR((p - Vector3(0, 0.1f, 0)).length(), 0.0f, 1e-5f);
    EXPECT_LT(rotationError(r, aboutY(-30)), 1e-5);

    // no Model, no bones: false
    Skeleton none;
    EXPECT_FALSE(buildSkeleton(f.scene.get(), f.root, none, entities));
}

TEST(PoseModelRig, RemovingTheAnimatorRestoresTheBones){
    Fixture f;
    Scene& scene = *f.scene;
    scene.addComponent<PoseAnimatorComponent>(f.model, {});
    PoseAnimatorComponent& animator = scene.getComponent<PoseAnimatorComponent>(f.model);
    animator.runtime = std::make_shared<PoseAnimatorRuntime>();
    PoseAnimatorRuntime& runtime = *animator.runtime;

    // the animator wrote the spine over its rest rotation, a clip wrote the hips after it
    Transform& spine = scene.getComponent<Transform>(f.spine);
    const Quaternion rest = spine.rotation;
    const int slot = boneIndex(HumanBone::Spine);
    runtime.blendEntities[slot] = f.spine;
    spine.rotation = runtime.blend[slot].blendRotation(spine.rotation, aboutY(60), 1.0f);
    const int hipsSlot = boneIndex(HumanBone::Hips);
    Transform& hips = scene.getComponent<Transform>(f.hips);
    runtime.blendEntities[hipsSlot] = f.hips;
    hips.rotation = runtime.blend[hipsSlot].blendRotation(hips.rotation, aboutY(40), 1.0f);
    const Quaternion clip = aboutY(-15);
    hips.rotation = clip;

    scene.removeComponent<PoseAnimatorComponent>(f.model);
    EXPECT_LT(rotationError(scene.getComponent<Transform>(f.spine).rotation, rest), 1e-6);
    // what a clip wrote since stays
    EXPECT_LT(rotationError(scene.getComponent<Transform>(f.hips).rotation, clip), 1e-6);
}

// NaN / infinity from Lua (setSpeed, setWeight, setTime) or the component fields (Inspector, MCP)
// never reach the clock or the quaternions
TEST(PoseModelRig, NonFiniteInputIsIgnored){
    Fixture f;
    Scene& scene = *f.scene;
    PoseAnimator handle(&scene, f.model);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    handle.setSpeed(nan);
    handle.setSpeed(inf);
    EXPECT_FLOAT_EQ(handle.getSpeed(), 1.0f);
    handle.setWeight(nan);
    EXPECT_FLOAT_EQ(handle.getWeight(), 1.0f);
    handle.setWeight(-inf);
    EXPECT_FLOAT_EQ(handle.getWeight(), 0.0f);
    handle.setWeight(0.5f);
    handle.setTime(0.25f);
    handle.setTime(nan);
    handle.setTime(inf);
    EXPECT_FLOAT_EQ(handle.getTime(), 0.25f);

    // a playing animator (built on a synthetic rig, driving no bone entity) with non-finite fields
    PoseAnimatorComponent& animator = scene.getComponent<PoseAnimatorComponent>(f.model);
    auto runtime = std::make_shared<PoseAnimatorRuntime>();
    runtime->data = makeAnimation({bodyWith({{"spine.bend", 0}}), bodyWith({{"spine.bend", 30}})}, true);
    runtime->dataLoaded = true;
    std::string error;
    ASSERT_TRUE(runtime->build(tposeRig().skinned, &error)) << error;
    animator.runtime = runtime;
    animator.needsLoad = false;
    animator.started = true;
    animator.playing = true;
    animator.speed = nan;
    animator.weight = nan;
    animator.time = 0.1f;
    PoseAnimatorSystem* system = scene.getSystem<PoseAnimatorSystem>().get();
    ASSERT_NE(system, nullptr);
    system->update(0.1);
    EXPECT_TRUE(std::isfinite(animator.time));
    EXPECT_FLOAT_EQ(animator.time, 0.1f);
    animator.speed = inf;
    system->update(0.1);
    EXPECT_TRUE(std::isfinite(animator.time));
    animator.speed = 1.0f;
    animator.time = nan;
    system->update(0.1);
    EXPECT_TRUE(std::isfinite(animator.time));
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        const Quaternion& q = runtime->displayed.rotations[b];
        EXPECT_TRUE(std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z)) << b;
    }
}
