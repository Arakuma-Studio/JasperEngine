// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Ragdoll wizard's core and the Ragdoll component (physics-3d spec T13): an in-memory
// Mixamo-named humanoid skeleton through RagdollBuilder (the editor dialog, MCP create_ragdoll and
// engine code share it): 11 bodies and 10 swing-twist joints with Unity's table, masses summing to
// the total; animated, the kinematic parts follow their bones; simulated, it falls and comes to
// rest on a floor within its joint limits, the bones (what skinning reads) at the bodies' poses;
// animated again, kinematic and the Animation Player back on.

#include <gtest/gtest.h>

#include "Physics3DTestWorld.h"

#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/ModelComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/RagdollComponent.h"
#include "object/physics/Ragdoll.h"
#include "physics/RagdollBuilder.h"

#include <cmath>
#include <functional>
#include <map>
#include <string>

using namespace jasperengine;
using namespace jasperengine::physics3dtest;

namespace {

    struct Character {
        World3D world;
        Entity root;
        std::map<std::string, Entity> bones;

        Entity bone(const std::string& name, const Vector3& local, Entity parent) {
            const Entity e = world.entity(local, parent);
            world.scene.setEntityName(e, "mixamorig:" + name);
            bones[name] = e;
            return e;
        }

        explicit Character(float height = 0.0f) {
            world.floor();
            root = world.entity(Vector3(0, height, 0));
            world.scene.setEntityName(root, "Character");
            const Entity hips = bone("Hips", Vector3(0, 1.0f, 0), root);
            const Entity leftUpLeg = bone("LeftUpLeg", Vector3(0.1f, -0.05f, 0), hips);
            const Entity leftLeg = bone("LeftLeg", Vector3(0, -0.45f, 0), leftUpLeg);
            bone("LeftFoot", Vector3(0, -0.42f, 0), leftLeg);
            const Entity rightUpLeg = bone("RightUpLeg", Vector3(-0.1f, -0.05f, 0), hips);
            const Entity rightLeg = bone("RightLeg", Vector3(0, -0.45f, 0), rightUpLeg);
            bone("RightFoot", Vector3(0, -0.42f, 0), rightLeg);
            const Entity spine = bone("Spine", Vector3(0, 0.1f, 0), hips);
            const Entity spine1 = bone("Spine1", Vector3(0, 0.15f, 0), spine);
            const Entity spine2 = bone("Spine2", Vector3(0, 0.15f, 0), spine1);
            const Entity neck = bone("Neck", Vector3(0, 0.15f, 0), spine2);
            bone("Head", Vector3(0, 0.1f, 0), neck);
            const Entity leftShoulder = bone("LeftShoulder", Vector3(0.05f, 0.1f, 0), spine2);
            const Entity leftArm = bone("LeftArm", Vector3(0.12f, 0, 0), leftShoulder);
            const Entity leftForeArm = bone("LeftForeArm", Vector3(0.28f, 0, 0), leftArm);
            bone("LeftHand", Vector3(0.25f, 0, 0), leftForeArm);
            const Entity rightShoulder = bone("RightShoulder", Vector3(-0.05f, 0.1f, 0), spine2);
            const Entity rightArm = bone("RightArm", Vector3(-0.12f, 0, 0), rightShoulder);
            const Entity rightForeArm = bone("RightForeArm", Vector3(-0.28f, 0, 0), rightArm);
            bone("RightHand", Vector3(-0.25f, 0, 0), rightForeArm);
            world.resolve();
        }

        RagdollBuildResult build(bool simulated) {
            std::vector<std::string> notes;
            const RagdollBones found = RagdollBuilder::guessBones(&world.scene, root, &notes);
            RagdollBuildSettings settings;
            settings.startSimulated = simulated;
            return RagdollBuilder::build(&world.scene, root, found, settings);
        }
    };

    float rotationDegrees(const Quaternion& q) {
        const float w = std::fmin(1.0f, std::fabs(q.w));
        return Angle::radToDeg(2.0f * std::acos(w));
    }

}

