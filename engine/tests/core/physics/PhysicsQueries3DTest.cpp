// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The 3D shape queries (docs/specs/S96-physics-gaps.md REQ-10, REQ-12..REQ-15; AC-10, AC-11,
// AC-12, AC-14): sphere, box and capsule casts against a box, a sphere, a mesh and a terrain
// heightfield at their analytic first-contact distances; ...All, overlaps and checks; the
// layer mask, ignore list and trigger rules; Unity's start-inside rule of Physics.raycast; the
// hit fields and the CharacterController's inner body. A scene with only its PhysicsSystem
// running, stepped once so the bodies exist.

#ifdef JASPERENGINE_PHYSICS_3D

#include "Engine.h"
#include "LogCapture.h"
#include "Scene.h"
#include "component/CharacterControllerComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "math/Ray.h"
#include "physics/PhysicsQueries3D.h"
#include "registry/PhysicsLayers.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;
    constexpr float EPS = 1.0e-3f;
    const Vector3 DOWN(0.0f, -1.0f, 0.0f);

    class PhysicsQueries3DTest : public ::testing::Test {
    protected:
        std::unique_ptr<Scene> scene;
        float updateTime = 0.0f;
        std::vector<uint8_t> heights;
        bool hitTriggers = true;
        bool hitBackfaces = false;

        void SetUp() override{
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            hitTriggers = Physics::getQueriesHitTriggers();
            hitBackfaces = Physics::getQueriesHitBackfaces();
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            scene->getSystem<PhysicsSystem>()->setGravity3D(0.0f, -9.81f, 0.0f);
        }

        void TearDown() override{
            scene.reset();
            Engine::setUpdateTime(updateTime);
            Physics::setQueriesHitTriggers(hitTriggers);
            Physics::setQueriesHitBackfaces(hitBackfaces);
        }

        Entity entityAt(Vector3 position){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = position;
            transform.needUpdate = true;
            return entity;
        }

        Entity body(Vector3 position, std::vector<Collider3D> colliders, BodyType type = BodyType::STATIC){
            const Entity entity = entityAt(position);
            RigidbodyComponent rigidbody;
            rigidbody.type = type;
            rigidbody.colliders = std::move(colliders);
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            return entity;
        }

        static Collider3D boxCollider(Vector3 size, bool trigger = false){
            Collider3D collider;
            collider.type = Collider3DType::BOX;
            collider.width = size.x;
            collider.height = size.y;
            collider.depth = size.z;
            collider.isTrigger = trigger;
            return collider;
        }

        static Collider3D sphereCollider(float radius, bool trigger = false){
            Collider3D collider;
            collider.type = Collider3DType::SPHERE;
            collider.radius = radius;
            collider.isTrigger = trigger;
            return collider;
        }

        // a 4 x 4 square facing +y, at the GameObject's origin
        static Collider3D quadCollider(){
            Collider3D collider;
            collider.type = Collider3DType::MESH;
            collider.source = Collider3DSource::RAW_MESH;
            collider.vertices = {Vector3(-2, 0, -2), Vector3(-2, 0, 2), Vector3(2, 0, 2), Vector3(2, 0, -2)};
            collider.indices = {0, 1, 2, 0, 2, 3};
            return collider;
        }

        // a flat terrain of 8 x 8 m whose surface is at y = 1, its
        // heightfield the body's collider
        Entity terrain(Vector3 position){
            const Entity entity = entityAt(position);
            constexpr int MAP = 9;
            heights.assign((size_t)MAP * MAP, 128);
            TerrainComponent component;
            component.terrainSize = 8.0f;
            component.maxHeight = 255.0f / 128.0f;  // the level byte 128: 1 m
            component.heightMap.setData(std::string("physics-queries-") + ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                                        TextureData(MAP, MAP, (unsigned int)heights.size(), ColorFormat::RED, 1, heights.data()));
            // built: the collider reads the map, not a node tree this scene has no system to make
            component.needUpdateTerrain = false;
            component.heightMapLoaded = true;
            scene->addComponent<TerrainComponent>(entity, component);
            Collider3D collider;
            collider.type = Collider3DType::HEIGHTFIELD;
            collider.source = Collider3DSource::ENTITY_HEIGHTFIELD;
            collider.sourceEntity = entity;
            RigidbodyComponent rigidbody;
            rigidbody.type = BodyType::STATIC;
            rigidbody.colliders = {collider};
            scene->addComponent<RigidbodyComponent>(entity, rigidbody);
            return entity;
        }

        Entity controller(Vector3 foot){
            const Entity entity = entityAt(foot + Vector3(0.0f, 0.9f, 0.0f));
            CharacterControllerComponent component;
            component.radius = 0.4f;
            component.height = 1.8f;
            scene->addComponent<CharacterControllerComponent>(entity, component);
            return entity;
        }

        void step(){
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++){
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate)
                    scene->getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
            scene->fixedUpdate(DT);
        }

        // a box (2 m) at the origin, a sphere (radius 1) at x = 10, a 4 m square at z = 20 and a
        // flat terrain (surface y = 1) at z = -30
        struct World{ Entity box, sphere, quad, ground; };
        World world(){
            World w;
            w.box = body(Vector3(0, 0, 0), {boxCollider(Vector3(2, 2, 2))});
            w.sphere = body(Vector3(10, 0, 0), {sphereCollider(1.0f)});
            w.quad = body(Vector3(0, 0, 20), {quadCollider()});
            w.ground = terrain(Vector3(0, 0, -30));
            step();
            return w;
        }

        static void expectHit(const RayReturn& hit, Entity body, float distance, Vector3 normal, const char* what){
            ASSERT_TRUE(hit.hit) << what;
            EXPECT_EQ(hit.body, body) << what;
            EXPECT_NEAR(hit.distance, distance, EPS) << what;
            EXPECT_NEAR(hit.normal.x, normal.x, EPS) << what;
            EXPECT_NEAR(hit.normal.y, normal.y, EPS) << what;
            EXPECT_NEAR(hit.normal.z, normal.z, EPS) << what;
        }
    };

}

