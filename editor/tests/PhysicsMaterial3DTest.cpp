// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Physics materials on 3D colliders (physics-3d spec T6, the 3D part; the pure combine table and
// the asset format are shared with 2D: Physics2DCollidersTest, Physics2DReviewTest): a box on a
// 30 degree slope slides or sticks by the combined friction, a ball bounces by Maximum, colliders
// without a material combine exactly as before (geometric mean friction, maximum restitution),
// a runtime edit of a shared material reaches every collider using it, and setColliderFriction
// gives one collider its own copy.

#include <gtest/gtest.h>

#include "Physics3DTestWorld.h"

#include "physics/PhysicsMaterial.h"

#include <cmath>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::physics3dtest;

namespace {

    struct Slope {
        World3D world;
        Entity slope, box;
        Vector3 start;
        Slope(const PhysicsMaterial& boxMaterial, const PhysicsMaterial& slopeMaterial, float boxFriction = 0.2f, float slopeFriction = 0.2f) {
            const Quaternion tilt = degreesAbout(30.0f, Vector3(0, 0, 1));
            slope = world.box(Vector3(0, 0, 0), Vector3(30, 1, 4), BodyType::STATIC, NULL_ENTITY, tilt);
            world.component(slope).friction = slopeFriction;
            if (slopeMaterial.valid()) world.rb(slope).setColliderMaterial(0, slopeMaterial);
            const Vector3 normal = tilt * Vector3(0, 1, 0);
            start = normal * (0.5f + 0.5f + 0.005f);
            box = world.box(start, Vector3(1, 1, 1), BodyType::DYNAMIC, NULL_ENTITY, tilt);
            world.component(box).friction = boxFriction;
            if (boxMaterial.valid()) world.rb(box).setColliderMaterial(0, boxMaterial);
            world.load();
        }
        float travelled(int steps) {
            world.step(steps);
            return (world.rb(box).getPosition() - start).length();
        }
    };

    float combinedFrictionSeen(Slope& s) {
        float seen = -1.0f;
        s.world.physics()->onContactAdded3D.add("friction", std::function<void(Rigidbody, Rigidbody, Contact3D)>(
            [&](Rigidbody, Rigidbody, Contact3D contact) { seen = contact.getCombinedFriction(); }));
        s.world.step(3);
        s.world.physics()->onContactAdded3D.remove("friction");
        return seen;
    }

}

TEST(PhysicsMaterial3D, SlopeFrictionCombinesByMode) {
    const PhysicsMaterial ice = PhysicsMaterial::create(0.0f, 0.0f, PhysicsMaterialCombine::MINIMUM);
    const PhysicsMaterial rubber = PhysicsMaterial::create(1.0f, 0.0f);
    Slope sliding(ice, rubber);
    EXPECT_GT(sliding.travelled(60), 1.5f) << "minimum of 0 and 1: slides (g sin 30 = 4.9 m/s^2)";

    const PhysicsMaterial sticky = PhysicsMaterial::create(2.0f, 0.0f, PhysicsMaterialCombine::MULTIPLY);
    const PhysicsMaterial half = PhysicsMaterial::create(0.5f, 0.0f);
    Slope sticking(sticky, half);
    EXPECT_LT(sticking.travelled(60), 0.05f) << "2 x 0.5 = 1 > tan 30: sticks";
}

TEST(PhysicsMaterial3D, WithoutMaterialsTheOldRuleExactly) {
    Slope legacy(PhysicsMaterial(), PhysicsMaterial(), 0.3f, 0.7f);
    EXPECT_FLOAT_EQ(combinedFrictionSeen(legacy), std::sqrt(0.3f * 0.7f)) << "geometric mean";

    // one side with a material, the other without: the material's mode
    const PhysicsMaterial average = PhysicsMaterial::create(0.9f, 0.0f, PhysicsMaterialCombine::AVERAGE);
    Slope mixed(average, PhysicsMaterial(), 0.3f, 0.7f);
    EXPECT_FLOAT_EQ(combinedFrictionSeen(mixed), 0.5f * (0.9f + 0.7f));
}

