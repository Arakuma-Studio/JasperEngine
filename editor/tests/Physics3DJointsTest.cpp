// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// 3D joints with every parameter (physics-3d spec T8-T11, and the cone exception of T1): hinge
// limits (re-centred on the start pose), spring and motor, slider limits and motor, distance
// limits and soft rope, swing-twist limits with Unity's names, six-DOF motions, the cone's 45
// degrees, breaking with its four actions and onJointBreak, local anchors that move with their
// prefab, and enableCollision.

#include <gtest/gtest.h>

#include "Physics3DTestWorld.h"

#include "component/ScriptComponent.h"
#include "script/LuaBinding.h"
#include "script/ScriptBase.h"
#include "script/ScriptMessages.h"

#include "lua.hpp"

#include <cmath>
#include <limits>
#include <string>

using namespace jasperengine;
using namespace jasperengine::physics3dtest;

namespace {

    // degrees of the rotation about axis (the twist part of q about it)
    float degreesAbout(const Quaternion& q, const Vector3& axis) {
        const Vector3 v(q.x, q.y, q.z);
        const float along = v.dotProduct(axis.normalized());
        return Angle::radToDeg(2.0f * std::atan2(along, q.w));
    }

    // the angle between two directions, degrees
    float degreesBetween(const Vector3& a, const Vector3& b) {
        const float c = std::fmax(-1.0f, std::fmin(1.0f, a.normalized().dotProduct(b.normalized())));
        return Angle::radToDeg(std::acos(c));
    }

    // a door: a 1 x 2 x 0.1 box of 1 kg hinged at its left edge about its local y, to the world
    struct Door {
        World3D world{Vector3(0, 0, 0)};
        Entity door, joint;
        explicit Door(const Joint3DComponent& settings) {
            door = world.box(Vector3(0.5f, 1, 0), Vector3(1, 2, 0.1f), BodyType::DYNAMIC);
            world.rb(door).setMass(1.0f);
            Joint3DComponent hinge = settings;
            hinge.type = Joint3DType::HINGE;
            hinge.bodyA = door;
            hinge.anchor = Vector3(-0.5f, 0, 0);
            hinge.axis = Vector3(0, 1, 0);
            hinge.normal = Vector3(1, 0, 0);
            joint = world.joint(hinge);
            world.load();
        }
        float angle() { return world.j(joint).getAngle(); }
    };

}

// ---- T8: hinge ---------------------------------------------------------------------------------

TEST(Physics3DJoints, HingeLimitsAreMeasuredFromTheStartPose) {
    Joint3DComponent settings = makeJoint3D(Joint3DType::HINGE);
    settings.useLimits = true;
    settings.limitsMin = Angle::degToDefault(10.0f);
    settings.limitsMax = Angle::degToDefault(50.0f);
    Door d(settings);
    EXPECT_NEAR(Angle::defaultToDeg(d.angle()), 0.0f, 1e-3f) << "starts at 0";
    d.world.step(120);
    const float angle = Angle::defaultToDeg(d.angle());
    EXPECT_GE(angle, 10.0f - 1.0f);
    EXPECT_LE(angle, 50.0f + 1.0f);
    // the read-out is the bodies' relative rotation about the axis
    EXPECT_NEAR(degreesAbout(d.world.rb(d.door).getRotation(), Vector3(0, 1, 0)), angle, 0.5f);

    // pushed the other way it stops at 50
    d.world.rb(d.door).setAngularVelocity(Vector3(0, 10, 0));
    d.world.step(60);
    d.world.rb(d.door).setAngularVelocity(Vector3(0, -10, 0));
    d.world.step(60);
    const float other = Angle::defaultToDeg(d.angle());
    EXPECT_GE(other, 10.0f - 1.0f);
    EXPECT_LE(other, 50.0f + 1.0f);
}

