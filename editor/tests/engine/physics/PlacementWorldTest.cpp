// SPDX-License-Identifier: MIT
//
// PlacementWorld (engine/core/physics), the edit-time physics core of Physics Placement
// (design spec docs/superpowers/specs/2026-10-23-physics-placement-design.md sections 5.2, 6
// and 9): settling, the target-tracking control law, contacts, refusals, collider policies,
// context gathering, read-back and determinism, on scenes of engine primitives.

#include <gtest/gtest.h>

#include "PhysicsTestScene.h"
#include "component/InstancedMeshComponent.h"
#include "component/Joint3DComponent.h"
#include "component/ModelComponent.h"
#include "component/TerrainComponent.h"
#include "physics/ConvexDecomposition.h"
#include "physics/JoltShapeBuilder.h"
#include "physics/PlacementWorld.h"
#include "subsystem/MeshSystem.h"
#include "util/FileUtils.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::physicstest;

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    const AABB everywhere(-100, -100, -100, 100, 100, 100);

    PlacementWorldSettings zeroGravitySettings() {
        PlacementWorldSettings settings;
        settings.tuning.teleportDistance = 20.0f;
        return settings;
    }

    int stepUntilAsleep(PlacementWorld& world, int maxSteps) {
        for (int i = 0; i < maxSteps; i++) {
            world.step();
            if (world.allAsleep()) return i + 1;
        }
        return -1;
    }

    float angleBetween(const Quaternion& a, const Quaternion& b) {
        const float dot = std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
        return 2.0f * std::acos(std::min(1.0f, dot));
    }

    const float DEG = 3.14159265f / 180.0f;

    // A floor whose top is y = 0 (a static triangle mesh)
    Entity floorBox(TestScene& t, float size = 20.0f) {
        return t.box(Vector3(size, 1, size), Vector3(0, -0.5f, 0));
    }

}

// ---- settling -------------------------------------------------------------------------------

TEST(PlacementWorldSettle, BoxRestsOnBoxFloor) {
    TestScene t;
    const Entity floor = floorBox(t);
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0.3f, 1.5f, -0.2f));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    std::string why;
    ASSERT_TRUE(world.addMover(crate, &why)) << why;
    EXPECT_TRUE(why.empty());
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getContextBodyCount(), 1u);

    const int steps = stepUntilAsleep(world, 1200);
    ASSERT_GT(steps, 0) << "never came to rest";
    const PlacementBodyState state = world.getState(crate);
    EXPECT_NEAR(state.pose.position.y, 0.5f, 1e-3f);
    EXPECT_NEAR(state.pose.position.x, 0.3f, 1e-3f);
    EXPECT_NEAR(state.pose.position.z, -0.2f, 1e-3f);
    EXPECT_TRUE(state.asleep);
    EXPECT_TRUE(state.mover);
    EXPECT_EQ(state.shape, PlacementShapeKind::BOX);
    EXPECT_LT(state.maxPenetration, 1e-3f);
    ASSERT_EQ(state.restingOn.size(), 1u);
    EXPECT_EQ(state.restingOn[0], floor);
    EXPECT_TRUE(state.touchingContext);
    EXPECT_NEAR(state.averageContactNormal.y, 1.0f, 1e-3f);
}

// Jolt drops a body's contacts when it falls asleep; the world keeps them, so what rests where
// is still reported at the end of a settle
TEST(PlacementWorldSettle, AsleepBodiesKeepTheirContacts) {
    TestScene t;
    floorBox(t);
    const Entity crate = t.entity(Vector3(0, 0.5f, 0));
    const Entity bottom = t.box(Vector3(3, 0.2f, 3), Vector3(0, 0.1f, 0), crate);
    t.box(Vector3(0.2f, 1, 3), Vector3(-1.6f, 0.5f, 0), crate);
    t.box(Vector3(0.2f, 1, 3), Vector3(1.6f, 0.5f, 0), crate);
    t.box(Vector3(3.4f, 1, 0.2f), Vector3(0, 0.5f, -1.6f), crate);
    t.box(Vector3(3.4f, 1, 0.2f), Vector3(0, 0.5f, 1.6f), crate);
    std::vector<Entity> boxes;
    boxes.push_back(t.box(Vector3(0.5f, 0.5f, 0.5f), Vector3(-0.8f, 1.3f, 0.5f)));
    boxes.push_back(t.box(Vector3(0.5f, 0.5f, 0.5f), Vector3(0.0f, 2.3f, -0.6f)));
    boxes.push_back(t.box(Vector3(0.5f, 0.5f, 0.5f), Vector3(0.8f, 3.6f, 0.4f)));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    for (Entity e : boxes) ASSERT_TRUE(world.addMover(e));
    world.ensureContext(everywhere);
    ASSERT_GT(stepUntilAsleep(world, 1500), 0);
    // a while longer, every body asleep in Jolt too
    for (int i = 0; i < 120; i++) world.step();
    ASSERT_TRUE(world.allAsleep());

    for (Entity e : boxes) {
        const PlacementBodyState state = world.getState(e);
        EXPECT_TRUE(state.asleep) << e;
        EXPECT_NEAR(state.pose.position.y, 0.95f, 1e-3f) << e;
        EXPECT_EQ(state.restingOn, std::vector<Entity>{bottom}) << e;
        EXPECT_GE(state.contacts, 1u) << e;
        EXPECT_TRUE(state.touchingContext) << e;
        EXPECT_NEAR(state.averageContactNormal.y, 1.0f, 1e-3f) << e;
        EXPECT_LT(state.maxPenetration, 1e-3f) << e;
    }

    // woken, they still rest on it; lifted off and asleep again in the air, on nothing
    world.wakeAll();
    world.step();
    EXPECT_EQ(world.getState(boxes[0]).restingOn, std::vector<Entity>{bottom});
    world.setGravity(Vector3(0, 0, 0));
    world.teleport(boxes[0], {Vector3(-0.8f, 3.0f, 0.5f), Quaternion()});
    world.step();
    EXPECT_TRUE(world.getState(boxes[0]).restingOn.empty());
    ASSERT_GT(stepUntilAsleep(world, 1500), 0);
    for (int i = 0; i < 120; i++) world.step();
    const PlacementBodyState lifted = world.getState(boxes[0]);
    EXPECT_TRUE(lifted.asleep);
    EXPECT_NEAR(lifted.pose.position.y, 3.0f, 1e-4f);
    EXPECT_TRUE(lifted.restingOn.empty());
    EXPECT_EQ(lifted.contacts, 0u);
    EXPECT_FALSE(lifted.touchingContext);
    // the others still rest where they were
    EXPECT_EQ(world.getState(boxes[1]).restingOn, std::vector<Entity>{bottom});
}

// A body driven up off the floor (not teleported) and left to sleep in the air rests on nothing
TEST(PlacementWorldSettle, LiftedBodiesForgetTheFloor) {
    TestScene t;
    floorBox(t);
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0.5f, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    ASSERT_GT(stepUntilAsleep(world, 600), 0);
    for (int i = 0; i < 120; i++) world.step();
    ASSERT_FALSE(world.getState(crate).restingOn.empty());

    world.setGravity(Vector3(0, 0, 0));
    world.setTarget(crate, {Vector3(0, 2.5f, 0), Quaternion()});
    for (int i = 0; i < 120; i++) world.step();
    world.clearTargets();
    world.stopAll();
    ASSERT_GT(stepUntilAsleep(world, 600), 0);
    for (int i = 0; i < 120; i++) world.step();
    const PlacementBodyState state = world.getState(crate);
    EXPECT_NEAR(state.pose.position.y, 2.5f, 1e-3f);
    EXPECT_TRUE(state.restingOn.empty());
    EXPECT_EQ(state.contacts, 0u);
}

TEST(PlacementWorldSettle, SphereRollsOffRamp) {
    TestScene t;
    const Entity ramp = t.box(Vector3(6, 0.2f, 2), Vector3(0, 1, 0));
    t.setRotation(ramp, axisAngle(Vector3(0, 0, 1), 20.0f * DEG));
    const Entity ball = t.sphere(0.25f, Vector3(2.0f, 2.2f, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(ball));
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getState(ball).shape, PlacementShapeKind::SPHERE);

    bool touchedRamp = false;
    // 20 degrees, slowed by rolling resistance (0.2): about 1 m/s^2, off in 4 s
    for (int i = 0; i < 480; i++) {
        world.step();
        const PlacementBodyState state = world.getState(ball);
        touchedRamp = touchedRamp || std::find(state.restingOn.begin(), state.restingOn.end(), ramp) != state.restingOn.end();
    }
    EXPECT_TRUE(touchedRamp);
    const PlacementBodyState state = world.getState(ball);
    // off the low end (x = -2.8) and falling
    EXPECT_LT(state.pose.position.x, -2.9f);
    EXPECT_LT(state.pose.position.y, 0.0f);
    EXPECT_FALSE(state.asleep);
}

TEST(PlacementWorldSettle, BoxRestsOnTerrainSurface) {
    TestScene t;
    const Vector3 origin(5.0f, -1.0f, 3.0f);
    const Vector3 scale(1.25f, 0.8f, 1.25f);
    // a plateau in the middle, slopes around it
    const Entity terrain = t.terrain(65, 40.0f, 4.0f, origin, [](float u, float v) {
        const float d = std::max(std::fabs(u - 0.5f), std::fabs(v - 0.5f));
        return d < 0.15f ? 0.5f : 0.5f + (d - 0.15f) * 1.2f;
    });
    t.setScale(terrain, scale);
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(origin.x + 0.6f, 4.0f, origin.z - 0.4f));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getContextBodyCount(), 1u);
    ASSERT_GT(stepUntilAsleep(world, 1200), 0);

    TerrainComponent& component = t.scene.getComponent<TerrainComponent>(terrain);
    const PlacementBodyState state = world.getState(crate);
    float height = 0.0f;
    Vector3 normal;
    t.scene.getSystem<MeshSystem>()->sampleTerrainSurface(component, (state.pose.position.x - origin.x) / scale.x,
                                                          (state.pose.position.z - origin.z) / scale.z, height, normal);
    const float surface = origin.y + height * scale.y;
    EXPECT_NEAR(state.pose.position.y - 0.5f, surface, 2e-3f);
    ASSERT_EQ(state.restingOn.size(), 1u);
    EXPECT_EQ(state.restingOn[0], terrain);
}

// ---- the control law ------------------------------------------------------------------------

TEST(PlacementWorldControl, WallStopsTargetAndSlidingKeepsFullSpeed) {
    TestScene t;
    const Entity wall = t.box(Vector3(0.2f, 3, 12), Vector3(3, 0, 0));
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    t.resolve();

    PlacementWorldSettings settings = zeroGravitySettings();
    PlacementWorld world(&t.scene, settings);
    world.setGravity(Vector3(0, 0, 0));
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);

    // the target lies beyond the wall: the crate stops against it, never through it
    world.setTarget(crate, {Vector3(6, 0, 0), Quaternion()});
    float furthest = 0.0f;
    for (int i = 0; i < 240; i++) {
        world.step();
        furthest = std::max(furthest, world.getState(crate).pose.position.x);
    }
    PlacementBodyState state = world.getState(crate);
    EXPECT_NEAR(state.pose.position.x, 2.4f, 5e-3f);
    EXPECT_LT(furthest, 2.41f);
    EXPECT_TRUE(state.touchingContext);
    EXPECT_NEAR(state.averageContactNormal.x, -1.0f, 1e-3f);
    EXPECT_LT(state.maxPenetration, 2e-3f);
    EXPECT_FALSE(state.teleported);

    // along the wall nothing slows it: maxSpeed (10 m/s) for 0.1 s is 1 m
    const float x = state.pose.position.x;
    world.setTarget(crate, {Vector3(x, 0, 4), Quaternion()});
    for (int i = 0; i < 12; i++) world.step();
    state = world.getState(crate);
    EXPECT_NEAR(state.pose.position.z, 1.0f, 0.02f);
    EXPECT_NEAR(state.linearVelocity.length(), settings.tuning.maxSpeed, 0.2f);
    EXPECT_NEAR(state.pose.position.x, x, 5e-3f);
    EXPECT_TRUE(state.touchingContext);
    (void)wall;
}

