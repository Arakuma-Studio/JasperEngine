// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Spatial queries beside S96's (docs/specs/spatial-queries.md AC1, AC7, AC13; the queries
// themselves are S96's, tested by PhysicsQueries3DTest / PhysicsQueries2DTest):
// PhysicsQueryReference's body-by-body sweep agrees with S96's overlaps, and the query debug
// draw ring and the "Physics queries" counters see the queries.

#include "registry/PhysicsLayers.h"
#include "physics/PhysicsQueryDebug.h"
#include "physics/PhysicsQueryReference.h"
#include "jobs/JobSystem.h"
#include "Engine.h"
#include "QueryCityTestUtil.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "physics/PhysicsQueries3D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_2D
#include "physics/PhysicsQueries2D.h"
#endif

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

namespace PhysicsQueryTest_cpp {

using namespace jasperengine;

namespace {

    bool sameColliders(const std::vector<physics::QueryCollider>& a, const std::vector<physics::QueryCollider>& b, std::string& why){
        if (a.size() != b.size()){
            why = "count " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
            return false;
        }
        for (size_t i = 0; i < a.size(); i++){
            if (!(a[i] == b[i])){
                why = "hit " + std::to_string(i) + ": " + std::to_string(a[i].entity) + "/" + std::to_string(a[i].index) + " vs " +
                      std::to_string(b[i].entity) + "/" + std::to_string(b[i].index);
                return false;
            }
        }
        return true;
    }

    QueryOptions withTriggers(QueryTriggerInteraction triggers){
        QueryOptions options;
        options.triggers = triggers;
        return options;
    }

    // restores the process-wide settings a test changes
    struct QuerySettingsGuard{
        ~QuerySettingsGuard(){
            Physics::setDebugDrawQueries(false);
        }
    };

}

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    physics::QueryShape randomShape3D(std::mt19937& rng, float extent){
        std::uniform_real_distribution<float> along(-extent, extent);
        std::uniform_real_distribution<float> up(-0.5f, 4.0f);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::uniform_real_distribution<float> size(0.3f, 6.0f);
        std::uniform_int_distribution<int> kind(0, 3);
        const Vector3 at(along(rng), up(rng), along(rng));
        switch (kind(rng)){
            case 0: return physics::QueryShape::sphere(at, size(rng));
            case 1: {
                const Vector3 half(size(rng), size(rng) * 0.5f, size(rng));
                Quaternion rotation(unit(rng) + 1.5f, unit(rng), unit(rng), unit(rng));
                return physics::QueryShape::box(at, half, rotation.normalize());
            }
            case 2: {
                const Vector3 other = at + Vector3(unit(rng), unit(rng), unit(rng)) * 4.0f;
                return physics::QueryShape::capsule(at, other, size(rng) * 0.4f);
            }
            default: return physics::QueryShape::point(at);
        }
    }

    Entity boxBodyAt(Scene& scene, const Vector3& position, float half){
        const Entity entity = test::placedEntity(scene, position);
        RigidbodyComponent body;
        body.type = BodyType::STATIC;
        Collider3D collider;
        collider.type = Collider3DType::BOX;
        collider.width = collider.height = collider.depth = 2.0f * half;
        body.colliders.push_back(collider);
        scene.addComponent<RigidbodyComponent>(entity, body);
        return entity;
    }

}

