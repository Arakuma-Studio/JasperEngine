// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Humanoid retargeting and the IK stack in a scene (docs/specs/S21-animation.md AC5-AC7, R11,
// R12, D11, D12): a clip retargeted onto its own rig equals direct playback, a Mixamo T-pose walk
// on the Rigify A-pose robot keeps the limbs' directions, FootIK grounds both feet on a 20 degree
// ramp without lifting the pelvis, FootLock keeps a planted foot in place.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include "Engine.h"
#include "component/RigidbodyComponent.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "object/Model.h"
#include "pose/HumanBones.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"

#include <cmath>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    constexpr float kDt = 1.0f / 60.0f;

    std::string fixturePath(const std::string& name){
        return std::string(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models") + "/" + name;
    }

    float angleBetween(const Vector3& a, const Vector3& b){
        const float d = a.normalized().dotProduct(b.normalized());
        return std::acos(std::min(1.0f, std::max(-1.0f, d))) * 57.29577951f;
    }

    class AnimatorRetargeting : public ::testing::Test, public AnimatorFixture {
    protected:
        // the Model handles own their GameObjects: kept for the test
        std::vector<std::unique_ptr<Model>> models;

        Entity load(const std::string& file, float x){
            models.push_back(std::make_unique<Model>(&scene));
            Model& model = *models.back();
            // as the editor loads a ModelComponent (MeshSystem's update): the file's root nodes
            // under the model's entity, the root transform kept
            EXPECT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), fixturePath(file), false, false, false));
            // loaded at once (MeshSystem's update would clear the flag the next frame)
            scene.getComponent<ModelComponent>(model.getEntity()).needUpdateModel = false;
            scene.getComponent<Transform>(model.getEntity()).position = Vector3(x, 0, 0);
            return model.getEntity();
        }
    };

}

// the clip's own model plays it by path; the same rig from another file (no animations: its
// hierarchy holds the joints only, so the clip's paths do not bind there) plays it retargeted
TEST_F(AnimatorRetargeting, SameRigIdentity){
    const Entity direct = load("hero_animated.gltf", -1);
    const Entity retargeted = load("hero_tpose.gltf", 1);
    const std::string walk = fixturePath("hero_animated.gltf") + "#Walk";
    const std::string path = controllerFile("walk", controller({}, {state("Walk", walk)}));
    animator(path, direct).retarget = AnimatorRetarget::Off;
    animator(path, retargeted);
    step(0.0);
    ASSERT_TRUE(instance(direct).bound);
    ASSERT_TRUE(instance(retargeted).bound);
    ASSERT_TRUE(instance(retargeted).humanoid.valid) << instance(retargeted).humanoid.error;
    ASSERT_TRUE(instance(retargeted).clips[0].retargeted) << "another model file: retargeted";
    EXPECT_FALSE(instance(direct).clips[0].retargeted);
    ASSERT_EQ(instance(direct).rig.unresolved, 0u) << "the clip binds on its own model";
    for (int frame = 0; frame < 90; frame++){
        step(kDt);
        for (int s = 0; s < pose::HUMAN_BONE_COUNT; s++){
            const int a = instance(direct).humanoid.slotBones[(size_t)s];
            const int b = instance(retargeted).humanoid.slotBones[(size_t)s];
            ASSERT_EQ(a < 0, b < 0);
            if (a < 0) continue;
            const Quaternion qa = instance(direct).pose().rotation((size_t)a);
            const Quaternion qb = instance(retargeted).pose().rotation((size_t)b);
            EXPECT_GE(std::fabs(qa.dot(qb)), 1.0f - 1e-5f) << pose::humanBoneName(pose::boneAt(s)) << " frame " << frame;
        }
        const int hipsA = instance(direct).humanoid.slotBones[(size_t)pose::HumanBone::Hips];
        const int hipsB = instance(retargeted).humanoid.slotBones[(size_t)pose::HumanBone::Hips];
        EXPECT_LE((instance(direct).pose().position((size_t)hipsA) - instance(retargeted).pose().position((size_t)hipsB)).length(), 1e-5f);
    }
}