// A mover that starts deep inside something (the fixture's Overlap pair: two unit cubes 0.2 m
// into each other on the floor, or a cube sunk into the floor slab) is lifted out by a Move
// drag, released and settled: it passes through what it started in while driven, nothing is
// launched, nothing outruns maxSpeed, nothing leaves the floor or falls out of the world, and
// everything ends at rest on something
TEST(PlacementWorldControl, DeepOverlapLiftedOutThenSettled) {
    struct Case { Vector3 b; int other; Vector3 lift; int dragSteps; };
    // other: 0 = A is context, 1 = A is a mover too, 2 = A is a pushed neighbour
    // Each case covers a distinct recovery condition. The Cartesian product of pose,
    // collision role, lift vector and drag duration repeated the same contracts 216 times.
    const Case cases[] = {
        {Vector3(0.8f, 0.5f, 0), 0, Vector3(0, 1, 0), 60},        // static context, lifted vertically
        {Vector3(0.2f, 0.5f, 0), 1, Vector3(0.5f, 1, 0), 10},     // another mover, diagonal drag
        {Vector3(0.0f, 0.5f, 0), 2, Vector3(-0.3f, 0.5f, 0), 60}, // coincident pushed neighbour
        {Vector3(0.2f, 0.1f, 0.1f), 0, Vector3(0, 0, 0), 0},      // release while sunk in the floor
        {Vector3(0.8f, 0.0f, 0), 1, Vector3(0, 0, 0), 10},        // motionless hold with another mover
        {Vector3(0.8f, -0.3f, 0), 2, Vector3(0.5f, 1, 0), 60},    // deep floor overlap while pushing
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(testing::Message() << "B at (" << c.b.x << ", " << c.b.y << ") other " << c.other << " lift (" << c.lift.x << ", "
                                        << c.lift.y << ") over " << c.dragSteps << " steps");
        TestScene t;
        t.box(Vector3(12, 0.2f, 12), Vector3(0, -0.1f, 0));
        const Entity overlap = t.entity(Vector3(3.5f, 0, 0));
        const Entity a = t.box(Vector3(1, 1, 1), Vector3(0, 0.5f, 0), overlap);
        const Entity b = t.box(Vector3(1, 1, 1), c.b, overlap);
        t.resolve();

        PlacementWorldSettings settings;
        settings.pushNeighbours = c.other == 2;
        PlacementWorld world(&t.scene, settings);
        ASSERT_TRUE(world.addMover(b));
        if (c.other == 1) {
            ASSERT_TRUE(world.addMover(a));
        }
        AABB region = world.getMoversBounds();
        region = AABB(region.getMinimum() - Vector3(2, 2, 2), region.getMaximum() + Vector3(2, 2, 2));
        settings.killY = region.getMinimum().y - 50.0f;
        world.setSettings(settings);
        world.ensureContext(region);
        ASSERT_EQ(world.getNeighbours().size(), c.other == 2 ? 1u : 0u);

        float fastest = 0.0f;
        auto check = [&]() {
            for (const PlacementBodyState& state : world.getStates()) {
                fastest = std::max(fastest, state.linearVelocity.length());
                ASSERT_FALSE(state.fellOut) << state.entity;
            }
        };
        const PlacementPose start = world.getState(b).pose;
        for (int i = 0; i < c.dragSteps; i++) {
            const float along = std::min(1.0f, (i + 1) / float(std::max(1, c.dragSteps / 2)));
            world.setTarget(b, {start.position + c.lift * along, start.rotation});
            world.step();
            check();
        }
        // released where it is, as the session's settle does
        world.clearTargets();
        world.stopAll();
        world.wakeAll();
        int steps = 0;
        for (; steps < 1200 && !world.allAsleep(); steps++) {
            world.step();
            check();
        }
        EXPECT_TRUE(world.allAsleep());
        EXPECT_LE(fastest, settings.tuning.maxSpeed + 1e-3f);
        for (const PlacementBodyState& state : world.getStates()) {
            EXPECT_GT(state.pose.position.y, 0.49f) << state.entity;
            EXPECT_LT(std::fabs(state.pose.position.x - 3.5f), 5.0f) << state.entity;
            EXPECT_LT(std::fabs(state.pose.position.z), 5.0f) << state.entity;
            EXPECT_FALSE(state.restingOn.empty()) << state.entity;
        }
    }
}

// Nothing outruns maxSpeed, a fall included; a lower limit applies at once
TEST(PlacementWorldControl, SpeedIsCappedAtMaxSpeed) {
    TestScene t;
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    t.resolve();

    PlacementWorldSettings settings;
    PlacementWorld world(&t.scene, settings);
    ASSERT_TRUE(world.addMover(crate));
    for (int i = 0; i < 240; i++) world.step();
    EXPECT_NEAR(world.getState(crate).linearVelocity.length(), settings.tuning.maxSpeed, 1e-3f);
    settings.tuning.maxSpeed = 4.0f;
    world.setSettings(settings);
    EXPECT_NEAR(world.getState(crate).linearVelocity.length(), 4.0f, 1e-3f);
    world.step();
    EXPECT_NEAR(world.getState(crate).linearVelocity.length(), 4.0f, 1e-3f);
}

TEST(PlacementWorldControl, ContactSlowsOnlyMotionIntoIt) {
    TestScene t;
    t.box(Vector3(0.2f, 3, 12), Vector3(3, 0, 0));
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(2.39f, 0, 0));
    t.resolve();

    PlacementWorld world(&t.scene, zeroGravitySettings());
    world.setGravity(Vector3(0, 0, 0));
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    world.step();   // contacts known

    // straight into the wall the speed is capped at contactSpeed (1 m/s, a 1 m crate)
    world.setTarget(crate, {Vector3(5, 0, 0), Quaternion()});
    world.step();
    world.step();
    const PlacementBodyState into = world.getState(crate);
    EXPECT_TRUE(into.touchingContext);
    EXPECT_LT(into.pose.position.x, 2.41f);

    // away from the wall it is not capped by the contact
    world.setTarget(crate, {Vector3(0, 0, 0), Quaternion()});
    world.step();
    const PlacementBodyState away = world.getState(crate);
    EXPECT_NEAR(away.linearVelocity.x, -10.0f, 0.1f);
}

TEST(PlacementWorldControl, TeleportPastThreshold) {
    TestScene t;
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    t.resolve();

    PlacementWorldSettings settings;
    settings.tuning.teleportDistance = 5.0f;
    PlacementWorld world(&t.scene, settings);
    world.setGravity(Vector3(0, 0, 0));
    ASSERT_TRUE(world.addMover(crate));

    // within the threshold: driven at maxSpeed
    world.setTarget(crate, {Vector3(3, 0, 0), Quaternion()});
    world.step();
    PlacementBodyState state = world.getState(crate);
    EXPECT_FALSE(state.teleported);
    EXPECT_NEAR(state.pose.position.x, 10.0f / 120.0f, 1e-3f);

    // past it: there in one step, flagged
    const Quaternion turned = axisAngle(Vector3(0, 1, 0), 30.0f * DEG);
    world.setTarget(crate, {Vector3(8, 1, -2), turned});
    world.step();
    state = world.getState(crate);
    EXPECT_TRUE(state.teleported);
    EXPECT_NEAR(state.pose.position.x, 8.0f, 1e-4f);
    EXPECT_NEAR(state.pose.position.y, 1.0f, 1e-4f);
    EXPECT_NEAR(state.pose.position.z, -2.0f, 1e-4f);
    EXPECT_LT(angleBetween(state.pose.rotation, turned), 1e-3f);
    EXPECT_TRUE(world.hasTarget(crate));

    world.clearFlags();
    EXPECT_FALSE(world.getState(crate).teleported);
    world.clearTargets();
    EXPECT_FALSE(world.hasTarget(crate));
}

TEST(PlacementWorldControl, RotationTakesTheShorterWay) {
    for (const float degrees : {170.0f, 190.0f, -170.0f}) {
        TestScene t;
        const Entity crate = t.box(Vector3(1, 0.5f, 2), Vector3(1, 2, 3));
        t.resolve();

        PlacementWorldSettings settings;
        PlacementWorld world(&t.scene, settings);
        world.setGravity(Vector3(0, 0, 0));
        ASSERT_TRUE(world.addMover(crate));

        const Quaternion target = axisAngle(Vector3(0, 1, 0), degrees * DEG);
        world.setTarget(crate, {Vector3(1, 2, 3), target});
        world.step();
        const PlacementBodyState first = world.getState(crate);
        // 170 turns +170, 190 and -170 both turn -170; capped at maxAngularSpeed
        const float expectedSign = degrees == 170.0f ? 1.0f : -1.0f;
        EXPECT_GT(first.angularVelocity.y * expectedSign, 0.0f) << degrees;
        EXPECT_NEAR(std::fabs(first.angularVelocity.y), settings.tuning.maxAngularSpeed, 0.05f) << degrees;
        EXPECT_NEAR(first.angularVelocity.x, 0.0f, 1e-3f);
        EXPECT_NEAR(first.angularVelocity.z, 0.0f, 1e-3f);

        for (int i = 0; i < 120; i++) world.step();
        const PlacementBodyState done = world.getState(crate);
        EXPECT_LT(angleBetween(done.pose.rotation, target), 0.5f * DEG) << degrees;
        // turning about its centre leaves the root where it is
        EXPECT_NEAR(done.pose.position.x, 1.0f, 1e-3f);
        EXPECT_NEAR(done.pose.position.y, 2.0f, 1e-3f);
        EXPECT_NEAR(done.pose.position.z, 3.0f, 1e-3f);
    }
}

TEST(PlacementWorldControl, SubstepsReachTheSameTarget) {
    TestScene t;
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    t.resolve();

    PlacementWorldSettings settings;
    settings.tuning.substeps = 3;
    PlacementWorld world(&t.scene, settings);
    world.setGravity(Vector3(0, 0, 0));
    ASSERT_TRUE(world.addMover(crate));
    world.setTarget(crate, {Vector3(0.05f, 0, 0), Quaternion()});
    world.step();
    EXPECT_NEAR(world.getState(crate).pose.position.x, 0.05f, 1e-4f);
    EXPECT_EQ(world.getStepCount(), 1u);
    EXPECT_NEAR(world.getSimulatedSeconds(), settings.fixedStep, 1e-9);
}

// ---- Separate, damping, stopping ------------------------------------------------------------