// AC1: 2,000 random spheres, boxes, capsules and points over the city: S96's overlaps equal the
// body-by-body sweep, element for element, in their order
TEST(PhysicsQuery, OverlapMatchesReference){
    test::QueryCity city = test::buildQueryCity3D(2000);
    Scene* scene = city.scene.get();
    std::mt19937 rng(11);
    std::vector<physics::QueryCollider> hits;
    std::vector<physics::QueryCollider> reference;
    size_t total = 0;
    size_t withSeveral = 0;
    for (int i = 0; i < 2000; i++){
        const physics::QueryShape shape = randomShape3D(rng, city.extent);
        const QueryOptions options = withTriggers(i % 3 == 0 ? QueryTriggerInteraction::IGNORE : QueryTriggerInteraction::USE_GLOBAL);
        physics::queryOverlap(scene, shape, options, hits);
        physics::referenceOverlap(scene, shape, options, reference);
        std::string why;
        ASSERT_TRUE(sameColliders(hits, reference, why)) << "query " << i << " (type " << (int)shape.type << "): " << why;
        ASSERT_TRUE(physics::compare(hits, reference).ok()) << "query " << i;
        total += hits.size();
        withSeveral += hits.size() >= 2 ? 1 : 0;
    }
    EXPECT_GT(total, 2000u) << "the queries found too little to compare";
    ASSERT_GT(withSeveral, 0u);

    // the reference tells a collider the query left out, and one it made up (the red control)
    for (int i = 0; ; i++){
        const physics::QueryShape shape = randomShape3D(rng, city.extent);
        physics::queryOverlap(scene, shape, {}, hits);
        if (hits.size() >= 2){
            physics::referenceOverlap(scene, shape, {}, reference);
            std::vector<physics::QueryCollider> wrong(hits.begin() + 1, hits.end());
            const Entity madeUp = (Entity)~0u;
            wrong.push_back({madeUp, 0});
            const physics::ReferenceDiff diff = physics::compare(wrong, reference);
            EXPECT_FALSE(diff.ok());
            ASSERT_EQ(diff.missing.size(), 1u);
            EXPECT_TRUE(diff.missing[0] == hits[0]);
            ASSERT_EQ(diff.extra.size(), 1u);
            EXPECT_EQ(diff.extra[0].entity, madeUp);
            EXPECT_EQ(diff.referenceCount, reference.size());
            break;
        }
        ASSERT_LT(i, 1000);
    }
}

// AC13: off, nothing is recorded; on, the ring keeps the newest 256 for 1 s of unscaled time;
// queries from jobs are not recorded; every query is counted for the monitors
TEST(PhysicsQuery, DebugDrawRecordsOnlyWhenOn){
    QuerySettingsGuard guard;
    std::unique_ptr<Scene> owner;
    Scene& scene = *test::newPhysicsScene(owner);
    boxBodyAt(scene, Vector3::ZERO, 1.0f);
    test::stepScene(scene, 1);
    PhysicsSystem* physics = scene.getSystem<PhysicsSystem>().get();
    std::vector<ColliderRef> hits;

    Physics::setDebugDrawQueries(false);
    const uint64_t queriesBefore = Physics::getQueryCount();
    const uint64_t hitsBefore = Physics::getQueryHitCount();
    for (int i = 0; i < 10; i++) PhysicsQueries3D::overlapSphere(&scene, Vector3::ZERO, 2.0f, hits);
    EXPECT_FALSE(PhysicsQueries3D::checkSphere(&scene, Vector3(0, 10, 0), 0.5f));
    EXPECT_FALSE(PhysicsQueries3D::sphereCast(&scene, Vector3(0, 10, 0), 0.5f, Vector3(0, 1, 0), 5.0f).hit);
    EXPECT_TRUE(Physics::raycast(&scene, Vector3(0, 10, 0), Vector3(0, -1, 0), 20.0f).hit);
    EXPECT_EQ(physics->findQueryDebug(), nullptr) << "off: the ring is never made";
    EXPECT_EQ(Physics::getQueryCount() - queriesBefore, 13u) << "overlaps, a check, a cast and a raycast count";
    EXPECT_EQ(Physics::getQueryHitCount() - hitsBefore, 11u);

    Physics::setDebugDrawQueries(true);
    jobs::parallelFor(4, [&](size_t){
        std::vector<ColliderRef> local;
        PhysicsQueries3D::overlapSphere(&scene, Vector3::ZERO, 2.0f, local);
    }, jobs::ParallelForOptions{.grain = 1});
    const PhysicsQueryDebug* ring = physics->findQueryDebug();
    EXPECT_TRUE(!ring || !ring->allocated()) << "queries from jobs are not recorded";

    const double now = Engine::getUnscaledTime();
    for (int i = 0; i < 300; i++) PhysicsQueries3D::overlapSphere(&scene, Vector3((float)i, 0, 0), 2.0f, hits);
    ring = physics->findQueryDebug();
    ASSERT_NE(ring, nullptr);
    EXPECT_EQ(ring->liveCount(now), PhysicsQueryDebug::CAPACITY);
    // the oldest kept is query 300 - 256 = 44
    float first = -1.0f;
    ring->forEachLive(now, [&](const PhysicsQueryDebug::Entry& entry){ if (first < 0.0f) first = entry.shape.a.x; });
    EXPECT_EQ(first, 44.0f);
    EXPECT_EQ(ring->liveCount(now + 0.9), PhysicsQueryDebug::CAPACITY);
    EXPECT_EQ(ring->liveCount(now + 1.01), 0u) << "expired after 1 s";

    // a hit overlap marks the GameObject it found; a cast keeps its sweep
    PhysicsQueries3D::overlapSphere(&scene, Vector3::ZERO, 2.0f, hits);
    PhysicsQueries3D::sphereCast(&scene, Vector3(-6, 0, 0), 0.5f, Vector3(1, 0, 0), 10.0f);
    std::vector<PhysicsQueryDebug::Entry> newest;
    ring->forEachLive(Engine::getUnscaledTime(), [&](const PhysicsQueryDebug::Entry& entry){ newest.push_back(entry); });
    ASSERT_GE(newest.size(), 2u);
    const PhysicsQueryDebug::Entry& overlap = newest[newest.size() - 2];
    EXPECT_TRUE(overlap.hit);
    EXPECT_FALSE(overlap.cast);
    ASSERT_EQ(overlap.hitPoints, 1);
    EXPECT_NEAR(overlap.points[0].x, 0.0f, 1e-4f);
    const PhysicsQueryDebug::Entry& cast = newest.back();
    EXPECT_TRUE(cast.cast);
    EXPECT_TRUE(cast.hit);
    EXPECT_NEAR(cast.distance, 4.5f, 1e-3f) << "the sphere's centre stops 0.5 short of the box at x = -1";
    EXPECT_NEAR(cast.direction.x, 1.0f, 1e-5f);

    // the ring's own clock: an idle ring off for 10 s is freed
    PhysicsQueryDebug own;
    PhysicsQueryDebug::Entry entry;
    own.record(entry, 100.0, PhysicsQueryDebug::LIFETIME);
    EXPECT_EQ(own.liveCount(100.5), 1u);
    own.releaseIfIdle(103.0, false);
    own.releaseIfIdle(112.0, false);
    EXPECT_TRUE(own.allocated());
    own.releaseIfIdle(113.5, false);
    EXPECT_FALSE(own.allocated());
}

