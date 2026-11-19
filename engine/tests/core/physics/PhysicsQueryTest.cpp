// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Spatial queries (docs/specs/spatial-queries.md AC1-AC9, AC13): the overlaps, checks and casts
// of Physics and Physics2D against PhysicsQueryReference's body-by-body sweep, their trigger,
// layer and ignore filters, the order of their results, their allocations, jobs, the physics
// step, and the debug draw ring.

#include "registry/PhysicsLayers.h"
#include "physics/PhysicsQueryDebug.h"
#include "physics/PhysicsQueryReference.h"
#include "jobs/JobSystem.h"
#include "Engine.h"
#include "System.h"
#include "LogCapture.h"
#include "QueryCityTestUtil.h"
#include "support/AllocationCounter.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "component/CharacterControllerComponent.h"
#include "object/physics/CollideShapeResult3D.h"
#include "util/JoltPhysicsAux.h"
#include "Jolt/Physics/Collision/CollideShape.h"
#include "Jolt/Physics/Collision/CollisionCollectorImpl.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    bool sameHits(const std::vector<OverlapHit>& a, const std::vector<OverlapHit>& b, std::string& why){
        if (a.size() != b.size()){
            why = "count " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
            return false;
        }
        for (size_t i = 0; i < a.size(); i++){
            if (a[i].body != b[i].body || a[i].colliderIndex != b[i].colliderIndex || a[i].trigger != b[i].trigger){
                why = "hit " + std::to_string(i) + ": " + std::to_string(a[i].body) + "/" + std::to_string(a[i].colliderIndex) + " vs " +
                      std::to_string(b[i].body) + "/" + std::to_string(b[i].colliderIndex);
                return false;
            }
        }
        return true;
    }

    // R6: by (body, colliderIndex), each once
    bool inQueryOrder(const std::vector<OverlapHit>& hits){
        for (size_t i = 1; i < hits.size(); i++){
            const OverlapHit& a = hits[i - 1];
            const OverlapHit& b = hits[i];
            if (a.body > b.body || (a.body == b.body && a.colliderIndex >= b.colliderIndex)){
                return false;
            }
        }
        return true;
    }

    bool hasBody(const std::vector<OverlapHit>& hits, Entity body){
        for (const OverlapHit& hit : hits) if (hit.body == body) return true;
        return false;
    }

    PhysicsQueryOptions withTriggers(QueryTriggerInteraction triggers){
        PhysicsQueryOptions options;
        options.triggers = triggers;
        return options;
    }

    // restores the process-wide settings a test changes
    struct QuerySettingsGuard{
        ~QuerySettingsGuard(){
            Physics::setQueriesHitTriggers(true);
            Physics2D::setQueriesHitTriggers(true);
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
        std::uniform_int_distribution<int> kind(0, 2);
        physics::QueryShape shape;
        shape.a = Vector3(along(rng), up(rng), along(rng));
        switch (kind(rng)){
            case 0: shape.type = physics::QueryShape::Type::Sphere; shape.radius = size(rng); break;
            case 1: {
                shape.type = physics::QueryShape::Type::Box;
                shape.halfExtents = Vector3(size(rng), size(rng) * 0.5f, size(rng));
                Quaternion rotation(unit(rng) + 1.5f, unit(rng), unit(rng), unit(rng));
                shape.rotation = rotation.normalize();
                break;
            }
            default:
                shape.type = physics::QueryShape::Type::Capsule;
                shape.b = shape.a + Vector3(unit(rng), unit(rng), unit(rng)) * 4.0f;
                shape.radius = size(rng) * 0.4f;
                break;
        }
        return shape;
    }

    Entity bodyAt(Scene& scene, const Vector3& position, BodyType type, std::vector<Collider3D> colliders, int layer = 0,
                  uint16_t categoryBits = 1, uint16_t maskBits = 0xFFFF){
        const Entity entity = test::placedEntity(scene, position);
        if (layer != 0) scene.setEntityLayer(entity, layer);
        RigidbodyComponent body;
        body.type = type;
        body.colliders = std::move(colliders);
        body.categoryBits = categoryBits;
        body.maskBits = maskBits;
        scene.addComponent<RigidbodyComponent>(entity, body);
        return entity;
    }

    Collider3D boxCollider(float half, bool trigger = false, Vector3 offset = Vector3::ZERO){
        Collider3D collider;
        collider.type = Collider3DType::BOX;
        collider.width = collider.height = collider.depth = 2.0f * half;
        collider.isTrigger = trigger;
        collider.position = offset;
        return collider;
    }

}