TEST(PlacementWorldSeparate, OverlappingBoxesEndApart) {
    TestScene t;
    const Entity a = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    const Entity b = t.box(Vector3(1, 1, 1), Vector3(0.8f, 0.05f, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    world.setGravity(Vector3(0, 0, 0));
    ASSERT_TRUE(world.addMover(a));
    ASSERT_TRUE(world.addMover(b));
    world.setDamping(5.0f, 5.0f);
    world.setSeparationBoost(4.0f);

    world.step();
    EXPECT_GT(world.getMaxPenetration(), 0.1f);
    for (int i = 0; i < 600 && world.getMaxPenetration() >= 1e-3f; i++) world.step();
    EXPECT_LT(world.getMaxPenetration(), 1e-3f);
    for (const PlacementBodyState& state : world.getStates()) EXPECT_LT(state.maxPenetration, 1e-3f);
    const Vector3 gap = world.getState(b).pose.position - world.getState(a).pose.position;
    EXPECT_GT(std::fabs(gap.x), 0.998f);

    world.stopAll();
    for (const PlacementBodyState& state : world.getStates()) EXPECT_LT(state.linearVelocity.length(), 1e-6f);
    world.setSeparationBoost(1.0f);
    world.setDamping(0.0f, 0.0f);
}

TEST(PlacementWorldSeparate, WakeAllAndExplicitTeleport) {
    TestScene t;
    floorBox(t);
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0.5f, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    ASSERT_GT(stepUntilAsleep(world, 600), 0);
    world.wakeAll();
    EXPECT_FALSE(world.allAsleep());

    world.teleport(crate, {Vector3(0, 3, 0), Quaternion()});
    PlacementBodyState state = world.getState(crate);
    EXPECT_NEAR(state.pose.position.y, 3.0f, 1e-5f);
    EXPECT_FALSE(state.teleported);   // only the control law's teleports are flagged
    ASSERT_GT(stepUntilAsleep(world, 1200), 0);
    EXPECT_NEAR(world.getState(crate).pose.position.y, 0.5f, 1e-3f);
}

// ---- refusals and caveats -------------------------------------------------------------------

TEST(PlacementWorldRefusal, EveryReason) {
    TestScene t;
    const Entity empty = t.entity(Vector3(0, 0, 0));
    const Entity huge = t.box(Vector3(30, 1, 1), Vector3(0, 0, 10));
    const Entity layered = t.box(Vector3(1, 1, 1), Vector3(5, 0, 0));
    t.scene.setEntityLayer(layered, 3);
    const Entity inactive = t.box(Vector3(1, 1, 1), Vector3(-5, 0, 0));
    t.scene.setEntityActive(inactive, false);
    const Entity loading = t.box(Vector3(1, 1, 1), Vector3(0, 0, -5));
    ModelComponent model;
    model.filename = "crate.glb";
    model.needUpdateModel = true;
    t.scene.addComponent<ModelComponent>(loading, model);
    const Entity instanced = t.box(Vector3(1, 1, 1), Vector3(0, 5, 0));
    t.scene.addComponent<InstancedMeshComponent>(instanced, InstancedMeshComponent());
    const Entity terrain = t.terrain(16, 10.0f, 1.0f, Vector3(0, -10, 0), [](float, float) { return 0.0f; });
    const Entity parent = t.entity(Vector3(10, 0, 0));
    const Entity mover = t.box(Vector3(1, 1, 1), Vector3(10, 0, 0), parent);
    const Entity child = t.box(Vector3(0.5f, 0.5f, 0.5f), Vector3(0, 1, 0), mover);
    t.resolve();

    PlacementWorldSettings settings;
    settings.ignoreLayerMask = 1u << 3;
    PlacementWorld world(&t.scene, settings);
    std::string why;
    ASSERT_TRUE(world.addMover(mover, &why)) << why;

    auto refused = [&](Entity e) {
        std::string reason = "unset";
        EXPECT_FALSE(world.addMover(e, &reason));
        return reason;
    };
    EXPECT_EQ(refused(empty), "no mesh or collider");
    EXPECT_EQ(refused(huge), "too large (> 25 m), treated as environment");
    EXPECT_EQ(refused(layered), "ignored layer");
    EXPECT_EQ(refused(inactive), "inactive");
    EXPECT_EQ(refused(loading), "mesh still loading");
    EXPECT_EQ(refused(instanced), "instanced mesh, treated as environment");
    EXPECT_EQ(refused(terrain), "terrain, treated as environment");
    EXPECT_EQ(refused(mover), "already a mover");
    EXPECT_EQ(refused(child), "part of another mover");
    EXPECT_EQ(refused(parent), "contains another mover");
    EXPECT_EQ(refused(NULL_ENTITY), "no mesh or collider");
    EXPECT_EQ(world.getMovers(), std::vector<Entity>{mover});

    // a lower limit refuses what fit before
    settings.maxMoverSize = 0.5f;
    world.setSettings(settings);
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(20, 0, 0));
    t.resolve();
    EXPECT_EQ(refused(crate), "too large (> 0.5 m), treated as environment");

    // removed, it can be added again
    world.removeMover(mover);
    EXPECT_FALSE(world.isMover(mover));
    settings.maxMoverSize = 25.0f;
    world.setSettings(settings);
    EXPECT_TRUE(world.addMover(child, &why)) << why;
}

TEST(PlacementWorldRefusal, SkinnedAndJointedAreAcceptedWithAWarning) {
    TestScene t;
    const Entity skinned = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    t.scene.getComponent<MeshComponent>(skinned).submeshes[0].hasSkinning = true;
    const Entity jointed = t.box(Vector3(1, 1, 1), Vector3(3, 0, 0));
    const Entity jointEntity = t.entity(Vector3(0, 0, 0));
    Joint3DComponent joint;
    joint.bodyA = jointed;
    t.scene.addComponent<Joint3DComponent>(jointEntity, joint);
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    std::string why;
    ASSERT_TRUE(world.addMover(skinned, &why));
    EXPECT_EQ(why, "skinned mesh: simulated in its bind pose");
    ASSERT_TRUE(world.addMover(jointed, &why));
    EXPECT_EQ(why, "has joints: simulated free (joints are not recreated)");
}

// ---- collider policies and primitive fits ---------------------------------------------------

TEST(PlacementWorldColliders, AuthoredPolicyNeedsColliders) {
    TestScene t;
    const Entity plain = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    const Entity authored = t.box(Vector3(1, 1, 1), Vector3(3, 0, 0));
    Collider3D& collider = t.collider(authored, Collider3DType::BOX, BodyType::DYNAMIC);
    collider.width = 1; collider.height = 1; collider.depth = 1;
    t.resolve();

    PlacementWorldSettings settings;
    settings.colliders = PlacementColliderPolicy::AUTHORED;
    PlacementWorld world(&t.scene, settings);
    std::string why;
    EXPECT_FALSE(world.addMover(plain, &why));
    EXPECT_EQ(why, "no authored collider");
    ASSERT_TRUE(world.addMover(authored, &why)) << why;
    EXPECT_EQ(world.getState(authored).shape, PlacementShapeKind::AUTHORED);
}

TEST(PlacementWorldColliders, PoliciesChooseTheirSources) {
    // a 1 m box mesh with an authored sphere 2 m above it
    auto bounds = [](PlacementColliderPolicy policy, PlacementShapeKind& kind) {
        TestScene t;
        const Entity e = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
        Collider3D& sphere = t.collider(e, Collider3DType::SPHERE);
        sphere.radius = 0.5f;
        sphere.position = Vector3(0, 2, 0);
        t.resolve();
        PlacementWorldSettings settings;
        settings.colliders = policy;
        PlacementWorld world(&t.scene, settings);
        EXPECT_TRUE(world.addMover(e));
        kind = world.getState(e).shape;
        return world.getMoversBounds();
    };

    PlacementShapeKind kind;
    AABB both = bounds(PlacementColliderPolicy::BOTH, kind);
    EXPECT_EQ(kind, PlacementShapeKind::AUTHORED);
    EXPECT_NEAR(both.getMinimum().y, -0.5f, 0.02f);
    EXPECT_NEAR(both.getMaximum().y, 2.5f, 0.02f);

    AABB automatic = bounds(PlacementColliderPolicy::AUTO, kind);
    EXPECT_EQ(kind, PlacementShapeKind::AUTHORED);
    EXPECT_NEAR(automatic.getMinimum().y, 1.5f, 0.02f);
    EXPECT_NEAR(automatic.getMaximum().y, 2.5f, 0.02f);

    AABB generated = bounds(PlacementColliderPolicy::GENERATED, kind);
    EXPECT_EQ(kind, PlacementShapeKind::BOX);
    EXPECT_NEAR(generated.getMinimum().y, -0.5f, 0.02f);
    EXPECT_NEAR(generated.getMaximum().y, 0.5f, 0.02f);
}

TEST(PlacementWorldColliders, ContextUsesAuthoredCollidersFirst) {
    // a floor mesh whose authored collider is a thin box 1 m higher than the mesh
    TestScene t;
    const Entity floor = floorBox(t);
    Collider3D& collider = t.collider(floor, Collider3DType::BOX);
    collider.width = 20; collider.height = 0.1f; collider.depth = 20;
    collider.position = Vector3(0, 1.45f, 0);
    t.resolve();

    for (PlacementColliderPolicy policy : {PlacementColliderPolicy::AUTO, PlacementColliderPolicy::GENERATED, PlacementColliderPolicy::BOTH}) {
        PlacementWorldSettings settings;
        settings.colliders = policy;
        PlacementWorld world(&t.scene, settings);
        world.ensureContext(everywhere);
        PlacementRayHit hit;
        ASSERT_TRUE(world.raycast(Ray(Vector3(1, 5, 1), Vector3(0, -10, 0)), hit));
        EXPECT_EQ(hit.entity, floor);
        EXPECT_NEAR(hit.point.y, policy == PlacementColliderPolicy::GENERATED ? 0.0f : 1.0f, 1e-3f) << int(policy);
        EXPECT_EQ(world.getContextBodyCount(), policy == PlacementColliderPolicy::BOTH ? 2u : 1u);
    }
}

TEST(PlacementWorldColliders, AuthoredMoverShapes) {
    TestScene t;
    floorBox(t);
    // an authored triangle mesh can't move: it becomes the hull of the same mesh
    const Entity meshed = t.box(Vector3(1, 1, 1), Vector3(-8, 2, 0));
    t.collider(meshed, Collider3DType::MESH).source = Collider3DSource::ENTITY_MESH;
    // raw triangles become raw hull points
    const Entity raw = t.entity(Vector3(-5, 2, 0));
    Collider3D& rawMesh = t.collider(raw, Collider3DType::MESH);
    rawMesh.source = Collider3DSource::RAW_MESH;
    for (int i = 0; i < 8; i++) {
        rawMesh.vertices.push_back(Vector3((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f));
    }
    const uint16_t faces[36] = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    for (uint16_t i : faces) rawMesh.indices.push_back(i);
    // a heightfield is left out, the box next to it kept
    const Entity mixed = t.box(Vector3(1, 1, 1), Vector3(-2, 2, 0));
    Collider3D& mixedBox = t.collider(mixed, Collider3DType::BOX);
    mixedBox.width = 1; mixedBox.height = 1; mixedBox.depth = 1;
    t.collider(mixed, Collider3DType::HEIGHTFIELD);
    // a hull of a child's mesh stands for that mesh: AUTO generates nothing more for it
    const Entity holder = t.entity(Vector3(1, 2, 0));
    const Entity part = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0), holder);
    Collider3D& partHull = t.collider(holder, Collider3DType::CONVEX_HULL);
    partHull.source = Collider3DSource::ENTITY_MESH;
    partHull.sourceEntity = part;
    // sensors and disabled Rigidbodies don't count
    const Entity sensor = t.box(Vector3(1, 1, 1), Vector3(4, 2, 0));
    t.collider(sensor, Collider3DType::SPHERE).radius = 3.0f;
    t.scene.getComponent<RigidbodyComponent>(sensor).sensor = true;
    const Entity disabled = t.box(Vector3(1, 1, 1), Vector3(7, 2, 0));
    t.collider(disabled, Collider3DType::SPHERE).radius = 3.0f;
    t.scene.getComponent<RigidbodyComponent>(disabled).enabled = false;
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    for (Entity e : {meshed, raw, mixed, holder, sensor, disabled}) {
        std::string why;
        ASSERT_TRUE(world.addMover(e, &why)) << why;
    }
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getState(meshed).shape, PlacementShapeKind::AUTHORED);
    EXPECT_EQ(world.getState(raw).shape, PlacementShapeKind::AUTHORED);
    EXPECT_EQ(world.getState(mixed).shape, PlacementShapeKind::AUTHORED);
    EXPECT_EQ(world.getState(holder).shape, PlacementShapeKind::AUTHORED);
    EXPECT_EQ(world.getState(sensor).shape, PlacementShapeKind::BOX);
    EXPECT_EQ(world.getState(disabled).shape, PlacementShapeKind::BOX);
    ASSERT_GT(stepUntilAsleep(world, 1500), 0);
    for (Entity e : {meshed, raw, mixed, holder, sensor, disabled}) {
        EXPECT_NEAR(world.getState(e).pose.position.y, 0.5f, 2e-3f) << e;
    }
}