TEST(Physics3DJoints, HingeSpringReturnsToItsTarget) {
    Joint3DComponent settings = makeJoint3D(Joint3DType::HINGE);
    settings.useLimits = false;
    settings.useSpring = true;
    settings.springStiffness = 50.0f;
    settings.springDamper = 5.0f;
    settings.springTargetPosition = 0.0f;
    Door d(settings);
    // turning about the hinge (the edge): the centre moves too
    d.world.rb(d.door).setAngularVelocity(Vector3(0, 3, 0));
    d.world.rb(d.door).setLinearVelocity(Vector3(0, 3, 0).crossProduct(Vector3(0.5f, 0, 0)));
    float widest = 0.0f;
    for (int i = 0; i < 20; i++) {
        d.world.step();
        widest = std::fmax(widest, Angle::defaultToDeg(d.angle()));
    }
    EXPECT_GT(widest, 5.0f) << "swung away (positive: the door's own turn about the axis)";
    d.world.step(180);
    EXPECT_NEAR(Angle::defaultToDeg(d.angle()), 0.0f, 1.0f);
}

TEST(Physics3DJoints, HingeMotorReachesItsSpeed) {
    Joint3DComponent settings = makeJoint3D(Joint3DType::HINGE);
    settings.useLimits = false;
    settings.useMotor = true;
    settings.motorTargetVelocity = Angle::degToDefault(90.0f);
    settings.motorMaxForce = 1000.0f;
    Door d(settings);
    d.world.step(60);
    EXPECT_NEAR(Angle::defaultToDeg(d.world.j(d.joint).getVelocity()), 90.0f, 90.0f * 0.02f);
    EXPECT_NEAR(Angle::radToDeg(std::fabs(d.world.rb(d.door).getAngularVelocity().y)), 90.0f, 90.0f * 0.02f);
}

TEST(Physics3DJoints, HingeLimitsLiveEditsReachTheJoint) {
    Joint3DComponent settings = makeJoint3D(Joint3DType::HINGE);
    settings.useLimits = true;
    settings.limitsMin = Angle::degToDefault(-20.0f);
    settings.limitsMax = Angle::degToDefault(20.0f);
    Door d(settings);
    const JPH::TwoBodyConstraint* before = d.world.jointComponent(d.joint).joint;
    d.world.j(d.joint).setUseMotor(true);
    d.world.j(d.joint).setMotorTargetVelocity(Angle::degToDefault(90.0f));
    d.world.step(60);
    EXPECT_EQ(d.world.jointComponent(d.joint).joint, before) << "a parameter is set on the live joint";
    EXPECT_NEAR(Angle::defaultToDeg(d.angle()), 20.0f, 1.5f) << "the motor pushes it to the limit";
}

// ---- T9: slider, distance, swing-twist, six-DOF, cone ------------------------------------------

TEST(Physics3DJoints, SliderMotorStopsAtItsLimit) {
    World3D world(Vector3(0, 0, 0));
    const Entity carriage = world.box(Vector3(0, 0, 0), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC);
    Joint3DComponent slider = makeJoint3D(Joint3DType::PRISMATIC);
    slider.bodyA = carriage;
    slider.axis = Vector3(1, 0, 0);
    slider.useLimits = true;
    slider.limitsMin = 0.0f;
    slider.limitsMax = 2.0f;
    slider.useMotor = true;
    slider.motorTargetVelocity = 1.0f;
    slider.motorMaxForce = 10000.0f;
    const Entity joint = world.joint(slider);
    world.load();
    world.step(60);
    EXPECT_NEAR(world.j(joint).getSpeed(), 1.0f, 0.03f);
    EXPECT_NEAR(world.j(joint).getPosition(), 1.0f, 0.05f);
    world.step(120);
    EXPECT_NEAR(world.j(joint).getPosition(), 2.0f, 0.02f) << "held at the limit";
    EXPECT_NEAR(world.rb(carriage).getPosition().x, 2.0f, 0.02f);
    EXPECT_NEAR(world.rb(carriage).getPosition().y, 0.0f, 1e-3f) << "only along the axis";
}

