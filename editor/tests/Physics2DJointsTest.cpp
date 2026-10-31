// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// 2D joints with every parameter (physics-2d spec T1, T3-T7): joints of scenes saved before the
// fields existed build as Box2D's defaults did, local anchors follow their bodies (a car placed
// anywhere), wheel suspension and motors, revolute limits, prismatic, distance spring, weld,
// motor and mouse joints, and breaking with its four actions and the onJointBreak2D callback.

#include <gtest/gtest.h>

#include "Physics2DTestWorld.h"

#include "Stream.h"
#include "component/ScriptComponent.h"
#include "physics/PhysicsMaterial.h"
#include "script/LuaBinding.h"
#include "script/ScriptBase.h"
#include "script/ScriptMessages.h"
#include "yaml-cpp/yaml.h"

#include "lua.hpp"

#include <limits>
#include <sstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::physics2dtest;

namespace {

    // a joint as the editor wrote it before (type, bodies, world anchors, axis, rope)
    Joint2DComponent decodeLegacyJoint(const std::string& type, Entity bodyA, Entity bodyB, const std::string& anchorA,
                                       const std::string& anchorB, const std::string& axis, bool autoAnchors, bool rope) {
        std::ostringstream yaml;
        yaml << "enabled: true\ntype: " << type << "\nbodyA: " << bodyA << "\nbodyB: " << bodyB
             << "\nanchorA: " << anchorA << "\nanchorB: " << anchorB << "\naxis: " << axis
             << "\ntarget: [0, 0]\nautoAnchors: " << (autoAnchors ? "true" : "false")
             << "\nrope: " << (rope ? "true" : "false") << "\n";
        return Stream::decodeJoint2DComponent(YAML::Load(yaml.str()));
    }

    void addBox(World2D& world, Entity e, float width, float height) {
        world.rb(e).createCenteredBoxCollider(width, height);
    }

}

// ---- T1: joints of old scenes are built exactly as before ---------------------------------------

