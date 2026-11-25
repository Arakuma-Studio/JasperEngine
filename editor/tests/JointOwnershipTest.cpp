// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Joints made from scripts (S96 REQ-25..REQ-28): a joint handle that made its GameObject hands
// it to bodyA once bodyA is set, so the joint outlives the handle (a Lua wrapper collected) and
// goes with bodyA as editor-made joints do; one never given a bodyA still goes with its handle;
// addJoint2D / addJoint3D make a child joint of the body; the handle's GameObject and destroy;
// a joint whose other body was destroyed leaves the simulation and logs once.

#include <gtest/gtest.h>

#include "Physics2DTestWorld.h"
#include "Physics3DTestWorld.h"

#include "manager/ObjectManager.h"
#include "object/GameObject.h"

#include <cmath>
#include <string>

using namespace jasperengine;

namespace {

    using physics2dtest::World2D;
    using physics3dtest::LogCapture;
    using physics3dtest::World3D;

    // a static frame and a dynamic wheel below it: held by a joint, the wheel stays near the
    // frame; without one it falls about 300 points in a second
    struct Hanging2D {
        World2D world;
        Entity frame = world.body(Vector3(0, 200, 0), BodyType::STATIC);
        Entity wheel = world.body(Vector3(0, 150, 0), BodyType::DYNAMIC);

        Hanging2D() {
            world.rb(frame).createCenteredBoxCollider(40, 20);
            world.rb(wheel).createCircleCollider(Vector2(0, 0), 10);
            world.load();
        }

        float drop() { return 150.0f - world.scene.getComponent<Transform>(wheel).worldPosition.y; }
    };

    struct Hanging3D {
        World3D world;
        Entity frame = world.box(Vector3(0, 5, 0), Vector3(1, 0.2f, 1), BodyType::STATIC);
        Entity ball = world.sphere(Vector3(0, 4, 0), 0.25f, BodyType::DYNAMIC);

        Hanging3D() { world.load(); }

        float drop() { return 4.0f - world.rb(ball).getPosition().y; }
    };

    Entity parentOf(Scene& scene, Entity entity) {
        return scene.getComponent<Transform>(entity).parent;
    }

}

// ---- REQ-25: a script's joint lives under bodyA -------------------------------------------------

TEST(JointOwnership, Joint2DGoesUnderBodyAAndOutlivesItsHandle) {
    Hanging2D h;
    Entity jointEntity = NULL_ENTITY;
    {
        Joint2D joint(&h.world.scene);
        jointEntity = joint.getEntity();
        EXPECT_TRUE(joint.isEntityOwned());
        EXPECT_EQ(h.world.scene.getEntityName(jointEntity), "Joint 2D");
        joint.setWheelJoint(h.frame, h.wheel, Vector2(0, 150), Vector2(0, 1));
        EXPECT_FALSE(joint.isEntityOwned()) << "bodyA's GameObject holds it now";
        EXPECT_EQ(parentOf(h.world.scene, jointEntity), h.frame);
    }
    // the handle went (a Lua wrapper collected): the joint stays and works
    ASSERT_TRUE(h.world.scene.isEntityCreated(jointEntity));
    h.world.step(60);
    EXPECT_TRUE(h.world.j(jointEntity).isLive());
    EXPECT_LT(std::fabs(h.drop()), 64.0f) << "the wheel hangs from the frame";
}

TEST(JointOwnership, Joint2DBodyAPropertyKeepsTheWorldPose) {
    Hanging2D h;
    Joint2D joint(&h.world.scene);
    h.world.scene.getComponent<Transform>(joint.getEntity()).position = Vector3(10, 20, 0);
    h.world.scene.getComponent<Transform>(joint.getEntity()).needUpdate = true;
    h.world.resolve();
    joint.setType(Joint2DType::REVOLUTE);
    joint.setBodyA(h.frame);
    EXPECT_FALSE(joint.isEntityOwned());
    EXPECT_EQ(parentOf(h.world.scene, joint.getEntity()), h.frame);
    h.world.resolve();
    const Vector3 world = h.world.scene.getComponent<Transform>(joint.getEntity()).worldPosition;
    EXPECT_NEAR(world.x, 10.0f, 1e-3f);
    EXPECT_NEAR(world.y, 20.0f, 1e-3f);
}

TEST(JointOwnership, Joint3DGoesUnderBodyAAndOutlivesItsHandle) {
    Hanging3D h;
    Entity jointEntity = NULL_ENTITY;
    {
        Joint3D joint(&h.world.scene);
        jointEntity = joint.getEntity();
        EXPECT_EQ(h.world.scene.getEntityName(jointEntity), "Joint 3D");
        joint.setPointJoint(h.frame, h.ball, Vector3(0, 4.5f, 0));
        EXPECT_FALSE(joint.isEntityOwned());
        EXPECT_EQ(parentOf(h.world.scene, jointEntity), h.frame);
    }
    ASSERT_TRUE(h.world.scene.isEntityCreated(jointEntity));
    h.world.step(60);
    EXPECT_TRUE(h.world.j(jointEntity).isLive());
    EXPECT_LT(std::fabs(h.drop()), 0.5f) << "the ball hangs from the frame";
}