TEST(Ragdoll, WizardFindsTheBonesAndBuildsUnitysTable) {
    Character c;
    std::vector<std::string> notes;
    const RagdollBones found = RagdollBuilder::guessBones(&c.world.scene, c.root, &notes);
    EXPECT_EQ(found.pelvis, c.bones["Hips"]);
    EXPECT_EQ(found.leftHips, c.bones["LeftUpLeg"]);
    EXPECT_EQ(found.leftKnee, c.bones["LeftLeg"]);
    EXPECT_EQ(found.leftFoot, c.bones["LeftFoot"]);
    EXPECT_EQ(found.rightElbow, c.bones["RightForeArm"]);
    EXPECT_EQ(found.head, c.bones["Head"]);
    EXPECT_NE(found.middleSpine, NULL_ENTITY);

    const RagdollBuildResult result = c.build(false);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.parts.size(), 11u);
    size_t joints = 0;
    float mass = 0.0f;
    for (const RagdollBuiltPart& part : result.parts) {
        mass += part.mass;
        if (part.hasJoint) {
            joints++;
            EXPECT_EQ(part.joint.type, Joint3DType::SWINGTWIST) << part.slot;
            EXPECT_EQ(part.joint.anchorSpace, JointAnchorSpace::LOCAL) << part.slot;
            EXPECT_FALSE(part.joint.enableCollision) << part.slot;
            EXPECT_EQ(part.joint.bodyA, part.entity) << part.slot;
        }
        EXPECT_EQ(part.body.type, BodyType::KINEMATIC) << "animated: " << part.slot;
        EXPECT_FALSE(part.body.automaticMass);
        if (part.slot == "left_hips") {
            EXPECT_NEAR(Angle::defaultToDeg(part.joint.twistMinAngle), -20.0f, 1e-3f);
            EXPECT_NEAR(Angle::defaultToDeg(part.joint.twistMaxAngle), 70.0f, 1e-3f);
            EXPECT_NEAR(Angle::defaultToDeg(part.joint.normalHalfConeAngle), 30.0f, 1e-3f) << "swing 1";
            EXPECT_NEAR(Angle::defaultToDeg(part.joint.planeHalfConeAngle), 0.0f, 1e-3f) << "swing 2";
            EXPECT_EQ(part.joint.bodyB, c.bones["Hips"]);
            EXPECT_EQ(part.collider, "capsule");
        }
        if (part.slot == "left_elbow") {
            EXPECT_NEAR(Angle::defaultToDeg(part.joint.twistMinAngle), -90.0f, 1e-3f);
            EXPECT_NEAR(Angle::defaultToDeg(part.joint.twistMaxAngle), 0.0f, 1e-3f);
            EXPECT_EQ(part.joint.bodyB, c.bones["LeftArm"]);
        }
        if (part.slot == "head") { EXPECT_EQ(part.collider, "sphere"); }
        if (part.slot == "pelvis") {
            EXPECT_EQ(part.collider, "box");
            EXPECT_FALSE(part.hasJoint);
        }
    }
    EXPECT_EQ(joints, 10u);
    EXPECT_NEAR(mass, 20.0f, 1e-4f);

    // the same poses and settings give the same ragdoll
    const RagdollBuildResult again = c.build(false);
    ASSERT_EQ(again.parts.size(), result.parts.size());
    for (size_t i = 0; i < result.parts.size(); i++) {
        EXPECT_EQ(again.parts[i].slot, result.parts[i].slot);
        EXPECT_FLOAT_EQ(again.parts[i].mass, result.parts[i].mass);
    }
}