TEST(Physics2DJoints, LegacyJointsBuildWithBoxDefaults) {
    World2D world;
    const Entity anchor = world.body(Vector3(0, 200, 0), BodyType::STATIC);
    addBox(world, anchor, 20, 20);
    const Entity mover = world.body(Vector3(64, 200, 0), BodyType::DYNAMIC);
    addBox(world, mover, 20, 20);

    struct Case { std::string type; bool rope; };
    const Case cases[] = {{"distance", false}, {"distance", true}, {"revolute", false}, {"prismatic", false},
                          {"wheel", false}, {"weld", false}, {"motor", false}, {"mouse", false}};
    std::vector<Entity> joints;
    for (const Case& c : cases) {
        const bool distance = c.type == "distance";
        const Joint2DComponent legacy = decodeLegacyJoint(c.type, c.type == "mouse" ? mover : anchor, c.type == "mouse" ? anchor : mover,
                                                          distance ? "[0, 200]" : "[32, 200]", distance ? "[64, 200]" : "[0, 0]",
                                                          "[0, 1]", false, c.rope);
        EXPECT_EQ(legacy.anchorSpace, JointAnchorSpace::WORLD) << c.type;
        EXPECT_TRUE(std::isinf(legacy.breakForce)) << c.type;
        joints.push_back(world.joint(legacy));
    }
    world.load();

    const b2DistanceJointDef distanceDef = b2DefaultDistanceJointDef();
    const b2RevoluteJointDef revoluteDef = b2DefaultRevoluteJointDef();
    const b2PrismaticJointDef prismaticDef = b2DefaultPrismaticJointDef();
    const b2WheelJointDef wheelDef = b2DefaultWheelJointDef();
    const b2WeldJointDef weldDef = b2DefaultWeldJointDef();
    const b2MotorJointDef motorDef = b2DefaultMotorJointDef();
    const b2MouseJointDef mouseDef = b2DefaultMouseJointDef();

    for (size_t i = 0; i < joints.size(); i++) {
        const b2JointId id = world.jointComponent(joints[i]).joint;
        ASSERT_TRUE(b2Joint_IsValid(id)) << cases[i].type;
        EXPECT_FALSE(b2Joint_GetCollideConnected(id)) << cases[i].type;
    }

    // distance: rigid, no limit, no motor; the length between the world anchors
    b2JointId id = world.jointComponent(joints[0]).joint;
    EXPECT_EQ(b2DistanceJoint_IsSpringEnabled(id), distanceDef.enableSpring);
    EXPECT_EQ(b2DistanceJoint_GetSpringHertz(id), distanceDef.hertz);
    EXPECT_EQ(b2DistanceJoint_GetSpringDampingRatio(id), distanceDef.dampingRatio);
    EXPECT_EQ(b2DistanceJoint_IsLimitEnabled(id), distanceDef.enableLimit);
    EXPECT_EQ(b2DistanceJoint_GetMaxLength(id), distanceDef.maxLength);
    EXPECT_EQ(b2DistanceJoint_IsMotorEnabled(id), distanceDef.enableMotor);
    EXPECT_EQ(b2DistanceJoint_GetMaxMotorForce(id), distanceDef.maxMotorForce);
    EXPECT_EQ(b2DistanceJoint_GetMotorSpeed(id), distanceDef.motorSpeed);
    EXPECT_NEAR(b2DistanceJoint_GetLength(id), 1.0f, 1e-6f);
    EXPECT_NEAR(b2Joint_GetLocalAnchorA(id).x, 0.0f, 1e-6f);
    EXPECT_NEAR(b2Joint_GetLocalAnchorB(id).x, 0.0f, 1e-6f);

    // rope: a zero-hertz spring with the limit [0, length], as the old code made it
    id = world.jointComponent(joints[1]).joint;
    EXPECT_TRUE(b2DistanceJoint_IsSpringEnabled(id));
    EXPECT_EQ(b2DistanceJoint_GetSpringHertz(id), 0.0f);
    EXPECT_TRUE(b2DistanceJoint_IsLimitEnabled(id));
    EXPECT_NEAR(b2DistanceJoint_GetMaxLength(id), 1.0f, 1e-6f);

    // revolute: no spring, limit, motor; the pivot's local points; reference angle 0
    id = world.jointComponent(joints[2]).joint;
    EXPECT_EQ(b2RevoluteJoint_IsSpringEnabled(id), revoluteDef.enableSpring);
    EXPECT_EQ(b2RevoluteJoint_GetSpringHertz(id), revoluteDef.hertz);
    EXPECT_EQ(b2RevoluteJoint_IsLimitEnabled(id), revoluteDef.enableLimit);
    EXPECT_EQ(b2RevoluteJoint_GetLowerLimit(id), revoluteDef.lowerAngle);
    EXPECT_EQ(b2RevoluteJoint_GetUpperLimit(id), revoluteDef.upperAngle);
    EXPECT_EQ(b2RevoluteJoint_IsMotorEnabled(id), revoluteDef.enableMotor);
    EXPECT_EQ(b2RevoluteJoint_GetMaxMotorTorque(id), revoluteDef.maxMotorTorque);
    EXPECT_EQ(b2RevoluteJoint_GetMotorSpeed(id), revoluteDef.motorSpeed);
    EXPECT_NEAR(b2Joint_GetLocalAnchorA(id).x, 0.5f, 1e-6f);
    EXPECT_NEAR(b2Joint_GetLocalAnchorB(id).x, -0.5f, 1e-6f);
    EXPECT_NEAR(world.j(joints[2]).getReferenceAngle(), 0.0f, 1e-6f);

    id = world.jointComponent(joints[3]).joint;
    EXPECT_EQ(b2PrismaticJoint_IsSpringEnabled(id), prismaticDef.enableSpring);
    EXPECT_EQ(b2PrismaticJoint_IsLimitEnabled(id), prismaticDef.enableLimit);
    EXPECT_EQ(b2PrismaticJoint_GetLowerLimit(id), prismaticDef.lowerTranslation);
    EXPECT_EQ(b2PrismaticJoint_IsMotorEnabled(id), prismaticDef.enableMotor);
    EXPECT_EQ(b2PrismaticJoint_GetMaxMotorForce(id), prismaticDef.maxMotorForce);

    // wheel: Box2D's 1 Hz / 0.7 suspension
    id = world.jointComponent(joints[4]).joint;
    EXPECT_EQ(b2WheelJoint_IsSpringEnabled(id), wheelDef.enableSpring);
    EXPECT_EQ(b2WheelJoint_GetSpringHertz(id), wheelDef.hertz);
    EXPECT_EQ(b2WheelJoint_GetSpringDampingRatio(id), wheelDef.dampingRatio);
    EXPECT_EQ(b2WheelJoint_IsLimitEnabled(id), wheelDef.enableLimit);
    EXPECT_EQ(b2WheelJoint_IsMotorEnabled(id), wheelDef.enableMotor);
    EXPECT_EQ(b2WheelJoint_GetMaxMotorTorque(id), wheelDef.maxMotorTorque);

    id = world.jointComponent(joints[5]).joint;
    EXPECT_EQ(b2WeldJoint_GetReferenceAngle(id), weldDef.referenceAngle);
    EXPECT_EQ(b2WeldJoint_GetLinearHertz(id), weldDef.linearHertz);
    EXPECT_EQ(b2WeldJoint_GetAngularHertz(id), weldDef.angularHertz);
    EXPECT_EQ(b2WeldJoint_GetLinearDampingRatio(id), weldDef.linearDampingRatio);

    id = world.jointComponent(joints[6]).joint;
    EXPECT_EQ(b2MotorJoint_GetMaxForce(id), motorDef.maxForce);
    EXPECT_EQ(b2MotorJoint_GetMaxTorque(id), motorDef.maxTorque);
    EXPECT_EQ(b2MotorJoint_GetCorrectionFactor(id), motorDef.correctionFactor);
    EXPECT_EQ(b2MotorJoint_GetLinearOffset(id).x, motorDef.linearOffset.x);
    EXPECT_EQ(b2MotorJoint_GetAngularOffset(id), motorDef.angularOffset);

    // mouse: 4 Hz, 1, and max(10, 1000 x mass)
    id = world.jointComponent(joints[7]).joint;
    EXPECT_EQ(b2MouseJoint_GetSpringHertz(id), mouseDef.hertz);
    EXPECT_EQ(b2MouseJoint_GetSpringDampingRatio(id), mouseDef.dampingRatio);
    EXPECT_NEAR(b2MouseJoint_GetMaxForce(id), std::max(10.0f, 1000.0f * b2Body_GetMass(world.component(mover).body)), 1e-3f);
}

