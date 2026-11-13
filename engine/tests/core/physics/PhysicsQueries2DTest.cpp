// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The 2D shape queries (docs/specs/S96-physics-gaps.md REQ-11, REQ-12; AC-13): circle, box
// (turned) and capsule casts and the Overlap* family on Box2D shapes, in points at 64 and at 1
// point a metre; startInColliders off (today's rule, no hit from inside a polygon) and on (a hit
// at 0, normal -direction); triggers, the ignore list and Rigidbody2D.Cast. Box2D casts stop a
// linear slop (5 mm) before contact: distances are checked to 1 cm.

#ifdef JASPERENGINE_PHYSICS_2D

#include "Engine.h"
#include "Scene.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Transform.h"
#include "math/Ray.h"
#include "physics/PhysicsQueries2D.h"
#include "registry/PhysicsLayers.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"
#include "../ecs/EcsTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float DT = 1.0f / 60.0f;

    // the points a metre of each run
    class PhysicsQueries2DTest : public ::testing::TestWithParam<float> {
    protected:
        std::unique_ptr<Scene> scene;
        float updateTime = 0.0f;
        float M = 64.0f;
        bool startInColliders = false;
        bool hitTriggers = true;

        void SetUp() override{
            M = GetParam();
            updateTime = Engine::getUpdateTime();
            Engine::setUpdateTime(DT);
            startInColliders = Physics2D::getQueriesStartInColliders();
            hitTriggers = Physics2D::getQueriesHitTriggers();
            scene = std::make_unique<Scene>();
            test::disableSubSystems(*scene);
            scene->getScheduler().setEnabled("PhysicsSystem", true);
            PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();
            physics->setPointsToMeterScale2D(M);
            physics->setGravity2D(0.0f, 0.0f);
        }

        void TearDown() override{
            scene.reset();
            Engine::setUpdateTime(updateTime);
            Physics2D::setQueriesStartInColliders(startInColliders);
            Physics2D::setQueriesHitTriggers(hitTriggers);
        }

        float tolerance() const{
            return 0.01f * M;
        }

        // a box of a size (metres) centred at a position (metres)
        Entity box(Vector2 position, Vector2 size, BodyType type = BodyType::STATIC, bool sensor = false){
            const Entity entity = test::makeTransformEntity(*scene);
            Transform& transform = scene->getComponent<Transform>(entity);
            transform.position = Vector3(position.x * M, position.y * M, 0.0f);
            transform.needUpdate = true;
            Rigidbody2DComponent rigidbody;
            rigidbody.type = type;
            rigidbody.colliders.resize(1);
            rigidbody.colliders[0].type = Collider2DType::POLYGON;
            rigidbody.colliders[0].pointA = Vector2(-size.x * 0.5f * M, -size.y * 0.5f * M);
            rigidbody.colliders[0].pointB = Vector2(size.x * 0.5f * M, size.y * 0.5f * M);
            rigidbody.colliders[0].sensor = sensor;
            scene->addComponent<Rigidbody2DComponent>(entity, rigidbody);
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

        Vector2 at(float x, float y) const{
            return Vector2(x * M, y * M);
        }

        void expectHit(const RayReturn& hit, Entity body, float metres, Vector2 normal, const char* what){
            ASSERT_TRUE(hit.hit) << what;
            EXPECT_EQ(hit.body, body) << what;
            EXPECT_NEAR(hit.distance, metres * M, tolerance()) << what;
            EXPECT_NEAR(hit.normal.x, normal.x, 1.0e-3f) << what;
            EXPECT_NEAR(hit.normal.y, normal.y, 1.0e-3f) << what;
            EXPECT_TRUE(hit.body2D) << what;
            EXPECT_EQ(hit.scene, scene.get()) << what;
        }
    };

}