// AC1: 2,000 random spheres, boxes and capsules over the city equal the body-by-body sweep,
// element for element, in R6's order
TEST(PhysicsQuery, OverlapMatchesReference){
    QuerySettingsGuard guard;
    test::QueryCity city = test::buildQueryCity3D(2000);
    Scene* scene = city.scene.get();
    std::mt19937 rng(11);
    std::vector<OverlapHit> hits;
    std::vector<OverlapHit> reference;
    size_t total = 0;
    size_t triggers = 0;
    size_t withSeveral = 0;
    for (int i = 0; i < 2000; i++){
        const physics::QueryShape shape = randomShape3D(rng, city.extent);
        const PhysicsQueryOptions options = withTriggers(i % 3 == 0 ? QueryTriggerInteraction::Ignore : QueryTriggerInteraction::UseGlobal);
        physics::queryOverlap(scene, shape, options, hits);
        physics::referenceOverlap(scene, false, shape, options, reference);
        std::string why;
        ASSERT_TRUE(sameHits(hits, reference, why)) << "query " << i << ": " << why;
        ASSERT_TRUE(inQueryOrder(hits)) << "query " << i;
        total += hits.size();
        for (const OverlapHit& hit : hits) triggers += hit.trigger ? 1 : 0;
        withSeveral += hits.size() >= 2 ? 1 : 0;
    }
    EXPECT_GT(total, 2000u) << "the queries found too little to compare";
    EXPECT_GT(triggers, 0u);
    ASSERT_GT(withSeveral, 0u);

    // the order check catches an unsorted result (the claim's red control)
    for (int i = 0; ; i++){
        physics::queryOverlap(scene, randomShape3D(rng, city.extent), {}, hits);
        if (hits.size() >= 2){
            std::vector<OverlapHit> reversed(hits.rbegin(), hits.rend());
            EXPECT_FALSE(inQueryOrder(reversed));
            std::string why;
            EXPECT_FALSE(sameHits(reversed, hits, why));
            break;
        }
        ASSERT_LT(i, 1000);
    }
}

// AC2: Ignore, Collide and UseGlobal (both ways), for overlaps and raycasts
TEST(PhysicsQuery, TriggersFollowInteraction){
    QuerySettingsGuard guard;
    std::unique_ptr<Scene> owner;
    Scene& scene = *test::newPhysicsScene(owner);
    const Entity solid = bodyAt(scene, Vector3(-3, 0, 0), BodyType::STATIC, {boxCollider(0.5f)});
    const Entity trigger = bodyAt(scene, Vector3(0, 0, 0), BodyType::STATIC, {boxCollider(0.5f, true)});
    // a solid collider and a trigger one: the trigger lives on a companion sensor body
    const Entity both = bodyAt(scene, Vector3(3, 0, 0), BodyType::DYNAMIC, {boxCollider(0.5f), boxCollider(0.5f, true, Vector3(0, 0, 2))});
    scene.getSystem<PhysicsSystem>()->setGravity3D(0, 0, 0);
    test::stepScene(scene, 1);

    std::vector<OverlapHit> hits;
    const Vector3 half(6, 1, 4);
    Physics::overlapBox(&scene, Vector3(0, 0, 1), half, Quaternion::IDENTITY, hits, withTriggers(QueryTriggerInteraction::Ignore));
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_TRUE(hasBody(hits, solid));
    EXPECT_TRUE(hasBody(hits, both));
    for (const OverlapHit& hit : hits) EXPECT_FALSE(hit.trigger);

    Physics::overlapBox(&scene, Vector3(0, 0, 1), half, Quaternion::IDENTITY, hits, withTriggers(QueryTriggerInteraction::Collide));
    ASSERT_EQ(hits.size(), 4u) << "the solid, the trigger, and both colliders of the mixed body";
    EXPECT_TRUE(hasBody(hits, trigger));
    size_t ofBoth = 0;
    for (const OverlapHit& hit : hits) ofBoth += hit.body == both ? 1 : 0;
    EXPECT_EQ(ofBoth, 2u);

    Physics::setQueriesHitTriggers(false);
    EXPECT_EQ(Physics::overlapBox(&scene, Vector3(0, 0, 1), half, Quaternion::IDENTITY, hits), 2u);
    EXPECT_FALSE(Physics::checkSphere(&scene, Vector3(0, 0, 0), 0.3f));
    Physics::setQueriesHitTriggers(true);
    EXPECT_EQ(Physics::overlapBox(&scene, Vector3(0, 0, 1), half, Quaternion::IDENTITY, hits), 4u);
    EXPECT_TRUE(Physics::checkSphere(&scene, Vector3(0, 0, 0), 0.3f));

    // raycast down onto the trigger: hit by default, passed through with Ignore
    const Vector3 above(0, 5, 0);
    const Vector3 down(0, -1, 0);
    EXPECT_EQ(Physics::raycast(&scene, above, down, 10.0f).body, trigger);
    EXPECT_FALSE(Physics::raycast(&scene, above, down, 10.0f, Physics::DEFAULT_RAYCAST_LAYERS, QueryTriggerInteraction::Ignore).hit);
    Physics::setQueriesHitTriggers(false);
    EXPECT_FALSE(Physics::raycast(&scene, above, down, 10.0f).hit);
    EXPECT_TRUE(Physics::raycast(&scene, above, down, 10.0f, Physics::DEFAULT_RAYCAST_LAYERS, QueryTriggerInteraction::Collide).hit);
    EXPECT_TRUE(Physics::raycastAll(&scene, above, down, 10.0f).empty());
}