TEST(Physics2DJoints, LegacyKeysDecodeToTheLegacyTableAndNewJointsToUnity) {
    const Joint2DComponent wheel = decodeLegacyJoint("wheel", 1, 2, "[0, 0]", "[0, 0]", "[0, 1]", true, false);
    EXPECT_TRUE(wheel.enableSpring);
    EXPECT_EQ(wheel.frequency, 1.0f);
    EXPECT_EQ(wheel.dampingRatio, 0.7f);
    EXPECT_EQ(wheel.maxMotorTorque, 0.0f);
    EXPECT_EQ(wheel.anchorSpace, JointAnchorSpace::WORLD);
    EXPECT_FALSE(wheel.autoOffset);

    const Joint2DComponent mouse = decodeLegacyJoint("mouse", 1, 2, "[0, 0]", "[0, 0]", "[0, 0]", true, false);
    EXPECT_EQ(mouse.frequency, 4.0f);
    EXPECT_EQ(mouse.maxForce, 0.0f);
    const Joint2DComponent motor = decodeLegacyJoint("motor", 1, 2, "[0, 0]", "[0, 0]", "[0, 0]", true, false);
    EXPECT_EQ(motor.maxForce, 1.0f);
    EXPECT_EQ(motor.maxTorque, 1.0f);
    const Joint2DComponent revolute = decodeLegacyJoint("revolute", 1, 2, "[0, 0]", "[0, 0]", "[0, 0]", true, false);
    EXPECT_EQ(revolute.lowerAngle, 0.0f);
    EXPECT_EQ(revolute.upperAngle, 0.0f);

    // new joints: Unity's defaults, per type
    const Joint2DComponent fresh = makeJoint2D(Joint2DType::WHEEL);
    EXPECT_EQ(fresh.anchorSpace, JointAnchorSpace::LOCAL);
    EXPECT_EQ(fresh.frequency, 2.0f);
    EXPECT_EQ(fresh.dampingRatio, 0.7f);
    EXPECT_EQ(fresh.axis, Vector2(0.0f, 1.0f));
    EXPECT_EQ(fresh.maxMotorTorque, 10000.0f);
    EXPECT_TRUE(std::isinf(fresh.breakForce));
    EXPECT_EQ(fresh.breakAction, JointBreakAction::DESTROY);
    EXPECT_FALSE(fresh.enableCollision);
    const Joint2DComponent hinge = makeJoint2D(Joint2DType::REVOLUTE);
    EXPECT_EQ(hinge.lowerAngle, -90.0f);
    EXPECT_EQ(hinge.upperAngle, 90.0f);
    EXPECT_FALSE(hinge.useLimits);

    // a type change keeps edited fields and gives the others the new type's defaults
    Joint2DComponent changed = makeJoint2D(Joint2DType::DISTANCE);
    changeJoint2DType(changed, Joint2DType::WHEEL);
    EXPECT_TRUE(changed.enableSpring);
    EXPECT_EQ(changed.frequency, 2.0f);
    Joint2DComponent edited = makeJoint2D(Joint2DType::DISTANCE);
    edited.frequency = 7.0f;
    changeJoint2DType(edited, Joint2DType::WHEEL);
    EXPECT_EQ(edited.frequency, 7.0f);
    changeJoint2DType(edited, Joint2DType::MOUSE);
    EXPECT_EQ(edited.maxForce, 0.0f);
}

// ---- T3, T4: the hill climb car -------------------------------------------------------------------

namespace {

    struct Car {
        Entity body = NULL_ENTITY;
        Entity front = NULL_ENTITY;
        Entity rear = NULL_ENTITY;
        Entity frontJoint = NULL_ENTITY;
        Entity rearJoint = NULL_ENTITY;
    };

    // PlayerCar.prefab: two wheel joints, Unity anchors x64, suspension frequency / 0.35, mass 1
    // everywhere, tyre friction 2; dropped on a flat static ground at `origin`
    Car buildCar(World2D& world, const Vector3& origin, float frequency, bool groundUnder = true, BodyType bodyType = BodyType::DYNAMIC) {
        if (groundUnder) {
            const Entity ground = world.body(origin + Vector3(0, -40, 0), BodyType::STATIC);
            addBox(world, ground, 3000, 40);
        }
        Car car;
        const Vector2 frontAnchor(1.153f * 64.0f, 0.31f * 64.0f);
        const Vector2 rearAnchor(-1.444f * 64.0f, 0.31f * 64.0f);
        // wheels 4 points above the ground (top at -20; wheel bottoms 1.92 below the car's origin)
        const Vector3 carPosition = origin + Vector3(0, -14, 0);
        car.body = world.body(carPosition, bodyType);
        // the body's box sits above the wheels' bottoms (the prefab's chassis): the car rests on them
        world.rb(car.body).createCenteredBoxCollider(200, 30, Vector2(0, 45), 0);
        const PhysicsMaterial tire = PhysicsMaterial::create(2.0f, 0.0f);
        for (int w = 0; w < 2; w++) {
            const Vector2 anchor = w == 0 ? frontAnchor : rearAnchor;
            Entity wheel = world.body(carPosition + Vector3(anchor.x, anchor.y, 0), BodyType::DYNAMIC);
            world.rb(wheel).createCircleCollider(Vector2(0, 0), 0.34f * 64.0f);
            world.rb(wheel).setColliderMaterial(0, tire);
            Rigidbody2DComponent& wheelBody = world.component(wheel);
            wheelBody.useAutoMass = false;
            wheelBody.mass = 1.0f;
            wheelBody.angularDamping = 0.05f;
            wheelBody.collisionDetection = CollisionDetectionMode2D::CONTINUOUS;

            Joint2DComponent joint = makeJoint2D(Joint2DType::WHEEL);
            joint.bodyA = car.body;
            joint.bodyB = wheel;
            joint.anchorA = anchor;
            joint.axis = Vector2(0.0f, 1.0f);
            joint.frequency = frequency;
            joint.dampingRatio = 0.35f;
            joint.maxMotorTorque = 10000.0f;
            const Entity jointEntity = world.joint(joint, car.body);
            (w == 0 ? car.front : car.rear) = wheel;
            (w == 0 ? car.frontJoint : car.rearJoint) = jointEntity;
        }
        Rigidbody2DComponent& carBody = world.component(car.body);
        carBody.useAutoMass = false;
        carBody.mass = 1.0f;
        carBody.angularDamping = 0.05f;
        carBody.collisionDetection = CollisionDetectionMode2D::CONTINUOUS;
        carBody.material = "";
        return car;
    }

}