TEST(JointOwnership, JointWithoutBodyAGoesWithItsHandle) {
    Hanging2D h2;
    Entity joint2D = NULL_ENTITY;
    {
        Joint2D joint(&h2.world.scene);
        joint2D = joint.getEntity();
        joint.setBodyB(h2.wheel);
        EXPECT_TRUE(joint.isEntityOwned());
    }
    EXPECT_FALSE(h2.world.scene.isEntityCreated(joint2D));

    Hanging3D h3;
    Entity joint3D = NULL_ENTITY;
    {
        Joint3D joint(&h3.world.scene);
        joint3D = joint.getEntity();
    }
    EXPECT_FALSE(h3.world.scene.isEntityCreated(joint3D));
}

TEST(JointOwnership, JointParentedElsewhereIsLeftAlone) {
    Hanging2D h;
    const Entity holder = h.world.entity(Vector3(0, 0, 0));
    Joint2D joint(&h.world.scene);
    h.world.scene.addEntityChild(holder, joint.getEntity(), false);
    joint.setBodyA(h.frame);
    EXPECT_EQ(parentOf(h.world.scene, joint.getEntity()), holder);
    EXPECT_TRUE(joint.isEntityOwned());
}

// ---- REQ-26: addJoint and getJoints -------------------------------------------------------------

TEST(JointOwnership, AddJoint2DMakesAChildJointWithUnitysDefaults) {
    Hanging2D h;
    GameObject frame(&h.world.scene, h.frame);
    Joint2D joint = frame.addJoint2D(Joint2DType::WHEEL);
    ASSERT_TRUE((bool)joint);
    EXPECT_FALSE(joint.isEntityOwned());
    EXPECT_EQ(parentOf(h.world.scene, joint.getEntity()), h.frame);
    EXPECT_EQ(h.world.scene.getEntityName(joint.getEntity()), "Joint 2D");
    Joint2DComponent expected = makeJoint2D(Joint2DType::WHEEL);
    const Joint2DComponent& made = h.world.jointComponent(joint.getEntity());
    EXPECT_EQ(made.type, expected.type);
    EXPECT_EQ(made.bodyA, h.frame);
    EXPECT_EQ(made.bodyB, NULL_ENTITY);
    EXPECT_EQ(made.enableSpring, expected.enableSpring);
    EXPECT_EQ(made.frequency, expected.frequency);
    EXPECT_EQ(made.dampingRatio, expected.dampingRatio);
    EXPECT_EQ(made.anchorSpace, expected.anchorSpace);

    // a second one, and one on the wheel: getJoints2D lists the frame's
    const Joint2D second = frame.addJoint2D(Joint2DType::REVOLUTE);
    const Joint2D onWheel = GameObject(&h.world.scene, h.wheel).addJoint2D(Joint2DType::WELD);
    const std::vector<Joint2D> joints = frame.getJoints2D();
    ASSERT_EQ(joints.size(), 2u);
    EXPECT_EQ(joints[0].getEntity(), joint.getEntity());
    EXPECT_EQ(joints[1].getEntity(), second.getEntity());

    // connected, it holds the wheel
    joint.setBodyB(h.wheel);
    joint.setAnchorSpace(JointAnchorSpace::WORLD);
    joint.setAnchorA(Vector2(0, 150));
    joint.setAnchorB(Vector2(0, 150));
    joint.setAutoAnchors(false);
    joint.setAxis(Vector2(0, 1));
    // the others would hold the bodies to the world
    h.world.scene.removeComponent<Joint2DComponent>(second.getEntity());
    h.world.scene.removeComponent<Joint2DComponent>(onWheel.getEntity());
    h.world.step(60);
    EXPECT_TRUE(joint.isLive());
    EXPECT_LT(std::fabs(h.drop()), 64.0f);
}

TEST(JointOwnership, AddJointWithoutABodyIsRefused) {
    Hanging2D h2;
    LogCapture log;
    const Entity plain = h2.world.entity(Vector3(0, 0, 0));
    const Joint2D joint2D = GameObject(&h2.world.scene, plain).addJoint2D(Joint2DType::WHEEL);
    EXPECT_FALSE((bool)joint2D);
    EXPECT_EQ(log.count("addJoint2D"), 1u);

    Hanging3D h3;
    const Entity plain3D = h3.world.entity(Vector3(0, 0, 0));
    const Joint3D joint3D = GameObject(&h3.world.scene, plain3D).addJoint3D(Joint3DType::HINGE);
    EXPECT_FALSE((bool)joint3D);
    EXPECT_EQ(log.count("addJoint3D"), 1u);
}

