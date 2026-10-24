// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// EntityCommandBuffer: FIFO playback, PendingEntity resolution, stale references skipped
// with a warning, non-POD payloads destroyed exactly once, consecutive destroys batched.

#include "EntityRegistry.h"
#include "Scene.h"
#include "EcsTestUtil.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using jasperengine::test::makeTransformEntity;
using jasperengine::test::LogCapture;

namespace {

    // A Lua-reference stand-in: owns a reference that must be released once
    struct MockRef {
        static int releases;
        int ref = 0;

        MockRef() = default;
        explicit MockRef(int ref): ref(ref) {}
        MockRef(const MockRef&) = delete;
        MockRef& operator=(const MockRef&) = delete;
        MockRef(MockRef&& other) noexcept: ref(other.ref) { other.ref = 0; }
        MockRef& operator=(MockRef&& other) noexcept {
            if (this != &other){
                release();
                ref = other.ref;
                other.ref = 0;
            }
            return *this;
        }
        ~MockRef(){ release(); }

        void release(){
            if (ref != 0){
                releases++;
                ref = 0;
            }
        }
    };
    int MockRef::releases = 0;

    struct Tracked {
        std::string text;
        std::shared_ptr<int> shared;
        MockRef luaRef;
    };

    // past the 64-byte inline storage: kept on the heap
    struct BigTracked {
        std::shared_ptr<int> shared;
        MockRef luaRef;
        char padding[256] = {};
    };

    class TrackedRegistry : public EntityRegistry {
    public:
        TrackedRegistry(){
            registerComponent<Tracked>();
            registerComponent<BigTracked>();
        }
    };

}

TEST(CommandBufferTest, FifoPlayback){
    EntityRegistry registry;
    const Entity entity = makeTransformEntity(registry);
    EntityCommandBuffer buffer;
    const EntityRef ref = registry.makeRef(entity);
    buffer.setName(ref, "first");
    buffer.setName(ref, "second");
    buffer.setLayer(ref, 3);
    buffer.setTag(ref, "Player");
    buffer.add<MeshComponent>(ref, MeshComponent());
    buffer.remove<MeshComponent>(ref);
    buffer.setActive(ref, false);
    EXPECT_EQ(buffer.size(), 7u);

    const Tick before = registry.changeTick();
    buffer.playback(registry);
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(registry.getEntityName(entity), "second");
    EXPECT_EQ(registry.getEntityLayer(entity), 3);
    EXPECT_EQ(registry.getEntityTag(entity), "Player");
    EXPECT_FALSE(registry.getSignature(entity).test(registry.getComponentId<MeshComponent>()));
    EXPECT_FALSE(registry.isEntityActive(entity));
    EXPECT_GT(registry.changeTick(), before);
}