TEST(Physics2DJoints, HillClimbCarRestsOnItsSuspension) {
    float compression[2] = {0.0f, 0.0f};
    for (int run = 0; run < 2; run++) {
        World2D world;
        Car car = buildCar(world, Vector3(0, 0, 0), run == 0 ? 3.0f : 6.0f);
        world.load();
        ASSERT_TRUE(b2Joint_IsValid(world.jointComponent(car.frontJoint).joint));
        // the wheel's connected anchor came from where anchor A was (auto configured)
        EXPECT_NEAR(world.jointComponent(car.frontJoint).anchorB.x, 0.0f, 1e-3f);
        world.step(120);

        Rigidbody2D body = world.rb(car.body);
        EXPECT_LT(body.getLinearVelocity().length(), 0.5f) << "at rest after 2 s";
        EXPECT_NEAR(body.getMass(), 1.0f, 1e-5f);
        for (Entity wheel : {car.front, car.rear}) {
            // on the ground (top at -20): centre one radius above
            EXPECT_NEAR(world.rb(wheel).getPosition().y, -20.0f + 0.34f * 64.0f, 1.0f);
            EXPECT_NEAR(world.rb(wheel).getMass(), 1.0f, 1e-5f);
        }
        // the body sank on the springs: wheel above the anchor along the axis (static deflection
        // g / (2 pi f)^2: 1.8 points at 3 Hz, 0.44 at 6 Hz)
        compression[run] = world.j(car.frontJoint).getJointTranslation();
        EXPECT_GT(compression[run], run == 0 ? 1.0f : 0.2f) << "frequency " << (run == 0 ? 3 : 6);
        EXPECT_GT(world.j(car.rearJoint).getJointTranslation(), run == 0 ? 1.0f : 0.2f);
    }
    EXPECT_GT(compression[0], compression[1] * 1.5f) << "stiffer suspension (6 Hz) compresses less than 3 Hz";
}

TEST(Physics2DJoints, LocalAnchorsGiveTheSameCarWhereverItIs) {
    std::vector<Vector2> poses[2];
    for (int run = 0; run < 2; run++) {
        World2D world;
        const Vector3 origin(run == 0 ? 0.0f : 1000.0f, 0, 0);
        Car car = buildCar(world, origin, 3.0f);
        world.load();
        world.step(120);
        for (Entity e : {car.body, car.front, car.rear}) {
            poses[run].push_back(world.rb(e).getPosition() - Vector2(origin.x, origin.y));
        }
    }
    for (size_t i = 0; i < poses[0].size(); i++) {
        EXPECT_NEAR(poses[0][i].x, poses[1][i].x, 1e-3f * 64.0f) << i;
        EXPECT_NEAR(poses[0][i].y, poses[1][i].y, 1e-3f * 64.0f) << i;
    }
}

TEST(Physics2DJoints, WheelMotorReachesItsSpeedQuickly) {
    World2D world;
    Car car = buildCar(world, Vector3(0, 0, 0), 3.0f, false, BodyType::STATIC);
    world.load();
    Joint2D motor = world.j(car.rearJoint);
    motor.setUseMotor(true);
    motor.setMotorSpeed(-720.0f);
    motor.setMaxMotorTorque(10000.0f);
    world.step(30);
    EXPECT_NEAR(motor.getJointSpeed(), -720.0f, 720.0f * 0.02f);
    EXPECT_NEAR(world.rb(car.rear).getAngularVelocity(), rad(-720.0f), rad(720.0f) * 0.02f);
    EXPECT_GT(std::fabs(motor.getMotorTorque()), 0.0f);
}

// ---- T5, T6: limits, springs, the other joint types ---------------------------------------------

TEST(Physics2DJoints, RevoluteLimitsHoldThePendulum) {
    for (int side = 0; side < 2; side++) {
        World2D world;
        const Entity pivot = world.body(Vector3(0, 0, 0), BodyType::STATIC);
        // a rod pendulum: Box2D 3's limit acts on the body's own inertia, a point bob on a long
        // arm overshoots it by degrees (Implementation notes)
        const Entity bob = world.body(Vector3(side == 0 ? 40.0f : -40.0f, 0, 0), BodyType::DYNAMIC);
        addBox(world, bob, 80, 8);
        Joint2DComponent joint = makeJoint2D(Joint2DType::REVOLUTE);
        joint.bodyA = pivot;
        joint.bodyB = bob;
        joint.useLimits = true;
        joint.lowerAngle = -30.0f;
        joint.upperAngle = 30.0f;
        const Entity hinge = world.joint(joint);
        world.load();
        float minAngle = 0.0f, maxAngle = 0.0f;
        for (int i = 0; i < 180; i++) {
            world.step();
            minAngle = std::min(minAngle, world.j(hinge).getJointAngle());
            maxAngle = std::max(maxAngle, world.j(hinge).getJointAngle());
        }
        EXPECT_GE(minAngle, -31.0f);
        EXPECT_LE(maxAngle, 31.0f);
        // the right bob swings down clockwise to the lower limit, the left one to the upper
        EXPECT_EQ(world.j(hinge).getLimitState(), side == 0 ? JointLimitState2D::LOWER : JointLimitState2D::UPPER);
        EXPECT_NEAR(world.j(hinge).getJointAngle(), side == 0 ? -30.0f : 30.0f, 1.5f);
    }
}