TEST(PlacementWorldColliders, PrimitiveFits) {
    TestScene t;
    const Entity cube = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    const Entity slab = t.box(Vector3(2, 0.3f, 1), Vector3(3, 0, 0));
    t.setRotation(slab, axisAngle(Vector3(0, 1, 0), 0.4f));
    const Entity ball = t.sphere(0.5f, Vector3(6, 0, 0));
    const Entity pill = t.capsule(0.3f, 1.2f, Vector3(9, 0, 0));
    const Entity ring = t.torus(0.5f, 0.15f, Vector3(12, 0, 0));
    const Entity can = t.cylinder(0.4f, 1.0f, Vector3(15, 0, 0));
    const Entity sheet = t.plane(1.0f, 0.7f, Vector3(18, 0, 0));
    const Entity pencil = t.box(Vector3(0.007f, 0.007f, 0.15f), Vector3(21, 0, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    for (Entity e : {cube, slab, ball, pill, ring, can, sheet, pencil}) {
        std::string why;
        ASSERT_TRUE(world.addMover(e, &why)) << why;
    }
    EXPECT_EQ(world.getState(cube).shape, PlacementShapeKind::BOX);
    EXPECT_EQ(world.getState(slab).shape, PlacementShapeKind::BOX);
    EXPECT_EQ(world.getState(ball).shape, PlacementShapeKind::SPHERE);
    EXPECT_EQ(world.getState(pill).shape, PlacementShapeKind::CAPSULE);
    EXPECT_EQ(world.getState(ring).shape, PlacementShapeKind::CONVEX_HULL);
    EXPECT_EQ(world.getState(ring).hullCount, 1);
    EXPECT_EQ(world.getState(can).shape, PlacementShapeKind::CONVEX_HULL);
    EXPECT_EQ(world.getState(sheet).shape, PlacementShapeKind::BOX);
    EXPECT_EQ(world.getState(pencil).shape, PlacementShapeKind::BOX);
    EXPECT_FALSE(world.hasPendingDecompositions());

    // a grain too small for Jolt's hull builder still gets a box
    const Entity grain = t.box(Vector3(0.0004f, 0.0004f, 0.0004f), Vector3(24, 0, 0));
    t.resolve();
    ASSERT_TRUE(world.addMover(grain));
    EXPECT_EQ(world.getState(grain).shape, PlacementShapeKind::BOX);

    // the pencil keeps its thin shape: its bounds are its own, not rounded up
    PlacementWorld alone(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(alone.addMover(pencil));
    const Vector3 size = alone.getMoversBounds().getSize();
    EXPECT_NEAR(size.x, 0.007f, 1e-4f);
    EXPECT_NEAR(size.z, 0.15f, 1e-4f);
}

TEST(PlacementWorldColliders, ChildMeshesMergeIntoOneScaledBody) {
    // a table: a top and four legs under one empty root, scaled and turned
    TestScene t;
    floorBox(t);
    const Entity table = t.entity(Vector3(0, 2, 0));
    t.box(Vector3(2, 0.1f, 1), Vector3(0, 0.95f, 0), table);
    for (int i = 0; i < 4; i++) {
        t.box(Vector3(0.1f, 0.9f, 0.1f), Vector3((i & 1) ? 0.9f : -0.9f, 0.45f, (i & 2) ? 0.4f : -0.4f), table);
    }
    // left out: an inactive part and one on an ignored layer
    const Entity hidden = t.box(Vector3(0.2f, 3, 0.2f), Vector3(0, 2, 0), table);
    t.scene.setEntityActive(hidden, false);
    const Entity ignored = t.box(Vector3(0.2f, 3, 0.2f), Vector3(0, -2, 0), table);
    t.scene.setEntityLayer(ignored, 5);
    t.setScale(table, Vector3(1.5f, 1.5f, 1.5f));
    t.setRotation(table, axisAngle(Vector3(0, 1, 0), 0.5f));
    t.resolve();

    PlacementWorldSettings settings;
    settings.ignoreLayerMask = 1u << 5;
    PlacementWorld world(&t.scene, settings);
    ASSERT_TRUE(world.addMover(table));
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getState(table).shape, PlacementShapeKind::BOX);
    ASSERT_GT(stepUntilAsleep(world, 1500), 0);
    // stands on its legs: the root (the legs' feet) on the floor, 1.5 m tall
    const PlacementBodyState state = world.getState(table);
    EXPECT_NEAR(state.pose.position.y, 0.0f, 1e-3f);
    EXPECT_NEAR(world.getMoversBounds().getMaximum().y, 1.5f, 2e-3f);
    EXPECT_LT(angleBetween(state.pose.rotation, axisAngle(Vector3(0, 1, 0), 0.5f)), 0.5f * DEG);
}

namespace {

    // Waits for the world's decomposition requests: the cache's completionVersion moves as each
    // finishes
    bool waitForDecompositions(PlacementWorld& world, ConvexDecompositionCache& cache, uint64_t version) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!world.hasPendingDecompositions() && cache.completionVersion() > version) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

}

TEST(PlacementWorldColliders, DecompositionWithTheCache) {
    if (!ConvexDecompositionCache::available()) GTEST_SKIP() << "no convex decomposition in this build";
    TestScene t;
    const Entity ring = t.torus(0.5f, 0.15f, Vector3(0, 0, 0));
    const Entity cube = t.box(Vector3(1, 1, 1), Vector3(3, 0, 0));
    t.resolve();

    ConvexDecompositionCache cache("");
    PlacementWorldSettings settings;
    settings.decompositionCache = &cache;
    settings.decompositionResolution = 20000;   // cache/shape transitions do not need production-quality voxels
    PlacementWorld world(&t.scene, settings);
    const uint64_t version = cache.completionVersion();
    ASSERT_TRUE(world.addMover(ring));
    ASSERT_TRUE(world.addMover(cube));
    // the hull meanwhile, a request queued; the cube fits a box and asks for nothing
    PlacementBodyState state = world.getState(ring);
    EXPECT_EQ(state.shape, PlacementShapeKind::CONVEX_HULL);
    EXPECT_TRUE(state.decompositionPending);
    EXPECT_TRUE(world.hasPendingDecompositions());
    EXPECT_EQ(cache.pendingCount(), 1u);
    EXPECT_FALSE(world.getState(cube).decompositionPending);

    ASSERT_TRUE(waitForDecompositions(world, cache, version));
    EXPECT_EQ(world.getState(ring).shape, PlacementShapeKind::CONVEX_HULL);   // until refreshed
    for (int i = 0; i < 10; i++) world.step();
    const PlacementBodyState before = world.getState(ring);
    ASSERT_GT(before.linearVelocity.length(), 0.0f);
    world.refreshShapes();
    state = world.getState(ring);
    EXPECT_NEAR((state.pose.position - before.pose.position).length(), 0.0f, 1e-6f);
    EXPECT_NEAR((state.linearVelocity - before.linearVelocity).length(), 0.0f, 1e-6f);
    EXPECT_EQ(state.shape, PlacementShapeKind::DECOMPOSED);
    EXPECT_GT(state.hullCount, 1);
    EXPECT_LE(state.hullCount, (int)settings.decompositionMaxHulls);
    EXPECT_FALSE(state.decompositionPending);
    EXPECT_EQ(world.getState(cube).shape, PlacementShapeKind::BOX);

    // cached now: a new world takes it at once, and NEVER still asks for nothing
    PlacementWorld cached(&t.scene, settings);
    ASSERT_TRUE(cached.addMover(ring));
    EXPECT_EQ(cached.getState(ring).shape, PlacementShapeKind::DECOMPOSED);
    EXPECT_FALSE(cached.hasPendingDecompositions());
    PlacementWorldSettings never = settings;
    never.decomposition = PlacementDecomposition::NEVER;
    PlacementWorld hullOnly(&t.scene, never);
    ASSERT_TRUE(hullOnly.addMover(ring));
    EXPECT_EQ(hullOnly.getState(ring).shape, PlacementShapeKind::CONVEX_HULL);
    EXPECT_FALSE(hullOnly.hasPendingDecompositions());
}

// Spec section 10: a torus dropped over a cylinder peg encircles it when decomposed, and rests
// on top with its hull alone
TEST(PlacementWorldColliders, RingOverPeg) {
    if (!ConvexDecompositionCache::available()) GTEST_SKIP() << "no convex decomposition in this build";
    ConvexDecompositionCache cache("");
    for (const bool decomposed : {true, false}) {
        TestScene t;
        floorBox(t);
        t.cylinder(0.15f, 1.0f, Vector3(0, 0.5f, 0));
        const Entity ring = t.torus(0.5f, 0.12f, Vector3(0.04f, 1.6f, -0.03f));
        t.resolve();

        PlacementWorldSettings settings;
        settings.decompositionCache = &cache;
        settings.decomposition = decomposed ? PlacementDecomposition::ALWAYS : PlacementDecomposition::NEVER;
        PlacementWorld world(&t.scene, settings);
        const uint64_t version = cache.completionVersion();
        ASSERT_TRUE(world.addMover(ring));
        if (decomposed && world.hasPendingDecompositions()) {
            ASSERT_TRUE(waitForDecompositions(world, cache, version));
            world.refreshShapes();
        }
        ASSERT_EQ(world.getState(ring).shape, decomposed ? PlacementShapeKind::DECOMPOSED : PlacementShapeKind::CONVEX_HULL);
        world.ensureContext(everywhere);
        ASSERT_GT(stepUntilAsleep(world, 1200), 0) << decomposed;

        const PlacementBodyState state = world.getState(ring);
        const float offAxis = std::sqrt(state.pose.position.x * state.pose.position.x + state.pose.position.z * state.pose.position.z);
        if (decomposed) {
            // down around the peg, on the floor
            EXPECT_LT(state.pose.position.y, 0.5f);
            EXPECT_LT(offAxis, 0.38f - 0.15f);
        } else {
            // on the peg's top
            EXPECT_NEAR(state.pose.position.y, 1.0f + 0.12f, 0.02f);
        }
    }
}

// The Bake buttons: requestDecompositions asks for what addMover would use, by the same keys
TEST(PlacementWorldColliders, BakeThenMoveIsDecomposedAtOnce) {
    if (!ConvexDecompositionCache::available()) GTEST_SKIP() << "no convex decomposition in this build";
    TestScene t;
    const Entity group = t.entity(Vector3(0, 0, 0));
    const Entity ring = t.torus(0.5f, 0.15f, Vector3(0, 0, 0), group);
    t.box(Vector3(1, 1, 1), Vector3(3, 0, 0), group);
    t.resolve();

    ConvexDecompositionCache cache("");
    PlacementWorldSettings settings;
    settings.decompositionCache = &cache;
    settings.decompositionResolution = 20000;   // cache/shape transitions do not need production-quality voxels
    EXPECT_EQ(PlacementWorld::requestDecompositions(&t.scene, group, settings), 1u);
    EXPECT_EQ(cache.pendingCount(), 1u);
    EXPECT_FALSE(PlacementWorld::decompositionReady(&t.scene, group, settings));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
    while (!PlacementWorld::decompositionReady(&t.scene, group, settings) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(PlacementWorld::decompositionReady(&t.scene, group, settings));
    EXPECT_TRUE(PlacementWorld::decompositionReady(&t.scene, ring, settings));
    // baking again finds it cached and queues nothing
    EXPECT_EQ(PlacementWorld::requestDecompositions(&t.scene, group, settings), 1u);
    EXPECT_EQ(cache.pendingCount(), 0u);

    // a new world (default CACHED: the concavity test passes the torus) takes the baked hulls
    PlacementWorld world(&t.scene, settings);
    ASSERT_TRUE(world.addMover(group));
    const PlacementBodyState state = world.getState(group);
    EXPECT_EQ(state.shape, PlacementShapeKind::DECOMPOSED);
    EXPECT_GT(state.hullCount, 1);
    EXPECT_FALSE(state.decompositionPending);
    EXPECT_FALSE(world.hasPendingDecompositions());
}

namespace {

    // A coarse decomposition: the real queue ordering without high-fidelity hull work (what the
    // ordering tests check does not depend on it). Under a sanitizer it still runs several times
    // slower: a longer wait
    constexpr int QUEUE_RACE_RESOLUTION = 20000;
#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    constexpr int QUEUE_RACE_SECONDS = 1200;
#else
    constexpr int QUEUE_RACE_SECONDS = 300;
#endif

    // The order a bake's roots and a mover finish their decompositions in, polled every 2 ms
    struct QueueRace {
        TestScene t;
        std::vector<Entity> baked;
        Entity mover = NULL_ENTITY;
        ConvexDecompositionCache cache{""};
        PlacementWorldSettings settings;

        QueueRace() {
            // One running bake plus two queued bakes proves overtaking and FIFO.
            for (int i = 0; i < 3; i++) baked.push_back(t.torus(0.4f + 0.05f * i, 0.12f, Vector3(3.0f * i, 0, 0)));
            mover = t.torus(0.75f, 0.12f, Vector3(-5, 0, 0));
            t.resolve();
            settings.decompositionCache = &cache;
            settings.decompositionResolution = QUEUE_RACE_RESOLUTION;
        }

        // the moment (poll index) each root's parts were all cached
        std::vector<int> finishOrder(const std::vector<Entity>& roots) {
            std::vector<int> when(roots.size(), -1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(QUEUE_RACE_SECONDS);
            for (int poll = 0; std::chrono::steady_clock::now() < deadline; poll++) {
                bool all = true;
                for (size_t i = 0; i < roots.size(); i++) {
                    if (when[i] < 0 && PlacementWorld::decompositionReady(&t.scene, roots[i], settings)) when[i] = poll;
                    all = all && when[i] >= 0;
                }
                if (all) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return when;
        }
    };

}

// A mover's decomposition goes ahead of a bake's queue (only the job running already finishes
// first), and a part the bake queued already moves up when it becomes a mover's; the bake
// itself runs first come, first served
TEST(PlacementWorldColliders, MoversJumpTheBakeQueue) {
    if (!ConvexDecompositionCache::available()) GTEST_SKIP() << "no convex decomposition in this build";
    for (const bool alreadyQueued : {false, true}) {
        SCOPED_TRACE(alreadyQueued ? "the mover's part queued by the bake" : "the mover's part new");
        QueueRace race;
        for (Entity e : race.baked) ASSERT_EQ(PlacementWorld::requestDecompositions(&race.t.scene, e, race.settings), 1u);
        if (alreadyQueued) {
            ASSERT_EQ(PlacementWorld::requestDecompositions(&race.t.scene, race.mover, race.settings), 1u);
        }
        PlacementWorld world(&race.t.scene, race.settings);
        ASSERT_TRUE(world.addMover(race.mover));
        ASSERT_TRUE(world.hasPendingDecompositions());

        std::vector<Entity> roots = race.baked;
        roots.push_back(race.mover);
        const std::vector<int> when = race.finishOrder(roots);
        const int moverDone = when.back();
        ASSERT_GE(moverDone, 0);
        int bakedBefore = 0;
        for (size_t i = 0; i + 1 < when.size(); i++) {
            ASSERT_GE(when[i], 0);
            if (when[i] < moverDone) bakedBefore++;
        }
        EXPECT_LE(bakedBefore, 1);   // the one that was running
        for (size_t i = 1; i + 1 < when.size(); i++) EXPECT_LE(when[i - 1], when[i]) << i;   // the bake in order
        world.refreshShapes();
        EXPECT_EQ(world.getState(race.mover).shape, PlacementShapeKind::DECOMPOSED);
    }
}

// A bake for a caller that waits (an agent's decompose "always") can be urgent too
TEST(PlacementWorldColliders, UrgentBakesJumpTheQueue) {
    if (!ConvexDecompositionCache::available()) GTEST_SKIP() << "no convex decomposition in this build";
    QueueRace race;
    for (Entity e : race.baked) ASSERT_EQ(PlacementWorld::requestDecompositions(&race.t.scene, e, race.settings), 1u);
    ASSERT_EQ(PlacementWorld::requestDecompositions(&race.t.scene, race.mover, race.settings, nullptr, true), 1u);
    std::vector<Entity> roots = race.baked;
    roots.push_back(race.mover);
    const std::vector<int> when = race.finishOrder(roots);
    ASSERT_GE(when.back(), 0) << "the urgent decomposition never completed";
    int bakedBefore = 0;
    for (size_t i = 0; i + 1 < when.size(); i++) {
        ASSERT_GE(when[i], 0);
        if (when[i] < when.back()) bakedBefore++;
    }
    EXPECT_LE(bakedBefore, 1);
}

TEST(PlacementWorldColliders, BakeSkipsPrimitivesAndRefusals) {
    TestScene t;
    const Entity cube = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    t.scene.setEntityName(cube, "Cube");
    const Entity hidden = t.torus(0.5f, 0.15f, Vector3(3, 0, 0));
    t.scene.setEntityName(hidden, "Hidden");
    t.scene.setEntityActive(hidden, false);
    t.resolve();

    ConvexDecompositionCache cache("");
    PlacementWorldSettings settings;
    settings.decompositionCache = &cache;
    std::vector<std::string> notes;
    // a primitive fit asks for nothing, and has nothing missing
    EXPECT_EQ(PlacementWorld::requestDecompositions(&t.scene, cube, settings, &notes), 0u);
    EXPECT_EQ(notes, std::vector<std::string>{"Cube: fits a box, nothing to decompose"});
    EXPECT_EQ(cache.pendingCount(), 0u);
    EXPECT_TRUE(PlacementWorld::decompositionReady(&t.scene, cube, settings));

    // what addMover refuses is not baked
    notes.clear();
    EXPECT_EQ(PlacementWorld::requestDecompositions(&t.scene, hidden, settings, &notes), 0u);
    EXPECT_EQ(notes, std::vector<std::string>{"Hidden: inactive"});
    EXPECT_FALSE(PlacementWorld::decompositionReady(&t.scene, hidden, settings));
    EXPECT_EQ(cache.pendingCount(), 0u);

    // nor anything without a cache
    notes.clear();
    EXPECT_EQ(PlacementWorld::requestDecompositions(&t.scene, cube, PlacementWorldSettings(), &notes), 0u);
    EXPECT_EQ(notes, std::vector<std::string>{"no decomposition cache"});
    EXPECT_FALSE(PlacementWorld::decompositionReady(&t.scene, cube, PlacementWorldSettings()));
}

// ---- context --------------------------------------------------------------------------------

TEST(PlacementWorldContext, IncrementalByRegion) {
    TestScene t;
    const Entity near = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    const Entity far = t.box(Vector3(1, 1, 1), Vector3(30, 0, 0));
    const Entity hidden = t.box(Vector3(1, 1, 1), Vector3(1, 0, 0));
    t.scene.setEntityActive(hidden, false);
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    world.ensureContext(AABB(-2, -2, -2, 2, 2, 2));
    EXPECT_EQ(world.getContextBodyCount(), 1u);
    world.ensureContext(AABB(-2, -2, -2, 2, 2, 2));
    EXPECT_EQ(world.getContextBodyCount(), 1u);
    world.ensureContext(AABB(-2, -2, -2, 32, 2, 2));
    EXPECT_EQ(world.getContextBodyCount(), 2u);
    EXPECT_NEAR(world.getContextRegion().getMaximum().x, 32.0f, 1e-6f);

    // a mover stops being context
    ASSERT_TRUE(world.addMover(near));
    EXPECT_EQ(world.getContextBodyCount(), 1u);
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getContextBodyCount(), 1u);
    // and is context again once removed
    world.removeMover(near);
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getContextBodyCount(), 2u);

    PlacementWorld explicitContext(&t.scene, PlacementWorldSettings());
    explicitContext.addContextEntity(far);
    EXPECT_EQ(explicitContext.getContextBodyCount(), 1u);
}

// Editor overlays (selection outlines, the terrain brush, the Physics Placement lines) live in
// the edited scene but are never something to land on, push or move
TEST(PlacementWorldContext, EditorOverlaysAreNeverContext) {
    TestScene t;
    const Entity floor = floorBox(t);
    const Entity outline = t.box(Vector3(0.5f, 0.5f, 0.5f), Vector3(0, 0.25f, 0));
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(3, 0.5f, 0));
    t.scene.getSystem<RenderSystem>()->setEditorOverlay(floor, true);
    t.scene.getSystem<RenderSystem>()->setEditorOverlay(outline, true);
    t.resolve();

    PlacementWorldSettings settings;
    settings.pushNeighbours = true;
    PlacementWorld world(&t.scene, settings);
    std::string why;
    EXPECT_FALSE(world.addMover(outline, &why));
    EXPECT_EQ(why, "no mesh or collider");
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    world.addContextEntity(floor);
    world.addContextEntity(outline);
    EXPECT_EQ(world.getContextBodyCount(), 0u);
    EXPECT_TRUE(world.getNeighbours().empty());
    PlacementRayHit hit;
    EXPECT_FALSE(world.raycast(Ray(Vector3(0, 5, 0), Vector3(0, -10, 0)), hit));
}

TEST(PlacementWorldContext, PushNeighboursMovesWhatItHits) {
    for (const bool push : {true, false}) {
        TestScene t;
        floorBox(t);
        const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0.5f, 0));
        const Entity pebble = t.box(Vector3(0.4f, 0.4f, 0.4f), Vector3(1.5f, 0.2f, 0));
        t.resolve();

        PlacementWorldSettings settings;
        settings.pushNeighbours = push;
        PlacementWorld world(&t.scene, settings);
        ASSERT_TRUE(world.addMover(crate));
        world.ensureContext(everywhere);
        if (push) {
            ASSERT_EQ(world.getNeighbours(), std::vector<Entity>{pebble});
            const std::vector<PlacementBodyState> states = world.getStates();
            ASSERT_EQ(states.size(), 2u);
            EXPECT_EQ(states[0].entity, crate);
            EXPECT_TRUE(states[0].mover);
            EXPECT_EQ(states[1].entity, pebble);
            EXPECT_FALSE(states[1].mover);
            EXPECT_EQ(world.getContextBodyCount(), 1u);   // the floor (too large to push)
        } else {
            EXPECT_TRUE(world.getNeighbours().empty());
            EXPECT_EQ(world.getContextBodyCount(), 2u);
        }

        world.setTarget(crate, {Vector3(3.5f, 0.5f, 0), Quaternion()});
        for (int i = 0; i < 300; i++) world.step();
        if (push) {
            EXPECT_GT(world.getState(pebble).pose.position.x, 1.9f);
            EXPECT_GT(world.getState(crate).pose.position.x, 2.5f);
        } else {
            // the pebble is a wall then
            EXPECT_LT(world.getState(crate).pose.position.x, 0.81f);
        }
    }
}