// AC3: raycast and raycastAll through the city give what the pre-change Ray::cast gave (the
// fixture was written by this test on the tree without the change, JASPER_TEST_UPDATE_GOLDEN=1)
TEST(PhysicsQuery, RaycastUnchangedByDefault){
    test::QueryCity city = test::buildQueryCity3D(2000);
    Scene* scene = city.scene.get();
    std::mt19937 rng(23);
    std::uniform_real_distribution<float> along(-city.extent, city.extent);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::ostringstream lines;
    char line[256];
    for (int i = 0; i < 1000; i++){
        const Vector3 origin(along(rng), 6.0f, along(rng));
        const Vector3 direction(unit(rng), -1.0f, unit(rng));
        const RayReturn hit = Physics::raycast(scene, origin, direction, 40.0f);
        const std::vector<RayReturn> all = Physics::raycastAll(scene, origin, direction, 40.0f);
        std::snprintf(line, sizeof(line), "%d %d %u %zu %.4f %zu\n", i, hit.hit ? 1 : 0, (unsigned)hit.body, hit.colliderIndex, hit.hit ? hit.distance : 0.0f, all.size());
        lines << line;
        for (const RayReturn& each : all){
            std::snprintf(line, sizeof(line), "  %u %zu %.4f\n", (unsigned)each.body, each.colliderIndex, each.distance);
            lines << line;
        }
    }
    const std::string path = JASPER_TEST_REPO_DIR "/engine/tests/core/fixtures/physics/ray_city_hits.txt";
    if (std::getenv("JASPER_TEST_UPDATE_GOLDEN")){
        std::ofstream(path) << lines.str();
        GTEST_SKIP() << "wrote " << path;
    }
    std::ifstream file(path);
    ASSERT_TRUE(file.good()) << path;
    std::stringstream expected;
    expected << file.rdbuf();
    // line by line, so a difference names its ray
    std::istringstream got(lines.str());
    std::istringstream want(expected.str());
    std::string a, b;
    int number = 0;
    while (std::getline(want, b)){
        number++;
        ASSERT_TRUE(std::getline(got, a)) << "line " << number << " missing";
        ASSERT_EQ(a, b) << "line " << number;
    }
    EXPECT_FALSE(std::getline(got, a)) << "more lines than the fixture";
}