TEST(Physics2DJoints, PrismaticMotorStopsAtItsLimit) {
    World2D world(Vector2(0, 0));
    const Entity base = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    const Entity slider = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
    addBox(world, slider, 64, 64);
    Joint2DComponent joint = makeJoint2D(Joint2DType::PRISMATIC);
    joint.bodyA = base;
    joint.bodyB = slider;
    joint.axis = Vector2(1, 0);
    joint.useLimits = true;
    joint.lowerTranslation = 0.0f;
    joint.upperTranslation = 50.0f;
    joint.useMotor = true;
    joint.motorSpeed = 100.0f;
    // 1 kg: a force in proportion (Box2D 3 limits are soft, Unity's default 10000 N goes 18 points past)
    joint.maxMotorForce = 100.0f;
    const Entity e = world.joint(joint);
    world.load();
    world.step(60);
    EXPECT_NEAR(world.j(e).getJointTranslation(), 50.0f, 1.0f);
    EXPECT_NEAR(world.rb(slider).getPosition().x, 50.0f, 1.0f);
    EXPECT_EQ(world.j(e).getLimitState(), JointLimitState2D::UPPER);
    // the motor pushes against the limit
    EXPECT_GT(world.j(e).getMotorForce(), 0.0f);
}

TEST(Physics2DJoints, DistanceSpringOscillatesAtItsFrequency) {
    World2D world(Vector2(0, 0));
    const Entity hook = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    const Entity ball = world.body(Vector3(100, 0, 0), BodyType::DYNAMIC);
    world.rb(ball).createCircleCollider(Vector2(0, 0), 4);
    Joint2DComponent joint = makeJoint2D(Joint2DType::DISTANCE);
    joint.bodyA = hook;
    joint.bodyB = ball;
    joint.autoLength = false;
    joint.length = 80.0f;
    joint.enableSpring = true;
    joint.frequency = 1.0f;
    joint.dampingRatio = 0.0f;
    const Entity e = world.joint(joint);
    world.load();

    // upward crossings of the rest length
    std::vector<int> crossings;
    float previous = world.rb(ball).getPosition().x - 80.0f;
    for (int i = 1; i <= 300; i++) {
        world.step();
        const float offset = world.rb(ball).getPosition().x - 80.0f;
        if (previous < 0.0f && offset >= 0.0f) crossings.push_back(i);
        previous = offset;
    }
    ASSERT_GE(crossings.size(), 3u);
    const float period = (crossings.back() - crossings.front()) * STEP / (crossings.size() - 1);
    EXPECT_NEAR(period, 1.0f, 0.05f);
    EXPECT_NEAR(world.j(e).getCurrentLength(), world.rb(ball).getPosition().x, 1e-2f);
}

TEST(Physics2DJoints, WeldStiffnessBendsAndReturns) {
    World2D world(Vector2(0, 0));
    const Entity base = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    const Entity arm = world.body(Vector3(40, 0, 0), BodyType::DYNAMIC);
    addBox(world, arm, 80, 8);
    Joint2DComponent joint = makeJoint2D(Joint2DType::WELD);
    joint.bodyA = base;
    joint.bodyB = arm;
    joint.anchorA = Vector2(0, 0);
    joint.angularFrequency = 2.0f;
    joint.angularDampingRatio = 0.7f;
    world.joint(joint);
    world.load();
    world.rb(arm).applyAngularImpulse(0.2f, true);
    float maxAngle = 0.0f;
    for (int i = 0; i < 180; i++) {
        world.step();
        maxAngle = std::max(maxAngle, std::fabs(world.rb(arm).getAngle()));
    }
    EXPECT_GT(maxAngle, 2.0f) << "a soft weld bends";
    EXPECT_LT(std::fabs(world.rb(arm).getAngle()), 1.0f) << "and returns";
}

TEST(Physics2DJoints, MotorJointHoldsTheRelativePoseAndMouseKeepsItsForce) {
    World2D world;
    const Entity base = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    const Entity follower = world.body(Vector3(50, 30, 0), BodyType::DYNAMIC);
    addBox(world, follower, 10, 10);
    Joint2DComponent joint = makeJoint2D(Joint2DType::MOTOR);
    joint.bodyA = base;
    joint.bodyB = follower;
    const Entity e = world.joint(joint);

    const Entity dragged = world.body(Vector3(-100, 0, 0), BodyType::DYNAMIC);
    addBox(world, dragged, 10, 10);
    Joint2DComponent mouse = makeJoint2D(Joint2DType::MOUSE);
    mouse.bodyA = dragged;
    mouse.target = Vector2(-100, 0);
    const Entity mouseEntity = world.joint(mouse);
    world.load();
    world.step(120);
    EXPECT_NEAR(world.rb(follower).getPosition().x, 50.0f, 1.0f);
    EXPECT_NEAR(world.rb(follower).getPosition().y, 30.0f, 1.0f);
    // the offsets were written back (Unity autoConfigureOffset)
    EXPECT_NEAR(world.jointComponent(e).linearOffset.x, 50.0f, 1e-3f);

    // a mouse joint without bodyB pulls to the world target; maxForce 0 is the old formula
    const b2JointId mouseId = world.jointComponent(mouseEntity).joint;
    ASSERT_TRUE(b2Joint_IsValid(mouseId));
    EXPECT_NEAR(b2MouseJoint_GetMaxForce(mouseId), std::max(10.0f, 1000.0f * world.rb(dragged).getMass()), 1e-2f);
    EXPECT_NEAR(world.rb(dragged).getPosition().y, 0.0f, 2.0f);
}