TEST(PhysicsMaterial3D, BouncinesOneWithMaximumBouncesBack) {
    World3D world;
    world.floor();
    const Entity ball = world.sphere(Vector3(0, 2.5f, 0), 0.5f, BodyType::DYNAMIC);
    world.rb(ball).setLinearDamping(0.0f);
    world.rb(ball).setColliderMaterial(0, PhysicsMaterial::create(0.4f, 1.0f, PhysicsMaterialCombine::AVERAGE, PhysicsMaterialCombine::MAXIMUM));
    world.load();
    const float dropped = 2.0f;  // the ball's bottom over the floor
    bool bounced = false;
    float highest = -1.0f;
    for (int i = 0; i < 150; i++) {
        world.step();
        const float bottom = world.rb(ball).getPosition().y - 0.5f;
        if (!bounced) {
            if (world.rb(ball).getLinearVelocity().y > 0.0f && bottom < 0.5f) bounced = true;
        } else {
            highest = std::fmax(highest, bottom);
        }
    }
    EXPECT_TRUE(bounced);
    EXPECT_GE(highest, 0.9f * dropped);
}

TEST(PhysicsMaterial3D, SharedMaterialEditReachesEveryColliderAndInstancesStayApart) {
    PhysicsMaterial shared = PhysicsMaterial::create(0.0f, 0.0f, PhysicsMaterialCombine::MINIMUM);
    World3D world;
    const Quaternion tilt = degreesAbout(30.0f, Vector3(0, 0, 1));
    const Vector3 normal = tilt * Vector3(0, 1, 0);
    world.box(Vector3(0, 0, 0), Vector3(30, 1, 10), BodyType::STATIC, NULL_ENTITY, tilt);
    const Vector3 startA = normal * 1.005f + Vector3(0, 0, -2);
    const Vector3 startB = normal * 1.005f + Vector3(0, 0, 2);
    const Entity a = world.box(startA, Vector3(1, 1, 1), BodyType::DYNAMIC, NULL_ENTITY, tilt);
    const Entity b = world.box(startB, Vector3(1, 1, 1), BodyType::DYNAMIC, NULL_ENTITY, tilt);
    world.rb(a).setColliderMaterial(0, shared);
    world.rb(b).setColliderMaterial(0, shared);
    world.load();
    world.step(8);
    EXPECT_GT((world.rb(a).getPosition() - startA).length(), 0.02f) << "both slide on ice";
    EXPECT_GT((world.rb(b).getPosition() - startB).length(), 0.02f);

    // the shared material grips (1 > tan 30, by Maximum over the slope's 0.2): both stop
    shared.setFriction(1.0f);
    shared.setFrictionCombine(PhysicsMaterialCombine::MAXIMUM);
    world.step(40);
    const Vector3 restA = world.rb(a).getPosition();
    const Vector3 restB = world.rb(b).getPosition();
    world.step(30);
    EXPECT_LT((world.rb(a).getPosition() - restA).length(), 0.01f) << "the edit reached a";
    EXPECT_LT((world.rb(b).getPosition() - restB).length(), 0.01f) << "and b";

    // a's own copy made slippery: b keeps the shared one and stays
    world.rb(a).setColliderFriction(0, 0.0f);
    EXPECT_FLOAT_EQ(shared.getFriction(), 1.0f) << "the shared material is unchanged";
    EXPECT_FLOAT_EQ(world.rb(a).getColliderFriction(0), 0.0f);
    EXPECT_FLOAT_EQ(world.rb(b).getColliderFriction(0), 1.0f);
    world.step(40);
    EXPECT_GT((world.rb(a).getPosition() - restA).length(), 0.1f) << "a slides on its copy (0 by Maximum with the slope's 0.2)";
    EXPECT_LT((world.rb(b).getPosition() - restB).length(), 0.02f) << "b still sticks";
}