// AC4: layer mask, category and mask bits, the ignore list, no world, character controllers,
// sleeping bodies
TEST(PhysicsQuery, LayerAndIgnore){
    QuerySettingsGuard guard;
    {
        // a scene with no 3D body has no world, and a query does not make one
        std::unique_ptr<Scene> owner;
        Scene& empty = *test::newPhysicsScene(owner);
        std::vector<OverlapHit> hits;
        EXPECT_EQ(Physics::overlapSphere(&empty, Vector3::ZERO, 10.0f, hits), 0u);
        EXPECT_FALSE(Physics::checkBox(&empty, Vector3::ZERO, Vector3(5, 5, 5), Quaternion::IDENTITY));
        EXPECT_FALSE(Physics::sphereCast(&empty, Vector3::ZERO, 1.0f, Vector3(1, 0, 0), 10.0f).hit);
        EXPECT_FALSE(empty.getSystem<PhysicsSystem>()->hasWorld3D());
    }

    std::unique_ptr<Scene> owner;
    Scene& scene = *test::newPhysicsScene(owner);
    scene.getSystem<PhysicsSystem>()->setGravity3D(0, 0, 0);
    const Entity plain = bodyAt(scene, Vector3(-2, 0, 0), BodyType::STATIC, {boxCollider(0.5f)});
    const Entity enemy = bodyAt(scene, Vector3(2, 0, 0), BodyType::STATIC, {boxCollider(0.5f)}, 8);
    const Entity unmasked = bodyAt(scene, Vector3(0, 0, 2), BodyType::STATIC, {boxCollider(0.5f)}, 0, 1, 0);
    const Entity sleeper = bodyAt(scene, Vector3(0, 0, -2), BodyType::DYNAMIC, {boxCollider(0.5f)});
    const Entity seen = test::placedEntity(scene, Vector3(4, 0, 4));
    CharacterControllerComponent controller;
    scene.addComponent<CharacterControllerComponent>(seen, controller);
    const Entity unseen = test::placedEntity(scene, Vector3(-4, 0, 4));
    controller.detectCollisions = false;
    scene.addComponent<CharacterControllerComponent>(unseen, controller);
    test::stepScene(scene, 1);
    Rigidbody(&scene, sleeper).sleep();
    test::stepScene(scene, 1);
    ASSERT_TRUE(Rigidbody(&scene, sleeper).isSleeping());

    std::vector<OverlapHit> hits;
    Physics::overlapSphere(&scene, Vector3::ZERO, 10.0f, hits);
    EXPECT_TRUE(hasBody(hits, plain));
    EXPECT_TRUE(hasBody(hits, enemy));
    EXPECT_TRUE(hasBody(hits, sleeper)) << "a sleeping body is found";
    EXPECT_FALSE(hasBody(hits, unmasked)) << "a collider whose mask bits meet nothing is not";
    EXPECT_TRUE(hasBody(hits, seen)) << "a Character Controller with Detect Collisions";
    EXPECT_FALSE(hasBody(hits, unseen)) << "a Character Controller without Detect Collisions";

    PhysicsQueryOptions onlyEnemies;
    onlyEnemies.layerMask = 1u << 8;
    Physics::overlapSphere(&scene, Vector3::ZERO, 10.0f, hits, onlyEnemies);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].body, enemy);

    const std::vector<Entity> ignore = {plain, sleeper};
    PhysicsQueryOptions ignoring;
    ignoring.ignore = ignore;
    Physics::overlapSphere(&scene, Vector3::ZERO, 10.0f, hits, ignoring);
    EXPECT_FALSE(hasBody(hits, plain));
    EXPECT_FALSE(hasBody(hits, sleeper));
    EXPECT_TRUE(hasBody(hits, enemy));
    EXPECT_FALSE(Physics::sphereCast(&scene, Vector3(-6, 0, 0), 0.2f, Vector3(1, 0, 0), 3.5f, ignoring).hit);
}

// AC5: nearest hit with its distance and a normal facing the cast; a start-inside collider is
// not reported; the All form in distance order; check agrees with overlap
TEST(PhysicsQuery, CastsHitNearestAndSkipStartInside){
    std::unique_ptr<Scene> owner;
    Scene& scene = *test::newPhysicsScene(owner);
    const Entity inside = bodyAt(scene, Vector3(0, 0, 0), BodyType::STATIC, {boxCollider(1.0f)});
    const Entity near = bodyAt(scene, Vector3(5, 0, 0), BodyType::STATIC, {boxCollider(0.5f)});
    const Entity middle = bodyAt(scene, Vector3(10, 0, 0), BodyType::STATIC, {boxCollider(0.5f)});
    const Entity far = bodyAt(scene, Vector3(15, 0, 0), BodyType::STATIC, {boxCollider(0.5f)});
    test::stepScene(scene, 1);

    const RayReturn hit = Physics::sphereCast(&scene, Vector3(0, 0, 0), 0.5f, Vector3(2, 0, 0), 30.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, near) << "the box the sphere starts in is skipped";
    // the sphere's surface meets the box's face at x = 4.5: its centre moved 4.0
    EXPECT_NEAR(hit.distance, 4.0f, 1e-3f);
    EXPECT_NEAR(hit.normal.x, -1.0f, 1e-3f);
    EXPECT_NEAR(hit.point.x, 4.5f, 1e-3f);

    std::vector<RayReturn> all;
    ASSERT_EQ(Physics::sphereCastAll(&scene, Vector3(0, 0, 0), 0.5f, Vector3(1, 0, 0), 30.0f, all), 3u);
    EXPECT_EQ(all[0].body, near);
    EXPECT_EQ(all[1].body, middle);
    EXPECT_EQ(all[2].body, far);
    EXPECT_LT(all[0].distance, all[1].distance);
    EXPECT_LT(all[1].distance, all[2].distance);
    for (const RayReturn& each : all) EXPECT_NE(each.body, inside);

    const RayReturn box = Physics::boxCast(&scene, Vector3(0, 0, 0), Vector3(0.25f, 0.25f, 0.25f), Vector3(1, 0, 0), 30.0f, Quaternion::IDENTITY);
    ASSERT_TRUE(box.hit);
    EXPECT_NEAR(box.distance, 4.25f, 1e-3f);
    const RayReturn capsule = Physics::capsuleCast(&scene, Vector3(0, -0.5f, 0), Vector3(0, 0.5f, 0), 0.25f, Vector3(1, 0, 0), 30.0f);
    ASSERT_TRUE(capsule.hit);
    EXPECT_EQ(capsule.body, near);
    EXPECT_NEAR(capsule.distance, 4.25f, 1e-3f);
    EXPECT_FALSE(Physics::sphereCast(&scene, Vector3(0, 0, 0), 0.5f, Vector3(1, 0, 0), 3.9f).hit) << "short of the first box";

    std::mt19937 rng(5);
    std::uniform_real_distribution<float> along(-3.0f, 18.0f);
    std::uniform_real_distribution<float> radius(0.1f, 2.0f);
    std::vector<OverlapHit> hits;
    for (int i = 0; i < 1000; i++){
        const Vector3 center(along(rng), along(rng) * 0.2f, along(rng) * 0.2f);
        const float r = radius(rng);
        ASSERT_EQ(Physics::checkSphere(&scene, center, r), Physics::overlapSphere(&scene, center, r, hits) > 0) << i;
    }
}