TEST(Physics2DJoints, LiveSettersReachTheJointWithoutRebuilding) {
    World2D world;
    const Entity pivot = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    const Entity wheel = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
    world.rb(wheel).createCircleCollider(Vector2(0, 0), 10);
    Joint2DComponent joint = makeJoint2D(Joint2DType::REVOLUTE);
    joint.bodyA = pivot;
    joint.bodyB = wheel;
    const Entity e = world.joint(joint);
    world.load();
    const b2JointId before = world.jointComponent(e).joint;
    Joint2D handle = world.j(e);
    handle.setUseMotor(true);
    handle.setMotorSpeed(90.0f);
    handle.setMaxMotorTorque(1000.0f);
    handle.setUseLimits(true);
    handle.setLowerAngle(-45.0f);
    handle.setUpperAngle(45.0f);
    EXPECT_TRUE(b2RevoluteJoint_IsMotorEnabled(before));
    EXPECT_NEAR(b2RevoluteJoint_GetMotorSpeed(before), rad(90.0f), 1e-5f);
    EXPECT_NEAR(b2RevoluteJoint_GetLowerLimit(before), rad(-45.0f), 1e-5f);
    world.step(5);
    EXPECT_EQ(world.jointComponent(e).joint.index1, before.index1);
    EXPECT_EQ(world.jointComponent(e).joint.revision, before.revision);

    // a body change rebuilds it (Unity: a new connectedBody)
    const Entity other = world.body(Vector3(30, 0, 0), BodyType::DYNAMIC);
    world.rb(other).createCircleCollider(Vector2(0, 0), 5);
    handle.setBodyB(other);
    world.step();
    ASSERT_TRUE(b2Joint_IsValid(world.jointComponent(e).joint));
    EXPECT_TRUE(B2_ID_EQUALS(b2Joint_GetBodyB(world.jointComponent(e).joint), world.component(other).body));
}

// ---- T7: breaking -------------------------------------------------------------------------------

namespace {

    struct BreakCounter : public ScriptBase {
        int* calls;
        JointBreakAction* seenAction;
        BreakCounter(Scene* scene, Entity entity, int* calls, JointBreakAction* seenAction)
            : ScriptBase(scene, entity), calls(calls), seenAction(seenAction) {}
        void onJointBreak2D(Joint2D& joint) override {
            (*calls)++;
            *seenAction = joint.getBreakAction();
        }
    };

    void addCppListener(World2D& world, Entity entity, BreakCounter* script) {
        ScriptComponent scripts;
        ScriptEntry entry;
        entry.type = ScriptType::CPP;
        entry.enabled = true;
        entry.className = "BreakCounter";
        entry.instance = script;
        entry.cppScript = ScriptMessages::asScriptBase(script);
        entry.cppMessageMask = ScriptMessages::cppMask<BreakCounter>();
        scripts.scripts.push_back(entry);
        world.scene.addComponent<ScriptComponent>(entity, scripts);
        awakeScripts(world.scene, entity);
    }

    // a 10 kg body hanging on a revolute joint that breaks at 50 N (its weight is 98 N)
    struct Hanging {
        World2D world;
        Entity anchor, weight, joint;
        explicit Hanging(JointBreakAction action, Entity* parentForJoint = nullptr) {
            anchor = world.body(Vector3(0, 100, 0), BodyType::STATIC);
            weight = world.body(Vector3(0, 60, 0), BodyType::DYNAMIC);
            world.rb(weight).createCircleCollider(Vector2(0, 0), 8);
            world.component(weight).useAutoMass = false;
            world.component(weight).mass = 10.0f;
            Joint2DComponent revolute = makeJoint2D(Joint2DType::REVOLUTE);
            revolute.bodyA = weight;
            revolute.bodyB = anchor;
            revolute.anchorA = Vector2(0, 40);
            revolute.breakForce = 50.0f;
            revolute.breakAction = action;
            joint = world.joint(revolute, parentForJoint ? *parentForJoint : NULL_ENTITY);
        }
    };

}

TEST(Physics2DJoints, BreakingDestroyCallsBackThenRemovesTheJoint) {
    Hanging h(JointBreakAction::DESTROY);
    int jointCalls = 0, bodyCalls = 0;
    JointBreakAction seen = JointBreakAction::IGNORE;
    BreakCounter onJoint(&h.world.scene, h.joint, &jointCalls, &seen);
    BreakCounter onBody(&h.world.scene, h.weight, &bodyCalls, &seen);
    addCppListener(h.world, h.joint, &onJoint);
    addCppListener(h.world, h.weight, &onBody);
    h.world.load();
    h.world.step();
    EXPECT_EQ(jointCalls, 1) << "the joint's GameObject";
    EXPECT_EQ(bodyCalls, 1) << "and bodyA's";
    EXPECT_EQ(seen, JointBreakAction::DESTROY) << "the joint still exists during the callback";
    EXPECT_EQ(h.world.scene.findComponent<Joint2DComponent>(h.joint), nullptr) << "removed after the dispatch";
    h.world.step(10);
    EXPECT_EQ(jointCalls, 1);
    EXPECT_LT(h.world.rb(h.weight).getPosition().y, 55.0f) << "falls";
    h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    h.world.scene.getComponent<ScriptComponent>(h.weight).scripts.clear();
}