TEST(Physics3DJoints, DistanceLimitsAndSoftRope) {
    auto hang = [](bool spring) {
        World3D world;
        const Entity hook = world.sphere(Vector3(0, 10, 0), 0.1f, BodyType::STATIC);
        const Entity weight = world.sphere(Vector3(0, 8.5f, 0), 0.25f, BodyType::DYNAMIC);
        Joint3DComponent rope = makeJoint3D(Joint3DType::DISTANCE);
        rope.bodyA = weight;
        rope.bodyB = hook;
        rope.minDistance = 1.0f;
        rope.maxDistance = 2.0f;
        rope.useSpring = spring;
        rope.springStiffness = 2000.0f;
        rope.springDamper = 50.0f;
        const Entity joint = world.joint(rope);
        world.load();
        world.step(240);
        return world.j(joint).getCurrentLength();
    };
    const float hard = hang(false);
    EXPECT_NEAR(hard, 2.0f, 0.02f) << "hangs at max";
    const float soft = hang(true);
    EXPECT_GT(soft, 2.05f) << "the spring gives";
    EXPECT_LT(soft, 3.0f);
}

namespace {

    // an arm (1 x 0.2 x 0.2, along x) on a swing-twist joint at its left end to the world;
    // pushed by a torque about the world axis, how far it turns
    Quaternion pushedArm(const Joint3DComponent& settings, const Vector3& torque) {
        World3D world(Vector3(0, 0, 0));
        const Entity arm = world.box(Vector3(0.5f, 0, 0), Vector3(1, 0.2f, 0.2f), BodyType::DYNAMIC);
        world.rb(arm).setAngularDamping(2.0f);
        Joint3DComponent joint = settings;
        joint.bodyA = arm;
        joint.anchor = Vector3(-0.5f, 0, 0);
        world.joint(joint);
        world.load();
        for (int i = 0; i < 120; i++) {
            world.rb(arm).applyTorque(torque);
            world.step();
        }
        return world.rb(arm).getRotation();
    }

}

TEST(Physics3DJoints, SwingTwistLimitsUseUnitysNames) {
    Joint3DComponent settings = makeJoint3D(Joint3DType::SWINGTWIST);
    settings.twistAxis = Vector3(1, 0, 0);
    settings.planeAxis = Vector3(0, 1, 0);
    settings.normalHalfConeAngle = Angle::degToDefault(20.0f);   // swing 1, about the swing (plane) axis
    settings.planeHalfConeAngle = Angle::degToDefault(40.0f);    // swing 2, about their normal
    settings.twistMinAngle = Angle::degToDefault(-10.0f);        // low twist
    settings.twistMaxAngle = Angle::degToDefault(30.0f);         // high twist
    settings.swingType = JointSwingType::PYRAMID;

    const Quaternion aboutPlane = pushedArm(settings, Vector3(0, 20, 0));
    EXPECT_NEAR(degreesBetween(aboutPlane * Vector3(1, 0, 0), Vector3(1, 0, 0)), 20.0f, 2.0f) << "swing 1";
    const Quaternion aboutNormal = pushedArm(settings, Vector3(0, 0, 20));
    EXPECT_NEAR(degreesBetween(aboutNormal * Vector3(1, 0, 0), Vector3(1, 0, 0)), 40.0f, 2.0f) << "swing 2";
    const Quaternion twistUp = pushedArm(settings, Vector3(20, 0, 0));
    EXPECT_NEAR(degreesAbout(twistUp, Vector3(1, 0, 0)), 30.0f, 2.0f) << "high twist";
    const Quaternion twistDown = pushedArm(settings, Vector3(-20, 0, 0));
    EXPECT_NEAR(degreesAbout(twistDown, Vector3(1, 0, 0)), -10.0f, 2.0f) << "low twist";
}

TEST(Physics3DJoints, ConeSwingsToItsHalfAngle) {
    Joint3DComponent settings = makeJoint3D(Joint3DType::CONE);
    settings.twistAxis = Vector3(1, 0, 0);
    EXPECT_NEAR(Angle::defaultToDeg(settings.coneHalfAngle), 45.0f, 1e-4f);
    const Quaternion swung = pushedArm(settings, Vector3(0, 0, 20));
    EXPECT_NEAR(degreesBetween(swung * Vector3(1, 0, 0), Vector3(1, 0, 0)), 45.0f, 1.0f)
        << "45 degrees (it was 0 before the fix)";
}

