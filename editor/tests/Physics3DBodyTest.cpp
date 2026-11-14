// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// 3D Rigidbody fields and the body pipeline (physics-3d spec T2-T5, T7, T12, T14-T16): damping,
// mass / centre of mass / inertia, type switching, per-collider triggers, collider edits as shape
// swaps, the hierarchy-ordered write-back of nested bodies, per-body interpolation (3D and 2D),
// the world capacity (and its measured memory) and the runtime debug draw.

#include <gtest/gtest.h>

#include "Physics3DTestWorld.h"

#include "Stream.h"
#include "component/LineRendererComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "object/physics/Rigidbody2D.h"
#include "registry/PhysicsLayers.h"
#include "yaml-cpp/yaml.h"

#include "Jolt/Core/Memory.h"
#include "Jolt/Physics/PhysicsStepListener.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::physics3dtest;

namespace {

    float speedAfterDamping(float v0, float damping, int steps) {
        return v0 * std::pow(1.0f - damping * STEP, (float)steps);
    }

    Vector3 up(const Quaternion& rotation) {
        return rotation * Vector3(0, 1, 0);
    }

}

// ---- T2: damping -----------------------------------------------------------------------------

TEST(Physics3DBody, DampingSlowsAsJoltIntegratesIt) {
    World3D world(Vector3(0, 0, 0));
    const Entity e = world.sphere(Vector3(0, 0, 0), 0.5f, BodyType::DYNAMIC);
    world.rb(e).setLinearDamping(1.0f);
    world.rb(e).setAngularDamping(1.0f);
    world.load();
    world.rb(e).setLinearVelocity(Vector3(10, 0, 0));
    world.rb(e).setAngularVelocity(Vector3(0, 5, 0));
    world.step(60);
    const float expected = speedAfterDamping(10.0f, 1.0f, 60);
    EXPECT_NEAR(expected, 3.65f, 0.01f);
    EXPECT_NEAR(world.rb(e).getLinearVelocity().x, expected, expected * 0.01f);
    EXPECT_NEAR(world.rb(e).getAngularVelocity().y, speedAfterDamping(5.0f, 1.0f, 60), 0.05f);

    // a collider edit swaps the shape, the damping stays
    world.rb(e).setColliderRadius(0, 0.6f);
    world.step();
    world.rb(e).setLinearVelocity(Vector3(10, 0, 0));
    world.step(60);
    EXPECT_NEAR(world.rb(e).getLinearVelocity().x, expected, expected * 0.01f);

    // and a save / load
    const RigidbodyComponent decoded = Stream::decodeRigidbodyComponent(Stream::encodeRigidbodyComponent(world.component(e)));
    EXPECT_FLOAT_EQ(decoded.linearDamping, 1.0f);
    EXPECT_FLOAT_EQ(decoded.angularDamping, 1.0f);
}

TEST(Physics3DBody, DefaultsAreJoltsImplicitOnes) {
    const RigidbodyComponent body{};
    EXPECT_FLOAT_EQ(body.linearDamping, 0.05f);
    EXPECT_FLOAT_EQ(body.angularDamping, 0.05f);
    EXPECT_FLOAT_EQ(body.maxLinearVelocity, 500.0f);
    EXPECT_NEAR(body.maxAngularVelocity, 0.25f * (float)M_PI * 60.0f, 1e-4f);
    EXPECT_TRUE(body.automaticMass);
    EXPECT_TRUE(body.allowSleeping);
    EXPECT_FALSE(body.collideKinematicVsNonDynamic);
}

// ---- T3: mass, centre of mass, inertia ----------------------------------------------------------

