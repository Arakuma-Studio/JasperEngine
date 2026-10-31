// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Regressions of the physics-3d review: damper-only six-DOF drives, automatic mass after an
// explicit one and DOF changes, trigger colliders and continuous collision, equal-limit sliders,
// live limit edits keeping the joint's zero, pair filter changes inside a step, static and
// kinematic bodies nested under a dynamic one, the LOW items (cone swing limits, local pulley
// and slider anchors, live twist limits, gear axes, old gears colliding, waking after a shape
// swap, the lazy 3D world, the exported capacity default) and the spec's weaker tests (values and
// contacts surviving collider edits).

#include <gtest/gtest.h>

#include "Physics3DTestWorld.h"

#include "Generator.h"
#include "math/Ray.h"
#include "Project.h"
#include "util/JoltPhysicsAux.h"

#include "Jolt/Physics/Constraints/SixDOFConstraint.h"
#include "Jolt/Physics/Constraints/SliderConstraint.h"
#include "Jolt/Physics/Constraints/SwingTwistConstraint.h"

#include <cmath>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::physics3dtest;

// ---- HIGH 2: six-DOF drives ------------------------------------------------------------------

namespace {

    // a 1 kg box on a six-DOF joint to the world, every axis free, sliding at 10 m/s along x
    float dampedSpeed(bool useDrive, float stiffness, float damping, int steps) {
        World3D world(Vector3(0, 0, 0));
        const Entity e = world.box(Vector3(0, 0, 0), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC);
        world.rb(e).setMass(1.0f);
        world.rb(e).setLinearDamping(0.0f);
        Joint3DComponent sixDOF = makeJoint3D(Joint3DType::SIXDOF);
        sixDOF.bodyA = e;
        sixDOF.axisX = Vector3(1, 0, 0);
        sixDOF.axisY = Vector3(0, 1, 0);
        sixDOF.useDrive = useDrive;
        sixDOF.driveStiffness = stiffness;
        sixDOF.driveDamping = damping;
        world.joint(sixDOF);
        world.load();
        world.rb(e).setLinearVelocity(Vector3(10, 0, 0));
        world.step(steps);
        return world.rb(e).getLinearVelocity().x;
    }

}

TEST(Physics3DReview, SixDOFDriveWithoutStiffnessOrDampingDoesNothing) {
    EXPECT_NEAR(dampedSpeed(true, 0.0f, 0.0f, 30), 10.0f, 0.05f) << "no drive: the axis is free (it was locked rigidly)";
    EXPECT_NEAR(dampedSpeed(false, 0.0f, 0.0f, 30), 10.0f, 0.05f);
}

TEST(Physics3DReview, SixDOFDamperOnlyDampsLikeUnity) {
    // force = damping x (0 - v): v(n) = v0 (1 - c dt / m)^n
    const float c = 2.0f;
    const float expected = 10.0f * std::pow(1.0f - c * STEP / 1.0f, 60.0f);
    const float measured = dampedSpeed(true, 0.0f, c, 60);
    EXPECT_NEAR(measured, expected, expected * 0.02f);
    EXPECT_GT(measured, 1.0f) << "damped, not stopped";
}

// ---- MEDIUM 3: mass back to automatic, DOF changes keep the mass fields ------------------------