// A neighbour brings in what it stands on: three boxes dropped into a crate that stands on a
// table, the region only half a metre around the boxes, so the floor under the table (and the
// table under the crate) lie outside it
TEST(PlacementWorldContext, NeighboursStandOnContextOutsideTheRegion) {
    TestScene t;
    const Entity floor = t.box(Vector3(12, 0.2f, 12), Vector3(0, -0.1f, 0));
    const Entity table = t.entity(Vector3(0, 0, 0));
    t.box(Vector3(1.6f, 0.1f, 1.0f), Vector3(0, 0.95f, 0), table);
    for (int i = 0; i < 4; i++) {
        t.box(Vector3(0.08f, 0.9f, 0.08f), Vector3((i & 1) ? 0.7f : -0.7f, 0.45f, (i & 2) ? 0.4f : -0.4f), table);
    }
    const Entity props = t.entity(Vector3(0, 0, 0));
    const Entity crate = t.entity(Vector3(0, 1.0f, 0), props);
    t.box(Vector3(0.6f, 0.04f, 0.6f), Vector3(0, 0.02f, 0), crate);
    t.box(Vector3(0.6f, 0.38f, 0.04f), Vector3(0, 0.23f, 0.28f), crate);
    t.box(Vector3(0.6f, 0.38f, 0.04f), Vector3(0, 0.23f, -0.28f), crate);
    t.box(Vector3(0.04f, 0.38f, 0.52f), Vector3(0.28f, 0.23f, 0), crate);
    t.box(Vector3(0.04f, 0.38f, 0.52f), Vector3(-0.28f, 0.23f, 0), crate);
    std::vector<Entity> boxes;
    boxes.push_back(t.box(Vector3(0.14f, 0.14f, 0.14f), Vector3(-0.1f, 1.75f, -0.08f), props));
    boxes.push_back(t.box(Vector3(0.12f, 0.12f, 0.12f), Vector3(0.09f, 2.0f, 0.06f), props));
    boxes.push_back(t.box(Vector3(0.16f, 0.1f, 0.12f), Vector3(0.0f, 2.3f, 0.1f), props));
    t.setRotation(boxes[0], axisAngle(Vector3(0, 1, 0), 25.0f * DEG) * axisAngle(Vector3(1, 0, 0), 12.0f * DEG));
    t.setRotation(boxes[1], axisAngle(Vector3(0, 1, 0), 40.0f * DEG) * axisAngle(Vector3(0, 0, 1), 15.0f * DEG));
    t.setRotation(boxes[2], axisAngle(Vector3(1, 0, 0), 30.0f * DEG) * axisAngle(Vector3(0, 0, 1), -10.0f * DEG));
    t.resolve();

    PlacementWorldSettings settings;
    settings.pushNeighbours = true;
    PlacementWorld world(&t.scene, settings);
    for (Entity e : boxes) ASSERT_TRUE(world.addMover(e));
    AABB region = world.getMoversBounds();
    region = AABB(region.getMinimum() - Vector3(0.5f, 0.5f, 0.5f), region.getMaximum() + Vector3(0.5f, 0.5f, 0.5f));
    ASSERT_GT(region.getMinimum().y, 1.0f);   // the floor and the table's legs are outside
    settings.killY = region.getMinimum().y - 50.0f;
    world.setSettings(settings);
    world.ensureContext(region);

    std::vector<Entity> neighbours = world.getNeighbours();
    std::sort(neighbours.begin(), neighbours.end());
    EXPECT_EQ(neighbours, (std::vector<Entity>{table, crate}));
    EXPECT_LT(world.getContextRegion().getMinimum().y, 0.0f);   // grown down to the floor
    PlacementRayHit hit;
    ASSERT_TRUE(world.raycast(Ray(Vector3(3, 5, 3), Vector3(0, -10, 0)), hit));
    EXPECT_EQ(hit.entity, floor);
    const AABB dynamic = world.getDynamicBounds();
    EXPECT_LT(dynamic.getMinimum().y, 0.01f);   // the table's feet
    EXPECT_GT(dynamic.getMaximum().y, 2.3f);    // the highest box

    ASSERT_GT(stepUntilAsleep(world, 1500), 0);
    for (const PlacementBodyState& state : world.getStates()) EXPECT_FALSE(state.fellOut) << state.entity;
    const PlacementBodyState tableState = world.getState(table);
    EXPECT_LT(tableState.pose.position.length(), 0.02f);
    EXPECT_EQ(tableState.restingOn, std::vector<Entity>{floor});
    const PlacementBodyState crateState = world.getState(crate);
    EXPECT_NEAR(crateState.pose.position.y, 1.0f, 0.02f);
    EXPECT_EQ(crateState.restingOn, std::vector<Entity>{table});
    for (Entity e : boxes) {
        const Vector3 p = world.getState(e).pose.position - crateState.pose.position;
        EXPECT_LT(std::fabs(p.x), 0.26f) << e;
        EXPECT_LT(std::fabs(p.z), 0.26f) << e;
        EXPECT_GT(p.y, 0.04f) << e;
        EXPECT_LT(p.y, 0.42f) << e;
    }
}