TEST_F(PhysicsQueries3DTest, CastsMatchGeometry){
    const World w = world();
    Scene* s = scene.get();
    const Vector3 up(0, 1, 0);
    struct Target{ Entity body; Vector3 above; float surface; const char* name; };
    // the flat terrain's surface where its sampled heights put it (about y = 1)
    const RayReturn ground = Physics::raycast(s, Vector3(0, 5, -30), DOWN, 10.0f);
    ASSERT_TRUE(ground.hit);
    ASSERT_EQ(ground.body, w.ground);
    const float groundY = 5.0f - ground.distance;
    EXPECT_NEAR(groundY, 1.0f, 0.05f);
    // straight down from y = 5 onto each: the box's and sphere's tops, the square at y = 0, the
    // terrain
    const Target targets[] = {
        {w.box, Vector3(0, 5, 0), 1.0f, "box"},
        {w.sphere, Vector3(10, 5, 0), 1.0f, "sphere"},
        {w.quad, Vector3(0, 5, 20), 0.0f, "mesh"},
        {w.ground, Vector3(0, 5, -30), groundY, "heightfield"},
    };
    for (const Target& t : targets){
        SCOPED_TRACE(t.name);
        const float gap = t.above.y - t.surface;
        expectHit(PhysicsQueries3D::sphereCast(s, t.above, 0.5f, DOWN, 10.0f), t.body, gap - 0.5f, up, "sphereCast");
        expectHit(PhysicsQueries3D::boxCast(s, t.above, Vector3(0.25f, 0.5f, 0.25f), DOWN, Quaternion::IDENTITY, 10.0f), t.body, gap - 0.5f, up, "boxCast");
        expectHit(PhysicsQueries3D::capsuleCast(s, t.above, t.above + Vector3(0, 1, 0), 0.5f, DOWN, 10.0f), t.body, gap - 0.5f, up, "capsuleCast");
        const RayReturn hit = PhysicsQueries3D::sphereCast(s, t.above, 0.5f, DOWN, 10.0f);
        EXPECT_NEAR(hit.point.y, t.surface, EPS);
        EXPECT_NEAR(hit.point.x, t.above.x, EPS);
        EXPECT_NEAR(hit.point.z, t.above.z, EPS);
        // too short to reach
        EXPECT_FALSE(PhysicsQueries3D::sphereCast(s, t.above, 0.5f, DOWN, gap - 0.6f).hit);
    }

    // a box turned 45 degrees about y meets the box's side with its edge: sqrt(2) * 0.5 out
    const Quaternion turned(0.0f, 45.0f, 0.0f);
    expectHit(PhysicsQueries3D::boxCast(s, Vector3(-5, 0, 0), Vector3(0.5f, 0.5f, 0.5f), Vector3(1, 0, 0), turned, 20.0f),
              w.box, 5.0f - 1.0f - std::sqrt(2.0f) * 0.5f, Vector3(-1, 0, 0), "turned boxCast");

    // ...All along x: the box, then the sphere, each once, nearest first
    std::vector<RayReturn> hits;
    ASSERT_EQ(PhysicsQueries3D::sphereCastAll(s, Vector3(-5, 0, 0), 0.5f, Vector3(1, 0, 0), 30.0f, hits), 2u);
    expectHit(hits[0], w.box, 3.5f, Vector3(-1, 0, 0), "all[0]");
    expectHit(hits[1], w.sphere, 13.5f, Vector3(-1, 0, 0), "all[1]");
    ASSERT_EQ(PhysicsQueries3D::boxCastAll(s, Vector3(-5, 0, 0), Vector3(0.5f, 0.5f, 0.5f), Vector3(1, 0, 0), Quaternion::IDENTITY, 30.0f, hits), 2u);
    EXPECT_NEAR(hits[1].distance, 13.5f, EPS);
    ASSERT_EQ(PhysicsQueries3D::capsuleCastAll(s, Vector3(-5, -0.5f, 0), Vector3(-5, 0.5f, 0), 0.5f, Vector3(1, 0, 0), 30.0f, hits), 2u);
    EXPECT_NEAR(hits[0].distance, 3.5f, EPS);
    // maxHits keeps the nearest
    QueryOptions one;
    one.maxHits = 1;
    ASSERT_EQ(PhysicsQueries3D::sphereCastAll(s, Vector3(-5, 0, 0), 0.5f, Vector3(1, 0, 0), 30.0f, hits, one), 1u);
    EXPECT_EQ(hits[0].body, w.box);

    // overlaps and checks agree on 200 random probes (a fixed seed)
    std::mt19937 random(96);
    std::uniform_real_distribution<float> x(-3.0f, 13.0f), yz(-3.0f, 3.0f), radius(0.1f, 1.5f);
    int touching = 0;
    std::vector<ColliderRef> colliders;
    for (int i = 0; i < 200; i++){
        const Vector3 center(x(random), yz(random), yz(random));
        const float r = radius(random);
        const bool any = PhysicsQueries3D::overlapSphere(s, center, r, colliders) > 0;
        EXPECT_EQ(PhysicsQueries3D::checkSphere(s, center, r), any) << "probe " << i;
        // the analytic answer for the box and the sphere
        const Vector3 nearestOnBox(std::clamp(center.x, -1.0f, 1.0f), std::clamp(center.y, -1.0f, 1.0f), std::clamp(center.z, -1.0f, 1.0f));
        const bool box = (center - nearestOnBox).length() < r - EPS;
        const bool sphere = (center - Vector3(10, 0, 0)).length() < r + 1.0f - EPS;
        if (box || sphere){
            EXPECT_TRUE(any) << "probe " << i;
        }
        touching += any ? 1 : 0;
    }
    EXPECT_GT(touching, 10);
    EXPECT_LT(touching, 190);

    // an overlap lists each collider once, by GameObject
    ASSERT_EQ(PhysicsQueries3D::overlapBox(s, Vector3(5, 0, 0), Vector3(6, 0.5f, 0.5f), Quaternion::IDENTITY, colliders), 2u);
    EXPECT_EQ(colliders[0].entity, std::min(w.box, w.sphere));
    EXPECT_EQ(colliders[1].entity, std::max(w.box, w.sphere));
    ASSERT_EQ(PhysicsQueries3D::overlapCapsule(s, Vector3(0, 0, 18), Vector3(0, 0, 22), 0.2f, colliders), 1u);
    EXPECT_EQ(colliders[0].entity, w.quad);
    EXPECT_TRUE(PhysicsQueries3D::checkCapsule(s, Vector3(0, 0, 18), Vector3(0, 0, 22), 0.2f));
    EXPECT_FALSE(PhysicsQueries3D::checkBox(s, Vector3(5, 0, 0), Vector3(1, 1, 1), Quaternion::IDENTITY));
    ASSERT_EQ(PhysicsQueries3D::overlapPoint(s, Vector3(0.5f, 0.5f, 0.5f), colliders), 1u);
    EXPECT_EQ(colliders[0].entity, w.box);

    // closestPoint: on the box's face, on the sphere, the point itself inside; none for a mesh
    Vector3 closest;
    ASSERT_TRUE(PhysicsQueries3D::closestPoint(s, Vector3(-4, 0.5f, 0.25f), ColliderRef(s, w.box, 0), closest));
    EXPECT_NEAR(closest.x, -1.0f, EPS);
    EXPECT_NEAR(closest.y, 0.5f, EPS);
    EXPECT_NEAR(closest.z, 0.25f, EPS);
    ASSERT_TRUE(PhysicsQueries3D::closestPoint(s, Vector3(10, 4, 0), ColliderRef(s, w.sphere, 0), closest));
    EXPECT_NEAR(closest.y, 1.0f, EPS);
    ASSERT_TRUE(PhysicsQueries3D::closestPoint(s, Vector3(0.2f, 0.1f, 0), ColliderRef(s, w.box, 0), closest));
    EXPECT_NEAR(closest.x, 0.2f, EPS);
    EXPECT_FALSE(PhysicsQueries3D::closestPoint(s, Vector3(0, 3, 20), ColliderRef(s, w.quad, 0), closest));
}