TEST(Ragdoll, AnimatedFollowsThenSimulatesThenAnimatesAgain) {
    Character c;
    c.world.scene.addComponent<AnimationPlayerComponent>(c.root, AnimationPlayerComponent());
    const RagdollBuildResult result = c.build(false);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(RagdollBuilder::apply(&c.world.scene, c.root, result));
    ASSERT_NE(c.world.scene.findComponent<RagdollComponent>(c.root), nullptr);
    c.world.load();
    c.world.step();
    Ragdoll ragdoll(&c.world.scene, c.root);
    EXPECT_EQ(ragdoll.getPartCount(), 11u);
    for (Entity part : ragdoll.getPartEntities()) {
        EXPECT_EQ(c.world.component(part).type, BodyType::KINEMATIC);
    }

    // animated: a bone the animation turns, its body follows
    const Entity thigh = c.bones["LeftUpLeg"];
    c.world.transform(thigh).rotation = degreesAbout(40.0f, Vector3(1, 0, 0));
    c.world.transform(thigh).needUpdate = true;
    c.world.resolve();
    const Vector3 knee = c.world.transform(c.bones["LeftLeg"]).worldPosition;
    c.world.step(2);
    EXPECT_NEAR((c.world.rb(c.bones["LeftLeg"]).getPosition() - knee).length(), 0.0f, 1e-3f);
    EXPECT_NEAR(std::fabs(c.world.rb(thigh).getRotation().dot(c.world.transform(thigh).worldRotation)), 1.0f, 1e-3f);

    // simulated: dynamic, the Animation Player paused, it falls and rests on the floor
    std::map<Entity, Quaternion> startRelative;
    for (const RagdollBuiltPart& part : result.parts) {
        if (!part.hasJoint) continue;
        startRelative[part.entity] = c.world.rb(part.joint.bodyB).getRotation().inverse() * c.world.rb(part.entity).getRotation();
    }
    ragdoll.setSimulated(true);
    c.world.step();
    for (Entity part : ragdoll.getPartEntities()) {
        EXPECT_EQ(c.world.component(part).type, BodyType::DYNAMIC);
    }
    EXPECT_FALSE(c.world.scene.getComponent<AnimationPlayerComponent>(c.root).enabled) << "paused";
    c.world.step(300);
    float fastest = 0.0f;
    for (Entity part : ragdoll.getPartEntities()) {
        fastest = std::fmax(fastest, c.world.rb(part).getLinearVelocity().length());
        EXPECT_GT(c.world.rb(part).getPosition().y, -0.05f) << "above the floor";
        EXPECT_LT(c.world.rb(part).getPosition().y, 1.0f) << "fallen";
        // the bone (what the skin reads) is where its body is
        const Transform& transform = c.world.transform(part);
        EXPECT_NEAR((transform.worldPosition - c.world.rb(part).getPosition()).length(), 0.0f, 1e-4f);
        const Vector3 joltOrigin = c.world.rb(part).getPosition();
        const Vector3 modelOrigin(transform.modelMatrix[3][0], transform.modelMatrix[3][1], transform.modelMatrix[3][2]);
        EXPECT_NEAR((modelOrigin - joltOrigin).length(), 0.0f, 1e-4f) << "modelMatrix";
    }
    EXPECT_LT(fastest, 0.2f) << "at rest within 5 s";
    // every joint within its limits (twist and swing together) + 2 degrees
    for (const RagdollBuiltPart& part : result.parts) {
        if (!part.hasJoint) continue;
        const Quaternion relative = c.world.rb(part.joint.bodyB).getRotation().inverse() * c.world.rb(part.entity).getRotation();
        const Quaternion fromStart = startRelative[part.entity].inverse() * relative;
        const float twist = std::fmax(std::fabs(Angle::defaultToDeg(part.joint.twistMinAngle)), std::fabs(Angle::defaultToDeg(part.joint.twistMaxAngle)));
        const float swing = Angle::defaultToDeg(part.joint.planeHalfConeAngle) + Angle::defaultToDeg(part.joint.normalHalfConeAngle);
        EXPECT_LE(rotationDegrees(fromStart), twist + swing + 2.0f) << part.slot;
    }

    // animated again: kinematic, the player back on
    ragdoll.setSimulated(false);
    c.world.step();
    for (Entity part : ragdoll.getPartEntities()) {
        EXPECT_EQ(c.world.component(part).type, BodyType::KINEMATIC);
    }
    EXPECT_TRUE(c.world.scene.getComponent<AnimationPlayerComponent>(c.root).enabled);
}

TEST(Ragdoll, ImpulseAndSelfCollision) {
    Character c(0.0f);
    const RagdollBuildResult result = c.build(true);
    ASSERT_TRUE(RagdollBuilder::apply(&c.world.scene, c.root, result));
    c.world.load();
    c.world.step();
    Ragdoll ragdoll(&c.world.scene, c.root);
    EXPECT_TRUE(ragdoll.isSimulated());
    const Entity head = c.bones["Head"];
    ragdoll.applyImpulse(Vector3(0, 0, 30), c.world.rb(head).getPosition());
    c.world.step();
    EXPECT_GT(c.world.rb(head).getLinearVelocity().z, 0.5f) << "the nearest part takes it";
    EXPECT_NEAR(ragdoll.getTotalMass(), 20.0f, 1e-5f);
}

// ---- review: the documented idiom, the other bone drivers, the restore hook --------------------