TEST(PlacementWorldContext, StaticRigidbodiesAreNeverNeighbours) {
    TestScene t;
    floorBox(t);
    const Entity post = t.box(Vector3(0.3f, 0.3f, 0.3f), Vector3(1, 0.15f, 0));
    Collider3D& c = t.collider(post, Collider3DType::BOX, BodyType::STATIC);
    c.width = 0.3f; c.height = 0.3f; c.depth = 0.3f;
    const Entity ball = t.sphere(0.2f, Vector3(-1, 0.2f, 0));
    t.collider(ball, Collider3DType::SPHERE, BodyType::DYNAMIC).radius = 0.2f;
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 3, 0));
    t.resolve();

    PlacementWorldSettings settings;
    settings.pushNeighbours = true;
    PlacementWorld world(&t.scene, settings);
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getNeighbours(), std::vector<Entity>{ball});
    EXPECT_EQ(world.getState(ball).shape, PlacementShapeKind::AUTHORED);

    // a dynamic Rigidbody over the size limit stays environment
    const Entity platform = t.box(Vector3(30, 0.5f, 4), Vector3(0, 6, 0));
    Collider3D& deck = t.collider(platform, Collider3DType::BOX, BodyType::DYNAMIC);
    deck.width = 30; deck.height = 0.5f; deck.depth = 4;
    t.resolve();
    const size_t before = world.getContextBodyCount();
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getNeighbours(), std::vector<Entity>{ball});
    EXPECT_EQ(world.getContextBodyCount(), before + 1);
}

TEST(PlacementWorldContext, KillYFreezesFallenBodies) {
    TestScene t;
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    t.resolve();

    PlacementWorldSettings settings;
    settings.killY = -5.0f;
    PlacementWorld world(&t.scene, settings);
    ASSERT_TRUE(world.addMover(crate));
    EXPECT_FALSE(world.allAsleep());
    int steps = 0;
    while (!world.getState(crate).fellOut && steps < 600) {
        world.step();
        steps++;
    }
    PlacementBodyState state = world.getState(crate);
    ASSERT_TRUE(state.fellOut);
    EXPECT_LT(state.pose.position.y, -5.0f);
    EXPECT_GT(state.pose.position.y, -5.5f);
    EXPECT_FALSE(state.asleep);
    EXPECT_TRUE(world.allAsleep());
    const float frozen = state.pose.position.y;
    for (int i = 0; i < 10; i++) world.step();
    EXPECT_EQ(world.getState(crate).pose.position.y, frozen);

    // teleported back, it simulates again
    world.teleport(crate, {Vector3(0, 0, 0), Quaternion()});
    EXPECT_FALSE(world.getState(crate).fellOut);
    world.step();
    EXPECT_LT(world.getState(crate).pose.position.y, 0.0f);
}

TEST(PlacementWorldContext, MeshesAboveTheLimitAreSimplified) {
    auto measure = [](float simplifyAbove, size_t& segments, float& top) {
        TestScene t;
        Primitive& rock = t.primitive(Vector3(0, 0, 0));
        rock.createSphere(1.0f, 96, 48);
        t.resolve();
        PlacementWorldSettings settings;
        settings.meshSimplifyAbove = simplifyAbove;
        PlacementWorld world(&t.scene, settings);
        world.ensureContext(everywhere);
        std::vector<Vector3> lines;
        world.collectShapeLines(lines, false, 1000000);
        segments = lines.size() / 2;
        PlacementRayHit hit;
        top = world.raycast(Ray(Vector3(0.1f, 5, 0.1f), Vector3(0, -10, 0)), hit) ? hit.point.y : -1.0f;
    };
    size_t full = 0, simplified = 0;
    float fullTop = 0, simplifiedTop = 0;
    measure(20000, full, fullTop);
    measure(500, simplified, simplifiedTop);
    EXPECT_GT(full, 3 * simplified);
    EXPECT_GT(simplified, 0u);
    EXPECT_NEAR(fullTop, 0.99f, 0.01f);
    EXPECT_NEAR(simplifiedTop, 0.99f, 0.03f);
}

TEST(PlacementWorldContext, InstancedMeshesAreBoxes) {
    TestScene t;
    const Entity field = t.box(Vector3(1, 1, 1), Vector3(0, 0, 0));
    InstancedMeshComponent instanced;
    InstanceData a, b, hidden;
    a.position = Vector3(-2, 0, 0);
    b.position = Vector3(2, 0, 0);
    b.scale = Vector3(1, 2, 1);
    hidden.position = Vector3(0, 0, 3);
    hidden.visible = false;
    instanced.instances = {a, b, hidden};
    t.scene.addComponent<InstancedMeshComponent>(field, instanced);
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getContextBodyCount(), 1u);
    PlacementRayHit hit;
    ASSERT_TRUE(world.raycast(Ray(Vector3(2, 5, 0), Vector3(0, -10, 0)), hit));
    EXPECT_EQ(hit.entity, field);
    EXPECT_NEAR(hit.point.y, 1.0f, 1e-3f);
    ASSERT_TRUE(world.raycast(Ray(Vector3(-2, 5, 0), Vector3(0, -10, 0)), hit));
    EXPECT_NEAR(hit.point.y, 0.5f, 1e-3f);
    EXPECT_FALSE(world.raycast(Ray(Vector3(0, 5, 0), Vector3(0, -10, 0)), hit));
    EXPECT_FALSE(world.raycast(Ray(Vector3(0, 5, 3), Vector3(0, -10, 0)), hit));

    // one instance, turned
    TestScene single;
    const Entity one = single.box(Vector3(2, 1, 1), Vector3(0, 0, 0));
    InstancedMeshComponent lone;
    InstanceData turned;
    turned.rotation = axisAngle(Vector3(0, 1, 0), 90.0f * DEG);
    lone.instances = {turned};
    single.scene.addComponent<InstancedMeshComponent>(one, lone);
    single.resolve();
    PlacementWorld singleWorld(&single.scene, PlacementWorldSettings());
    singleWorld.ensureContext(everywhere);
    EXPECT_TRUE(singleWorld.raycast(Ray(Vector3(0, 5, 0.9f), Vector3(0, -10, 0)), hit));
    EXPECT_FALSE(singleWorld.raycast(Ray(Vector3(0.9f, 5, 0), Vector3(0, -10, 0)), hit));
}

TEST(PlacementWorldContext, TerrainsWithoutHeights) {
    TestScene t;
    // no heightmap: flat at the terrain's height
    const Entity flat = t.entity(Vector3(0, 1.5f, 0));
    TerrainComponent plain;
    plain.terrainSize = 20.0f;
    t.scene.addComponent<TerrainComponent>(flat, plain);
    // a heightmap that fails to load (no such file) is flat too, as the terrain picking reads it
    const Entity missing = t.entity(Vector3(40, 0, 0));
    TerrainComponent unreadable;
    unreadable.terrainSize = 20.0f;
    unreadable.heightMap.setPath("no-such-heightmap.png");
    unreadable.needUpdateTerrain = false;
    t.scene.addComponent<TerrainComponent>(missing, unreadable);
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    // the heightmap loads on a worker: the terrain joins the context once that has failed
    world.ensureContext(everywhere);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (world.getContextBodyCount() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        world.ensureContext(everywhere);
    }
    EXPECT_EQ(world.getContextBodyCount(), 2u);
    PlacementRayHit hit;
    ASSERT_TRUE(world.raycast(Ray(Vector3(3, 5, -4), Vector3(0, -10, 0)), hit));
    EXPECT_EQ(hit.entity, flat);
    EXPECT_NEAR(hit.point.y, 1.5f, 1e-4f);
    ASSERT_TRUE(world.raycast(Ray(Vector3(40, 5, 0), Vector3(0, -10, 0)), hit));
    EXPECT_EQ(hit.entity, missing);
    EXPECT_NEAR(hit.point.y, 0.0f, 1e-4f);
    // nothing for an empty region
    world.ensureContext(AABB());
    EXPECT_EQ(world.getContextBodyCount(), 2u);
}

// ---- read-back ------------------------------------------------------------------------------

TEST(PlacementWorldReadBack, RaycastHitsTrianglesAndHonoursIgnore) {
    TestScene t;
    const Entity floor = floorBox(t);
    const Entity rock = t.sphere(1.0f, Vector3(5, 0, 0));
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 2, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);

    PlacementRayHit hit;
    ASSERT_TRUE(world.raycast(Ray(Vector3(0, 5, 0), Vector3(0, -10, 0)), hit));
    EXPECT_EQ(hit.entity, crate);
    EXPECT_NEAR(hit.point.y, 2.5f, 1e-3f);
    EXPECT_NEAR(hit.distance, 2.5f, 1e-3f);

    ASSERT_TRUE(world.raycast(Ray(Vector3(0, 5, 0), Vector3(0, -10, 0)), hit, {crate}));
    EXPECT_EQ(hit.entity, floor);
    EXPECT_NEAR(hit.point.y, 0.0f, 1e-4f);
    EXPECT_NEAR(hit.normal.y, 1.0f, 1e-4f);

    EXPECT_FALSE(world.raycast(Ray(Vector3(0, 5, 0), Vector3(0, -10, 0)), hit, {crate, floor}));
    // too short to reach
    EXPECT_FALSE(world.raycast(Ray(Vector3(0, 5, 0), Vector3(0, -1, 0)), hit));

    // the sphere's triangles, not its bounding box: a ray past its silhouette's corner misses
    EXPECT_FALSE(world.raycast(Ray(Vector3(5.85f, 5, 0.85f), Vector3(0, -4.5f, 0)), hit));
    ASSERT_TRUE(world.raycast(Ray(Vector3(5.3f, 5, 0.2f), Vector3(0, -10, 0)), hit));
    EXPECT_EQ(hit.entity, rock);
    EXPECT_NEAR((hit.point - Vector3(5, 0, 0)).length(), 1.0f, 0.01f);
}