// AC6: no heap allocation once warmed up, both backends; the red control is Jolt's own collector
TEST(PhysicsQuery, NoAllocations){
    if (!testsupport::countsAllocations()){
        GTEST_SKIP() << "allocations are counted on Linux only";
    }
    test::QueryCity city = test::buildQueryCity3D(2000);
    Scene* scene = city.scene.get();
    std::vector<OverlapHit> hits;
    hits.reserve(1024);
    std::vector<Vector3> centers;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> along(-city.extent, city.extent);
    for (int i = 0; i < 1000; i++) centers.push_back(Vector3(along(rng), 1.5f, along(rng)));
    const Quaternion turned(0.9238795f, 0.0f, 0.3826834f, 0.0f);
    auto run = [&](){
        size_t found = 0;
        for (const Vector3& c : centers){
            found += Physics::overlapSphere(scene, c, 5.0f, hits);
            found += Physics::overlapBox(scene, c, Vector3(5, 5, 5), turned, hits);
            found += Physics::overlapCapsule(scene, c, c + Vector3(0, 3, 0), 2.0f, hits);
            found += Physics::checkSphere(scene, c, 5.0f) ? 1 : 0;
            found += Physics::checkBox(scene, c, Vector3(3, 3, 3), turned) ? 1 : 0;
            found += Physics::checkCapsule(scene, c, c + Vector3(0, 3, 0), 1.0f) ? 1 : 0;
            found += Physics::sphereCast(scene, c, 0.5f, Vector3(1, 0, 0), 30.0f).hit ? 1 : 0;
        }
        return found;
    };
    run();
    size_t found = 0;
    uint64_t allocations = 0;
    {
        testsupport::AllocationCountScope scope;
        found = run();
        allocations = scope.count();
    }
    EXPECT_GT(found, 1000u);
    EXPECT_EQ(allocations, 0u);

    // red control: the same overlaps through Jolt's AllHitCollisionCollector allocate
    PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();
    uint64_t controlAllocations = 0;
    {
        testsupport::AllocationCountScope scope;
        for (const Vector3& c : centers){
            JPH::SphereShape sphere(5.0f);
            sphere.SetEmbedded();
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            physics->getNarrowPhaseQuery3D().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::Vec3(c.x, c.y, c.z)),
                                                          JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector);
        }
        controlAllocations = scope.count();
    }
    EXPECT_GT(controlAllocations, 0u);

#ifdef JASPERENGINE_PHYSICS_2D
    test::QueryCity city2D = test::buildQueryCity2D(2000);
    Scene* scene2D = city2D.scene.get();
    std::uniform_real_distribution<float> along2D(-city2D.extent, city2D.extent);
    std::vector<Vector2> points;
    for (int i = 0; i < 1000; i++) points.push_back(Vector2(along2D(rng), along2D(rng)));
    auto run2D = [&](){
        size_t found2D = 0;
        for (const Vector2& p : points){
            found2D += Physics2D::overlapCircleAll(scene2D, p, 320.0f, hits);
            found2D += Physics2D::overlapBoxAll(scene2D, p, Vector2(640.0f, 320.0f), 30.0f, hits);
            found2D += Physics2D::overlapCircle(scene2D, p, 320.0f).hit ? 1 : 0;
            found2D += Physics2D::circleCast(scene2D, p, 16.0f, Vector2(1, 0), 2000.0f).hit ? 1 : 0;
        }
        return found2D;
    };
    run2D();
    {
        testsupport::AllocationCountScope scope;
        found = run2D();
        allocations = scope.count();
    }
    EXPECT_GT(found, 1000u);
    EXPECT_EQ(allocations, 0u) << "2D";
