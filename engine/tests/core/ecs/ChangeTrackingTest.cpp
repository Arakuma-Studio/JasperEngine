// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Change ticks: which paths stamp them (add, markChanged, an editor command, the transform
// pass), the gate's entry-sample contract, the O(1) skip of an older type and the trimming
// of Removed<T>.

#include "Scene.h"
#include "EntityRegistry.h"
#include "subsystem/TransformPropagation.h"
#include "EcsTestUtil.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;

using jasperengine::test::disableSubSystems;

namespace {

    Entity makeEntity(EntityRegistry& registry, Entity parent = NULL_ENTITY){
        return jasperengine::test::makeTransformEntity(registry, parent);
    }

}

TEST(ChangeTrackingTest, AddStampsAddedAndChanged){
    EntityRegistry registry;
    const Entity entity = registry.createEntity();
    const Tick before = registry.changeTick();
    registry.addComponent<MeshComponent>(entity);
    auto meshes = registry.getComponentArray<MeshComponent>();
    EXPECT_GT(meshes->getAddedTickOf(entity), before);
    EXPECT_GT(meshes->getChangedTickOf(entity), before);
    EXPECT_GT(meshes->getLastAddedTick(), before);
    EXPECT_GT(meshes->getLastChangedTick(), before);
}

TEST(ChangeTrackingTest, MarkChangedStampsChangedOnly){
    EntityRegistry registry;
    const Entity entity = makeEntity(registry);
    auto transforms = registry.getComponentArray<Transform>();
    const Tick added = transforms->getAddedTickOf(entity);
    const Tick before = registry.changeTick();

    registry.getComponent<Transform>(entity).position = Vector3(1, 2, 3); // untracked write
    EXPECT_LE(transforms->getChangedTickOf(entity), before);

    registry.markChanged<Transform>(entity);
    EXPECT_GT(transforms->getChangedTickOf(entity), before);
    EXPECT_EQ(transforms->getAddedTickOf(entity), added);

    // by id, as editor and MCP code does it
    const Tick again = registry.changeTick();
    EXPECT_TRUE(registry.markChanged(registry.getComponentId<Transform>(), entity));
    EXPECT_GT(transforms->getChangedTickOf(entity), again);
    EXPECT_FALSE(registry.markChanged(registry.getComponentId<MeshComponent>(), entity));
}

TEST(ChangeTrackingTest, EditorCommandStandIn){
    // an editor command: writes through the component, stamps it, then the tick advances
    EntityRegistry registry;
    const Entity entity = makeEntity(registry);
    const Tick before = registry.changeTick();
    registry.getComponent<Transform>(entity).scale = Vector3(2, 2, 2);
    registry.markChanged(registry.getComponentId<Transform>(), entity);
    registry.advanceChangeTick();
    const Tick changed = registry.getComponentArray<Transform>()->getChangedTickOf(entity);
    EXPECT_GT(changed, before);
    EXPECT_GT(registry.changeTick(), changed);

    std::vector<Entity> found;
    registry.view<Read<Transform>>().changedSince(before).each([&found](Entity e, const Transform&){ found.push_back(e); });
    EXPECT_EQ(found, std::vector<Entity>{entity});
}

TEST(ChangeTrackingTest, TransformPassStampsRecomputedTransforms){
    EntityRegistry registry;
    const Entity parent = makeEntity(registry);
    const Entity child = makeEntity(registry, parent);
    const Entity other = makeEntity(registry);
    for (Entity entity : {parent, child, other}){
        registry.getComponent<Transform>(entity).needUpdate = false;
    }
    const Tick before = registry.changeTick();

    registry.getComponent<Transform>(parent).needUpdate = true;
    std::vector<Entity> updated;
    propagateTransforms(registry, [&updated](Transform&, Entity entity){ updated.push_back(entity); });

    EXPECT_EQ(updated, (std::vector<Entity>{parent, child}));
    auto transforms = registry.getComponentArray<Transform>();
    EXPECT_GT(transforms->getChangedTickOf(parent), before);
    EXPECT_GT(transforms->getChangedTickOf(child), before);
    EXPECT_LE(transforms->getChangedTickOf(other), before);
}