TEST_P(PhysicsQueries2DTest, CastsAndOverlaps){
    Scene* s = scene.get();
    // the ground's top at y = 0, a 1 m crate at (5, 2)
    const Entity ground = box(Vector2(0, -0.5f), Vector2(20, 1));
    const Entity crate = box(Vector2(5, 2), Vector2(1, 1));
    step();
    const Vector2 down(0, -1);
    const Vector2 up(0, 1);

    expectHit(PhysicsQueries2D::circleCast(s, at(0, 3), 0.5f * M, down, 10 * M), ground, 2.5f, up, "circleCast");
    const RayReturn circle = PhysicsQueries2D::circleCast(s, at(0, 3), 0.5f * M, down, 10 * M);
    EXPECT_NEAR(circle.point.y, 0.0f, tolerance());
    // a 1 m box turned 45 degrees comes down on its corner
    expectHit(PhysicsQueries2D::boxCast(s, at(0, 3), at(1, 1), Angle::degToDefault(45.0f), down, 10 * M), ground,
              3.0f - std::sqrt(0.5f), up, "turned boxCast");
    expectHit(PhysicsQueries2D::boxCast(s, at(0, 3), at(1, 1), 0.0f, down, 10 * M), ground, 2.5f, up, "boxCast");
    expectHit(PhysicsQueries2D::capsuleCast(s, at(0, 3), at(1, 2), CapsuleDirection2D::VERTICAL, 0.0f, down, 10 * M), ground, 2.0f, up, "vertical capsuleCast");
    expectHit(PhysicsQueries2D::capsuleCast(s, at(0, 3), at(2, 1), CapsuleDirection2D::HORIZONTAL, 0.0f, down, 10 * M), ground, 2.5f, up, "horizontal capsuleCast");
    // turned 90 degrees the vertical capsule lies down
    expectHit(PhysicsQueries2D::capsuleCast(s, at(0, 3), at(1, 2), CapsuleDirection2D::VERTICAL, Angle::degToDefault(90.0f), down, 10 * M), ground,
              2.5f, up, "turned capsuleCast");
    EXPECT_FALSE(PhysicsQueries2D::circleCast(s, at(0, 3), 0.5f * M, down, 2 * M).hit);

    // ...All: the crate, then the ground, each once, nearest first
    std::vector<RayReturn> hits;
    ASSERT_EQ(PhysicsQueries2D::circleCastAll(s, at(5, 4), 0.25f * M, down, 10 * M, hits), 2u);
    expectHit(hits[0], crate, 4.0f - 2.5f - 0.25f, up, "all[0]");
    expectHit(hits[1], ground, 4.0f - 0.25f, up, "all[1]");
    EXPECT_EQ(PhysicsQueries2D::boxCastAll(s, at(5, 4), at(0.5f, 0.5f), 0.0f, down, 10 * M, hits), 2u);
    EXPECT_EQ(PhysicsQueries2D::capsuleCastAll(s, at(5, 4), at(0.5f, 1), CapsuleDirection2D::VERTICAL, 0.0f, down, 10 * M, hits), 2u);

    // overlaps: the first by GameObject, or all
    ColliderRef2D first;
    ASSERT_TRUE(PhysicsQueries2D::overlapCircle(s, at(5, 1.0f), 1.25f * M, first));
    EXPECT_EQ(first.entity, std::min(ground, crate));
    std::vector<ColliderRef2D> colliders;
    EXPECT_EQ(PhysicsQueries2D::overlapCircleAll(s, at(5, 1.0f), 1.25f * M, colliders), 2u);
    EXPECT_EQ(PhysicsQueries2D::overlapCircleAll(s, at(5, 1.0f), 0.25f * M, colliders), 0u);
    EXPECT_EQ(PhysicsQueries2D::overlapBoxAll(s, at(5, 1), at(0.5f, 3), 0.0f, colliders), 2u);
    // the same thin box turned 90 degrees lies between them
    EXPECT_EQ(PhysicsQueries2D::overlapBoxAll(s, at(5, 1), at(0.5f, 3), Angle::degToDefault(90.0f), colliders), 0u);
    EXPECT_TRUE(PhysicsQueries2D::overlapCapsule(s, at(5, 1), at(0.5f, 3), CapsuleDirection2D::VERTICAL, 0.0f, first));
    EXPECT_EQ(PhysicsQueries2D::overlapCapsuleAll(s, at(5, 1), at(0.5f, 3), CapsuleDirection2D::HORIZONTAL, 0.0f, colliders), 0u);
    ASSERT_TRUE(PhysicsQueries2D::overlapPoint(s, at(5.25f, 2.25f), first));
    EXPECT_EQ(first.entity, crate);
    EXPECT_FALSE(PhysicsQueries2D::overlapPoint(s, at(5, 1), first));
    EXPECT_EQ(first.entity, NULL_ENTITY);
    EXPECT_EQ(PhysicsQueries2D::overlapPointAll(s, at(0, -0.5f), colliders), 1u);
    EXPECT_EQ(PhysicsQueries2D::overlapAreaAll(s, at(4, -0.25f), at(6, 3), colliders), 2u);
    EXPECT_TRUE(PhysicsQueries2D::overlapArea(s, at(6, 3), at(4.75f, 2.25f), first));
    EXPECT_EQ(first.entity, crate);

    // Rigidbody2D.Cast: the body's shapes, never itself
    const Entity mover = box(Vector2(5, 4), Vector2(1, 1), BodyType::KINEMATIC);
    step();
    expectHit(PhysicsQueries2D::cast(s, mover, down, 10 * M), crate, 1.0f, up, "Rigidbody2D.cast");
    ASSERT_EQ(PhysicsQueries2D::castAll(s, mover, down, 10 * M, hits), 2u);
    EXPECT_EQ(hits[1].body, ground);
}