TEST_F(PhysicsQueries3DTest, FiltersAndOptions){
    const World w = world();
    Scene* s = scene.get();
    const Vector3 along(1, 0, 0);

    // the layer mask: the box on layer 9
    scene->setEntityLayer(w.box, 9);
    step();
    QueryOptions mask;
    mask.layerMask = ~(1u << 9);
    EXPECT_EQ(PhysicsQueries3D::sphereCast(s, Vector3(-5, 0, 0), 0.5f, along, 30.0f, mask).body, w.sphere);
    EXPECT_EQ(PhysicsQueries3D::sphereCast(s, Vector3(-5, 0, 0), 0.5f, along, 30.0f).body, w.box);
    std::vector<ColliderRef> colliders;
    EXPECT_EQ(PhysicsQueries3D::overlapSphere(s, Vector3(0, 0, 0), 0.5f, colliders, mask), 0u);

    // the ignore list
    const std::vector<Entity> ignore{w.box};
    QueryOptions ignoring;
    ignoring.ignore = &ignore;
    EXPECT_EQ(PhysicsQueries3D::sphereCast(s, Vector3(-5, 0, 0), 0.5f, along, 30.0f, ignoring).body, w.sphere);
    EXPECT_EQ(Physics::raycast(s, Vector3(-5, 0, 0), along, 30.0f, ignoring).body, w.sphere);
    EXPECT_FALSE(PhysicsQueries3D::checkSphere(s, Vector3(0, 0, 0), 0.5f, ignoring));

    // a trigger-only body and a solid body with a trigger collider (its companion sensor body)
    const Entity zone = body(Vector3(5, 0, 0), {boxCollider(Vector3(1, 4, 4), true)});
    const Entity mixed = body(Vector3(0, 0, 10), {boxCollider(Vector3(1, 1, 1)), sphereCollider(2.0f, true)});
    step();
    QueryOptions collide, ignoreTriggers, global;
    collide.triggers = QueryTriggerInteraction::COLLIDE;
    ignoreTriggers.triggers = QueryTriggerInteraction::IGNORE;
    const Vector3 start(2, 0, 0);
    EXPECT_EQ(PhysicsQueries3D::sphereCast(s, start, 0.25f, along, 20.0f, collide).body, zone);
    EXPECT_EQ(PhysicsQueries3D::sphereCast(s, start, 0.25f, along, 20.0f, ignoreTriggers).body, w.sphere);
    Physics::setQueriesHitTriggers(true);
    EXPECT_EQ(PhysicsQueries3D::sphereCast(s, start, 0.25f, along, 20.0f, global).body, zone);
    EXPECT_EQ(Physics::raycast(s, start, along, 20.0f, global).body, zone);
    Physics::setQueriesHitTriggers(false);
    EXPECT_EQ(PhysicsQueries3D::sphereCast(s, start, 0.25f, along, 20.0f, global).body, w.sphere);
    EXPECT_EQ(Physics::raycast(s, start, along, 20.0f, global).body, w.sphere);
    EXPECT_FALSE(PhysicsQueries3D::checkSphere(s, Vector3(5, 0, 0), 0.25f, global));
    EXPECT_TRUE(PhysicsQueries3D::checkSphere(s, Vector3(5, 0, 0), 0.25f, collide));
    Physics::setQueriesHitTriggers(true);

    // the mixed body: its trigger sphere first, as collider 1, and in an All query once
    std::vector<RayReturn> hits;
    PhysicsQueries3D::sphereCastAll(s, Vector3(-6, 0, 10), 0.25f, along, 12.0f, hits, collide);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].body, mixed);
    EXPECT_EQ(hits[0].colliderIndex, 1u);
    EXPECT_NEAR(hits[0].distance, 6.0f - 2.0f - 0.25f, EPS);
    EXPECT_EQ(hits[1].body, mixed);
    EXPECT_EQ(hits[1].colliderIndex, 0u);
    PhysicsQueries3D::sphereCastAll(s, Vector3(-6, 0, 10), 0.25f, along, 12.0f, hits, ignoreTriggers);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].colliderIndex, 0u);
    ASSERT_EQ(PhysicsQueries3D::overlapSphere(s, Vector3(0, 0, 10), 0.1f, colliders, collide), 2u);
    EXPECT_EQ(colliders[0].index, 0);
    EXPECT_EQ(colliders[1].index, 1);

    // a cast starting in a collider skips it, or with hitFromInside reports it at 0
    const RayReturn skipped = PhysicsQueries3D::sphereCast(s, Vector3(0, 0, 0), 0.25f, along, 20.0f, ignoreTriggers);
    EXPECT_EQ(skipped.body, w.sphere);
    QueryOptions inside = ignoreTriggers;
    inside.hitFromInside = true;
    const RayReturn atStart = PhysicsQueries3D::sphereCast(s, Vector3(0, 0, 0), 0.25f, along, 20.0f, inside);
    expectHit(atStart, w.box, 0.0f, Vector3(-1, 0, 0), "start overlap");
    EXPECT_NEAR(atStart.point.x, 0.0f, EPS);

    // sweepTest: the body's own colliders, never itself
    const Entity mover = body(Vector3(-4, 0, 0), {boxCollider(Vector3(1, 1, 1))}, BodyType::KINEMATIC);
    step();
    const RayReturn swept = PhysicsQueries3D::sweepTest(s, mover, along, 20.0f);
    expectHit(swept, w.box, 2.5f, Vector3(-1, 0, 0), "sweepTest");
    PhysicsQueries3D::sweepTestAll(s, mover, along, 20.0f, hits, ignoreTriggers);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[1].body, w.sphere);

    // nothing to stand on: invalid arguments log and miss
    test::LogCapture log;
    EXPECT_FALSE(PhysicsQueries3D::sphereCast(s, Vector3(0, 5, 0), -1.0f, DOWN, 10.0f).hit);
    EXPECT_FALSE(PhysicsQueries3D::sphereCast(s, Vector3(0, 5, 0), 1.0f, Vector3(0, 0, 0), 10.0f).hit);
    EXPECT_FALSE(PhysicsQueries3D::boxCast(s, Vector3(0, 5, 0), Vector3(1, 0, 1), DOWN, Quaternion::IDENTITY, 10.0f).hit);
    EXPECT_EQ(log.count(D_LOG_ERROR, "radius must be"), 1u);
    EXPECT_EQ(log.count(D_LOG_ERROR, "direction must be"), 1u);
    EXPECT_EQ(log.count(D_LOG_ERROR, "halfExtents must be"), 1u);
}

