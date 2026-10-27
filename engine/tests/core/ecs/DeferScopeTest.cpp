// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// StructuralDeferScope: destroys and removals inside a scope wait for the outermost scope to
// close; an index loop over an array inside the scope never sees it shift.

#include "Scene.h"
#include "manager/ObjectManager.h"
#include "EcsTestUtil.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;
using jasperengine::test::makeTransformEntity;

TEST(DeferScopeTest, DestroyImmediateWaitsForTheScope){
    Scene scene;
    const Entity parent = makeTransformEntity(scene);
    const Entity child = makeTransformEntity(scene, parent);
    {
        StructuralDeferScope scope(scene);
        EXPECT_TRUE(scene.isStructuralDeferActive());
        ObjectManager::destroyImmediate(&scene, parent);
        EXPECT_TRUE(scene.isEntityCreated(parent));
        EXPECT_TRUE(scene.isEntityCreated(child));
    }
    EXPECT_FALSE(scene.isStructuralDeferActive());
    EXPECT_FALSE(scene.isEntityCreated(parent));
    EXPECT_FALSE(scene.isEntityCreated(child)); // GameObject semantics: the children go too
}

TEST(DeferScopeTest, NestedScopesPlayBackOnce){
    Scene scene;
    const Entity a = makeTransformEntity(scene);
    const Entity b = makeTransformEntity(scene);
    scene.addComponent<MeshComponent>(b);
    {
        StructuralDeferScope outer(scene);
        {
            StructuralDeferScope inner(scene);
            scene.destroyEntity(a);
            scene.removeComponent<MeshComponent>(b);
        }
        // the inner scope closing plays nothing back
        EXPECT_TRUE(scene.isEntityCreated(a));
        EXPECT_NE(scene.findComponent<MeshComponent>(b), nullptr);
        EXPECT_EQ(scene.getDeferredCommands().size(), 2u);
    }
    EXPECT_FALSE(scene.isEntityCreated(a));
    EXPECT_EQ(scene.findComponent<MeshComponent>(b), nullptr);
    EXPECT_TRUE(scene.getDeferredCommands().empty());
}

TEST(DeferScopeTest, IndexLoopNeverSeesAShift){
    Scene scene;
    std::vector<Entity> entities;
    for (int i = 0; i < 20; i++){
        entities.push_back(makeTransformEntity(scene));
    }
    auto transforms = scene.getComponentArray<Transform>();
    std::vector<Entity> visited;
    {
        StructuralDeferScope scope(scene);
        for (size_t i = 0; i < transforms->size(); i++){
            const Entity entity = transforms->getEntity(i);
            visited.push_back(entity);
            // every visit destroys the next entity, as a collision callback destroying the
            // other body would
            if (i + 1 < entities.size()){
                scene.destroyEntity(entities[i + 1]);
                ObjectManager::destroyImmediate(&scene, entities[i + 1]);
            }
        }
    }
    EXPECT_EQ(visited, entities);
    EXPECT_EQ(transforms->size(), 1u);
    EXPECT_EQ(transforms->getEntity(0), entities[0]);
}

TEST(DeferScopeTest, AddsStayImmediate){
    Scene scene;
    const Entity entity = makeTransformEntity(scene);
    {
        StructuralDeferScope scope(scene);
        scene.addComponent<MeshComponent>(entity);
        EXPECT_NE(scene.findComponent<MeshComponent>(entity), nullptr);
        const Entity created = scene.createEntity();
        EXPECT_TRUE(scene.isEntityCreated(created));
    }
}

TEST(DeferScopeTest, ARemovalRecordedTwiceRemovesOnce){
    // a contact callback toggling a component on a GameObject (tools/fixtures/ecs-collision):
    // remove (deferred), get or add (the component is still there), remove (deferred again).
    // The second removal finds nothing to remove when the scope plays back; the removal hooks
    // (PhysicsSystem's reads the Rigidbody2D) must not see it (the exported game aborted with
    // "Retrieving non-existent component")
    Scene scene;
    const Entity entity = makeTransformEntity(scene);
    scene.addComponent<Rigidbody2DComponent>(entity);
    {
        StructuralDeferScope scope(scene);
        scene.removeComponent<Rigidbody2DComponent>(entity);
        EXPECT_NE(scene.findComponent<Rigidbody2DComponent>(entity), nullptr);
        scene.removeComponent<Rigidbody2DComponent>(entity);
        EXPECT_EQ(scene.getDeferredCommands().size(), 2u);
    }
    EXPECT_EQ(scene.findComponent<Rigidbody2DComponent>(entity), nullptr);
    EXPECT_FALSE(scene.getSignature(entity).test(scene.getComponentId<Rigidbody2DComponent>()));
    // and added again afterwards, as the next callback does
    scene.addComponent<Rigidbody2DComponent>(entity);
    EXPECT_NE(scene.findComponent<Rigidbody2DComponent>(entity), nullptr);
}