TEST(Physics2DJoints, BreakingDisableIgnoreAndCallbackOnly) {
    {
        Hanging h(JointBreakAction::DISABLE);
        int calls = 0;
        JointBreakAction seen;
        BreakCounter onJoint(&h.world.scene, h.joint, &calls, &seen);
        addCppListener(h.world, h.joint, &onJoint);
        h.world.load();
        h.world.step(2);
        EXPECT_EQ(calls, 1);
        ASSERT_NE(h.world.scene.findComponent<Joint2DComponent>(h.joint), nullptr);
        EXPECT_FALSE(h.world.jointComponent(h.joint).enabled);
        EXPECT_TRUE(h.world.j(h.joint).isBroken());
        EXPECT_FALSE(b2Joint_IsValid(h.world.jointComponent(h.joint).joint)) << "out of the world";
        // a script enables it again: a fresh joint
        h.world.jointComponent(h.joint).breakForce = std::numeric_limits<float>::infinity();
        h.world.j(h.joint).setEnabled(true);
        h.world.step();
        EXPECT_TRUE(b2Joint_IsValid(h.world.jointComponent(h.joint).joint));
        EXPECT_FALSE(h.world.j(h.joint).isBroken());
        h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    }
    {
        Hanging h(JointBreakAction::IGNORE);
        int calls = 0;
        JointBreakAction seen;
        BreakCounter onJoint(&h.world.scene, h.joint, &calls, &seen);
        addCppListener(h.world, h.joint, &onJoint);
        h.world.load();
        h.world.step(30);
        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(b2Joint_IsValid(h.world.jointComponent(h.joint).joint));
        EXPECT_NEAR(h.world.rb(h.weight).getPosition().y, 60.0f, 2.0f) << "still hanging";
        h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    }
    {
        Hanging h(JointBreakAction::CALLBACK_ONLY);
        int calls = 0;
        JointBreakAction seen;
        BreakCounter onJoint(&h.world.scene, h.joint, &calls, &seen);
        addCppListener(h.world, h.joint, &onJoint);
        h.world.load();
        h.world.step(30);
        EXPECT_EQ(calls, 1) << "once while the load stays over the threshold";
        EXPECT_TRUE(b2Joint_IsValid(h.world.jointComponent(h.joint).joint));
        // the load removed (no gravity on it), the joint re-arms; loaded again, it reports again
        h.world.component(h.weight).gravityScale = 0.0f;
        h.world.component(h.weight).needApplyParameters = true;
        h.world.step(30);
        EXPECT_TRUE(h.world.jointComponent(h.joint).breakArmed);
        h.world.component(h.weight).gravityScale = 1.0f;
        h.world.component(h.weight).needApplyParameters = true;
        h.world.step(30);
        EXPECT_EQ(calls, 2);
        h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    }
}

TEST(Physics2DJoints, BreakingCallsLuaOnJointBreak2D) {
    Hanging h(JointBreakAction::DESTROY);
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    ASSERT_EQ(luaL_dostring(L,
        "jointBreakLog = {}\n"
        "JointBreakScript = {}\n"
        "function JointBreakScript:onJointBreak2D(joint)\n"
        "  jointBreakLog[#jointBreakLog + 1] = tostring(joint.breakForce) .. ':' .. tostring(joint.breakAction == JointBreakAction.DESTROY)\n"
        "end\n"
        "jointBreakInstance = setmetatable({}, {__index = JointBreakScript})\n"), LUA_OK);
    lua_getglobal(L, "jointBreakInstance");
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.enabled = false;  // disabled scripts get the message too (Unity)
    entry.className = "JointBreakScript";
    entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
    scripts.scripts.push_back(entry);
    h.world.scene.addComponent<ScriptComponent>(h.joint, scripts);
    awakeScripts(h.world.scene, h.joint);
    EXPECT_TRUE(ScriptMessages::receivers(&h.world.scene, h.joint) & scriptMessageBit(ScriptMessage::JointBreak2D));

    h.world.load();
    h.world.step(3);
    ASSERT_EQ(luaL_dostring(L, "jointBreakResult = table.concat(jointBreakLog, '|')"), LUA_OK);
    lua_getglobal(L, "jointBreakResult");
    const std::string result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "50.0:true");

    h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

// ---- T15: serialization of every joint field ------------------------------------------------------