TEST(PlacementWorldReadBack, ShapeLines) {
    TestScene t;
    floorBox(t);
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 2, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);

    std::vector<Vector3> all, movers, capped;
    world.collectShapeLines(all, false, 100000);
    world.collectShapeLines(movers, true, 100000);
    world.collectShapeLines(capped, false, 5);
    EXPECT_FALSE(movers.empty());
    EXPECT_GT(all.size(), movers.size());
    EXPECT_EQ(all.size() % 2, 0u);
    EXPECT_EQ(capped.size(), 10u);
    // the mover's lines lie on its box
    for (const Vector3& p : movers) {
        EXPECT_LE(std::fabs(p.x), 0.5f + 1e-4f);
        EXPECT_NEAR(std::max(std::fabs(p.x), std::max(std::fabs(p.y - 2.0f), std::fabs(p.z))), 0.5f, 1e-4f);
    }
    const AABB bounds = world.getMoversBounds();
    EXPECT_NEAR(bounds.getMinimum().y, 1.5f, 1e-3f);
    EXPECT_NEAR(bounds.getMaximum().y, 2.5f, 1e-3f);
}

TEST(PlacementWorldReadBack, SameCallsSameHash) {
    auto run = [](float nudge) {
        TestScene t;
        floorBox(t);
        std::vector<Entity> crates;
        for (int i = 0; i < 6; i++) {
            crates.push_back(t.box(Vector3(0.6f, 0.4f, 0.5f), Vector3(0.3f * (i % 3) + (i == 2 ? nudge : 0.0f), 0.5f + 0.55f * i, 0.2f * (i % 2))));
            t.setRotation(crates.back(), axisAngle(Vector3(0.3f, 1, 0.2f), 0.4f * i));
        }
        crates.push_back(t.sphere(0.3f, Vector3(0.1f, 4.5f, 0.1f)));
        t.resolve();
        PlacementWorld world(&t.scene, PlacementWorldSettings());
        for (Entity e : crates) EXPECT_TRUE(world.addMover(e));
        world.ensureContext(everywhere);
        world.setTarget(crates[0], {Vector3(0.5f, 0.3f, 0.4f), Quaternion()});
        for (int i = 0; i < 300; i++) world.step();
        return world.stateHash();
    };
    const uint64_t first = run(0.0f);
    const uint64_t second = run(0.0f);
    EXPECT_EQ(first, second);
    EXPECT_NE(first, run(0.01f));
}

#endif

// ---- review fixes ---------------------------------------------------------------------------

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    // A plank 2 x 0.2 x 0.2 turned 90 degrees about Y under a parent scaled (4, 1, 1): the
    // renderer's modelMatrix makes it 0.8 m along x and 2 m along z (not 8 m by 0.2 m)
    Entity stretchedPlank(TestScene& t, const Vector3& at) {
        const Entity parent = t.entity(at);
        t.setScale(parent, Vector3(4, 1, 1));
        const Entity plank = t.box(Vector3(2, 0.2f, 0.2f), Vector3(0, 0, 0), parent);
        t.setRotation(plank, axisAngle(Vector3(0, 1, 0), 90.0f * DEG));
        return parent;
    }

}

TEST(PlacementWorldReview, NonUniformParentScaleWithARotatedChild) {
    TestScene t;
    const Entity mover = stretchedPlank(t, Vector3(0, 3, 0));
    const Entity context = stretchedPlank(t, Vector3(10, 0, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(mover));
    const Vector3 size = world.getMoversBounds().getSize();
    EXPECT_NEAR(size.x, 0.8f, 1e-3f);
    EXPECT_NEAR(size.y, 0.2f, 1e-3f);
    EXPECT_NEAR(size.z, 2.0f, 1e-3f);

    world.ensureContext(everywhere);
    PlacementRayHit hit;
    EXPECT_TRUE(world.raycast(Ray(Vector3(10.35f, 5, 0.9f), Vector3(0, -10, 0)), hit));
    EXPECT_NEAR(hit.point.y, 0.1f, 1e-3f);
    EXPECT_FALSE(world.raycast(Ray(Vector3(10.0f, 5, 3.0f), Vector3(0, -10, 0)), hit));
    (void)context;

    // instanced boxes follow the same map
    TestScene field;
    const Entity parent = field.entity(Vector3(0, 0, 0));
    field.setScale(parent, Vector3(4, 1, 1));
    const Entity instancedEntity = field.box(Vector3(2, 0.2f, 0.2f), Vector3(0, 0, 0), parent);
    InstancedMeshComponent instanced;
    InstanceData turned;
    turned.rotation = axisAngle(Vector3(0, 1, 0), 90.0f * DEG);
    instanced.instances = {turned};
    field.scene.addComponent<InstancedMeshComponent>(instancedEntity, instanced);
    field.resolve();
    PlacementWorld fieldWorld(&field.scene, PlacementWorldSettings());
    fieldWorld.ensureContext(everywhere);
    EXPECT_TRUE(fieldWorld.raycast(Ray(Vector3(0.35f, 5, 0.9f), Vector3(0, -10, 0)), hit));
    EXPECT_FALSE(fieldWorld.raycast(Ray(Vector3(0.0f, 5, 3.0f), Vector3(0, -10, 0)), hit));
}

TEST(PlacementWorldReview, MirroredContextStillHolds) {
    TestScene t;
    const Entity floor = floorBox(t);
    t.setScale(floor, Vector3(-1, 1, 1));
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0.5f, 1.5f, 0.3f));
    // a terrain mirrored along x: its high side goes from +x to -x
    const Entity terrain = t.terrain(33, 20.0f, 4.0f, Vector3(40, 0, 0), [](float u, float) { return 0.2f + 0.6f * u; });
    t.setScale(terrain, Vector3(-1, 1, 1));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    ASSERT_GT(stepUntilAsleep(world, 1200), 0);
    EXPECT_NEAR(world.getState(crate).pose.position.y, 0.5f, 1e-3f);

    TerrainComponent& component = t.scene.getComponent<TerrainComponent>(terrain);
    for (const float worldX : {35.0f, 44.0f}) {
        float height = 0.0f;
        Vector3 normal;
        t.scene.getSystem<MeshSystem>()->sampleTerrainSurface(component, (worldX - 40.0f) / -1.0f, 1.0f, height, normal);
        PlacementRayHit hit;
        ASSERT_TRUE(world.raycast(Ray(Vector3(worldX, 10, 1.0f), Vector3(0, -20, 0)), hit));
        EXPECT_NEAR(hit.point.y, height, 2e-3f) << worldX;
    }
}

TEST(PlacementWorldReview, DeletedEntitiesAreHarmless) {
    TestScene t;
    floorBox(t);
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0.5f, 0));
    const Entity pebble = t.box(Vector3(0.3f, 0.3f, 0.3f), Vector3(1.5f, 0.15f, 0));
    t.resolve();

    PlacementWorldSettings settings;
    settings.pushNeighbours = true;
    PlacementWorld world(&t.scene, settings);
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    ASSERT_EQ(world.getNeighbours(), std::vector<Entity>{pebble});
    world.setTarget(crate, {Vector3(0, 1, 0), Quaternion()});
    world.step();

    t.scene.destroyEntity(crate);
    t.scene.destroyEntity(pebble);
    EXPECT_NO_THROW({
        world.refreshShapes();
        world.getState(crate);
        world.getStates();
        world.teleport(crate, {Vector3(0, 2, 0), Quaternion()});
        world.setTarget(crate, {Vector3(0, 2, 0), Quaternion()});
        world.hasTarget(crate);
        world.clearTarget(crate);
        world.step();
        world.stateHash();
        world.getMoversBounds();
        world.getDynamicBounds();
        std::vector<Vector3> lines;
        world.collectShapeLines(lines, false, 1000);
        world.ensureContext(everywhere);
        world.addContextEntity(crate);
        PlacementRayHit hit;
        world.raycast(Ray(Vector3(0, 5, 0), Vector3(0, -10, 0)), hit, {crate, pebble});
        std::string why;
        EXPECT_FALSE(world.addMover(crate, &why));
        world.removeMover(crate);
        world.stopAll();
        world.wakeAll();
        world.step();
    });
    // refreshShapes dropped the gone bodies
    EXPECT_TRUE(world.getMovers().empty());
    EXPECT_TRUE(world.getNeighbours().empty());
}

TEST(PlacementWorldReview, NeighboursNeverNest) {
    TestScene t;
    floorBox(t);
    const Entity table = t.box(Vector3(1.2f, 0.1f, 0.8f), Vector3(0, 0.75f, 0));
    const Entity vase = t.box(Vector3(0.2f, 0.4f, 0.2f), Vector3(-0.3f, 0.25f, 0), table);
    const Entity cup = t.box(Vector3(0.1f, 0.12f, 0.1f), Vector3(0.3f, 0.11f, 0), table);
    t.resolve();

    PlacementWorldSettings settings;
    settings.pushNeighbours = true;
    PlacementWorld world(&t.scene, settings);
    ASSERT_TRUE(world.addMover(vase));
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getNeighbours(), std::vector<Entity>{cup});
    world.removeMover(vase);
    world.ensureContext(everywhere);
    // the table (with its vase and cup) is one neighbour now, the cup no longer one of its own
    EXPECT_EQ(world.getNeighbours(), std::vector<Entity>{table});
    EXPECT_EQ(world.getStates().size(), 1u);
}

TEST(PlacementWorldReview, RefreshShapesKeepsWhatRestsWhere) {
    TestScene t;
    const Entity floor = floorBox(t);
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 0.5f, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    ASSERT_GT(stepUntilAsleep(world, 600), 0);
    for (int i = 0; i < 60; i++) world.step();
    ASSERT_EQ(world.getState(crate).restingOn, std::vector<Entity>{floor});
    world.refreshShapes();
    for (int i = 0; i < 10; i++) world.step();
    EXPECT_EQ(world.getState(crate).restingOn, std::vector<Entity>{floor});
    EXPECT_NEAR(world.getState(crate).pose.position.y, 0.5f, 1e-3f);
}

// Under gravity, however weak, a body touching nothing never counts as asleep, nor is let sleep
// by Jolt: it falls until it rests on the floor. Without gravity it may rest in the air.
TEST(PlacementWorldReview, NothingSleepsInMidAirUnderGravity) {
    for (const float g : {-0.3f, -9.81f}) {
        TestScene t;
        const Entity floor = floorBox(t);
        const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 5, 0));
        t.resolve();
        PlacementWorld world(&t.scene, PlacementWorldSettings());
        world.setGravity(Vector3(0, g, 0));
        ASSERT_TRUE(world.addMover(crate));
        world.ensureContext(everywhere);
        int steps = 0;
        for (; steps < 6000 && !world.allAsleep(); steps++) {
            world.step();
            const PlacementBodyState state = world.getState(crate);
            if (state.pose.position.y > 0.51f) {
                ASSERT_FALSE(state.asleep) << "g " << g << " step " << steps << " y " << state.pose.position.y;
            }
        }
        ASSERT_TRUE(world.allAsleep()) << g;
        const PlacementBodyState state = world.getState(crate);
        EXPECT_NEAR(state.pose.position.y, 0.5f, 1e-3f) << g;
        EXPECT_EQ(state.restingOn, std::vector<Entity>{floor}) << g;
    }

    // weightless, a still body sleeps where it is
    TestScene t;
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 5, 0));
    t.resolve();
    PlacementWorld world(&t.scene, PlacementWorldSettings());
    world.setGravity(Vector3(0, 0, 0));
    ASSERT_TRUE(world.addMover(crate));
    ASSERT_GT(stepUntilAsleep(world, 120), 0);
    EXPECT_NEAR(world.getState(crate).pose.position.y, 5.0f, 1e-5f);
}