TEST_F(PhysicsQueries3DTest, RayFromInsideIsNotAHit){
    const World w = world();
    Scene* s = scene.get();
    const Vector3 along(1, 0, 0);

    // Unity: the box around the origin is not a hit; the sphere behind it is
    const RayReturn hit = Physics::raycast(s, Vector3(0, 0, 0), along, 30.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, w.sphere);
    EXPECT_NEAR(hit.distance, 9.0f, EPS);
    const RayReturn rayForm = Physics::raycast(s, Ray(Vector3(0, 0, 0), Vector3(30, 0, 0)));
    EXPECT_EQ(rayForm.body, w.sphere);
    const std::vector<RayReturn> all = Physics::raycastAll(s, Vector3(0, 0, 0), along, 30.0f);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].body, w.sphere);

    // hitFromInside: the old result, the box at 0
    QueryOptions inside;
    inside.hitFromInside = true;
    const RayReturn old = Physics::raycast(s, Vector3(0, 0, 0), along, 30.0f, inside);
    ASSERT_TRUE(old.hit);
    EXPECT_EQ(old.body, w.box);
    EXPECT_NEAR(old.distance, 0.0f, EPS);
    EXPECT_EQ(Physics::raycastAll(s, Vector3(0, 0, 0), along, 30.0f, inside).size(), 2u);

    // from outside nothing changed
    EXPECT_EQ(Physics::raycast(s, Vector3(-5, 0, 0), along, 30.0f).body, w.box);

    // Ray:intersects, the pre-Unity API, keeps its rule (S96 REQ-14)
    const RayReturn legacy = Ray(Vector3(0, 0, 0), Vector3(30, 0, 0)).intersects(s, RayFilter::BODY_3D);
    ASSERT_TRUE(legacy.hit);
    EXPECT_EQ(legacy.body, w.box);
    EXPECT_NEAR(legacy.distance, 0.0f, EPS);

    // mesh back faces: from under the square only with hitBackfaces
    EXPECT_FALSE(Physics::raycast(s, Vector3(0, -1, 20), Vector3(0, 1, 0), 5.0f).hit);
    QueryOptions backfaces;
    backfaces.hitBackfaces = true;
    const RayReturn under = Physics::raycast(s, Vector3(0, -1, 20), Vector3(0, 1, 0), 5.0f, backfaces);
    EXPECT_EQ(under.body, w.quad);
    EXPECT_NEAR(under.distance, 1.0f, EPS);
    Physics::setQueriesHitBackfaces(true);
    EXPECT_EQ(Physics::raycast(s, Vector3(0, -1, 20), Vector3(0, 1, 0), 5.0f).body, w.quad);
}