#endif // JASPERENGINE_PHYSICS_3D

#ifdef JASPERENGINE_PHYSICS_2D

// AC7: every 2D overlap shape against the sweep, triggers by the interaction, overlapArea with
// swapped corners
TEST(PhysicsQuery2D, OverlapsMatchReference){
    test::QueryCity city = test::buildQueryCity2D(2000);
    Scene* scene = city.scene.get();
    std::mt19937 rng(13);
    std::uniform_real_distribution<float> along(-city.extent, city.extent);
    std::uniform_real_distribution<float> size(16.0f, 400.0f);
    std::uniform_real_distribution<float> angle(-90.0f, 90.0f);
    std::vector<physics::QueryCollider> hits;
    std::vector<physics::QueryCollider> reference;
    size_t total = 0;
    for (int i = 0; i < 2000; i++){
        const Vector2 a(along(rng), along(rng));
        physics::QueryShape shape;
        switch (i % 5){
            case 0: shape = physics::QueryShape::point2D(a); break;
            case 1: shape = physics::QueryShape::circle2D(a, size(rng)); break;
            case 2: shape = physics::QueryShape::box2D(a, Vector2(size(rng), size(rng)), angle(rng)); break;
            case 3: {
                const Vector2 extent(size(rng), size(rng));
                shape = physics::QueryShape::capsule2D(a, extent, i % 2 ? CapsuleDirection2D::VERTICAL : CapsuleDirection2D::HORIZONTAL, angle(rng));
                break;
            }
            default: shape = physics::QueryShape::area2D(a, a + Vector2(size(rng), -size(rng))); break;
        }
        const QueryOptions options = withTriggers(i % 3 == 0 ? QueryTriggerInteraction::IGNORE : QueryTriggerInteraction::COLLIDE);
        physics::queryOverlap(scene, shape, options, hits);
        physics::referenceOverlap(scene, shape, options, reference);
        std::string why;
        ASSERT_TRUE(sameColliders(hits, reference, why)) << "query " << i << " (type " << (int)shape.type << "): " << why;
        total += hits.size();
        if (shape.type == physics::QueryShape::Type::Area2D){
            std::vector<physics::QueryCollider> swapped;
            physics::QueryShape turned = physics::QueryShape::area2D(Vector2(shape.b.x, shape.b.y), a);
            physics::referenceOverlap(scene, turned, options, swapped);
            ASSERT_TRUE(sameColliders(swapped, hits, why)) << "swapped corners: " << why;
        }
    }
    EXPECT_GT(total, 1000u);
}

#endif // JASPERENGINE_PHYSICS_2D

}  // namespace PhysicsQueryTest_cpp