// Simplified context strays at most 1 cm from the surface however large the mesh (it was 1% of
// the mesh's size, 40 cm on a 40 m rock)
TEST(PlacementWorldReview, SimplificationErrorIsAbsolute) {
    auto heights = [](float simplifyAbove) {
        TestScene t;
        Primitive& rock = t.primitive(Vector3(0, 0, 0));
        rock.createSphere(1.0f, 96, 48);
        t.setScale(rock.getEntity(), Vector3(20, 20, 20));
        t.resolve();
        PlacementWorldSettings settings;
        settings.meshSimplifyAbove = simplifyAbove;
        PlacementWorld world(&t.scene, settings);
        world.ensureContext(AABB(-30, -30, -30, 30, 30, 30));
        std::vector<float> out;
        for (int i = 0; i < 20; i++) {
            PlacementRayHit hit;
            const float x = -12.0f + 1.2f * i, z = 9.0f - 0.9f * i;
            out.push_back(world.raycast(Ray(Vector3(x, 40, z), Vector3(0, -80, 0)), hit) ? hit.point.y : -1.0f);
        }
        return out;
    };
    const std::vector<float> full = heights(1e9f);
    const std::vector<float> simplified = heights(500);
    for (size_t i = 0; i < full.size(); i++) EXPECT_NEAR(simplified[i], full[i], 0.012f) << i;
}

// The fixture's pencil (Plates.scene): a 2 cm, 12-sided cylinder scaled 0.35 across, so 7 mm
// thick, lying on a 20 cm slab. Jolt's default 1 mm hull tolerance cut its section lopsided
// (30% of its volume and its centre of mass 2 mm off), and it sank through the slab.
TEST(PlacementWorldReview, ThinScaledCylinderRestsOnTheSlab) {
    TestScene t;
    const Entity slab = t.box(Vector3(1, 1, 1), Vector3(0, -0.1f, 0));
    t.setScale(slab, Vector3(12, 0.2f, 12));
    const Entity group = t.entity(Vector3(0, 0, 0));
    Primitive& pencil = t.primitive(Vector3(0.6f, 0.3f, -0.2f), group);
    pencil.createCylinder(0.01f, 0.18f, 12, 18);
    t.setScale(pencil.getEntity(), Vector3(0.35f, 1, 0.35f));
    t.setRotation(pencil.getEntity(), Quaternion(0.70710677f, 0, 0, 0.70710677f));
    t.resolve();

    PlacementWorldSettings settings;
    PlacementWorld world(&t.scene, settings);
    ASSERT_TRUE(world.addMover(pencil.getEntity()));
    AABB region = world.getMoversBounds();
    region = AABB(region.getMinimum() - Vector3(2, 2, 2), region.getMaximum() + Vector3(2, 2, 2));
    settings.killY = region.getMinimum().y - 50.0f;
    world.setSettings(settings);
    world.ensureContext(region);
    ASSERT_GT(stepUntilAsleep(world, 1200), 0);
    const PlacementBodyState state = world.getState(pencil.getEntity());
    EXPECT_FALSE(state.fellOut);
    EXPECT_NEAR(state.pose.position.y, 0.0035f, 1e-3f);
    EXPECT_NEAR(world.getMoversBounds().getMinimum().y, 0.0f, 1e-3f);
    EXPECT_EQ(state.restingOn, std::vector<Entity>{slab});
}

// The fixture's six 12 mm plates (Plates.scene), dropped tilted into a pile: with the Balanced
// preset's 8 / 2 solver iterations the pile sank 6 mm into itself; thin bodies get more
TEST(PlacementWorldReview, ThinPlatesPileWithoutSinking) {
    TestScene t;
    const Entity slab = t.box(Vector3(1, 1, 1), Vector3(0, -0.1f, 0));
    t.setScale(slab, Vector3(12, 0.2f, 12));
    const Entity group = t.entity(Vector3(0, 0, 0));
    const float poses[6][7] = {{-0.05f, 0.4f, 0, 1, 0, 0, 0},
                               {0, 0.65f, 0.04f, 0.96186835f, 0.07857748f, 0.25500378f, -0.06006758f},
                               {0.05f, 0.9f, 0, 0.78045887f, -0.13537884f, 0.5958951f, 0.13215727f},
                               {-0.05f, 1.15f, 0.04f, 0.9811203f, 0.17068131f, 0.09033051f, 0.010551917f},
                               {0, 1.4f, 0, 0.49928352f, 0.11380611f, 0.8579609f, -0.04083237f},
                               {0.05f, 1.65f, 0.04f, 0.17505363f, -0.20864053f, 0.9621182f, 0.012403287f}};
    std::vector<Entity> plates;
    for (const auto& p : poses) {
        plates.push_back(t.box(Vector3(1, 1, 1), Vector3(p[0], p[1], p[2]), group));
        t.setScale(plates.back(), Vector3(0.5f, 0.012f, 0.35f));
        t.setRotation(plates.back(), Quaternion(p[3], p[4], p[5], p[6]));
    }
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    for (Entity e : plates) ASSERT_TRUE(world.addMover(e));
    world.ensureContext(everywhere);
    ASSERT_GT(stepUntilAsleep(world, 1200), 0);
    EXPECT_LT(world.getMaxPenetration(), 1e-3f);
    for (const PlacementBodyState& state : world.getStates()) {
        EXPECT_LT(state.maxPenetration, 1e-3f) << state.entity;
        // none sunk into the slab past the penetration bound: 6 mm half thickness less 1 mm
        // (Jolt's NEON solver on arm64 settles one 0.63 mm deep, x86-64's under 0.5 mm)
        EXPECT_GT(state.pose.position.y, 0.006f - 1e-3f) << state.entity;
    }
    (void)slab;
}

// Rolling resistance: a ball comes to rest on a 5 degree slope, and still rolls down 30 degrees
TEST(PlacementWorldReview, BallsStopOnGentleSlopesAndRollDownSteepOnes) {
    for (const float degrees : {5.0f, 30.0f}) {
        TestScene t;
        const Entity slope = t.box(Vector3(12, 0.2f, 4), Vector3(0, 0, 0));
        t.setRotation(slope, axisAngle(Vector3(0, 0, 1), degrees * DEG));
        // just above the slope's surface, 2 m up its length
        const float angle = degrees * DEG;
        const Vector3 normal(-std::sin(angle), std::cos(angle), 0);
        const Vector3 along(std::cos(angle), std::sin(angle), 0);
        const Entity ball = t.sphere(0.2f, along * 2.0f + normal * (0.1f + 0.21f));
        t.resolve();

        PlacementWorld world(&t.scene, PlacementWorldSettings());
        ASSERT_TRUE(world.addMover(ball));
        world.ensureContext(everywhere);
        const Vector3 start = world.getState(ball).pose.position;
        if (degrees < 10.0f) {
            ASSERT_GT(stepUntilAsleep(world, 1200), 0) << degrees;
            const PlacementBodyState state = world.getState(ball);
            EXPECT_EQ(state.restingOn, std::vector<Entity>{slope});
            EXPECT_LT((state.pose.position - start).length(), 0.3f);
        } else {
            for (int i = 0; i < 180; i++) world.step();
            const PlacementBodyState state = world.getState(ball);
            EXPECT_FALSE(state.asleep);
            // g (sin 30 - 0.2) / 1.4 = 2.1 m/s^2 down the slope: over 2 m in 1.5 s
            EXPECT_LT(state.pose.position.x, start.x - 1.7f);
            EXPECT_GT(state.angularVelocity.length(), 10.0f);   // rolling, not sliding
        }
    }
}

// A world made before any Scene, so before anything else set Jolt up, in a process of its own
// (jasperengine-placement-fresh-process, built next to this binary): this one can't, as the
// editor's static App builds a Scene before main
TEST(PlacementWorldReview, WorldBeforeAnySceneInAFreshProcess) {
    // (FileUtils: /proc/self/exe on Linux, _NSGetExecutablePath on macOS)
    const std::filesystem::path self = editor::FileUtils::getExecutablePath();
    ASSERT_FALSE(self.empty());
    const std::string program = (self.parent_path() / "jasperengine-placement-fresh-process").string();
    const int status = std::system((program + " > /dev/null 2>&1").c_str());
    ASSERT_TRUE(WIFEXITED(status)) << "it crashed";
    EXPECT_EQ(WEXITSTATUS(status), 0) << "2: Jolt was set up before main there too, nothing was checked";
}

TEST(PlacementWorldReview, OverflowingContactBuffersAreReported) {
    TestScene t;
    std::vector<Entity> pile;
    for (int i = 0; i < 300; i++) pile.push_back(t.box(Vector3(0.2f, 0.2f, 0.2f), Vector3(0.0001f * i, 0, 0)));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    world.setGravity(Vector3(0, 0, 0));
    for (Entity e : pile) ASSERT_TRUE(world.addMover(e));
    EXPECT_FALSE(world.contactsOverflowed());
    world.step();
    EXPECT_TRUE(world.contactsOverflowed());
    world.clearFlags();
    EXPECT_FALSE(world.contactsOverflowed());
}

TEST(PlacementWorldReview, CancelledDecompositionsAreAskedForAgain) {
    if (!ConvexDecompositionCache::available()) GTEST_SKIP() << "no convex decomposition in this build";
    TestScene t;
    const Entity ring = t.torus(0.5f, 0.15f, Vector3(0, 0, 0));
    t.resolve();

    ConvexDecompositionCache cache("");
    PlacementWorldSettings settings;
    settings.decompositionCache = &cache;
    {
        PlacementWorld world(&t.scene, settings);
        ASSERT_TRUE(world.addMover(ring));
        ASSERT_TRUE(world.hasPendingDecompositions());
        cache.cancelAll();
        EXPECT_FALSE(world.hasPendingDecompositions());
    }
    PlacementWorld again(&t.scene, settings);
    ASSERT_TRUE(again.addMover(ring));
    EXPECT_TRUE(again.hasPendingDecompositions());
    EXPECT_TRUE(again.getState(ring).decompositionPending);
    cache.cancelAll();
}

TEST(PlacementWorldReview, BodyLimitLeavesTheRestForLater) {
    TestScene t;
    for (int i = 0; i < 4096; i++) t.box(Vector3(0.5f, 0.5f, 0.5f), Vector3(float(i % 64), 0, float(i / 64)));
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 5, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(crate));
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getContextBodyCount(), 4095u);
    PlacementRayHit hit;
    // the last box (63, 0, 63) found no room
    EXPECT_FALSE(world.raycast(Ray(Vector3(63, 5, 63), Vector3(0, -10, 0)), hit, {crate}));
    world.removeMover(crate);
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getContextBodyCount(), 4096u);
    EXPECT_TRUE(world.raycast(Ray(Vector3(63, 5, 63), Vector3(0, -10, 0)), hit));
    // Once that last context body fills the slot, a new mover is refused too.
    std::string why;
    EXPECT_FALSE(world.addMover(crate, &why));
    EXPECT_EQ(why, "too many bodies in the placement world");
}

TEST(PlacementWorldReview, LargeTerrainsAreResampled) {
    TestScene t;
    const Entity terrain = t.terrain(1027, 400.0f, 20.0f, Vector3(0, 0, 0), [](float u, float v) {
        return 0.5f + 0.2f * std::sin(6.0f * u) * std::cos(5.0f * v);
    }, true);
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    const auto begin = std::chrono::steady_clock::now();
    world.ensureContext(everywhere);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("[placement] oversized terrain context in %.2f s\n", seconds);
    EXPECT_EQ(world.getContextBodyCount(), 1u);
    TerrainComponent& component = t.scene.getComponent<TerrainComponent>(terrain);
    // The first odd-sized map over the cap is enough to exercise resampling to 1025.
    // A small heightmap still keeps one sample per texel.
    EXPECT_EQ(JoltShapeBuilder::defaultHeightFieldSamples(component), 1025u);
    const Entity small = t.terrain(33, 10.0f, 1.0f, Vector3(500, 0, 0), [](float, float) { return 0.5f; });
    EXPECT_EQ(JoltShapeBuilder::defaultHeightFieldSamples(t.scene.getComponent<TerrainComponent>(small)), 33u);
    for (int i = 0; i < 20; i++) {
        const float x = -190.0f + 19.0f * i, z = 150.0f - 15.0f * i;
        float height = 0.0f;
        Vector3 normal;
        t.scene.getSystem<MeshSystem>()->sampleTerrainSurface(component, x, z, height, normal);
        PlacementRayHit hit;
        ASSERT_TRUE(world.raycast(Ray(Vector3(x, 50, z), Vector3(0, -100, 0)), hit));
        EXPECT_NEAR(hit.point.y, height, 0.02f) << x << ", " << z;
    }
}

#endif