TEST(ChangeTrackingTest, GateEntrySampleContract){
    Scene scene;
    disableSubSystems(scene);
    std::vector<Entity> entities;
    for (int i = 0; i < 4; i++){
        entities.push_back(makeEntity(scene));
    }
    const ComponentId transform = scene.getComponentId<Transform>();

    std::vector<std::vector<Entity>> seen;
    SystemDescriptor writer;
    writer.name = "Writer";
    writer.reads.set(transform);
    writer.writes.set(transform);
    scene.addSystemFunction(writer, [&](SystemContext& context){
        std::vector<Entity> changed;
        context.scene.view<Write<Transform>>().changedSince(context.gate).each([&](Entity e, Transform&){ changed.push_back(e); });
        seen.push_back(changed);
        // its own write of this run is not new to it next run
        context.scene.markChanged<Transform>(entities[0]);
    });

    scene.update(0.016);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].size(), 4u); // a fresh gate sees everything

    scene.update(0.016);
    EXPECT_TRUE(seen[1].empty());

    // a change from outside, between the runs, is new to it
    scene.markChanged<Transform>(entities[2]);
    scene.update(0.016);
    EXPECT_EQ(seen[2], std::vector<Entity>{entities[2]});
}

TEST(ChangeTrackingTest, OlderTypeIsSkipped){
    EntityRegistry registry;
    std::vector<Entity> entities;
    for (int i = 0; i < 10; i++){
        entities.push_back(makeEntity(registry));
    }
    const Tick gate = registry.changeTick();
    // holes left by a removal stay until an index observation
    registry.destroyEntity(entities[3]);
    auto transforms = registry.getComponentArray<Transform>();
    const Tick removalTick = transforms->getLastRemovedTick();
    EXPECT_GT(removalTick, gate);
    EXPECT_LE(transforms->getLastChangedTick(), gate);
    ASSERT_EQ(transforms->getHoleCount(), 1u);

    size_t rows = 0;
    registry.view<Read<Transform>>().changedSince(gate).each([&rows](Entity, const Transform&){ rows++; });
    EXPECT_EQ(rows, 0u);
    EXPECT_EQ(transforms->getHoleCount(), 1u); // skipped without settling the array
}

TEST(ChangeTrackingTest, RemovedLogIsTrimmed){
    Scene scene;
    disableSubSystems(scene);
    const Entity entity = makeEntity(scene);
    scene.addComponent<MeshComponent>(entity);
    scene.removeComponent<MeshComponent>(entity);
    auto meshes = scene.getComponentArray<MeshComponent>();
    ASSERT_EQ(meshes->getRemoved().size(), 1u);
    EXPECT_EQ(meshes->getRemoved()[0].entity, entity);

    // no system ran, so no gate: kept for two frame ends, trimmed at the third
    scene.endFrame();
    scene.endFrame();
    EXPECT_EQ(meshes->getRemoved().size(), 1u);
    scene.endFrame();
    EXPECT_TRUE(meshes->getRemoved().empty());

    // with a gate: what every enabled system has seen goes at the frame end
    SystemDescriptor reader;
    reader.name = "Reader";
    scene.addSystemFunction(reader, [](SystemContext&){});
    scene.addComponent<MeshComponent>(entity);
    scene.removeComponent<MeshComponent>(entity);
    ASSERT_EQ(meshes->getRemoved().size(), 1u);
    scene.getScheduler().runPhase(scene, SystemPhase::Update, 0.016);
    scene.endFrame();
    EXPECT_TRUE(meshes->getRemoved().empty());
}

TEST(ChangeTrackingTest, TickColumnRebases){
    ComponentArray<int> array;
    array.insert(1, 1);
    array.insert(2, 2);
    array.markChanged(1, 10);
    const Tick far = (Tick)1 << 40;
    array.markChanged(2, far);
    EXPECT_EQ(array.getChangedTickOf(2), far);
    // older than the new base: reported as the base, newer than any gate before it
    EXPECT_GE(array.getChangedTickOf(1), (Tick)10);
    EXPECT_LT(array.getChangedTickOf(1), far);
    array.markChanged(1, far + 5);
    EXPECT_EQ(array.getChangedTickOf(1), far + 5);
}
