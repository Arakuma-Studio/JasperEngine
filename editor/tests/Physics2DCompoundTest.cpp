// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Compound 2D bodies from child GameObjects (S96 REQ-18, REQ-19, AC-18): a Rigidbody 2D with
// Attach To Parent Body has no body of its own, its colliders become shapes of the nearest
// ancestor's Box2D body, placed by its Transform, adding their mass; their contacts reach the
// child and the ancestor; a moved child makes the ancestor's attached shapes again in that step.

#include <gtest/gtest.h>

#include "Physics2DTestWorld.h"
#include "Physics3DTestWorld.h"

#include "physics/AttachedColliders.h"
#include "physics/PhysicsQueries2D.h"

#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::physics2dtest;

namespace {

    // a size x size points box (64 points = 1 m at the default scale)
    void compoundBox2D(World2D& world, Entity e, float size) {
        Collider2D collider;
        collider.type = Collider2DType::POLYGON;
        const float h = size * 0.5f;
        collider.points = {Vector2(-h, -h), Vector2(h, -h), Vector2(h, h), Vector2(-h, h)};
        world.component(e).colliders.push_back(collider);
        world.component(e).needUpdateColliders = true;
    }

    Entity compoundChild2D(World2D& world, Entity parent, const Vector3& local, float rollDegrees = 0.0f, const Vector3& scale = Vector3(1, 1, 1)) {
        const Entity child = world.body(local, BodyType::STATIC, parent, rollDegrees);
        Transform& transform = world.scene.getComponent<Transform>(child);
        transform.scale = scale;
        transform.needUpdate = true;
        compoundBox2D(world, child, 64.0f);
        world.component(child).attachToParentBody = true;
        return child;
    }

}

TEST(Physics2DCompound, ChildrenJoinAncestorBody) {
    World2D world(Vector2(0, 0));
    const Entity car = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
    compoundBox2D(world, car, 64.0f);
    const Entity rotated = compoundChild2D(world, car, Vector3(-128, 0, 0), 45.0f);
    const Entity scaled = compoundChild2D(world, car, Vector3(128, 0, 0), 0.0f, Vector3(2, 1, 1));
    world.load();
    world.step();

    // one body: the children have none (red: three bodies)
    EXPECT_TRUE(b2Body_IsValid(world.component(car).body));
    EXPECT_FALSE(b2Body_IsValid(world.component(rotated).body));
    EXPECT_FALSE(b2Body_IsValid(world.component(scaled).body));
    EXPECT_EQ(world.component(car).attachedColliders.size(), 2u);
    EXPECT_EQ(b2Body_GetShapeCount(world.component(car).body), 3);
    // 1 m^2 + 1 m^2 + 2 m^2 of density 1
    EXPECT_NEAR(world.rb(car).getMass(), 4.0f, 0.04f);

    // the scaled child's box spans x 64..192 points: a point there is the child's collider
    ColliderRef2D collider;
    ASSERT_TRUE(PhysicsQueries2D::overlapPoint(&world.scene, Vector2(180, 0), collider));
    EXPECT_EQ(collider.entity, scaled);
    EXPECT_EQ(collider.index, 0);
    EXPECT_EQ(collider.getRigidbody().getEntity(), car);
    // the rotated one reaches sqrt(0.5) x 64 points from its centre along x
    EXPECT_TRUE(PhysicsQueries2D::overlapPoint(&world.scene, Vector2(-128.0f - 44.0f, 0), collider));
    EXPECT_EQ(collider.entity, rotated);
    EXPECT_FALSE(PhysicsQueries2D::overlapPoint(&world.scene, Vector2(-128.0f - 47.0f, 0), collider));
}

TEST(Physics2DCompound, MovedChildRecomposesOnceKeepingBodyAndVelocity) {
    World2D world(Vector2(0, 0));
    const Entity car = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
    compoundBox2D(world, car, 64.0f);
    const Entity wing = compoundChild2D(world, car, Vector3(128, 0, 0));
    world.load();
    world.step();
    const b2BodyId id = world.component(car).body;
    world.rb(car).setLinearVelocity(Vector2(64, 0));
    world.step();
    EXPECT_EQ(world.component(car).attachedRecomposeSteps, 0);

    world.scene.getComponent<Transform>(wing).position = Vector3(160, 0, 0);
    world.scene.getComponent<Transform>(wing).needUpdate = true;
    world.step();
    EXPECT_TRUE(B2_ID_EQUALS(world.component(car).body, id)) << "the body kept";
    EXPECT_EQ(world.component(car).attachedRecomposeSteps, 1) << "made again once in that step";
    ASSERT_EQ(world.component(car).attachedColliders.size(), 1u);
    EXPECT_NEAR(world.component(car).attachedColliders[0].collider.points[0].x, 160.0f - 32.0f, 1e-3f);
    EXPECT_NEAR(world.rb(car).getLinearVelocity().x, 64.0f, 0.5f);
    world.step();
    EXPECT_EQ(world.component(car).attachedRecomposeSteps, 0);
}

TEST(Physics2DCompound, AnimatedChildWarnsOnce) {
    World2D world(Vector2(0, 0));
    const Entity car = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
    compoundBox2D(world, car, 64.0f);
    const Entity arm = compoundChild2D(world, car, Vector3(128, 0, 0));
    world.load();
    world.step();
    physics3dtest::LogCapture log;
    for (int i = 0; i < 40; i++) {
        world.scene.getComponent<Transform>(arm).position = Vector3(128.0f + (float)(i + 1), 0, 0);
        world.scene.getComponent<Transform>(arm).needUpdate = true;
        world.step();
    }
    EXPECT_EQ(log.count("moved on"), 1u);
}