TEST_F(PhysicsQueries3DTest, HitFields){
    const World w = world();
    Scene* s = scene.get();
    const Entity mixed = body(Vector3(0, 0, 10), {boxCollider(Vector3(1, 1, 1)), sphereCollider(0.5f, false)});
    scene->getComponent<RigidbodyComponent>(mixed).colliders[1].position = Vector3(0, 0, 2);
    const Entity player = controller(Vector3(20, -1, 0));
    step();
    step();

    const RayReturn hit = Physics::raycast(s, Vector3(0, 0, 15), Vector3(0, 0, -1), 10.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.scene, s);
    EXPECT_FALSE(hit.body2D);
    EXPECT_EQ(hit.body, mixed);
    EXPECT_EQ(hit.colliderIndex, 1u);
    EXPECT_NEAR(hit.distance, 2.5f, EPS);
    EXPECT_TRUE(ColliderRef(s, hit.body, (int)hit.colliderIndex).isValid());

    const RayReturn cast = PhysicsQueries3D::sphereCast(s, Vector3(0, 5, 0), 0.5f, DOWN, 10.0f);
    EXPECT_EQ(cast.scene, s);
    EXPECT_EQ(cast.body, w.box);
    EXPECT_EQ(cast.colliderIndex, 0u);

    // the CharacterController's inner body: its GameObject, no Rigidbody
    const RayReturn character = Physics::raycast(s, Vector3(15, 0, 0), Vector3(1, 0, 0), 10.0f);
    ASSERT_TRUE(character.hit);
    EXPECT_EQ(character.body, player);
    EXPECT_EQ(scene->findComponent<RigidbodyComponent>(character.body), nullptr);
    EXPECT_EQ(PhysicsQueries3D::sphereCast(s, Vector3(15, 0, 0), 0.2f, Vector3(1, 0, 0), 10.0f).body, player);
    std::vector<ColliderRef> colliders;
    ASSERT_EQ(PhysicsQueries3D::overlapSphere(s, Vector3(20, 0, 0), 0.2f, colliders), 1u);
    EXPECT_EQ(colliders[0].entity, player);
    EXPECT_TRUE(colliders[0].hasGameObject());
    EXPECT_FALSE(colliders[0].isValid());
}

#endif // JASPERENGINE_PHYSICS_3D
