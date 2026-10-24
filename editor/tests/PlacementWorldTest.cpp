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
#include "physics/ConvexDecomposition.h"
#include "physics/PlacementWorld.h"
#include "subsystem/MeshSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    for (int i = 0; i < 360; i++) {
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
        rawMesh.vertices[rawMesh.numVertices++] = Vector3((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
    }
    const uint16_t faces[36] = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    for (uint16_t i : faces) rawMesh.indices[rawMesh.numIndices++] = i;
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
    world.refreshShapes();
    state = world.getState(ring);
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

TEST(PlacementWorldColliders, RefreshShapesKeepsPoseAndVelocity) {
    TestScene t;
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 3, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    ASSERT_TRUE(world.addMover(crate));
    for (int i = 0; i < 30; i++) world.step();
    const PlacementBodyState before = world.getState(crate);
    world.refreshShapes();
    const PlacementBodyState after = world.getState(crate);
    EXPECT_NEAR((after.pose.position - before.pose.position).length(), 0.0f, 1e-6f);
    EXPECT_NEAR((after.linearVelocity - before.linearVelocity).length(), 0.0f, 1e-6f);
    EXPECT_EQ(after.shape, PlacementShapeKind::BOX);
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

TEST(PlacementWorldContext, BodyLimit) {
    TestScene t;
    for (int i = 0; i < 4096; i++) t.box(Vector3(0.5f, 0.5f, 0.5f), Vector3(float(i % 64), 0, float(i / 64)));
    const Entity crate = t.box(Vector3(1, 1, 1), Vector3(0, 5, 0));
    t.resolve();

    PlacementWorld world(&t.scene, PlacementWorldSettings());
    world.ensureContext(everywhere);
    EXPECT_EQ(world.getContextBodyCount(), 4096u);
    std::string why;
    EXPECT_FALSE(world.addMover(crate, &why));
    EXPECT_EQ(why, "too many bodies in the placement world");
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