TEST_F(AnimatorRetargeting, CrossAxisConvention){
    const Entity hero = load("hero_animated.gltf", -1);
    const Entity robot = load("robot_apose.gltf", 1);
    const std::string walk = fixturePath("hero_animated.gltf") + "#Walk";
    const std::string path = controllerFile("walk", controller({}, {state("Walk", walk)}));
    animator(path, hero).retarget = AnimatorRetarget::Off;
    animator(path, robot);
    step(0.0);
    ASSERT_TRUE(instance(robot).bound);
    ASSERT_TRUE(instance(robot).humanoid.valid) << instance(robot).humanoid.error;
    ASSERT_TRUE(instance(robot).clips[0].retargeted);
    ASSERT_EQ(instance(hero).rig.unresolved, 0u);
    using B = pose::HumanBone;
    const std::pair<B, B> segments[] = {{B::LeftUpperArm, B::LeftLowerArm}, {B::LeftLowerArm, B::LeftHand},
                                        {B::RightUpperArm, B::RightLowerArm}, {B::RightLowerArm, B::RightHand},
                                        {B::LeftUpperLeg, B::LeftLowerLeg}, {B::LeftLowerLeg, B::LeftFoot},
                                        {B::RightUpperLeg, B::RightLowerLeg}, {B::RightLowerLeg, B::RightFoot}};
    float worst = 0.0f;
    for (int frame = 0; frame < 60; frame++){
        step(kDt);
        for (const auto& segment : segments){
            const auto dir = [&](Entity e){
                const AnimatorInstance& i = instance(e);
                const int a = i.humanoid.slotBones[(size_t)segment.first];
                const int b = i.humanoid.slotBones[(size_t)segment.second];
                return i.rigPositions()[(size_t)b] - i.rigPositions()[(size_t)a];
            };
            worst = std::max(worst, angleBetween(dir(hero), dir(robot)));
        }
    }
    EXPECT_LE(worst, 5.0f) << "hand and foot directions follow the source";
    // the hips move by the source's offset, scaled by the rigs' sizes
    const AnimatorInstance& r = instance(robot);
    EXPECT_GT(r.humanoid.rig->humanScale(), 0.0f);
}

namespace {

    // A two-legged rig standing on a ramp: Hero -> Hips -> {L/R}UpperLeg -> {L/R}LowerLeg -> {L/R}Foot
    class AnimatorFootIK : public ::testing::Test, public AnimatorFixture {
    protected:
        Entity upper[2], lower[2], ankles[2];
        float updateTime = 0.0f;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(kDt);
            const float side[2] = {0.12f, -0.12f};
            const float forward[2] = {0.2f, -0.2f};
            for (int i = 0; i < 2; i++){
                upper[i] = node(hips, i == 0 ? "LUpperLeg" : "RUpperLeg", Vector3(side[i], 0, forward[i]));
                lower[i] = node(upper[i], i == 0 ? "LLowerLeg" : "RLowerLeg", Vector3(0, -0.45f, 0.02f));
                ankles[i] = node(lower[i], i == 0 ? "LFoot" : "RFoot", Vector3(0, -0.47f, -0.02f));
            }
        }
        void TearDown() override{ Engine::setUpdateTime(updateTime); }

        void resolveTransforms(){
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }

        void ramp(){
            const Entity entity = scene.createEntity();
            Transform transform;
            transform.position = Vector3(0, -0.5f, 0);
            transform.rotation = aboutX(-20.0f);
            scene.addComponent<Transform>(entity, transform);
            RigidbodyComponent body;
            body.type = BodyType::STATIC;
            body.colliders.resize(1);
            body.colliders[0].type = Collider3DType::BOX;
            body.colliders[0].width = 10.0f;
            body.colliders[0].height = 1.0f;
            body.colliders[0].depth = 10.0f;
            scene.addComponent<RigidbodyComponent>(entity, body);
        }

        // the ramp's surface height at z: the top face of the 1 m box centred at y = -0.5, rotated
        // -20 degrees about x (it rises toward +z)
        static float ground(float z){
            const float slope = 20.0f * 0.0174532925f;
            return -0.5f + 0.5f / std::cos(slope) + std::tan(slope) * z;
        }

        AnimatorComponent& rig(){
            // the hips held at 0.92 m with straight legs: the feet at the rig's y = 0 (0.08 m soles)
            ClipData data = hold("Hips", Vector3(0, 0.92f + 0.08f, 0));
            AnimatorComponent& component = animator(controllerFile("stand", controller({}, {state("Stand", clip("stand", data))})));
            for (int i = 0; i < 2; i++){
                AnimatorIKConstraint c;
                c.name = i == 0 ? "LeftFoot" : "RightFoot";
                c.type = AnimatorIKType::FootIK;
                c.bones = {"Hips/" + scene.getEntityName(upper[i]), "Hips/" + scene.getEntityName(upper[i]) + "/" + scene.getEntityName(lower[i]),
                           "Hips/" + scene.getEntityName(upper[i]) + "/" + scene.getEntityName(lower[i]) + "/" + scene.getEntityName(ankles[i])};
                c.pelvis = "Hips";
                c.footHeight = 0.08f;
                component.ik.push_back(c);
            }
            return component;
        }

        void frame(){
            resolveTransforms();
            scene.fixedUpdate(kDt);
            step(kDt);
            resolveTransforms();
        }
    };

}