#endif
}

// AC8: a bad argument logs one error naming it and finds nothing
TEST(PhysicsQuery, BadArgumentsLogOnce){
    std::unique_ptr<Scene> owner;
    Scene& scene = *test::newPhysicsScene(owner);
    bodyAt(scene, Vector3::ZERO, BodyType::STATIC, {boxCollider(1.0f)});
    test::stepScene(scene, 1);
    std::vector<OverlapHit> hits;
    {
        test::LogCapture log;
        EXPECT_EQ(Physics::overlapSphere(&scene, Vector3::ZERO, 0.0f, hits), 0u);
        EXPECT_EQ(log.errors().size(), 1u);
        EXPECT_TRUE(log.contains("radius"));
    }
    {
        test::LogCapture log;
        EXPECT_FALSE(Physics::checkSphere(&scene, Vector3(NAN, 0, 0), 1.0f));
        EXPECT_EQ(log.errors().size(), 1u);
        EXPECT_TRUE(log.contains("center"));
    }
    {
        test::LogCapture log;
        EXPECT_FALSE(Physics::sphereCast(&scene, Vector3(0, 5, 0), 0.5f, Vector3::ZERO, 10.0f).hit);
        EXPECT_EQ(log.errors().size(), 1u);
        EXPECT_TRUE(log.contains("direction"));
    }
#ifdef JASPERENGINE_PHYSICS_2D
    {
        test::LogCapture log;
        EXPECT_FALSE(Physics2D::overlapBox(&scene, Vector2(0, 0), Vector2(0, 10), 0.0f).hit);
        EXPECT_EQ(log.errors().size(), 1u);
        EXPECT_TRUE(log.contains("size"));
    }
#endif
}

// AC9: overlaps from jobs equal the main thread's; inside the step a query finds nothing and
// logs once
TEST(PhysicsQuery, OverlapsFromJobs){
    test::QueryCity city = test::buildQueryCity3D(2000);
    Scene* scene = city.scene.get();
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> along(-city.extent, city.extent);
    std::vector<Vector3> centers;
    for (int i = 0; i < 8 * 500; i++) centers.push_back(Vector3(along(rng), 1.5f, along(rng)));
    std::vector<std::vector<OverlapHit>> expected(centers.size());
    for (size_t i = 0; i < centers.size(); i++) Physics::overlapSphere(scene, centers[i], 4.0f, expected[i]);

    std::atomic<size_t> mismatches{0};
    jobs::parallelFor(8, [&](size_t job){
        std::vector<OverlapHit> hits;
        for (size_t i = job * 500; i < (job + 1) * 500; i++){
            Physics::overlapSphere(scene, centers[i], 4.0f, hits);
            std::string why;
            if (!sameHits(hits, expected[i], why)) mismatches.fetch_add(1);
        }
    }, jobs::ParallelForOptions{.grain = 1});
    EXPECT_EQ(mismatches.load(), 0u);

    // shouldCollide3D runs inside the step, on the main thread
    PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();
    size_t calls = 0;
    size_t found = 0;
    physics->shouldCollide3D.add("test", std::function<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>(
        [&](Rigidbody, Rigidbody, Vector3, CollideShapeResult3D){
            std::vector<OverlapHit> hits;
            calls++;
            found += Physics::overlapSphere(scene, Vector3(0, 1.5f, 0), 50.0f, hits);
            return true;
        }));
    // two bodies made to touch
    for (Entity entity : city.bodies){
        if (scene->getComponent<RigidbodyComponent>(entity).type == BodyType::DYNAMIC){
            Rigidbody(scene, entity).wakeUp();
            Rigidbody(scene, entity).setLinearVelocity(Vector3(0, -20, 0));
        }
    }
    test::LogCapture log;
    test::stepScene(*scene, 10);
    physics->shouldCollide3D.remove("test");
    ASSERT_GT(calls, 0u);
    EXPECT_EQ(found, 0u);
    EXPECT_EQ(log.count(D_LOG_ERROR, "outside the physics step"), 1u) << log.describeErrors();
}