TEST(Physics3DBody, ExplicitMassScalesTheInertia) {
    World3D world(Vector3(0, 0, 0));
    const Entity automatic = world.box(Vector3(-5, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    const Entity explicitMass = world.box(Vector3(5, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    world.rb(explicitMass).setMass(3.0f);
    world.load();
    EXPECT_NEAR(world.rb(automatic).getMass(), 1000.0f, 1e-2f) << "1 m^3 x 1000 kg/m^3";
    EXPECT_FLOAT_EQ(world.rb(explicitMass).getMass(), 3.0f);

    world.rb(explicitMass).applyImpulse(Vector3(3, 0, 0));
    world.rb(automatic).applyImpulse(Vector3(1000, 0, 0));
    world.rb(explicitMass).applyAngularImpulse(Vector3(1, 0, 0));
    world.rb(automatic).applyAngularImpulse(Vector3(1, 0, 0));
    world.step();
    EXPECT_NEAR(world.rb(explicitMass).getLinearVelocity().x, 1.0f, 0.01f);
    EXPECT_NEAR(world.rb(automatic).getLinearVelocity().x, 1.0f, 0.01f);
    // a solid cube: I = m / 6, scaled with the mass
    EXPECT_NEAR(world.rb(explicitMass).getAngularVelocity().x, 1.0f / (3.0f / 6.0f), 0.05f);
    EXPECT_NEAR(world.rb(automatic).getAngularVelocity().x, 1.0f / (1000.0f / 6.0f), 1e-4f);
}

TEST(Physics3DBody, LowCenterOfMassSelfRights) {
    World3D world;
    world.floor();
    const Entity e = world.box(Vector3(0, 0.75f, 0), Vector3(1, 1, 1), BodyType::DYNAMIC, NULL_ENTITY,
                               degreesAbout(60.0f, Vector3(0, 0, 1)));
    world.rb(e).setCenterOfMass(Vector3(0, -0.5f, 0));
    EXPECT_FALSE(world.rb(e).isAutomaticCenterOfMass());
    world.load();
    world.step(180);
    EXPECT_GT(up(world.rb(e).getRotation()).dotProduct(Vector3(0, 1, 0)), std::cos(Angle::degToRad(5.0f)))
        << "settles upright within 3 s";

    // a twin with the automatic centre topples onto a face (stays near a multiple of 90 degrees, not upright)
    World3D other;
    other.floor();
    const Entity twin = other.box(Vector3(0, 0.75f, 0), Vector3(1, 1, 1), BodyType::DYNAMIC, NULL_ENTITY,
                                  degreesAbout(60.0f, Vector3(0, 0, 1)));
    other.load();
    other.step(180);
    EXPECT_LT(up(other.rb(twin).getRotation()).dotProduct(Vector3(0, 1, 0)), 0.5f) << "falls to its side";
}

// The legacy solid-box override (Rigidbody.setOverrideMassAndInertia): set before the body is made
// it is the mass the body is made with, set on a live body it takes effect at once. Its test was
// inverted: it set the fields only on a live body (where nothing applied them) and refused the rest
// with "Cannot override mass and inertia of loaded body".
TEST(Physics3DBody, OverrideMassAndInertiaTakesEffectBeforeAndAfterTheBodyIsMade) {
    World3D world(Vector3(0, 0, 0));
    const Entity early = world.box(Vector3(-5, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    const Entity live = world.box(Vector3(5, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    // a 2 x 1 x 1 m box of 250 kg/m3: 500 kg, where the 1 m collider alone gives 1000 kg
    world.rb(early).setOverrideMassAndInertia(Vector3(2, 1, 1), 250.0f);
    EXPECT_TRUE(world.component(early).overrideMassProperties);
    world.load();
    EXPECT_NEAR(world.rb(early).getMass(), 500.0f, 1e-2f);
    world.rb(early).applyImpulse(Vector3(500, 0, 0));
    world.step();
    EXPECT_NEAR(world.rb(early).getLinearVelocity().x, 1.0f, 0.01f) << "the body is made with the box's mass";

    // 2000 kg on the live body
    world.rb(live).setOverrideMassAndInertia(Vector3(1, 1, 1), 2000.0f);
    EXPECT_NEAR(world.rb(live).getMass(), 2000.0f, 1e-2f);
    world.rb(live).applyImpulse(Vector3(2000, 0, 0));
    world.step();
    EXPECT_NEAR(world.rb(live).getLinearVelocity().x, 1.0f, 0.01f) << "the live body takes it at once";
}

TEST(Physics3DBody, InertiaTensorResistsSpin) {
    World3D world(Vector3(0, 0, 0));
    const Entity a = world.box(Vector3(-5, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    const Entity b = world.box(Vector3(5, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    for (Entity e : {a, b}) world.rb(e).setInertiaTensor(Vector3(1, 100, 1));
    world.load();
    world.rb(a).applyAngularImpulse(Vector3(1, 0, 0));
    world.rb(b).applyAngularImpulse(Vector3(0, 1, 0));
    world.step();
    const float x = world.rb(a).getAngularVelocity().x;
    const float y = world.rb(b).getAngularVelocity().y;
    EXPECT_NEAR(x, 1.0f, 0.01f);
    EXPECT_NEAR(y / x, 0.01f, 1e-3f);

    // survives a collider edit and a save / load
    world.rb(a).setColliderBoxSize(0, Vector3(1.2f, 1, 1));
    world.step();
    EXPECT_EQ(world.rb(a).getInertiaTensor(), Vector3(1, 100, 1));
    const RigidbodyComponent decoded = Stream::decodeRigidbodyComponent(Stream::encodeRigidbodyComponent(world.component(a)));
    EXPECT_FALSE(decoded.automaticInertiaTensor);
    EXPECT_EQ(decoded.inertiaTensor, Vector3(1, 100, 1));
}

// ---- T4: type switching -------------------------------------------------------------------------

TEST(Physics3DBody, TypeSwitchesApplyLiveAndKeepTheBody) {
    World3D world;
    world.floor(-20.0f);
    const Entity e = world.box(Vector3(0, 5, 0), Vector3(1, 1, 1), BodyType::STATIC);
    world.load();
    const JPH::BodyID id = world.component(e).body;
    world.step(10);
    EXPECT_NEAR(world.rb(e).getPosition().y, 5.0f, 1e-5f) << "static stays";

    EXPECT_TRUE(world.rb(e).setType(BodyType::DYNAMIC));
    world.step(30);
    EXPECT_LT(world.rb(e).getPosition().y, 4.0f) << "dynamic falls";
    EXPECT_EQ(world.component(e).body, id);

    EXPECT_TRUE(world.rb(e).setType(BodyType::KINEMATIC));
    world.step();
    const float stoppedY = world.rb(e).getPosition().y;
    world.step(10);
    EXPECT_NEAR(world.rb(e).getPosition().y, stoppedY, 1e-3f) << "kinematic stops";
    world.transform(e).position = Vector3(3, 2, 0);
    world.transform(e).needUpdate = true;
    world.resolve();
    world.step(3);
    EXPECT_NEAR(world.rb(e).getPosition().x, 3.0f, 1e-3f) << "and follows its Transform";

    EXPECT_TRUE(world.rb(e).setType(BodyType::STATIC));
    world.step();
    EXPECT_TRUE(world.rb(e).setType(BodyType::KINEMATIC));
    world.step();
    EXPECT_EQ(world.component(e).body, id);

    std::mt19937 random(1234);
    for (int i = 0; i < 100; i++) {
        world.rb(e).setType((BodyType)(random() % 3));
        world.step();
        ASSERT_EQ(world.component(e).body, id) << "switch " << i;
    }
}

TEST(Physics3DBody, MeshColliderRefusesToMove) {
    LogCapture log;
    World3D world;
    const Entity e = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    world.rb(e).createMeshCollider({Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 0, 1)}, std::vector<uint32_t>{0, 1, 2});
    world.load();
    EXPECT_FALSE(world.rb(e).setType(BodyType::DYNAMIC));
    EXPECT_EQ(world.component(e).type, BodyType::STATIC);
    EXPECT_GE(log.count("collider 0"), 1u) << "the error names the collider";
}

// ---- T5: triggers per collider -----------------------------------------------------------------

TEST(Physics3DBody, TriggerCollidersPassThroughAndWeighNothing) {
    World3D world;
    world.floor();
    // a wall the trigger sphere reaches through
    world.box(Vector3(1.2f, 1, 0), Vector3(0.2f, 2, 2), BodyType::STATIC);
    const Entity e = world.box(Vector3(0, 0.6f, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    const int trigger = world.rb(e).createSphereCollider(Vector3(0, 0, 0), Quaternion::IDENTITY, 1.5f);
    world.rb(e).setColliderTrigger((size_t)trigger, true);
    EXPECT_NEAR(world.rb(e).getMass(), 1000.0f, 1e-2f) << "the box's mass alone";

    int sensorContacts = 0;
    world.physics()->onContactAdded3D.add("sensor", std::function<void(Rigidbody, Rigidbody, Contact3D)>(
        [&](Rigidbody, Rigidbody, Contact3D contact) { if (contact.isSensor()) sensorContacts++; }));
    world.load();
    world.step(120);
    EXPECT_NEAR(world.rb(e).getPosition().y, 0.5f, 0.05f) << "rests on the floor through the box";
    EXPECT_NEAR(world.rb(e).getPosition().x, 0.0f, 0.05f) << "the sphere does not push off the wall";
    EXPECT_GT(sensorContacts, 0);
}

TEST(Physics3DBody, KinematicTriggerSeesAStaticBox) {
    World3D world(Vector3(0, 0, 0));
    world.box(Vector3(0, 0, 0), Vector3(1, 1, 1), BodyType::STATIC);
    const Entity e = world.body(Vector3(0.5f, 0, 0), BodyType::KINEMATIC);
    world.rb(e).createSphereCollider(0.5f);
    world.rb(e).setColliderTrigger(0, true);
    int sensorContacts = 0;
    world.physics()->onContactAdded3D.add("sensor", std::function<void(Rigidbody, Rigidbody, Contact3D)>(
        [&](Rigidbody, Rigidbody, Contact3D contact) { if (contact.isSensor()) sensorContacts++; }));
    world.load();
    world.step(2);
    EXPECT_EQ(sensorContacts, 1);
}

// ---- T7: collider edits ------------------------------------------------------------------------

TEST(Physics3DBody, ColliderEditsSwapTheShapeAndKeepTheBody) {
    World3D world;
    world.floor();
    const Entity ball = world.sphere(Vector3(0, 0.5f, 0), 0.5f, BodyType::DYNAMIC);
    const Entity anchor = world.sphere(Vector3(0, 5, 0), 0.1f, BodyType::STATIC);
    Joint3DComponent rope = makeJoint3D(Joint3DType::DISTANCE);
    rope.bodyA = ball;
    rope.bodyB = anchor;
    rope.maxDistance = 10.0f;
    rope.minDistance = 0.0f;
    const Entity joint = world.joint(rope);
    world.load();
    world.step(60);
    const JPH::BodyID id = world.component(ball).body;
    const JPH::TwoBodyConstraint* constraint = world.jointComponent(joint).joint;
    ASSERT_NE(constraint, nullptr);

    world.rb(ball).setColliderRadius(0, 0.8f);
    world.step();
    EXPECT_EQ(world.component(ball).body, id) << "same body";
    EXPECT_EQ(world.jointComponent(joint).joint, constraint) << "same joint";
    EXPECT_FALSE(world.rb(ball).isSleeping()) << "woken by the swap";
    world.step(60);
    EXPECT_NEAR(world.rb(ball).getPosition().y, 0.8f, 0.05f) << "rests on its new radius";
}

TEST(Physics3DBody, RemovingAColliderShiftsTheOthers) {
    World3D world;
    const Entity e = world.body(Vector3(0, 0, 0), BodyType::DYNAMIC);
    Rigidbody rb = world.rb(e);
    rb.createSphereCollider(0.1f);
    rb.createSphereCollider(0.2f);
    rb.createSphereCollider(0.3f);
    world.load();
    EXPECT_TRUE(rb.removeCollider(0));
    ASSERT_EQ(world.component(e).colliders.size(), 2u);
    EXPECT_FLOAT_EQ(world.component(e).colliders[0].radius, 0.2f);
    EXPECT_FLOAT_EQ(world.component(e).colliders[1].radius, 0.3f);
    EXPECT_FALSE(rb.removeCollider(5));
    world.step();
    EXPECT_FALSE(world.component(e).body.IsInvalid());
}

TEST(Physics3DBody, LargeMeshColliderBuildsWithoutCaps) {
    World3D world;
    // 20 000 indices: a 41 x 41 grid of quads (40 x 40 x 2 triangles = 3200, 9600 indices) twice over
    std::vector<Vector3> vertices;
    std::vector<uint32_t> indices;
    const int n = 58;  // 57 x 57 quads x 6 = 19 494; with the extra row past 20 000
    for (int z = 0; z <= n; z++) {
        for (int x = 0; x <= n; x++) vertices.push_back(Vector3((float)x, 0.0f, (float)z));
    }
    for (int z = 0; z < n; z++) {
        for (int x = 0; x < n; x++) {
            const uint32_t i = (uint32_t)(z * (n + 1) + x);
            indices.insert(indices.end(), {i, i + (uint32_t)(n + 1), i + 1, i + 1, i + (uint32_t)(n + 1), i + (uint32_t)(n + 2)});
        }
    }
    ASSERT_GT(indices.size(), 20000u);
    ASSERT_GT(vertices.size(), 3000u);
    const Entity ground = world.body(Vector3(0, 0, 0), BodyType::STATIC);
    world.rb(ground).createMeshCollider(vertices, indices);
    const Entity ball = world.sphere(Vector3(20, 1, 20), 0.5f, BodyType::DYNAMIC);
    world.load();
    EXPECT_FALSE(world.component(ground).body.IsInvalid());
    world.step(90);
    EXPECT_NEAR(world.rb(ball).getPosition().y, 0.5f, 0.05f) << "rests on the mesh";
}

// ---- T12: nested bodies are written back in hierarchy order ------------------------------------

TEST(Physics3DBody, NestedBodiesWriteBackTheirJoltPoses) {
    World3D world;
    const Entity anchor = world.sphere(Vector3(0, 10, 0), 0.1f, BodyType::STATIC);
    const Entity top = world.box(Vector3(1, 10, 0), Vector3(1, 0.2f, 0.2f), BodyType::DYNAMIC);
    // a Transform without a body between two bodies (a bone), turned and offset
    const Entity middle = world.entity(Vector3(0.5f, 0, 0), top, degreesAbout(30.0f, Vector3(0, 0, 1)));
    const Entity bottom = world.box(Vector3(1.0f, 0, 0), Vector3(1, 0.2f, 0.2f), BodyType::DYNAMIC, middle);
    world.resolve();

    Joint3DComponent first = makeJoint3D(Joint3DType::POINT);
    first.bodyA = top;
    first.bodyB = anchor;
    first.anchor = Vector3(-1, 0, 0);
    world.joint(first);
    Joint3DComponent second = makeJoint3D(Joint3DType::POINT);
    second.bodyA = bottom;
    second.bodyB = top;
    second.anchor = Vector3(-0.5f, 0, 0);
    world.joint(second);
    world.load();

    for (int i = 0; i < 200; i++) {
        world.step();
        for (Entity e : {top, bottom}) {
            const Transform& transform = world.transform(e);
            const Vector3 joltPosition = world.rb(e).getPosition();
            const Quaternion joltRotation = world.rb(e).getRotation();
            ASSERT_NEAR((transform.worldPosition - joltPosition).length(), 0.0f, 1e-4f) << "step " << i << " body " << e;
            const float dot = std::fabs(transform.worldRotation.dot(joltRotation));
            ASSERT_NEAR(dot, 1.0f, 1e-4f) << "step " << i << " body " << e;
        }
    }
    EXPECT_LT(world.rb(bottom).getPosition().y, 10.0f) << "the chain swung down";
}

// ---- T14: interpolation per body ---------------------------------------------------------------

TEST(Physics3DBody, InterpolationIsPerBody) {
    World3D world(Vector3(0, 0, 0));
    world.scene.setPhysicsInterpolation(true);
    const Entity none = world.sphere(Vector3(0, 0, 0), 0.5f, BodyType::DYNAMIC);
    const Entity interpolated = world.sphere(Vector3(0, 0, 5), 0.5f, BodyType::DYNAMIC);
    const Entity extrapolated = world.sphere(Vector3(0, 0, 10), 0.5f, BodyType::DYNAMIC);
    world.rb(none).setInterpolation(RigidbodyInterpolation::NONE);
    world.rb(interpolated).setInterpolation(RigidbodyInterpolation::INTERPOLATE);
    world.rb(extrapolated).setInterpolation(RigidbodyInterpolation::EXTRAPOLATE);
    world.load();
    for (Entity e : {none, interpolated, extrapolated}) {
        world.rb(e).setLinearDamping(0.0f);
        world.rb(e).setLinearVelocity(Vector3(6, 0, 0));
    }
    world.step(2);
    world.physics()->interpolate(0.5);
    world.resolve();
    const float stepX = world.rb(none).getPosition().x;
    EXPECT_NEAR(world.transform(none).position.x, stepX, 1e-5f) << "NONE draws the step pose";
    EXPECT_NEAR(world.transform(interpolated).position.x, stepX - 6.0f * STEP * 0.5f, 1e-4f) << "half way from the previous step";
    EXPECT_NEAR(world.transform(extrapolated).position.x, stepX + 6.0f * STEP * 0.5f, 1e-4f) << "ahead by v alpha dt";
}

TEST(Physics3DBody, InterpolationIsPerBody2D) {
    physicstest::installTestSystem();
    Scene scene;
    scene.setPhysicsInterpolation(true);
    scene.setGravity2D(Vector2(0, 0));
    auto makeBody = [&](float y, RigidbodyInterpolation mode) {
        Entity e = scene.createEntity();
        Transform transform;
        transform.position = Vector3(0, y, 0);
        scene.addComponent<Transform>(e, transform);
        Rigidbody2DComponent component;
        component.type = BodyType::DYNAMIC;
        component.interpolation = mode;
        scene.addComponent<Rigidbody2DComponent>(e, component);
        Rigidbody2D(&scene, e).createCircleCollider(Vector2(0, 0), 5);
        return e;
    };
    const Entity none = makeBody(0, RigidbodyInterpolation::NONE);
    const Entity extrapolated = makeBody(100, RigidbodyInterpolation::EXTRAPOLATE);
    auto resolve = [&]() {
        auto transforms = scene.getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            scene.getSystem<RenderSystem>()->updateTransform(transforms->getComponentFromIndex(i), transforms->getEntity(i));
        }
    };
    resolve();
    PhysicsSystem* physics = scene.getSystem<PhysicsSystem>().get();
    physics->load();
    for (Entity e : {none, extrapolated}) {
        Rigidbody2D(&scene, e).setLinearDamping(0.0f);
        Rigidbody2D(&scene, e).setLinearVelocity(Vector2(120, 0));
    }
    for (int i = 0; i < 2; i++) { physics->fixedUpdate(STEP); resolve(); }
    physics->interpolate(0.5);
    resolve();
    const float stepX = Rigidbody2D(&scene, none).getPosition().x;
    EXPECT_NEAR(scene.getComponent<Transform>(none).position.x, stepX, 1e-3f);
    EXPECT_NEAR(scene.getComponent<Transform>(extrapolated).position.x, stepX + 120.0f * STEP * 0.5f, 1e-2f);
}

// ---- T15: capacity -----------------------------------------------------------------------------

namespace {

    // Jolt's heap use while the counters are on: every live block by its size
    struct JoltHeapCounter {
        static std::mutex& mutex() { static std::mutex m; return m; }
        static std::map<void*, size_t>& blocks() { static std::map<void*, size_t> b; return b; }
        static size_t& live() { static size_t bytes = 0; return bytes; }
        static JPH::AllocateFunction& allocate() { static JPH::AllocateFunction f = nullptr; return f; }
        static JPH::ReallocateFunction& reallocate() { static JPH::ReallocateFunction f = nullptr; return f; }
        static JPH::FreeFunction& release() { static JPH::FreeFunction f = nullptr; return f; }
        static JPH::AlignedAllocateFunction& alignedAllocate() { static JPH::AlignedAllocateFunction f = nullptr; return f; }
        static JPH::AlignedFreeFunction& alignedRelease() { static JPH::AlignedFreeFunction f = nullptr; return f; }

        static void add(void* block, size_t size) {
            if (!block) return;
            std::lock_guard<std::mutex> lock(mutex());
            blocks()[block] = size;
            live() += size;
        }
        static void remove(void* block) {
            if (!block) return;
            std::lock_guard<std::mutex> lock(mutex());
            auto it = blocks().find(block);
            if (it == blocks().end()) return;
            live() -= it->second;
            blocks().erase(it);
        }

        static void* countedAllocate(size_t size) { void* b = allocate()(size); add(b, size); return b; }
        static void* countedReallocate(void* block, size_t oldSize, size_t newSize) {
            remove(block);
            void* b = reallocate()(block, oldSize, newSize);
            add(b, newSize);
            return b;
        }
        static void countedFree(void* block) { remove(block); release()(block); }
        static void* countedAlignedAllocate(size_t size, size_t alignment) { void* b = alignedAllocate()(size, alignment); add(b, size); return b; }
        static void countedAlignedFree(void* block) { remove(block); alignedRelease()(block); }

        JoltHeapCounter() {
            allocate() = JPH::Allocate; reallocate() = JPH::Reallocate; release() = JPH::Free;
            alignedAllocate() = JPH::AlignedAllocate; alignedRelease() = JPH::AlignedFree;
            JPH::Allocate = countedAllocate; JPH::Reallocate = countedReallocate; JPH::Free = countedFree;
            JPH::AlignedAllocate = countedAlignedAllocate; JPH::AlignedFree = countedAlignedFree;
        }
        ~JoltHeapCounter() {
            // the defaults are malloc / free based: blocks counted here can be freed by them
            JPH::Allocate = allocate(); JPH::Reallocate = reallocate(); JPH::Free = release();
            JPH::AlignedAllocate = alignedAllocate(); JPH::AlignedFree = alignedRelease();
            std::lock_guard<std::mutex> lock(mutex());
            blocks().clear();
            live() = 0;
        }
    };

    struct CapacityGuard {
        ~CapacityGuard() { PhysicsSystem::setWorldCapacity3D(8192, 8192, 8192); }
    };

}

TEST(Physics3DBody, DefaultCapacityWorldFitsInFourMegabytes) {
    CapacityGuard guard;
    PhysicsSystem::setWorldCapacity3D(8192, 8192, 8192);
    EXPECT_EQ(PhysicsSystem::getDefaultMaxBodies3D(), 8192u);
    physicstest::installTestSystem();
    size_t bytes = 0;
    {
        JoltHeapCounter counter;
        auto scene = std::make_unique<Scene>();
        // made when first needed: made here
        scene->getSystem<PhysicsSystem>()->getWorld3D();
        bytes = JoltHeapCounter::live();
        EXPECT_EQ(scene->getSystem<PhysicsSystem>()->getMaxBodies3D(), 8192u);
        scene.reset();
    }
    std::printf("[ physics-3d ] Jolt world at 8192 bodies / 8192 pairs / 8192 contact constraints: %.2f MB\n", bytes / (1024.0 * 1024.0));
    EXPECT_GT(bytes, 0u) << "the counters saw the world";
    EXPECT_LE(bytes, 4u * 1024u * 1024u);
}

TEST(Physics3DBody, NineThousandBodiesWithALargerCapacity) {
    CapacityGuard guard;
    PhysicsSystem::setWorldCapacity3D(10000, 8192, 8192);
    World3D world(Vector3(0, 0, 0));
    std::vector<Entity> bodies;
    for (int i = 0; i < 9000; i++) {
        bodies.push_back(world.sphere(Vector3((float)(i % 100) * 3.0f, 0, (float)(i / 100) * 3.0f), 0.5f, BodyType::STATIC));
    }
    world.load();
    world.step();
    size_t made = 0;
    for (Entity e : bodies) made += world.component(e).body.IsInvalid() ? 0 : 1;
    EXPECT_EQ(made, 9000u);
}

TEST(Physics3DBody, PastMaxBodiesTheErrorNamesTheSetting) {
    CapacityGuard guard;
    PhysicsSystem::setWorldCapacity3D(100, 8192, 8192);
    LogCapture log;
    World3D world(Vector3(0, 0, 0));
    std::vector<Entity> bodies;
    for (int i = 0; i < 101; i++) {
        bodies.push_back(world.sphere(Vector3((float)i * 3.0f, 0, 0), 0.5f, BodyType::STATIC));
    }
    world.load();
    world.step();
    EXPECT_TRUE(world.component(bodies[99]).body.IsInvalid() == false);
    EXPECT_TRUE(world.component(bodies[100]).body.IsInvalid());
    EXPECT_EQ(log.count("physics3DWorld.maxBodies"), 1u) << "once per scene";
}

TEST(Physics3DBody, ContactOverflowIsReportedOnce) {
    CapacityGuard guard;
    PhysicsSystem::setWorldCapacity3D(1024, 1024, 16);
    LogCapture log;
    World3D world;
    world.floor();
    for (int i = 0; i < 64; i++) {
        world.box(Vector3((float)(i % 8) * 1.5f, 0.5f, (float)(i / 8) * 1.5f), Vector3(1, 1, 1), BodyType::DYNAMIC);
    }
    world.load();
    world.step(30);
    EXPECT_EQ(log.count("maxContactConstraints"), 1u);
}

// ---- T16: debug draw ---------------------------------------------------------------------------

namespace {
    struct DebugDrawGuard {
        ~DebugDrawGuard() { Physics::setDebugDraw(false); Physics::setDebugDrawContacts(false); }
    };
}

TEST(Physics3DBody, DebugDrawLinesFollowTheSwitch) {
    DebugDrawGuard guard;
    World3D world;
    world.floor();
    world.box(Vector3(0, 2, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    world.load();
    world.step();
    Physics::setDebugDraw(true);
    world.physics()->draw();
    EXPECT_GT(world.physics()->getDebugLineCount(), 0u);
    Physics::setDebugDraw(false);
    world.physics()->draw();
    EXPECT_EQ(world.physics()->getDebugLineCount(), 0u);
}

TEST(Physics3DBody, DebugDrawLines2D) {
    DebugDrawGuard guard;
    physicstest::installTestSystem();
    Scene scene;
    Entity e = scene.createEntity();
    scene.addComponent<Transform>(e, Transform());
    Rigidbody2DComponent component;
    component.type = BodyType::DYNAMIC;
    scene.addComponent<Rigidbody2DComponent>(e, component);
    Rigidbody2D(&scene, e).createCircleCollider(Vector2(0, 0), 10);
    PhysicsSystem* physics = scene.getSystem<PhysicsSystem>().get();
    physics->load();
    physics->fixedUpdate(STEP);
    Physics::setDebugDraw(true);
    physics->draw();
    EXPECT_GT(physics->getDebugLineCount(), 0u);
    Physics::setDebugDraw(false);
    physics->draw();
    EXPECT_EQ(physics->getDebugLineCount(), 0u);
}

// ---- S96 WP-C: movePosition / moveRotation, the interpolated teleport, zero-height capsules -----

TEST(Physics3DBody, MovePositionKinematicPushes) {
    World3D world(Vector3(0, 0, 0));
    const Entity mover = world.box(Vector3(0, 0, 0), Vector3(1, 1, 1), BodyType::KINEMATIC);
    // touching (within Jolt's speculative contact distance): the move pushes it in that step
    const Entity crate = world.box(Vector3(1.01f, 0, 0), Vector3(1, 1, 1), BodyType::DYNAMIC);
    world.load();
    world.step();
    world.rb(mover).movePosition(Vector3(1, 0, 0));
    world.rb(mover).moveRotation(degreesAbout(30.0f, Vector3(0, 1, 0)));
    world.step();
    // there at the end of the step, by velocity: what it touches is pushed
    EXPECT_NEAR(world.rb(mover).getPosition().x, 1.0f, 1e-4f);
    EXPECT_NEAR(world.rb(mover).getLinearVelocity().x, 60.0f, 1e-2f);
    EXPECT_GT(std::fabs(world.rb(mover).getRotation().dot(degreesAbout(30.0f, Vector3(0, 1, 0)))), 0.99999f);
    world.step(2);
    EXPECT_GT(world.rb(crate).getLinearVelocity().x, 0.5f) << "pushed";
    // the step after it stops where it was sent
    EXPECT_NEAR(world.rb(mover).getLinearVelocity().length(), 0.0f, 1e-6f);
    EXPECT_NEAR(world.rb(mover).getAngularVelocity().length(), 0.0f, 1e-6f);
    EXPECT_NEAR(world.rb(mover).getPosition().x, 1.0f, 1e-4f);
}

TEST(Physics3DBody, MovePositionDynamicTeleports) {
    World3D world(Vector3(0, 0, 0));
    const Entity ball = world.sphere(Vector3(0, 0, 0), 0.5f, BodyType::DYNAMIC);
    world.load();
    world.rb(ball).setLinearDamping(0.0f);
    world.rb(ball).setLinearVelocity(Vector3(0, 2, 0));
    world.step();
    const float y = world.rb(ball).getPosition().y;
    world.rb(ball).movePosition(Vector3(5, y, 0));
    world.step();
    // placed before the step, its velocity kept (Unity)
    EXPECT_NEAR(world.rb(ball).getPosition().x, 5.0f, 1e-4f);
    EXPECT_NEAR(world.rb(ball).getPosition().y, y + 2.0f * STEP, 1e-4f);
    EXPECT_NEAR(world.rb(ball).getLinearVelocity().y, 2.0f, 1e-4f);
    EXPECT_NEAR(world.rb(ball).getLinearVelocity().x, 0.0f, 1e-4f);
}

TEST(Physics3DBody, MoveIsInterpolated) {
    World3D world(Vector3(0, 0, 0));
    world.scene.setPhysicsInterpolation(true);
    const Entity moved = world.sphere(Vector3(0, 0, 0), 0.5f, BodyType::DYNAMIC);
    const Entity mover = world.box(Vector3(0, 0, 10), Vector3(1, 1, 1), BodyType::KINEMATIC);
    world.load();
    world.step();
    world.rb(moved).movePosition(Vector3(4, 0, 0));
    world.rb(mover).movePosition(Vector3(4, 0, 10));
    world.step();
    world.physics()->interpolate(0.5);
    world.resolve();
    // half way between the two step poses, for a dynamic and a kinematic body
    EXPECT_NEAR(world.transform(moved).position.x, 2.0f, 1e-4f);
    EXPECT_NEAR(world.transform(mover).position.x, 2.0f, 1e-4f);

    // a teleport jumps: drawn at the target
    world.step();
    world.rb(moved).setPosition(Vector3(8, 0, 0));
    world.step();
    world.physics()->interpolate(0.5);
    world.resolve();
    EXPECT_NEAR(world.transform(moved).position.x, 8.0f, 1e-4f);
}

TEST(Physics3DBody, SetPositionInterpolateOption) {
    World3D world(Vector3(0, 0, 0));
    world.scene.setPhysicsInterpolation(true);
    const Entity sliding = world.sphere(Vector3(0, 0, 0), 0.5f, BodyType::DYNAMIC);
    const Entity jumping = world.sphere(Vector3(0, 0, 5), 0.5f, BodyType::DYNAMIC);
    world.load();
    world.step();
    world.rb(sliding).setPosition(Vector3(4, 0, 0), true);
    world.rb(jumping).setPosition(Vector3(4, 0, 5), false);
    world.rb(sliding).setRotation(degreesAbout(90.0f, Vector3(0, 1, 0)), true);
    world.physics()->interpolate(0.5);
    world.resolve();
    EXPECT_NEAR(world.transform(sliding).position.x, 2.0f, 1e-4f) << "slides from the pose before the last step";
    EXPECT_NEAR(world.transform(jumping).position.x, 4.0f, 1e-4f) << "the default jumps";
    EXPECT_GT(std::fabs(world.transform(sliding).rotation.dot(degreesAbout(45.0f, Vector3(0, 1, 0)))), 0.9999f);
}

TEST(Physics3DBody, ZeroHalfHeightCapsuleIsSphere) {
    World3D world;
    world.floor();
    const Entity ball = world.body(Vector3(0, 2, 0), BodyType::DYNAMIC);
    world.rb(ball).createCapsuleCollider(0.0f, 0.5f);
    const Entity tapered = world.body(Vector3(3, 2, 0), BodyType::DYNAMIC);
    world.rb(tapered).createTaperedCapsuleCollider(0.0f, 0.25f, 0.5f);
    const Entity sphere = world.sphere(Vector3(-3, 2, 0), 0.5f, BodyType::DYNAMIC);
    world.load();
    world.step(240);
    // a sphere of its radius (the larger one for a tapered capsule): it rests as a sphere does
    // (Jolt's penetration slop lets a resting body sink up to 0.02 m)
    const float sphereY = world.rb(sphere).getPosition().y;
    EXPECT_NEAR(sphereY, 0.5f, 0.02f);
    EXPECT_NEAR(world.rb(ball).getPosition().y, sphereY, 1e-3f);
    EXPECT_NEAR(world.rb(tapered).getPosition().y, sphereY, 1e-3f);
    EXPECT_THROW(world.rb(ball).setColliderHalfHeight(0, -1.0f), std::runtime_error);
}

// ---- S96 REQ-35: the 3D world settings ---------------------------------------------------------

namespace {

    // Jolt calls a step listener once per collision step
    class CountingStepListener : public JPH::PhysicsStepListener {
    public:
        std::atomic<int> steps{0};
        void OnStep(const JPH::PhysicsStepListenerContext&) override { steps++; }
    };

    // ten boxes stacked on a floor, a little off centre each, stepped; their poses
    std::vector<float> stackPoses(World3D& world, int steps, const std::function<void()>& afterFirstStep = nullptr) {
        world.floor();
        std::vector<Entity> boxes;
        for (int i = 0; i < 10; i++) {
            boxes.push_back(world.box(Vector3(0.03f * (float)(i % 3), 0.5f + 1.01f * (float)i, 0.02f * (float)(i % 2)), Vector3(1, 1, 1), BodyType::DYNAMIC));
        }
        world.load();
        world.step(1);
        if (afterFirstStep) afterFirstStep();
        world.step(steps - 1);
        std::vector<float> poses;
        for (Entity e : boxes) {
            const Vector3 p = world.rb(e).getPosition();
            const Quaternion q = world.rb(e).getRotation();
            poses.insert(poses.end(), {p.x, p.y, p.z, q.w, q.x, q.y, q.z});
        }
        return poses;
    }

    float horizontalDrift(const std::vector<float>& poses) {
        float drift = 0.0f;
        for (size_t i = 0; i < poses.size(); i += 7) drift += std::fabs(poses[i]) + std::fabs(poses[i + 2]);
        return drift;
    }

}

// The scene's 3D solver settings reach Jolt when the world is made and when they change: velocity
// iterations change a stack's drift, collision steps run Jolt's collision passes per fixed step,
// and settings at their defaults (or changed and set back) step the same bits as before they
// existed. Red for the first two: the world never got Jolt settings and stepped one pass
TEST(Physics3DBody, WorldSettingsApply) {
    std::vector<float> reference;
    {
        World3D world;
        reference = stackPoses(world, 600);
    }
    {
        World3D world;
        const std::vector<float> roundTrip = stackPoses(world, 600, [&]() {
            ScenePhysics3DSettings changed;
            changed.velocityIterations = 3;
            changed.positionIterations = 5;
            changed.bounceThreshold = 4.0f;
            changed.sleepThreshold = 0.5f;
            changed.speculativeDistance = 0.1f;
            changed.collisionSteps = 3;
            world.scene.setPhysics3DWorldSettings(changed);
            world.scene.setPhysics3DWorldSettings(ScenePhysics3DSettings());
        });
        ASSERT_EQ(roundTrip.size(), reference.size());
        EXPECT_EQ(std::memcmp(roundTrip.data(), reference.data(), reference.size() * sizeof(float)), 0) << "defaults step the same bits";
    }

    float driftOne = 0.0f;
    {
        World3D world;
        world.scene.setPhysics3DVelocityIterations(1);
        driftOne = horizontalDrift(stackPoses(world, 300));
        EXPECT_EQ(world.physics()->getWorld3D()->GetPhysicsSettings().mNumVelocitySteps, 1u);
    }
    float driftTen = 0.0f;
    {
        World3D world;
        driftTen = horizontalDrift(stackPoses(world, 300));
    }
    std::printf("[WorldSettingsApply] stack drift: 1 velocity iteration %.4f m, 10 iterations %.4f m\n", driftOne, driftTen);
    EXPECT_NE(driftOne, driftTen);

    World3D world;
    world.sphere(Vector3(0, 5, 0), 0.5f, BodyType::DYNAMIC);
    world.load();
    world.step(1);
    CountingStepListener listener;
    world.physics()->getWorld3D()->AddStepListener(&listener);
    world.step(1);
    EXPECT_EQ(listener.steps.load(), 1) << "one collision step by default";
    world.scene.setPhysics3DCollisionSteps(2);
    world.scene.setPhysics3DSpeculativeDistance(0.05f);
    listener.steps = 0;
    world.step(1);
    EXPECT_EQ(listener.steps.load(), 2);
    EXPECT_FLOAT_EQ(world.physics()->getWorld3D()->GetPhysicsSettings().mSpeculativeContactDistance, 0.05f);
    world.physics()->getWorld3D()->RemoveStepListener(&listener);
}