TEST(JointOwnership, AddJoint3DMakesAChildJointWithUnitysDefaults) {
    Hanging3D h;
    GameObject frame(&h.world.scene, h.frame);
    Joint3D joint = frame.addJoint3D(Joint3DType::HINGE);
    ASSERT_TRUE((bool)joint);
    EXPECT_EQ(parentOf(h.world.scene, joint.getEntity()), h.frame);
    EXPECT_EQ(h.world.scene.getEntityName(joint.getEntity()), "Joint 3D");
    const Joint3DComponent expected = makeJoint3D(Joint3DType::HINGE);
    const Joint3DComponent& made = h.world.jointComponent(joint.getEntity());
    EXPECT_EQ(made.type, Joint3DType::HINGE);
    EXPECT_EQ(made.bodyA, h.frame);
    EXPECT_EQ(made.limitsMin, expected.limitsMin);
    EXPECT_EQ(made.limitsMax, expected.limitsMax);
    EXPECT_EQ(made.anchorSpace, expected.anchorSpace);
    ASSERT_EQ(frame.getJoints3D().size(), 1u);
    EXPECT_EQ(frame.getJoints3D()[0].getEntity(), joint.getEntity());
    EXPECT_TRUE(GameObject(&h.world.scene, h.ball).getJoints3D().empty());
}

// ---- REQ-27: the handle's GameObject and destroy ------------------------------------------------

TEST(JointOwnership, HandleGameObjectAndDestroy) {
    Hanging2D h;
    Joint2D joint(&h.world.scene);
    EXPECT_EQ(joint.getGameObject().getEntity(), joint.getEntity());
    joint.destroy();
    EXPECT_FALSE(joint.isEntityOwned()) << "destroyed once, by GameObject.destroy";
    EXPECT_TRUE(ObjectManager::isDestroyPending(&h.world.scene, joint.getEntity()));
    ObjectManager::update(0.0);
    EXPECT_FALSE(h.world.scene.isEntityCreated(joint.getEntity()));

    Hanging3D h3;
    Joint3D joint3D = GameObject(&h3.world.scene, h3.frame).addJoint3D(Joint3DType::FIXED);
    EXPECT_EQ(joint3D.getGameObject().getEntity(), joint3D.getEntity());
    joint3D.destroy(0.5f);
    ObjectManager::update(0.25);
    EXPECT_TRUE(h3.world.scene.isEntityCreated(joint3D.getEntity())) << "after its delay";
    ObjectManager::update(0.5);
    EXPECT_FALSE(h3.world.scene.isEntityCreated(joint3D.getEntity()));
}

// ---- REQ-28: bodies destroyed under a joint -----------------------------------------------------

TEST(JointOwnership, DestroyingBodyATakesTheJointWithoutErrors) {
    LogCapture log;
    Hanging2D h2;
    Entity joint2D = NULL_ENTITY;
    {
        Joint2D joint(&h2.world.scene);
        joint.setWheelJoint(h2.frame, h2.wheel, Vector2(0, 150), Vector2(0, 1));
        joint2D = joint.getEntity();
    }
    h2.world.step(10);
    h2.world.scene.destroyHierarchy(h2.frame);
    EXPECT_FALSE(h2.world.scene.isEntityCreated(joint2D));
    h2.world.step(120);

    Hanging3D h3;
    Entity joint3D = NULL_ENTITY;
    {
        Joint3D joint(&h3.world.scene);
        joint.setPointJoint(h3.frame, h3.ball, Vector3(0, 4.5f, 0));
        joint3D = joint.getEntity();
    }
    h3.world.step(10);
    h3.world.scene.destroyHierarchy(h3.frame);
    EXPECT_FALSE(h3.world.scene.isEntityCreated(joint3D));
    h3.world.step(120);
    EXPECT_EQ(log.count("Cannot create"), 0u);
}

TEST(JointOwnership, DestroyedBodyBLeavesTheSimulationAndLogsOnce) {
    {
        LogCapture log;
        Hanging2D h;
        Joint2D joint = GameObject(&h.world.scene, h.wheel).addJoint2D(Joint2DType::REVOLUTE);
        joint.setBodyB(h.frame);
        h.world.step(10);
        ASSERT_TRUE(joint.isLive());
        h.world.scene.destroyHierarchy(h.frame);
        h.world.step(120);
        EXPECT_TRUE(h.world.scene.isEntityCreated(joint.getEntity()));
        EXPECT_FALSE(joint.isLive());
        EXPECT_EQ(log.count("Cannot create joint"), 1u);
    }
    {
        LogCapture log;
        Hanging3D h;
        Joint3D joint = GameObject(&h.world.scene, h.ball).addJoint3D(Joint3DType::POINT);
        joint.setBodyB(h.frame);
        h.world.step(10);
        ASSERT_TRUE(joint.isLive());
        h.world.scene.destroyHierarchy(h.frame);
        h.world.step(120);
        EXPECT_TRUE(h.world.scene.isEntityCreated(joint.getEntity()));
        EXPECT_FALSE(joint.isLive());
        EXPECT_EQ(log.count("Cannot create joint"), 1u);
        EXPECT_GT(h.drop(), 1.0f) << "the ball fell free";
    }
}