TEST(Ragdoll, ImpulseInTheFrameItTurnsSimulatedIsKept) {
    Character c;
    const RagdollBuildResult result = c.build(false);
    ASSERT_TRUE(RagdollBuilder::apply(&c.world.scene, c.root, result));
    c.world.load();
    c.world.step();
    Ragdoll ragdoll(&c.world.scene, c.root);
    const Entity head = c.bones["Head"];
    EXPECT_EQ(c.world.component(head).type, BodyType::KINEMATIC);
    // a script hit: simulated, then the push, in the same frame (physics.md)
    ragdoll.setSimulated(true);
    EXPECT_EQ(c.world.component(head).type, BodyType::DYNAMIC) << "applied at once outside a step";
    ragdoll.applyImpulse(Vector3(0, 0, 40), c.world.rb(head).getPosition());
    c.world.step();
    EXPECT_GT(c.world.rb(head).getLinearVelocity().z, 0.5f) << "the push reached a dynamic part";
}

TEST(Ragdoll, PushBeforeThePartsExistWaitsForThem) {
    Character c;
    const RagdollBuildResult result = c.build(true);
    ASSERT_TRUE(RagdollBuilder::apply(&c.world.scene, c.root, result));
    Ragdoll ragdoll(&c.world.scene, c.root);
    const Entity head = c.bones["Head"];
    ragdoll.applyImpulse(Vector3(0, 0, 40), c.world.transform(head).worldPosition);
    EXPECT_EQ(c.world.scene.getComponent<RagdollComponent>(c.root).pendingPushes.size(), 1u) << "queued: no bodies yet";
    c.world.load();
    c.world.step(2);
    EXPECT_TRUE(c.world.scene.getComponent<RagdollComponent>(c.root).pendingPushes.empty());
    EXPECT_GT(c.world.rb(head).getLinearVelocity().z, 0.3f);
}

TEST(Ragdoll, OtherBoneDriversPauseAndResume) {
    Character c;
    const RagdollBuildResult result = c.build(false);
    ASSERT_TRUE(RagdollBuilder::apply(&c.world.scene, c.root, result));
    // an entity animation playing (Model:playAnimation), a Pose Animator and a Vertex Animator
    const Entity animation = c.world.scene.createEntity();
    ActionComponent action;
    action.state = ActionState::Running;
    c.world.scene.addComponent<ActionComponent>(animation, action);
    c.world.scene.addComponent<AnimationComponent>(animation, AnimationComponent());
    ModelComponent model;
    model.animations.push_back(animation);
    c.world.scene.addComponent<ModelComponent>(c.root, model);
    PoseAnimatorComponent pose;
    pose.playing = true;
    pose.started = true;
    c.world.scene.addComponent<PoseAnimatorComponent>(c.root, pose);
    c.world.scene.addComponent<VertexAnimatorComponent>(c.bones["Hips"], VertexAnimatorComponent());
    int restores = 0;
    Ragdoll ragdoll(&c.world.scene, c.root);
    ragdoll.onBeforeAnimatedRestore().add("count", std::function<void(int)>([&](int entity) {
        EXPECT_EQ((Entity)entity, c.root);
        EXPECT_EQ(c.world.component(c.bones["Head"]).type, BodyType::DYNAMIC) << "before the parts go back";
        restores++;
    }));
    c.world.load();
    c.world.step();

    ragdoll.setSimulated(true);
    EXPECT_TRUE(c.world.scene.getComponent<ActionComponent>(animation).pauseTrigger);
    EXPECT_FALSE(c.world.scene.getComponent<PoseAnimatorComponent>(c.root).playing);
    EXPECT_FALSE(c.world.scene.getComponent<VertexAnimatorComponent>(c.bones["Hips"]).enabled);
    c.world.step(10);
    ragdoll.setSimulated(false);
    EXPECT_EQ(restores, 1);
    EXPECT_TRUE(c.world.scene.getComponent<ActionComponent>(animation).startTrigger) << "resumed";
    EXPECT_TRUE(c.world.scene.getComponent<PoseAnimatorComponent>(c.root).playing);
    EXPECT_TRUE(c.world.scene.getComponent<VertexAnimatorComponent>(c.bones["Hips"]).enabled);
    c.world.physics()->onRagdollBeforeAnimatedRestore3D.remove("count");
}