TEST(Physics2DJoints, EveryFieldSurvivesEncodeAndDecode) {
    Joint2DComponent joint = makeJoint2D(Joint2DType::WHEEL);
    joint.bodyA = 7;
    joint.bodyB = 9;
    joint.anchorSpace = JointAnchorSpace::LOCAL;
    joint.anchorA = Vector2(1.5f, -2.0f);
    joint.anchorB = Vector2(3.0f, 4.0f);
    joint.autoAnchors = false;
    joint.axis = Vector2(0.6f, 0.8f);
    joint.target = Vector2(5.0f, 6.0f);
    joint.enableCollision = true;
    joint.breakForce = 120.0f;
    joint.breakAction = JointBreakAction::CALLBACK_ONLY;
    joint.useLimits = true;
    joint.lowerAngle = -10.0f;
    joint.upperAngle = 20.0f;
    joint.lowerTranslation = -5.0f;
    joint.upperTranslation = 6.0f;
    joint.useMotor = true;
    joint.motorSpeed = -720.0f;
    joint.maxMotorTorque = 500.0f;
    joint.maxMotorForce = 600.0f;
    joint.enableSpring = false;
    joint.frequency = 3.0f;
    joint.dampingRatio = 0.35f;
    joint.length = 40.0f;
    joint.autoLength = false;
    joint.minLength = 10.0f;
    joint.maxLength = 50.0f;
    joint.rope = true;
    joint.linearFrequency = 1.5f;
    joint.angularFrequency = 2.5f;
    joint.linearDampingRatio = 0.2f;
    joint.angularDampingRatio = 0.3f;
    joint.linearOffset = Vector2(7.0f, 8.0f);
    joint.angularOffset = 15.0f;
    joint.autoOffset = false;
    joint.maxForce = 70.0f;
    joint.maxTorque = 80.0f;
    joint.correctionFactor = 0.5f;
    joint.enabled = false;

    const YAML::Node node = Stream::encodeJoint2DComponent(joint);
    EXPECT_TRUE(node["breakForce"]);
    EXPECT_FALSE(node["breakTorque"]) << "infinite: not written";
    const Joint2DComponent decoded = Stream::decodeJoint2DComponent(YAML::Load(YAML::Dump(node)));
    EXPECT_EQ(YAML::Dump(Stream::encodeJoint2DComponent(decoded)), YAML::Dump(node));
    EXPECT_TRUE(std::isinf(decoded.breakTorque));
    EXPECT_EQ(decoded.breakForce, 120.0f);
    EXPECT_EQ(decoded.anchorSpace, JointAnchorSpace::LOCAL);
    EXPECT_EQ(decoded.breakAction, JointBreakAction::CALLBACK_ONLY);
    EXPECT_FALSE(decoded.enabled);
    EXPECT_EQ(decoded.correctionFactor, 0.5f);

    // decoded over a live component (Play's Stop restores the scene this way): a key left out
    // because it holds its default gives the default, not what the running scene had
    Joint2DComponent running = decoded;
    running.breakForce = 50.0f;
    running.breakTorque = 5.0f;
    Joint2DComponent never = joint;
    never.breakForce = std::numeric_limits<float>::infinity();
    const Joint2DComponent restored = Stream::decodeJoint2DComponent(YAML::Load(YAML::Dump(Stream::encodeJoint2DComponent(never))), &running);
    EXPECT_TRUE(std::isinf(restored.breakForce));
    EXPECT_TRUE(std::isinf(restored.breakTorque));
}

// ---- review fixes: the motor joint's frame, rebuilds for a body -------------------------------------

TEST(Physics2DJoints, MotorJointOffsetTurnsWithBodyA) {
    // Unity RelativeJoint2D: the offset is in bodyA's frame, so the follower orbits a turning base
    World2D world(Vector2(0, 0));
    const Entity base = world.body(Vector3(0, 0, 0), BodyType::KINEMATIC, NULL_ENTITY, 90.0f);
    addBox(world, base, 10, 10);
    const Entity follower = world.body(Vector3(50, 0, 0), BodyType::DYNAMIC);
    addBox(world, follower, 10, 10);
    Joint2DComponent joint = makeJoint2D(Joint2DType::MOTOR);
    joint.bodyA = base;
    joint.bodyB = follower;
    joint.maxForce = 1000.0f;
    joint.maxTorque = 1000.0f;
    joint.correctionFactor = 0.3f;
    const Entity e = world.joint(joint);
    world.load();
    // bodyA turned 90 degrees: bodyB's origin is 50 points along bodyA's -y
    EXPECT_NEAR(world.jointComponent(e).linearOffset.x, 0.0f, 1e-2f);
    EXPECT_NEAR(world.jointComponent(e).linearOffset.y, -50.0f, 1e-2f);
    world.step(30);
    EXPECT_NEAR(world.rb(follower).getPosition().x, 50.0f, 1.0f) << "held where it was";
    EXPECT_NEAR(world.rb(follower).getPosition().y, 0.0f, 1.0f);

    // the base turns another 90 degrees: the follower goes round to (0, 50)
    world.rb(base).setAngularVelocity(rad(90.0f));
    world.step(60);
    world.rb(base).setAngularVelocity(0.0f);
    world.step(90);
    EXPECT_NEAR(std::fabs(world.rb(base).getAngle()), 180.0f, 2.0f);
    EXPECT_NEAR(world.rb(follower).getPosition().x, 0.0f, 2.0f);
    EXPECT_NEAR(world.rb(follower).getPosition().y, 50.0f, 2.0f);
}

TEST(Physics2DJoints, ABodyRebuiltInPlayKeepsItsJointsAutoValues) {
    World2D world;
    Car car = buildCar(world, Vector3(0, 0, 0), 3.0f);
    world.load();
    world.step(120);
    const Vector2 anchorB = world.jointComponent(car.frontJoint).anchorB;
    const float translation = world.j(car.frontJoint).getJointTranslation();
    ASSERT_GT(translation, 1.0f) << "the suspension is compressed";

    // a collider edit in Play makes the wheel's body again (and Box2D drops its joint): the
    // joint is made again with the anchor it captured, not one taken from the compressed pose
    world.component(car.front).needReloadBody = true;
    world.step();
    ASSERT_TRUE(b2Joint_IsValid(world.jointComponent(car.frontJoint).joint));
    EXPECT_NEAR(world.jointComponent(car.frontJoint).anchorB.x, anchorB.x, 1e-4f);
    EXPECT_NEAR(world.jointComponent(car.frontJoint).anchorB.y, anchorB.y, 1e-4f);
    world.step(120);
    EXPECT_NEAR(world.j(car.frontJoint).getJointTranslation(), translation, 0.2f) << "rests as before";

    // an edit of the joint itself captures again (Unity: auto configured from the poses of now)
    world.jointComponent(car.frontJoint).needUpdateJoint = true;
    world.step();
    EXPECT_GT(std::fabs(world.jointComponent(car.frontJoint).anchorB.y - anchorB.y), 0.5f * translation) << "anchor B taken again";
    EXPECT_LT(world.j(car.frontJoint).getJointTranslation(), 0.5f * translation) << "the spring starts from the pose of now";
}