TEST(Physics3DJoints, SixDOFMotionsPerAxis) {
    World3D world;
    const Entity e = world.box(Vector3(0, 5, 0), Vector3(0.5f, 0.5f, 0.5f), BodyType::DYNAMIC);
    Joint3DComponent sixDOF = makeJoint3D(Joint3DType::SIXDOF);
    sixDOF.bodyA = e;
    sixDOF.axisX = Vector3(1, 0, 0);
    sixDOF.axisY = Vector3(0, 1, 0);
    sixDOF.xMotion = JointMotion::FREE;
    sixDOF.yMotion = JointMotion::LOCKED;
    sixDOF.zMotion = JointMotion::LIMITED;
    sixDOF.linearLimitMin = Vector3(0, 0, -0.5f);
    sixDOF.linearLimitMax = Vector3(0, 0, 0.5f);
    sixDOF.angularXMotion = sixDOF.angularYMotion = sixDOF.angularZMotion = JointMotion::LOCKED;
    world.joint(sixDOF);
    world.load();
    world.rb(e).setLinearVelocity(Vector3(1, 0, 2));
    world.rb(e).setAngularVelocity(Vector3(3, 3, 3));
    world.step(90);
    const Vector3 p = world.rb(e).getPosition();
    EXPECT_NEAR(p.y, 5.0f, 0.01f) << "y locked against gravity";
    EXPECT_NEAR(p.z, 0.5f, 0.02f) << "z limited";
    EXPECT_GT(p.x, 1.0f) << "x free";
    EXPECT_NEAR(std::fabs(world.rb(e).getRotation().w), 1.0f, 1e-3f) << "rotation locked";
}

// ---- T10: breaking -----------------------------------------------------------------------------

namespace {

    struct BreakCounter : public ScriptBase {
        int* calls;
        float* force;
        JointBreakAction* seenAction;
        BreakCounter(Scene* scene, Entity entity, int* calls, float* force, JointBreakAction* seenAction)
            : ScriptBase(scene, entity), calls(calls), force(force), seenAction(seenAction) {}
        void onJointBreak(float breakForce, Joint3D& joint) override {
            (*calls)++;
            *force = breakForce;
            *seenAction = joint.getBreakAction();
        }
    };

    void addCppListener(World3D& world, Entity entity, BreakCounter* script) {
        ScriptComponent scripts;
        ScriptEntry entry;
        entry.type = ScriptType::CPP;
        entry.enabled = true;
        entry.className = "BreakCounter";
        entry.instance = script;
        entry.cppScript = ScriptMessages::cppScript<BreakCounter>(script);
        entry.cppMessageMask = ScriptMessages::cppMask<BreakCounter>();
        scripts.scripts.push_back(entry);
        world.scene.addComponent<ScriptComponent>(entity, scripts);
    }

