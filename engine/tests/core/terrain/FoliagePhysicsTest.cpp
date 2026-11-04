// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC17, AC25: tree capsules. A cell's body exists while the scene simulates, one capsule per
// tree; colliderIndex in rays and contacts names the tree.

#ifdef JASPERENGINE_PHYSICS_3D

#include "FoliageTestSupport.h"

#include "component/RigidbodyComponent.h"
#include "math/Ray.h"
#include "registry/PhysicsLayers.h"
#include "subsystem/PhysicsSystem.h"

using namespace foliagetest;

namespace {

    void collidingTrees(FoliageFixture& fixture, std::initializer_list<std::pair<float, float>> points, float radius = 1.0f, float height = 6.0f) {
        withTrees(fixture, points);
        TerrainTreePrototype& prototype = fixture.terrain().treePrototypes.back();
        prototype.collision = TerrainTreeCollision::Capsule;
        prototype.collisionRadius = radius;
        prototype.collisionHeight = height;
        fixture.settle();
        fixture.frame();
    }

    Entity dynamicBall(Scene& scene, const Vector3& position, const Vector3& velocity, float radius = 0.5f) {
        const Entity entity = scene.createEntity();
        Transform transform;
        transform.position = position;
        transform.worldPosition = position;
        scene.addComponent<Transform>(entity, transform);
        RigidbodyComponent body;
        body.type = BodyType::DYNAMIC;
        body.gravityFactor = 0.0f;
        Collider3D collider;
        collider.type = Collider3DType::SPHERE;
        collider.radius = radius;
        body.colliders.push_back(collider);
        body.hasPendingVelocity = true;
        body.pendingLinearVelocity = velocity;
        body.pendingAwake = true;
        scene.addComponent<RigidbodyComponent>(entity, body);
        return entity;
    }

}

TEST(FoliagePhysics, NoCollisionMeansNoBody) {
    FoliageFixture fixture;
    withTrees(fixture, {{10.0f, 10.0f}});
    fixture.settle();
    fixture.frame();
    auto bodies = fixture.scene->getComponentArray<RigidbodyComponent>();
    EXPECT_EQ(bodies->size(), 0);
}

TEST(FoliagePhysics, ColliderIndexIdentifiesTheTree) {
    FoliageFixture fixture;
    collidingTrees(fixture, {{1.0f, 1.0f}, {10.0f, 10.0f}, {20.0f, 2.0f}});
    fixture.scene->fixedUpdate(1.0f / 60.0f);

    const std::shared_ptr<TreeStore> store = fixture.foliage->getTreeStore(fixture.entity);
    for (const TreeRecord& record : store->records()) {
        const float x = foliageTreeLocalX(record, WORLD);
        const float z = foliageTreeLocalZ(record, WORLD);
        const float y = record.height * fixture.terrain().maxHeight + 3.0f;
        const RayReturn hit = Physics::raycast(fixture.scene.get(), Ray(Vector3(x - 5.0f, y, z), Vector3(10.0f, 0.0f, 0.0f)));
        ASSERT_TRUE(hit.hit) << "tree " << record.id;
        Entity terrainEntity = NULL_ENTITY;
        TreeId id = 0;
        ASSERT_TRUE(fixture.foliage->getTreeFromBody(hit.body, hit.colliderIndex, terrainEntity, id));
        EXPECT_EQ(terrainEntity, fixture.entity);
        EXPECT_EQ(treeIdRecord(id), record.id);
    }
}

TEST(FoliagePhysics, CapsuleStopsADynamicBody) {
    FoliageFixture fixture;
    collidingTrees(fixture, {{0.0f, 0.0f}});
    const std::shared_ptr<TreeStore> store = fixture.foliage->getTreeStore(fixture.entity);
    const float y = store->records()[0].height * fixture.terrain().maxHeight + 3.0f;
    const Entity ball = dynamicBall(*fixture.scene, Vector3(-6.0f, y, 0.0f), Vector3(4.0f, 0.0f, 0.0f));
    for (int i = 0; i < 180; i++) fixture.scene->fixedUpdate(1.0f / 60.0f);
    const float x = fixture.scene->getComponent<Transform>(ball).worldPosition.x;
    EXPECT_LT(x, 0.0f) << "the capsule stops it before the trunk's far side";
}

TEST(FoliagePhysics, RemovedTreeFreesThePath) {
    FoliageFixture fixture;
    collidingTrees(fixture, {{0.0f, 0.0f}, {3.0f, 3.0f}});
    fixture.scene->fixedUpdate(1.0f / 60.0f);
    const std::shared_ptr<TreeStore> store = fixture.foliage->getTreeStore(fixture.entity);
    const TreeRecord record = store->records()[0];
    const float y = record.height * fixture.terrain().maxHeight + 3.0f;
    const Ray ray(Vector3(-5.0f, y, 0.0f), Vector3(10.0f, 0.0f, 0.0f));
    ASSERT_TRUE(Physics::raycast(fixture.scene.get(), ray).hit);

    // R30: in the same frame
    ASSERT_TRUE(fixture.foliage->removeTreeInstance(fixture.entity, makeTreeId(0, record.id)).has_value());
    fixture.scene->fixedUpdate(1.0f / 60.0f);
    EXPECT_FALSE(Physics::raycast(fixture.scene.get(), ray).hit);

    ASSERT_TRUE(fixture.foliage->restoreTreeInstance(fixture.entity, makeTreeId(0, record.id)));
    fixture.scene->fixedUpdate(1.0f / 60.0f);
    EXPECT_TRUE(Physics::raycast(fixture.scene.get(), ray).hit);
}

TEST(FoliagePhysics, OnTreeHitFiresOncePerContact) {
    FoliageFixture fixture;
    collidingTrees(fixture, {{0.0f, 0.0f}});
    const std::shared_ptr<TreeStore> store = fixture.foliage->getTreeStore(fixture.entity);
    const uint32_t id = store->records()[0].id;
    const float y = store->records()[0].height * fixture.terrain().maxHeight + 3.0f;

    int hits = 0;
    TreeHitEvent seen;
    Subscription subscription = fixture.foliage->onTreeHit.subscribe([&](Entity, const TreeHitEvent& hit) {
        hits++;
        seen = hit;
    });
    fixture.frame();

    const Entity ball = dynamicBall(*fixture.scene, Vector3(-3.0f, y, 0.0f), Vector3(3.0f, 0.0f, 0.0f));
    for (int i = 0; i < 120; i++) fixture.scene->fixedUpdate(1.0f / 60.0f);
    EXPECT_EQ(hits, 1) << "a contact's start, not each step it persists";
    EXPECT_EQ(treeIdRecord(seen.id), id);
    EXPECT_EQ(seen.prototype, 0);
    EXPECT_EQ(seen.body, ball);
    EXPECT_GT(seen.speed, 0.0f);
}

TEST(FoliagePhysics, NoSubscriberNoWork) {
    FoliageFixture fixture;
    collidingTrees(fixture, {{0.0f, 0.0f}});
    fixture.frame();
    EXPECT_FALSE(fixture.scene->getSystem<PhysicsSystem>()->onContactAdded3D.hasSubscribers());
    {
        Subscription subscription = fixture.foliage->onTreeHit.subscribe([](Entity, const TreeHitEvent&) {});
        fixture.frame();
        EXPECT_TRUE(fixture.scene->getSystem<PhysicsSystem>()->onContactAdded3D.hasSubscribers());
    }
    fixture.frame();
    EXPECT_FALSE(fixture.scene->getSystem<PhysicsSystem>()->onContactAdded3D.hasSubscribers());
}

#endif // JASPERENGINE_PHYSICS_3D