TEST_F(AnimatorFootIK, SlopeGrounding){
    ramp();
    rig();
    step(0.0);
    ASSERT_TRUE(instance().bound);
    for (int i = 0; i < 30; i++){
        frame();
        // the pelvis never rises above the animation's
        EXPECT_LE(position(hips).y, 1.0f + 1e-5f);
    }
    for (int i = 0; i < 2; i++){
        const Vector3 ankle = scene.getComponent<Transform>(ankles[i]).worldPosition;
        const float sole = ankle.y - 0.08f;
        EXPECT_NEAR(sole, ground(ankle.z), 0.01f) << (i == 0 ? "left" : "right") << " sole on the ramp";
    }
    EXPECT_LT(position(hips).y, 1.0f) << "the pelvis dropped for the lower foot";
}

// AC6 (R15): Cull Update Transforms has no pose while culled, so no IK reads the ground
TEST_F(AnimatorFootIK, CulledCastsNoRays){
    ramp();
    AnimatorComponent& component = rig();
    // a skin to cull by, and a camera whose frustum excludes everything (its planes all face away)
    const Entity mesh = node(root, "Mesh", Vector3(0, 0, 0));
    scene.addComponent<MeshComponent>(mesh, {});
    ModelComponent model;
    ModelSkinBinding skin;
    skin.mesh = mesh;
    skin.joints = {hips};
    skin.inverseBindMatrices = {Matrix4()};
    model.skinBindings.push_back(skin);
    scene.addComponent<ModelComponent>(root, model);
    scene.getComponent<ModelComponent>(root).needUpdateModel = false;
    const Entity camera = scene.createEntity();
    scene.addComponent<Transform>(camera, {});
    scene.addComponent<CameraComponent>(camera, {});
    CameraComponent& c = scene.getComponent<CameraComponent>(camera);
    for (int p = 0; p < 6; p++) c.frustumPlanes[p] = Plane(Vector3(0, 1, 0), -1000.0f);
    c.farClip = 1.0f;

    step(0.0);
    ASSERT_TRUE(instance().bound);
    component.cullingMode = AnimatorCullingMode::AlwaysAnimate;
    frame();
    EXPECT_EQ(system().getStats().footRaycasts, 2u) << "a ray per foot";
    EXPECT_TRUE(instance().ik[0].hit);

    component.cullingMode = AnimatorCullingMode::CullUpdateTransforms;
    for (int i = 0; i < 5; i++){
        frame();
        EXPECT_GT(system().getStats().culled, 0u);
        EXPECT_EQ(system().getStats().footRaycasts, 0u) << "frame " << i;
        EXPECT_FALSE(instance().ik[0].hit);
    }
}

TEST_F(AnimatorFootIK, FootLockNoSlide){
    // the hips slide forward 0.1 m/s while the right foot is in contact (the parameter)
    ClipData data;
    data.wrap = ClipWrapMode::Once;
    data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {0, 1, 0.1f})})};
    ControllerDef def = controller({parameter("Contact", AnimatorParameterType::Float, 1.0f)}, {state("Move", clip("move", data))});
    // bent knees: the planted foot stays within the leg's reach while the hips slide
    for (int i = 0; i < 2; i++){
        scene.getComponent<Transform>(upper[i]).rotation = aboutX(-20.0f);
        scene.getComponent<Transform>(lower[i]).rotation = aboutX(40.0f);
        scene.getComponent<Transform>(ankles[i]).rotation = aboutX(-20.0f);
    }
    AnimatorComponent& component = animator(controllerFile("lock", def));
    AnimatorIKConstraint c;
    c.name = "Lock";
    c.type = AnimatorIKType::FootLock;
    c.bones = {"Hips/RUpperLeg", "Hips/RUpperLeg/RLowerLeg", "Hips/RUpperLeg/RLowerLeg/RFoot"};
    c.contactParameter = "Contact";
    component.ik.push_back(c);
    step(0.0);
    resolveTransforms();
    step(kDt);
    resolveTransforms();
    const Vector3 start = scene.getComponent<Transform>(ankles[1]).worldPosition;
    const Vector3 freeStart = scene.getComponent<Transform>(ankles[0]).worldPosition;
    for (int i = 0; i < 40; i++){
        step(kDt);
        resolveTransforms();
    }
    const Vector3 end = scene.getComponent<Transform>(ankles[1]).worldPosition;
    EXPECT_LE((end - start).length(), 0.002f) << "a locked foot stays put";
    EXPECT_GT(scene.getComponent<Transform>(ankles[0]).worldPosition.z - freeStart.z, 0.05f) << "the free foot moves with the hips";
    // released: it follows the animation again
    Animator(&scene, root).setFloat("Contact", 0.0f);
    for (int i = 0; i < 30; i++){
        step(kDt);
        resolveTransforms();
    }
    EXPECT_GT(scene.getComponent<Transform>(ankles[1]).worldPosition.z - end.z, 0.01f);
}