    // a 100 kg box held to the world by a fixed joint that breaks at 500 N (its weight is 981 N)
    struct Held {
        World3D world;
        Entity box, joint;
        explicit Held(JointBreakAction action) {
            box = world.box(Vector3(0, 5, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
            world.rb(box).setMass(100.0f);
            Joint3DComponent fixed = makeJoint3D(Joint3DType::FIXED);
            fixed.bodyA = box;
            fixed.breakForce = 500.0f;
            fixed.breakAction = action;
            joint = world.joint(fixed);
        }
    };

}

TEST(Physics3DJoints, BreakingDestroyCallsBackOnceThenRemoves) {
    Held h(JointBreakAction::DESTROY);
    int calls = 0;
    float force = 0.0f;
    JointBreakAction seen = JointBreakAction::IGNORE;
    BreakCounter listener(&h.world.scene, h.joint, &calls, &force, &seen);
    addCppListener(h.world, h.joint, &listener);
    h.world.load();
    h.world.step();
    EXPECT_EQ(calls, 1);
    EXPECT_GE(force, 500.0f);
    EXPECT_EQ(seen, JointBreakAction::DESTROY) << "the joint exists during the callback";
    EXPECT_EQ(h.world.scene.findComponent<Joint3DComponent>(h.joint), nullptr) << "removed after the dispatch";
    h.world.step(30);
    EXPECT_EQ(calls, 1);
    EXPECT_LT(h.world.rb(h.box).getPosition().y, 4.5f) << "falls";
    h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
}

TEST(Physics3DJoints, BreakingDisableIgnoreAndCallbackOnly) {
    {
        Held h(JointBreakAction::DISABLE);
        int calls = 0;
        float force = 0.0f;
        JointBreakAction seen;
        BreakCounter listener(&h.world.scene, h.joint, &calls, &force, &seen);
        addCppListener(h.world, h.joint, &listener);
        h.world.load();
        h.world.step(2);
        EXPECT_EQ(calls, 1);
        ASSERT_NE(h.world.scene.findComponent<Joint3DComponent>(h.joint), nullptr);
        EXPECT_FALSE(h.world.jointComponent(h.joint).enabled);
        EXPECT_TRUE(h.world.j(h.joint).isBroken());
        EXPECT_FALSE(h.world.j(h.joint).isLive());
        h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    }
    {
        Held h(JointBreakAction::IGNORE);
        int calls = 0;
        float force = 0.0f;
        JointBreakAction seen;
        BreakCounter listener(&h.world.scene, h.joint, &calls, &force, &seen);
        addCppListener(h.world, h.joint, &listener);
        h.world.load();
        h.world.step(30);
        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(h.world.j(h.joint).isLive());
        EXPECT_NEAR(h.world.rb(h.box).getPosition().y, 5.0f, 0.05f) << "still held";
        h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    }
    {
        Held h(JointBreakAction::CALLBACK_ONLY);
        int calls = 0;
        float force = 0.0f;
        JointBreakAction seen;
        BreakCounter listener(&h.world.scene, h.joint, &calls, &force, &seen);
        addCppListener(h.world, h.joint, &listener);
        h.world.load();
        h.world.step(30);
        EXPECT_EQ(calls, 1) << "once while the load stays over the threshold";
        EXPECT_TRUE(h.world.j(h.joint).isLive());
        h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    }
}

TEST(Physics3DJoints, BreakingCallsLuaOnJointBreak) {
    Held h(JointBreakAction::DESTROY);
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    ASSERT_EQ(luaL_dostring(L,
        "joint3DBreakLog = {}\n"
        "Joint3DBreakScript = {}\n"
        "function Joint3DBreakScript:onJointBreak(force, joint)\n"
        "  joint3DBreakLog[#joint3DBreakLog + 1] = tostring(force >= 500) .. ':' .. tostring(joint.breakForce) .. ':' .. tostring(joint.breakAction == JointBreakAction.DESTROY)\n"
        "end\n"
        "joint3DBreakInstance = setmetatable({}, {__index = Joint3DBreakScript})\n"), LUA_OK) << lua_tostring(L, -1);
    lua_getglobal(L, "joint3DBreakInstance");
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.enabled = true;
    entry.className = "Joint3DBreakScript";
    entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
    scripts.scripts.push_back(entry);
    h.world.scene.addComponent<ScriptComponent>(h.joint, scripts);
    EXPECT_TRUE(ScriptMessages::receivers(&h.world.scene, h.joint) & scriptMessageBit(ScriptMessage::JointBreak));

    h.world.load();
    h.world.step(2);
    ASSERT_EQ(luaL_dostring(L, "joint3DBreakResult = table.concat(joint3DBreakLog, '|')"), LUA_OK);
    lua_getglobal(L, "joint3DBreakResult");
    const std::string result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "true:500.0:true");

    h.world.scene.getComponent<ScriptComponent>(h.joint).scripts.clear();
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

// ---- T11: local anchors, enableCollision -------------------------------------------------------

namespace {