// AC13: off, nothing is recorded; on, the ring keeps the newest 256 for 1 s of unscaled time;
// queries from jobs are not recorded
TEST(PhysicsQuery, DebugDrawRecordsOnlyWhenOn){
    QuerySettingsGuard guard;
    std::unique_ptr<Scene> owner;
    Scene& scene = *test::newPhysicsScene(owner);
    bodyAt(scene, Vector3::ZERO, BodyType::STATIC, {boxCollider(1.0f)});
    test::stepScene(scene, 1);
    PhysicsSystem* physics = scene.getSystem<PhysicsSystem>().get();
    std::vector<OverlapHit> hits;

    Physics::setDebugDrawQueries(false);
    for (int i = 0; i < 10; i++) Physics::overlapSphere(&scene, Vector3::ZERO, 2.0f, hits);
    EXPECT_EQ(physics->findQueryDebug(), nullptr) << "off: the ring is never made";

    Physics::setDebugDrawQueries(true);
    jobs::parallelFor(4, [&](size_t){
        std::vector<OverlapHit> local;
        Physics::overlapSphere(&scene, Vector3::ZERO, 2.0f, local);
    }, jobs::ParallelForOptions{.grain = 1});
    const PhysicsQueryDebug* ring = physics->findQueryDebug();
    EXPECT_TRUE(!ring || !ring->allocated()) << "queries from jobs are not recorded";

    const double now = Engine::getUnscaledTime();
    for (int i = 0; i < 300; i++) Physics::overlapSphere(&scene, Vector3((float)i, 0, 0), 2.0f, hits);
    ring = physics->findQueryDebug();
    ASSERT_NE(ring, nullptr);
    EXPECT_EQ(ring->liveCount(now), PhysicsQueryDebug::CAPACITY);
    // the oldest kept is query 300 - 256 = 44
    float first = -1.0f;
    ring->forEachLive(now, [&](const PhysicsQueryDebug::Entry& entry){ if (first < 0.0f) first = entry.shape.a.x; });
    EXPECT_EQ(first, 44.0f);
    EXPECT_EQ(ring->liveCount(now + 0.9), PhysicsQueryDebug::CAPACITY);
    EXPECT_EQ(ring->liveCount(now + 1.01), 0u) << "expired after 1 s";

    // the ring's own clock: a forced entry lives 2 s, an idle ring off for 10 s is freed
    PhysicsQueryDebug own;
    PhysicsQueryDebug::Entry entry;
    own.record(entry, 100.0, PhysicsQueryDebug::FORCED_LIFETIME);
    EXPECT_EQ(own.liveCount(101.5), 1u);
    own.releaseIfIdle(103.0, false);
    own.releaseIfIdle(112.0, false);
    EXPECT_TRUE(own.allocated());
    own.releaseIfIdle(113.5, false);
    EXPECT_FALSE(own.allocated());
}

#endif // JASPERENGINE_PHYSICS_3D

#ifdef JASPERENGINE_PHYSICS_2D