TEST(CommandBufferTest, DeferRunsInOrder){
    Scene scene;
    EntityCommandBuffer buffer;
    std::vector<int> order;
    const PendingEntity pending = buffer.create();
    buffer.defer([&order](Scene&){ order.push_back(1); });
    buffer.add<Transform>(pending, Transform());
    buffer.defer([&order, &scene](Scene& target){
        EXPECT_EQ(&target, &scene);
        order.push_back(2);
    });
    buffer.playback(scene);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(CommandBufferTest, PendingEntityResolutionAndParenting){
    EntityRegistry registry;
    const size_t before = registry.getEntityCount();
    EntityCommandBuffer buffer;
    const PendingEntity parent = buffer.create();
    buffer.add<Transform>(parent, Transform());
    buffer.setName(parent, "Parent");
    const PendingEntity child = buffer.create();
    buffer.add<Transform>(child, Transform());
    buffer.setName(child, "Child");
    buffer.setParent(child, parent, false);
    EXPECT_EQ(registry.getEntityCount(), before); // nothing until playback

    buffer.playback(registry);
    const Entity parentEntity = registry.findEntity("Parent");
    const Entity childEntity = registry.findEntity("Child");
    ASSERT_NE(parentEntity, NULL_ENTITY);
    ASSERT_NE(childEntity, NULL_ENTITY);
    EXPECT_EQ(registry.getComponent<Transform>(childEntity).parent, parentEntity);
    EXPECT_TRUE(registry.isParentOf(parentEntity, childEntity));
}

TEST(CommandBufferTest, StaleReferenceSkippedWithWarning){
    EntityRegistry registry(EntityPool::System);
    const Entity entity = makeTransformEntity(registry);
    const EntityRef ref = registry.makeRef(entity);
    registry.destroyEntity(entity);
    // the id goes to a new entity: the reference must not reach it
    const Entity reused = makeTransformEntity(registry);
    ASSERT_EQ(reused, entity);

    EntityCommandBuffer buffer;
    buffer.add<MeshComponent>(ref, MeshComponent());
    buffer.setName(ref, "stale");
    LogCapture log;
    buffer.playback(registry);
    EXPECT_GE(log.count(D_LOG_WARN, "no longer exists"), 2u);
    EXPECT_FALSE(registry.getSignature(reused).test(registry.getComponentId<MeshComponent>()));
    EXPECT_NE(registry.getEntityName(reused), "stale");
}

TEST(CommandBufferTest, NonPodPayloadDestroyedOnce){
    auto shared = std::make_shared<int>(5);
    MockRef::releases = 0;
    {
        // played back: the component owns the payload, released when it goes
        TrackedRegistry registry;
        const Entity entity = registry.createEntity();
        EntityCommandBuffer buffer;
        buffer.add<Tracked>(registry.makeRef(entity), Tracked{"text", shared, MockRef(11)});
        buffer.add<BigTracked>(registry.makeRef(entity), BigTracked{shared, MockRef(12)});
        EXPECT_EQ(shared.use_count(), 3);
        buffer.playback(registry);
        EXPECT_EQ(MockRef::releases, 0);
        EXPECT_EQ(shared.use_count(), 3);
        EXPECT_EQ(registry.getComponent<Tracked>(entity).text, "text");
        registry.destroyEntity(entity);
        EXPECT_EQ(MockRef::releases, 2);
        EXPECT_EQ(shared.use_count(), 1);
    }
    MockRef::releases = 0;
    {
        // discarded: released by the buffer
        TrackedRegistry registry;
        const Entity entity = registry.createEntity();
        EntityCommandBuffer buffer;
        buffer.add<Tracked>(registry.makeRef(entity), Tracked{"text", shared, MockRef(21)});
        buffer.add<BigTracked>(registry.makeRef(entity), BigTracked{shared, MockRef(22)});
        // the buffer grows (moves its commands) without touching the payloads' owners
        for (int i = 0; i < 100; i++){
            buffer.setName(registry.makeRef(entity), "n");
        }
        EXPECT_EQ(MockRef::releases, 0);
        buffer.clear();
        EXPECT_EQ(MockRef::releases, 2);
        EXPECT_EQ(shared.use_count(), 1);
    }
    MockRef::releases = 0;
    {
        // skipped (stale target): released by the playback
        TrackedRegistry registry;
        const Entity entity = registry.createEntity();
        const EntityRef ref = registry.makeRef(entity);
        registry.destroyEntity(entity);
        EntityCommandBuffer buffer;
        buffer.add<Tracked>(ref, Tracked{"text", shared, MockRef(31)});
        buffer.playback(registry);
        EXPECT_EQ(MockRef::releases, 1);
        EXPECT_EQ(shared.use_count(), 1);
    }
}

TEST(CommandBufferTest, ConsecutiveDestroysCompactOnce){
    EntityRegistry registry;
    std::vector<Entity> entities;
    for (int i = 0; i < 50; i++){
        entities.push_back(makeTransformEntity(registry));
    }
    auto transforms = registry.getComponentArray<Transform>();
    transforms->settle();
    const size_t compactions = transforms->getCompactionCount();

    EntityCommandBuffer buffer;
    for (size_t i = 0; i < entities.size(); i += 2){
        buffer.destroy(registry.makeRef(entities[i]));
    }
    buffer.playback(registry);
    EXPECT_EQ(transforms->size(), 25u);
    EXPECT_EQ(transforms->getEntity(0), entities[1]);
    EXPECT_EQ(transforms->getCompactionCount(), compactions + 1);
}