TEST(Physics3DReview, AutomaticMassComesBackAndDOFsKeepTheMassFields) {
    World3D world(Vector3(0, 0, 0));
    const Entity e = world.box(Vector3(0, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    world.load();
    world.rb(e).setMass(3.0f);
    world.rb(e).applyImpulse(Vector3(3, 0, 0));
    world.step();
    EXPECT_NEAR(world.rb(e).getLinearVelocity().x, 1.0f, 0.01f);

    world.rb(e).setLinearVelocity(Vector3(0, 0, 0));
    world.rb(e).setAutomaticMass(true);
    world.rb(e).applyImpulse(Vector3(1000, 0, 0));
    world.step();
    EXPECT_NEAR(world.rb(e).getLinearVelocity().x, 1.0f, 0.01f) << "Jolt's 1000 kg again";

    world.rb(e).setLinearVelocity(Vector3(0, 0, 0));
    world.rb(e).setMass(3.0f);
    world.rb(e).setAllowedDOFs(true, true, true, false, false, false);
    world.rb(e).applyImpulse(Vector3(3, 0, 0));
    world.step();
    EXPECT_NEAR(world.rb(e).getLinearVelocity().x, 1.0f, 0.01f) << "the explicit mass survives a DOF change";
}

TEST(Physics3DReview, ExplicitMassSurvivesAColliderEdit) {
    World3D world(Vector3(0, 0, 0));
    const Entity e = world.sphere(Vector3(0, 0, 0), 0.5f, BodyType::DYNAMIC);
    world.rb(e).setMass(3.0f);
    world.load();
    world.rb(e).setColliderRadius(0, 0.9f);
    world.step();
    world.rb(e).applyImpulse(Vector3(3, 0, 0));
    world.step();
    EXPECT_NEAR(world.rb(e).getLinearVelocity().x, 1.0f, 0.01f);
}

// ---- MEDIUM 4: trigger colliders and continuous collision -------------------------------------

TEST(Physics3DReview, FastBodyWithATriggerDoesNotTunnel) {
    World3D world(Vector3(0, 0, 0));
    world.box(Vector3(5, 0, 0), Vector3(0.05f, 4, 4), BodyType::STATIC);
    const Entity bullet = world.sphere(Vector3(0, 0, 0), 0.2f, BodyType::DYNAMIC);
    const int trigger = world.rb(bullet).createSphereCollider(Vector3(0, 0, 0), Quaternion::IDENTITY, 1.0f);
    world.rb(bullet).setColliderTrigger((size_t)trigger, true);
    world.rb(bullet).setMotionQuality(RigidbodyMotionQuality::LINEAR_CAST);
    int sensorContacts = 0;
    world.physics()->onContactAdded3D.add("sensor", std::function<void(Rigidbody, Rigidbody, Contact3D)>(
        [&](Rigidbody, Rigidbody, Contact3D contact) { if (contact.isSensor()) sensorContacts++; }));
    world.load();
    EXPECT_FALSE(world.component(bullet).triggerBody.IsInvalid()) << "the trigger lives on its own sensor body";
    world.rb(bullet).setLinearVelocity(Vector3(400, 0, 0));
    world.step(10);
    EXPECT_LT(world.rb(bullet).getPosition().x, 5.0f) << "stopped by the wall, not through it";
    EXPECT_GT(sensorContacts, 0) << "the trigger still reports";
}

TEST(Physics3DReview, FastBodyHitsAWallBehindAnotherBodysTrigger) {
    World3D world(Vector3(0, 0, 0));
    const Entity wall = world.box(Vector3(5, 0, 0), Vector3(0.05f, 4, 4), BodyType::STATIC);
    const int zone = world.rb(wall).createBoxCollider(Vector3(-1, 0, 0), Quaternion::IDENTITY, 1.5f, 4, 4);
    world.rb(wall).setColliderTrigger((size_t)zone, true);
    const Entity bullet = world.sphere(Vector3(0, 0, 0), 0.2f, BodyType::DYNAMIC);
    world.rb(bullet).setMotionQuality(RigidbodyMotionQuality::LINEAR_CAST);
    world.load();
    world.rb(bullet).setLinearVelocity(Vector3(400, 0, 0));
    world.step(10);
    EXPECT_LT(world.rb(bullet).getPosition().x, 5.0f);
}

TEST(Physics3DReview, TriggerCompanionMovesAndGoesWithItsBody) {
    World3D world(Vector3(0, 0, 0));
    const Entity e = world.box(Vector3(0, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    const int trigger = world.rb(e).createSphereCollider(Vector3(0, 0, 0), Quaternion::IDENTITY, 2.0f);
    world.rb(e).setColliderTrigger((size_t)trigger, true);
    world.load();
    const JPH::BodyID companion = world.component(e).triggerBody;
    ASSERT_FALSE(companion.IsInvalid());
    EXPECT_TRUE(world.physics()->isTriggerCompanion3D(companion));
    EXPECT_NEAR(world.rb(e).getMass(), 1000.0f, 1e-2f) << "the trigger adds no mass";
    world.rb(e).setLinearVelocity(Vector3(3, 0, 0));
    world.step(30);
    const JPH::RVec3 at = world.physics()->getBodyInterface3D().GetPosition(companion);
    EXPECT_NEAR((float)at.GetX(), world.rb(e).getPosition().x, 0.06f) << "with its body (a step behind at most)";
    // all triggers: no companion, the body itself is a sensor
    world.rb(e).setColliderTrigger(0, true);
    world.step();
    EXPECT_TRUE(world.component(e).triggerBody.IsInvalid());
    {
        JPH::BodyLockRead lock(world.physics()->getBodyLockInterface3D(), world.component(e).body);
        ASSERT_TRUE(lock.Succeeded());
        EXPECT_TRUE(lock.GetBody().IsSensor());
    }
    // removed with the body
    world.rb(e).setColliderTrigger(0, false);
    world.step();
    EXPECT_FALSE(world.component(e).triggerBody.IsInvalid());
    world.physics()->removeRigidbody(e);
    EXPECT_FALSE(world.physics()->isTriggerCompanion3D(companion));
}

// ---- MEDIUM 5: a slider with equal limits becomes one again -------------------------------------

TEST(Physics3DReview, EqualLimitSliderBecomesASliderAgain) {
    World3D world(Vector3(0, 0, 0));
    const Entity carriage = world.box(Vector3(0, 0, 0), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC);
    Joint3DComponent slider = makeJoint3D(Joint3DType::PRISMATIC);
    slider.bodyA = carriage;
    slider.axis = Vector3(1, 0, 0);
    slider.useLimits = true;
    slider.limitsMin = 1.0f;
    slider.limitsMax = 1.0f;
    const Entity joint = world.joint(slider);
    world.load();
    ASSERT_NE(world.jointComponent(joint).joint, nullptr);
    EXPECT_EQ(world.jointComponent(joint).joint->GetSubType(), JPH::EConstraintSubType::Fixed);
    world.j(joint).setLimitsMax(3.0f);
    world.step(2);
    ASSERT_NE(world.jointComponent(joint).joint, nullptr);
    EXPECT_EQ(world.jointComponent(joint).joint->GetSubType(), JPH::EConstraintSubType::Slider);
}

// ---- MEDIUM 6: live limit edits keep the joint's zero -----------------------------------------

TEST(Physics3DReview, HingeLimitEditKeepsItsZero) {
    World3D world(Vector3(0, 0, 0));
    const Entity door = world.box(Vector3(0.5f, 1, 0), Vector3(1, 2, 0.1f), BodyType::DYNAMIC);
    world.rb(door).setMass(1.0f);
    Joint3DComponent hinge = makeJoint3D(Joint3DType::HINGE);
    hinge.bodyA = door;
    hinge.anchor = Vector3(-0.5f, 0, 0);
    hinge.axis = Vector3(0, 1, 0);
    hinge.normal = Vector3(1, 0, 0);
    hinge.useLimits = true;
    hinge.limitsMin = Angle::degToDefault(0.0f);
    hinge.limitsMax = Angle::degToDefault(90.0f);
    hinge.useMotor = true;
    hinge.motorTargetVelocity = Angle::degToDefault(60.0f);
    const Entity joint = world.joint(hinge);
    world.load();
    world.step(80);  // 80 degrees open
    world.j(joint).setUseMotor(false);
    world.rb(door).setAngularVelocity(Vector3(0, 0, 0));
    world.rb(door).setLinearVelocity(Vector3(0, 0, 0));
    world.step();
    const float before = Angle::defaultToDeg(world.j(joint).getAngle());
    EXPECT_NEAR(before, 80.0f, 2.0f);

    world.j(joint).setLimitsMax(Angle::degToDefault(100.0f));
    world.step();
    EXPECT_NEAR(Angle::defaultToDeg(world.j(joint).getAngle()), before, 1.0f) << "the zero did not move";
    world.j(joint).setUseMotor(true);
    world.step(60);
    EXPECT_NEAR(Angle::defaultToDeg(world.j(joint).getAngle()), 100.0f, 1.5f) << "0..100 from the same zero";
    EXPECT_NEAR(Angle::radToDeg(std::fabs(2.0f * std::asin(world.rb(door).getRotation().y))), 100.0f, 1.5f);
}

TEST(Physics3DReview, SixDOFLimitsChangeLive) {
    World3D world(Vector3(0, 0, 0));
    const Entity e = world.box(Vector3(0, 0, 0), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC);
    Joint3DComponent sixDOF = makeJoint3D(Joint3DType::SIXDOF);
    sixDOF.bodyA = e;
    sixDOF.axisX = Vector3(1, 0, 0);
    sixDOF.axisY = Vector3(0, 1, 0);
    sixDOF.xMotion = JointMotion::LIMITED;
    sixDOF.linearLimitMin = Vector3(-1, 0, 0);
    sixDOF.linearLimitMax = Vector3(1, 0, 0);
    // turning freely, the joint's frame (bodyA's) would turn under a lock off its anchor
    sixDOF.angularXMotion = sixDOF.angularYMotion = sixDOF.angularZMotion = JointMotion::LOCKED;
    const Entity joint = world.joint(sixDOF);
    world.load();
    const JPH::TwoBodyConstraint* made = world.jointComponent(joint).joint;
    world.rb(e).setLinearVelocity(Vector3(-3, 0, 0));
    world.step(60);
    EXPECT_NEAR(world.rb(e).getPosition().x, -1.0f, 0.03f);
    world.j(joint).setLinearLimitMin(Vector3(-2, 0, 0));
    world.j(joint).setYMotion(JointMotion::LOCKED);
    world.rb(e).setLinearVelocity(Vector3(-3, 2, 0));
    world.step(60);
    EXPECT_EQ(world.jointComponent(joint).joint, made) << "set on the live joint";
    EXPECT_NEAR(world.rb(e).getPosition().x, -2.0f, 0.03f) << "from the same zero";
    EXPECT_NEAR(world.rb(e).getPosition().y, 0.0f, 0.02f) << "y locked live";
}

// ---- MEDIUM 7: pair filter changes inside a step wait for its end -----------------------------

TEST(Physics3DReview, PairFilterChangedInsideAStepAfterIt) {
    World3D world;
    world.floor();
    const Entity box = world.box(Vector3(0, 0.5f, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    const Entity other = world.box(Vector3(5, 0.6f, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    bool sawDuringStep = true;
    bool asked = false;
    world.physics()->onContactAdded3D.add("filter", std::function<void(Rigidbody, Rigidbody, Contact3D)>(
        [&](Rigidbody, Rigidbody, Contact3D) {
            if (asked) return;
            asked = true;
            world.physics()->setBodyPairIgnored3D(box, other, JoltPairFilter3D::SCRIPT, true);
            sawDuringStep = world.physics()->getPairFilter3D().isIgnored(box, other);
        }));
    world.load();
    world.step(5);
    ASSERT_TRUE(asked);
    EXPECT_FALSE(sawDuringStep) << "the job threads' filter was not changed under them";
    EXPECT_TRUE(world.physics()->getPairFilter3D().isIgnored(box, other)) << "applied after the step";
}

// ---- MEDIUM 9: static and kinematic bodies nested under a dynamic one --------------------------

TEST(Physics3DReview, KinematicChildRidesItsDynamicParent) {
    World3D world;
    const Entity vehicle = world.box(Vector3(0, 10, 0), Vector3(2, 1, 4), BodyType::DYNAMIC);
    const Entity hitbox = world.box(Vector3(0, 2, 0), Vector3(1, 1, 1), BodyType::KINEMATIC, vehicle);
    world.load();
    world.rb(vehicle).setLinearVelocity(Vector3(3, 0, 0));
    world.step(30);
    const Vector3 expected = world.transform(vehicle).worldPosition + world.transform(vehicle).worldRotation * Vector3(0, 2, 0);
    EXPECT_NEAR((world.transform(hitbox).worldPosition - expected).length(), 0.0f, 1e-3f) << "keeps its local pose";
    EXPECT_NEAR((world.transform(hitbox).position - Vector3(0, 2, 0)).length(), 0.0f, 1e-3f);
    EXPECT_LT((world.rb(hitbox).getPosition() - expected).length(), 0.3f) << "its body follows";
}

// ---- LOW items ----------------------------------------------------------------------------------

TEST(Physics3DReview, LocalSixDOFConeSwingIsSymmetric) {
    World3D world(Vector3(0, 0, 0));
    const Entity e = world.box(Vector3(0, 0, 0), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC);
    Joint3DComponent sixDOF = makeJoint3D(Joint3DType::SIXDOF);
    sixDOF.bodyA = e;
    sixDOF.axisX = Vector3(1, 0, 0);
    sixDOF.axisY = Vector3(0, 1, 0);
    sixDOF.swingType = JointSwingType::CONE;
    sixDOF.angularYMotion = JointMotion::LIMITED;
    sixDOF.angularLimitMin = Vector3(0, Angle::degToDefault(-30.0f), 0);
    sixDOF.angularLimitMax = Vector3(0, Angle::degToDefault(10.0f), 0);
    const Entity joint = world.joint(sixDOF);
    world.load();
    const JPH::SixDOFConstraint* made = static_cast<const JPH::SixDOFConstraint*>(world.jointComponent(joint).joint);
    ASSERT_NE(made, nullptr);
    EXPECT_NEAR(Angle::radToDeg(made->GetRotationLimitsMax().GetY()), 30.0f, 1e-3f) << "the larger half, not Jolt's max only";
    EXPECT_NEAR(Angle::radToDeg(made->GetRotationLimitsMin().GetY()), -30.0f, 1e-3f);
}

TEST(Physics3DReview, LocalPulleyKeepsItsAnchorBAndSliderUsesIt) {
    World3D world(Vector3(0, 0, 0));
    const Entity a = world.box(Vector3(-2, 0, 0), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC);
    const Entity b = world.box(Vector3(2, 0, 0), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC);
    Joint3DComponent pulley = makeJoint3D(Joint3DType::PULLEY);
    pulley.bodyA = a;
    pulley.bodyB = b;
    pulley.autoAnchors = true;
    pulley.anchorB = Vector3(0, 0.25f, 0);
    pulley.fixedPointA = Vector3(-2, 5, 0);
    pulley.fixedPointB = Vector3(2, 5, 0);
    const Entity pulleyJoint = world.joint(pulley);
    world.load();
    EXPECT_EQ(world.jointComponent(pulleyJoint).anchorB, Vector3(0, 0.25f, 0)) << "not overwritten by autoAnchors";

    World3D other(Vector3(0, 0, 0));
    const Entity carriage = other.box(Vector3(0, 0, 0), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC);
    Joint3DComponent slider = makeJoint3D(Joint3DType::PRISMATIC);
    slider.bodyA = carriage;
    slider.axis = Vector3(1, 0, 0);
    slider.autoAnchors = false;
    slider.anchorB = Vector3(0, 0, 1);  // a world point (no connected body): the rail is at z = 1
    other.joint(slider);
    other.load();
    other.step(60);
    EXPECT_NEAR(other.rb(carriage).getPosition().z, 1.0f, 0.02f) << "held on the rail through anchorB";
}

TEST(Physics3DReview, LiveTwistLimitsCanMovePastEachOther) {
    World3D world(Vector3(0, 0, 0));
    const Entity arm = world.box(Vector3(0.5f, 0, 0), Vector3(1, 0.2f, 0.2f), BodyType::DYNAMIC);
    Joint3DComponent joint = makeJoint3D(Joint3DType::SWINGTWIST);
    joint.bodyA = arm;
    joint.anchor = Vector3(-0.5f, 0, 0);
    joint.twistAxis = Vector3(1, 0, 0);
    joint.planeAxis = Vector3(0, 1, 0);
    joint.twistMinAngle = Angle::degToDefault(-10.0f);
    joint.twistMaxAngle = Angle::degToDefault(10.0f);
    const Entity e = world.joint(joint);
    world.load();
    // bodyA's -40..-20: Jolt's 20..40 (its sign, LOCAL), whose minimum is past the old maximum 10
    world.j(e).setTwistMinAngle(Angle::degToDefault(-40.0f));
    world.j(e).setTwistMaxAngle(Angle::degToDefault(-20.0f));
    world.step();
    const JPH::SwingTwistConstraint* made = static_cast<const JPH::SwingTwistConstraint*>(world.jointComponent(e).joint);
    ASSERT_NE(made, nullptr);
    EXPECT_NEAR(Angle::radToDeg(made->GetTwistMinAngle()), 20.0f, 1e-3f);
    EXPECT_NEAR(Angle::radToDeg(made->GetTwistMaxAngle()), 40.0f, 1e-3f);
}

namespace {

    // two gears on hinges to a static frame, their bodies turned so local and world axes differ;
    // the first driven by a motor: how fast the second turns (rad/s about its world axis)
    float gearFollowerSpeed(JointAnchorSpace space, bool* ignoredPair = nullptr) {
        World3D world(Vector3(0, 0, 0));
        const Quaternion turned = degreesAbout(90.0f, Vector3(1, 0, 0));
        const Entity frame = world.box(Vector3(0, 0, -2), Vector3(0.2f, 0.2f, 0.2f), BodyType::STATIC);
        const Entity gearA = world.body(Vector3(-1, 0, 0), BodyType::DYNAMIC, NULL_ENTITY, turned);
        world.rb(gearA).createCylinderCollider(0.1f, 0.5f);
        const Entity gearB = world.body(Vector3(1, 0, 0), BodyType::DYNAMIC, NULL_ENTITY, turned);
        world.rb(gearB).createCylinderCollider(0.1f, 0.5f);
        auto hinge = [&](Entity gear) {
            Joint3DComponent h = makeJoint3D(Joint3DType::HINGE);
            h.bodyA = gear;
            h.bodyB = frame;
            h.anchorSpace = JointAnchorSpace::WORLD;
            h.enableCollision = true;
            h.anchor = world.transform(gear).position;
            h.axis = Vector3(0, 0, 1);
            h.normal = Vector3(1, 0, 0);
            return world.joint(h);
        };
        const Entity hingeA = hinge(gearA);
        const Entity hingeB = hinge(gearB);
        world.jointComponent(hingeA).useMotor = true;
        world.jointComponent(hingeA).motorTargetVelocity = Angle::degToDefault(90.0f);
        Joint3DComponent gear = makeJoint3D(Joint3DType::GEAR);
        gear.anchorSpace = space;
        gear.enableCollision = true;
        gear.bodyA = gearA;
        gear.bodyB = gearB;
        gear.hingeA = hingeA;
        gear.hingeB = hingeB;
        world.joint(gear);
        world.load();
        world.step(60);
        if (ignoredPair) *ignoredPair = world.physics()->getPairFilter3D().isIgnored(gearA, gearB);
        return std::fabs(world.rb(gearB).getAngularVelocity().z);
    }

}

TEST(Physics3DReview, GearsTurnTogetherAboutTheirWorldAxes) {
    bool ignored = true;
    const float follower = gearFollowerSpeed(JointAnchorSpace::WORLD, &ignored);
    EXPECT_NEAR(follower, Angle::degToRad(90.0f), Angle::degToRad(90.0f) * 0.05f);
    EXPECT_FALSE(ignored) << "an old (world) gear's bodies still collide, as before";
    gearFollowerSpeed(JointAnchorSpace::LOCAL, &ignored);
    EXPECT_TRUE(ignored) << "a new gear keeps its overlapping teeth apart";
}

TEST(Physics3DReview, ShapeSwapWakesWhatRestsOnIt) {
    World3D world;
    const Entity platform = world.box(Vector3(0, 0, 0), Vector3(4, 0.2f, 4), BodyType::KINEMATIC);
    const Entity crate = world.box(Vector3(0, 0.6f, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    world.load();
    for (int i = 0; i < 300 && !world.rb(crate).isSleeping(); i++) world.step();
    ASSERT_TRUE(world.rb(crate).isSleeping()) << "the crate fell asleep";
    world.rb(platform).setColliderBoxSize(0, Vector3(4, 0.1f, 4));
    world.step();
    EXPECT_FALSE(world.rb(crate).isSleeping()) << "woken by the swap";
    world.step(30);
    EXPECT_LT(world.rb(crate).getPosition().y, 0.58f) << "settled on the thinner platform";
}

TEST(Physics3DReview, The3DWorldIsMadeWhenFirstNeeded) {
    World3D world;
    world.physics()->load();
    world.step(3);
    world.physics()->interpolate(0.5);
    world.physics()->draw();
    Ray ray(Vector3(0, 10, 0), Vector3(0, -20, 0));
    EXPECT_FALSE(ray.intersects(&world.scene, RayFilter::BODY_3D).hit);
    EXPECT_FALSE(world.physics()->hasWorld3D()) << "a scene without 3D bodies pays nothing";
    world.box(Vector3(0, 0, 0), Vector3(1, 1, 1), BodyType::STATIC);
    world.step();
    EXPECT_TRUE(world.physics()->hasWorld3D());
    EXPECT_TRUE(ray.intersects(&world.scene, RayFilter::BODY_3D).hit);
}

TEST(Physics3DReview, ExportedCapacityDefaultsAreTheEditors) {
    const TagsAndLayers exported{};
    const Physics3DWorldSettings project{};
    EXPECT_EQ(exported.maxBodies3D, project.maxBodies);
    EXPECT_EQ(exported.maxBodyPairs3D, project.maxBodyPairs);
    EXPECT_EQ(exported.maxContactConstraints3D, project.maxContactConstraints);
    EXPECT_EQ(project.maxBodyPairs, 8192u);
}

// ---- spec T7: a collider edit keeps the body's settings, joints and contacts ----------------------

TEST(Physics3DReview, ColliderEditKeepsSettingsJointsAndContacts) {
    World3D world;
    world.floor();
    const Entity ball = world.sphere(Vector3(0, 0.5f, 0), 0.5f, BodyType::DYNAMIC);
    world.rb(ball).setAllowSleeping(false);
    world.rb(ball).setLinearDamping(0.3f);
    int removed = 0;
    world.physics()->onContactRemoved3D.add("removed", std::function<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>(
        [&](Rigidbody, Rigidbody, unsigned long, unsigned long) { removed++; }));
    world.load();
    world.step(60);
    const int before = removed;
    world.rb(ball).setColliderRadius(0, 0.55f);
    world.step(30);
    EXPECT_EQ(removed, before) << "the resting contact stayed";
    EXPECT_FALSE(world.component(ball).allowSleeping == true);
    EXPECT_FALSE(world.rb(ball).isSleeping()) << "allowSleeping false still holds";
    EXPECT_FLOAT_EQ(world.rb(ball).getLinearDamping(), 0.3f);
}