// AC7: every 2D overlap form against the sweep; first = the All form's first; sensors follow the
// interaction; casts in points; overlapArea with swapped corners
TEST(PhysicsQuery2D, OverlapsAndCastsMatchReference){
    QuerySettingsGuard guard;
    test::QueryCity city = test::buildQueryCity2D(2000);
    Scene* scene = city.scene.get();
    std::mt19937 rng(13);
    std::uniform_real_distribution<float> along(-city.extent, city.extent);
    std::uniform_real_distribution<float> size(16.0f, 400.0f);
    std::uniform_real_distribution<float> angle(-90.0f, 90.0f);
    std::vector<OverlapHit> hits;
    std::vector<OverlapHit> reference;
    size_t total = 0;
    size_t sensors = 0;
    for (int i = 0; i < 2000; i++){
        physics::QueryShape shape;
        shape.a = Vector3(along(rng), along(rng), 0.0f);
        switch (i % 5){
            case 0: shape.type = physics::QueryShape::Type::Point2D; break;
            case 1: shape.type = physics::QueryShape::Type::Circle2D; shape.radius = size(rng); break;
            case 2: shape.type = physics::QueryShape::Type::Box2D; shape.halfExtents = Vector3(size(rng), size(rng), 0); shape.angle = angle(rng); break;
            case 3:
                shape.type = physics::QueryShape::Type::Capsule2D;
                shape.halfExtents = Vector3(size(rng), size(rng), 0);
                shape.direction2D = i % 2 ? CapsuleDirection2D::Vertical : CapsuleDirection2D::Horizontal;
                shape.angle = angle(rng);
                break;
            default: shape.type = physics::QueryShape::Type::Area2D; shape.b = shape.a + Vector3(size(rng), -size(rng), 0); break;
        }
        const PhysicsQueryOptions options = withTriggers(i % 3 == 0 ? QueryTriggerInteraction::Ignore : QueryTriggerInteraction::Collide);
        physics::queryOverlap(scene, shape, options, hits);
        physics::referenceOverlap(scene, true, shape, options, reference);
        std::string why;
        ASSERT_TRUE(sameHits(hits, reference, why)) << "query " << i << ": " << why;
        ASSERT_TRUE(inQueryOrder(hits));
        total += hits.size();
        for (const OverlapHit& hit : hits){
            sensors += hit.trigger ? 1 : 0;
            if (options.triggers == QueryTriggerInteraction::Ignore) ASSERT_FALSE(hit.trigger);
        }

        // the first form is the All form's first
        const Vector2 a(shape.a.x, shape.a.y);
        const Vector2 full(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f);
        OverlapHit first;
        switch (shape.type){
            case physics::QueryShape::Type::Point2D: first = Physics2D::overlapPoint(scene, a, options); break;
            case physics::QueryShape::Type::Circle2D: first = Physics2D::overlapCircle(scene, a, shape.radius, options); break;
            case physics::QueryShape::Type::Box2D: first = Physics2D::overlapBox(scene, a, full, shape.angle, options); break;
            case physics::QueryShape::Type::Capsule2D: first = Physics2D::overlapCapsule(scene, a, full, shape.direction2D, shape.angle, options); break;
            default: {
                first = Physics2D::overlapArea(scene, a, Vector2(shape.b.x, shape.b.y), options);
                std::vector<OverlapHit> swapped;
                Physics2D::overlapAreaAll(scene, Vector2(shape.b.x, shape.b.y), a, swapped, options);
                ASSERT_TRUE(sameHits(swapped, hits, why)) << "swapped corners: " << why;
                break;
            }
        }
        ASSERT_EQ(first.hit, !hits.empty());
        if (first.hit){
            ASSERT_EQ(first.body, hits[0].body);
            ASSERT_EQ(first.colliderIndex, hits[0].colliderIndex);
        }
    }
    EXPECT_GT(total, 1000u);
    EXPECT_GT(sensors, 0u);

    // casts in points: a circle cast along x meets the first body's left side
    std::unique_ptr<Scene> owner;
    Scene& small = *test::newPhysicsScene(owner);
    PhysicsSystem* physics = small.getSystem<PhysicsSystem>().get();
    physics->setPointsToMeterScale2D(64.0f);
    physics->setGravity2D(0.0f, 0.0f);
    auto box2D = [&](float x, bool sensor){
        const Entity entity = test::placedEntity(small, Vector3(x, 0, 0));
        Rigidbody2DComponent body;
        body.type = BodyType::STATIC;
        body.colliders.resize(1);
        body.colliders[0].type = Collider2DType::POLYGON;
        body.colliders[0].pointA = Vector2(-32, -32);
        body.colliders[0].pointB = Vector2(32, 32);
        body.colliders[0].sensor = sensor;
        small.addComponent<Rigidbody2DComponent>(entity, body);
        return entity;
    };
    const Entity start = box2D(0, false);
    const Entity sensor = box2D(200, true);
    const Entity wall = box2D(400, false);
    test::stepScene(small, 1);
    RayReturn hit = Physics2D::circleCast(&small, Vector2(0, 0), 8.0f, Vector2(1, 0), 1000.0f);
    ASSERT_TRUE(hit.hit);
    EXPECT_EQ(hit.body, sensor) << "the start box is skipped; the sensor is hit by default";
    EXPECT_NEAR(hit.distance, 200.0f - 32.0f - 8.0f, 0.5f);
    EXPECT_NEAR(hit.normal.x, -1.0f, 1e-3f);
    EXPECT_EQ(hit.point.z, 0.0f);
    hit = Physics2D::circleCast(&small, Vector2(0, 0), 8.0f, Vector2(1, 0), 1000.0f, withTriggers(QueryTriggerInteraction::Ignore));
    EXPECT_EQ(hit.body, wall);
    std::vector<RayReturn> all;
    ASSERT_EQ(Physics2D::boxCastAll(&small, Vector2(0, 0), Vector2(16, 16), 0.0f, Vector2(1, 0), 1000.0f, all), 2u);
    EXPECT_EQ(all[0].body, sensor);
    EXPECT_EQ(all[1].body, wall);
    hit = Physics2D::capsuleCast(&small, Vector2(0, 0), Vector2(16, 40), CapsuleDirection2D::Vertical, 0.0f, Vector2(1, 0), 1000.0f);
    EXPECT_EQ(hit.body, sensor);
    EXPECT_NEAR(hit.distance, 200.0f - 32.0f - 8.0f, 0.5f);
    for (const RayReturn& each : all) EXPECT_NE(each.body, start);
    Physics2D::setQueriesHitTriggers(false);
    EXPECT_EQ(Physics2D::circleCast(&small, Vector2(0, 0), 8.0f, Vector2(1, 0), 1000.0f).body, wall);
}

#endif // JASPERENGINE_PHYSICS_2D