TEST_P(PhysicsQueries2DTest, StartInColliders){
    Scene* s = scene.get();
    const Entity crate = box(Vector2(5, 2), Vector2(1, 1));
    const Entity wall = box(Vector2(8, 2), Vector2(1, 4));
    step();
    const Vector2 right(1, 0);

    // today's rule, now named startInColliders = false: a ray from inside the crate does not
    // hit it (Box2D leaves a polygon without a hit), the wall behind it is the hit
    Physics2D::setQueriesStartInColliders(false);
    RayReturn hit = Physics2D::raycast(s, at(5, 2), right, 10 * M);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, wall);
    EXPECT_NEAR(hit.distance, 2.5f * M, tolerance());
    EXPECT_EQ(Physics2D::raycastAll(s, at(5, 2), right, 10 * M).size(), 1u);
    EXPECT_EQ(PhysicsQueries2D::circleCast(s, at(5, 2), 0.1f * M, right, 10 * M).body, wall);

    // startInColliders: the crate at 0, normal -direction
    QueryOptions inside;
    inside.startInColliders = true;
    hit = Physics2D::raycast(s, at(5, 2), right, 10 * M, inside);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, crate);
    EXPECT_NEAR(hit.distance, 0.0f, 1.0e-4f);
    EXPECT_NEAR(hit.normal.x, -1.0f, 1.0e-4f);
    EXPECT_NEAR(hit.point.x, 5 * M, 1.0e-3f * M);
    const std::vector<RayReturn> all = Physics2D::raycastAll(s, at(5, 2), right, 10 * M, inside);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].body, crate);
    EXPECT_EQ(all[1].body, wall);
    const RayReturn circle = PhysicsQueries2D::circleCast(s, at(5, 2), 0.1f * M, right, 10 * M, inside);
    EXPECT_EQ(circle.body, crate);
    EXPECT_NEAR(circle.distance, 0.0f, 1.0e-4f);
    // the global default
    Physics2D::setQueriesStartInColliders(true);
    EXPECT_EQ(Physics2D::raycast(s, at(5, 2), right, 10 * M).body, crate);
    // the Ray form too
    EXPECT_EQ(Physics2D::raycast(s, Ray(Vector3(5 * M, 2 * M, 0), Vector3(10 * M, 0, 0))).body, crate);
}

TEST_P(PhysicsQueries2DTest, TriggersAndIgnore){
    Scene* s = scene.get();
    const Entity ground = box(Vector2(0, -0.5f), Vector2(20, 1));
    const Entity zone = box(Vector2(0, 2), Vector2(4, 1), BodyType::STATIC, true);
    step();
    const Vector2 down(0, -1);

    QueryOptions collide, ignoreTriggers, global;
    collide.triggers = QueryTriggerInteraction::COLLIDE;
    ignoreTriggers.triggers = QueryTriggerInteraction::IGNORE;
    EXPECT_EQ(PhysicsQueries2D::circleCast(s, at(0, 4), 0.25f * M, down, 10 * M, collide).body, zone);
    EXPECT_EQ(PhysicsQueries2D::circleCast(s, at(0, 4), 0.25f * M, down, 10 * M, ignoreTriggers).body, ground);
    EXPECT_EQ(Physics2D::raycast(s, at(0, 4), down, 10 * M, ignoreTriggers).body, ground);
    Physics2D::setQueriesHitTriggers(false);
    EXPECT_EQ(PhysicsQueries2D::circleCast(s, at(0, 4), 0.25f * M, down, 10 * M, global).body, ground);
    ColliderRef2D first;
    EXPECT_FALSE(PhysicsQueries2D::overlapPoint(s, at(0, 2), first, global));
    Physics2D::setQueriesHitTriggers(true);
    EXPECT_TRUE(PhysicsQueries2D::overlapPoint(s, at(0, 2), first, global));
    EXPECT_EQ(first.entity, zone);
    EXPECT_EQ(Physics2D::raycast(s, at(0, 4), down, 10 * M, global).body, zone);

    // the ignore list and the layer mask
    const std::vector<Entity> ignore{zone};
    QueryOptions ignoring;
    ignoring.ignore = &ignore;
    EXPECT_EQ(PhysicsQueries2D::circleCast(s, at(0, 4), 0.25f * M, down, 10 * M, ignoring).body, ground);
    EXPECT_EQ(Physics2D::raycast(s, at(0, 4), down, 10 * M, ignoring).body, ground);
    scene->setEntityLayer(ground, 9);
    step();
    QueryOptions masked = ignoring;
    masked.layerMask = ~(1u << 9);
    EXPECT_FALSE(PhysicsQueries2D::circleCast(s, at(0, 4), 0.25f * M, down, 10 * M, masked).hit);
    std::vector<ColliderRef2D> colliders;
    EXPECT_EQ(PhysicsQueries2D::overlapAreaAll(s, at(-5, -1), at(5, 5), colliders, masked), 0u);
}

INSTANTIATE_TEST_SUITE_P(PointsPerMeter, PhysicsQueries2DTest, ::testing::Values(64.0f, 1.0f));

#endif // JASPERENGINE_PHYSICS_2D