    // a pendulum "prefab": a frame and a bar hinged under it about the frame's local z, placed at
    // position with rotation; the bar's pose in the frame's after the steps
    void swingPrefab(const Vector3& position, const Quaternion& rotation, Vector3& relativePosition, Quaternion& relativeRotation) {
        World3D world;
        const Entity root = world.entity(position, NULL_ENTITY, rotation);
        const Entity frame = world.box(Vector3(0, 0, 0), Vector3(0.2f, 0.2f, 0.2f), BodyType::STATIC, root);
        const Entity bar = world.box(Vector3(0.6f, 0, 0), Vector3(1, 0.1f, 0.1f), BodyType::DYNAMIC, root);
        Joint3DComponent hinge = makeJoint3D(Joint3DType::HINGE);
        hinge.useLimits = false;
        hinge.bodyA = bar;
        hinge.bodyB = frame;
        hinge.anchor = Vector3(-0.6f, 0, 0);
        hinge.axis = Vector3(0, 0, 1);
        hinge.normal = Vector3(1, 0, 0);
        world.joint(hinge, root);
        world.load();
        world.step(60);
        const Quaternion frameRotation = world.rb(frame).getRotation();
        relativePosition = frameRotation.inverse() * (world.rb(bar).getPosition() - world.rb(frame).getPosition());
        relativeRotation = frameRotation.inverse() * world.rb(bar).getRotation();
    }

}

TEST(Physics3DJoints, LocalAnchorsSwingTheSameWhereverThePrefabIs) {
    Vector3 positionA, positionB;
    Quaternion rotationA, rotationB;
    swingPrefab(Vector3(0, 5, 0), Quaternion::IDENTITY, positionA, rotationA);
    swingPrefab(Vector3(20, 5, -7), degreesAbout(90.0f, Vector3(0, 1, 0)), positionB, rotationB);
    EXPECT_GT((positionA - Vector3(0.6f, 0, 0)).length(), 0.1f) << "it swung";
    EXPECT_NEAR((positionA - positionB).length(), 0.0f, 1e-4f);
    EXPECT_NEAR(std::fabs(rotationA.dot(rotationB)), 1.0f, 1e-4f);
}

TEST(Physics3DJoints, EnableCollisionDecidesWhetherConnectedBodiesTouch) {
    auto separation = [](bool enableCollision) {
        World3D world(Vector3(0, 0, 0));
        const Entity a = world.box(Vector3(0, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
        const Entity b = world.box(Vector3(0.5f, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
        Joint3DComponent rope = makeJoint3D(Joint3DType::DISTANCE);
        rope.bodyA = a;
        rope.bodyB = b;
        rope.minDistance = 0.0f;
        rope.maxDistance = 5.0f;
        rope.enableCollision = enableCollision;
        world.joint(rope);
        world.load();
        world.step(30);
        return (world.rb(a).getPosition() - world.rb(b).getPosition()).length();
    };
    EXPECT_NEAR(separation(false), 0.5f, 1e-3f) << "they pass through each other";
    EXPECT_GT(separation(true), 0.9f) << "pushed apart";
}

TEST(Physics3DJoints, LegacyJointsKeepTheirOldParameters) {
    Joint3DComponent joint;
    setJoint3DLegacyParameters(joint, Joint3DType::HINGE);
    EXPECT_EQ(joint.anchorSpace, JointAnchorSpace::WORLD);
    EXPECT_TRUE(joint.enableCollision);
    EXPECT_FALSE(joint.useLimits);
    setJoint3DLegacyParameters(joint, Joint3DType::PRISMATIC);
    EXPECT_TRUE(joint.useLimits) << "the slider had its limits";
    const Joint3DComponent fresh = makeJoint3D(Joint3DType::HINGE);
    EXPECT_EQ(fresh.anchorSpace, JointAnchorSpace::LOCAL);
    EXPECT_FALSE(fresh.enableCollision) << "Unity's default";
}
