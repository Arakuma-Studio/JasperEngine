// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Characterization tests of EntityRegistry: components and signatures, entity lifetime, the
// metadata (names, tags, layers, active flags) and the Transform hierarchy, whose order is the
// order of the Transform array (a parent, then its whole branch).

#include "registry/EntityRegistry.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // records the registry's callbacks
    class ObservedRegistry : public EntityRegistry {
    public:
        std::vector<std::pair<Entity, ComponentId>> added;
        std::vector<std::pair<Entity, ComponentId>> removed;
        std::vector<std::pair<Entity, Signature>> destroyed;
        std::vector<Entity> layerChanged;

    protected:
        void onComponentAdded(Entity entity, ComponentId componentId) override{ added.emplace_back(entity, componentId); }
        void onComponentRemoved(Entity entity, ComponentId componentId) override{ removed.emplace_back(entity, componentId); }
        void onEntityDestroyed(Entity entity, Signature signature) override{ destroyed.emplace_back(entity, signature); }
        void onEntityLayerChanged(Entity entity) override{ layerChanged.push_back(entity); }
    };

    class EntityRegistryTest : public ::testing::Test {
    protected:
        ObservedRegistry registry;

        Entity withTransform(){
            const Entity entity = registry.createEntity();
            registry.addComponent<Transform>(entity, Transform{});
            return entity;
        }

        std::vector<Entity> transformOrder(){
            std::vector<Entity> order;
            ComponentArray<Transform>* transforms = registry.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++)
                order.push_back(transforms->getEntity(i));
            return order;
        }

        Entity parentOf(Entity entity){
            return registry.getComponent<Transform>(entity).parent;
        }

        template<typename T>
        bool has(Entity entity){
            return registry.getSignature(entity).test(registry.getComponentId<T>());
        }
    };

}

TEST_F(EntityRegistryTest, CreatesUserEntitiesByDefault){
    EXPECT_EQ(registry.getDefaultEntityPool(), EntityPool::User);
    EXPECT_EQ(registry.createEntity(), 1001u);
    EXPECT_EQ(registry.createEntity(), 1002u);
    EXPECT_EQ(registry.getLastEntity(), 1002u);
}

TEST_F(EntityRegistryTest, SystemPoolCreatesSystemEntities){
    EntityRegistry system(EntityPool::System);
    EXPECT_EQ(system.createEntity(), 1u);
    EXPECT_EQ(system.createUserEntity(), 1001u);
    EXPECT_EQ(system.createSystemEntity(), 2u);
}

TEST_F(EntityRegistryTest, AddComponentSetsTheSignatureBit){
    const Entity entity = registry.createEntity();
    EXPECT_FALSE(has<Transform>(entity));
    registry.addComponent<Transform>(entity);
    EXPECT_TRUE(has<Transform>(entity));
    EXPECT_FALSE(has<ActionComponent>(entity));
    EXPECT_EQ(registry.getSignature(entity).count(), 1u);
}

TEST_F(EntityRegistryTest, AddComponentNotifiesWithTheComponentId){
    const Entity entity = registry.createEntity();
    registry.addComponent<ActionComponent>(entity, ActionComponent{});
    ASSERT_EQ(registry.added.size(), 1u);
    EXPECT_EQ(registry.added[0].first, entity);
    EXPECT_EQ(registry.added[0].second, registry.getComponentId<ActionComponent>());
}

TEST_F(EntityRegistryTest, GetAndFindComponent){
    const Entity entity = registry.createEntity();
    Transform transform;
    transform.position = Vector3(1, 2, 3);
    registry.addComponent<Transform>(entity, transform);
    EXPECT_EQ(registry.getComponent<Transform>(entity).position, Vector3(1, 2, 3));
    registry.findComponent<Transform>(entity)->position.x = 9;
    EXPECT_EQ(registry.getComponent<Transform>(entity).position.x, 9.0f);
    EXPECT_EQ(registry.findComponent<ActionComponent>(entity), nullptr);
}

TEST_F(EntityRegistryTest, RemoveComponentClearsTheBitAndNotifies){
    const Entity entity = registry.createEntity();
    registry.addComponent<Transform>(entity);
    registry.addComponent<ActionComponent>(entity);
    registry.removeComponent<ActionComponent>(entity);
    EXPECT_FALSE(has<ActionComponent>(entity));
    EXPECT_TRUE(has<Transform>(entity));
    ASSERT_EQ(registry.removed.size(), 1u);
    EXPECT_EQ(registry.removed[0].second, registry.getComponentId<ActionComponent>());
    EXPECT_EQ(registry.findComponent<ActionComponent>(entity), nullptr);
}

TEST_F(EntityRegistryTest, DestroyEntityDropsEveryComponent){
    const Entity entity = registry.createEntity();
    registry.addComponent<Transform>(entity);
    registry.addComponent<ActionComponent>(entity);
    const Signature signature = registry.getSignature(entity);
    registry.destroyEntity(entity);
    EXPECT_FALSE(registry.isEntityCreated(entity));
    EXPECT_FALSE(registry.getComponentArray<Transform>()->hasEntity(entity));
    EXPECT_FALSE(registry.getComponentArray<ActionComponent>()->hasEntity(entity));
    ASSERT_EQ(registry.destroyed.size(), 1u);
    EXPECT_EQ(registry.destroyed[0].second, signature);
}

TEST_F(EntityRegistryTest, DestroyEntitiesTakesOnlyTheListedOnes){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity c = withTransform();
    registry.destroyEntities({a, c, 99999});
    EXPECT_FALSE(registry.isEntityCreated(a));
    EXPECT_TRUE(registry.isEntityCreated(b));
    EXPECT_FALSE(registry.isEntityCreated(c));
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{b}));
    EXPECT_EQ(registry.destroyed.size(), 2u);
}

TEST_F(EntityRegistryTest, DestroyEntitiesOfOne){
    const Entity a = withTransform();
    registry.destroyEntities({a});
    EXPECT_FALSE(registry.isEntityCreated(a));
}

TEST_F(EntityRegistryTest, ClearDestroysEverythingAndRestartsTheIds){
    withTransform();
    withTransform();
    registry.clear();
    EXPECT_TRUE(registry.getEntityList().empty());
    EXPECT_EQ(registry.createEntity(), 1001u);
}

TEST_F(EntityRegistryTest, SetLastEntityOnlyMovesForward){
    registry.setLastEntity(2000);
    EXPECT_EQ(registry.createEntity(), 2001u);
    registry.setLastEntity(1500);
    EXPECT_EQ(registry.createEntity(), 2002u);
}

TEST_F(EntityRegistryTest, RecreateEntityRestoresAnId){
    EXPECT_TRUE(registry.recreateEntity(1500));
    EXPECT_TRUE(registry.isEntityCreated(1500));
    EXPECT_FALSE(registry.recreateEntity(1500));
}

TEST_F(EntityRegistryTest, EveryBuiltInComponentFitsTheSignature){
    // the current contract (S07 lifts it): 64 component types at most
    EXPECT_LT(registry.getComponentId<UIDocumentComponent>(), 64);
    EXPECT_LT(registry.getComponentId<ReflectionProbeComponent>(), 64);
    EXPECT_EQ(registry.getComponentId<MeshComponent>(), 0);
    EXPECT_TRUE(registry.isComponentRegistered<PrefabComponent>());
}

TEST_F(EntityRegistryTest, NamesAndFindEntity){
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.setEntityName(a, "Player");
    registry.setEntityName(b, "Enemy");
    EXPECT_EQ(registry.getEntityName(b), "Enemy");
    EXPECT_EQ(registry.findEntity("Player"), a);
    EXPECT_EQ(registry.findEntity("Nobody"), Entity(NULL_ENTITY));
}

TEST_F(EntityRegistryTest, FindEntityUnderAParent){
    const Entity root1 = withTransform();
    const Entity root2 = withTransform();
    const Entity child1 = withTransform();
    const Entity child2 = withTransform();
    registry.addEntityChild(root1, child1, false);
    registry.addEntityChild(root2, child2, false);
    registry.setEntityName(child1, "Hand");
    registry.setEntityName(child2, "Hand");
    EXPECT_EQ(registry.findEntity("Hand"), child1);
    EXPECT_EQ(registry.findEntity("Hand", root2), child2);
}

TEST_F(EntityRegistryTest, TagsMustBeDefinedToSearch){
    const Entity a = registry.createEntity();
    registry.setEntityTag(a, "Player");
    EXPECT_EQ(registry.getEntityTag(a), "Player");
    EXPECT_EQ(registry.findEntityByTag("Player"), a);
    EXPECT_EQ(registry.findEntitiesByTag("Player"), (std::vector<Entity>{a}));
    LogCapture log;
    EXPECT_EQ(registry.findEntityByTag("NoSuchTag"), Entity(NULL_ENTITY));
    EXPECT_TRUE(registry.findEntitiesByTag("NoSuchTag").empty());
    EXPECT_EQ(log.count(D_LOG_ERROR, "is not defined"), 2u);
}

TEST_F(EntityRegistryTest, LayerChangeNotifiesOnlyWhenItChanges){
    const Entity a = registry.createEntity();
    registry.setEntityLayer(a, 4);
    registry.setEntityLayer(a, 4);
    EXPECT_EQ(registry.getEntityLayer(a), 4);
    EXPECT_EQ(registry.layerChanged, (std::vector<Entity>{a}));
}

TEST_F(EntityRegistryTest, InactiveParentMakesChildrenInactiveInHierarchy){
    const Entity parent = withTransform();
    const Entity child = withTransform();
    registry.addEntityChild(parent, child, false);
    EXPECT_TRUE(registry.isEntityActiveInHierarchy(child));
    registry.setEntityActive(parent, false);
    EXPECT_TRUE(registry.hasInactiveEntities());
    EXPECT_TRUE(registry.isEntityActive(child));
    EXPECT_FALSE(registry.isEntityActiveInHierarchy(child));
    registry.setEntityActive(parent, true);
    EXPECT_TRUE(registry.isEntityActiveInHierarchy(child));
}

TEST_F(EntityRegistryTest, AddEntityChildMovesTheChildAfterItsParent){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity c = withTransform();
    registry.addEntityChild(a, c, false);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{a, c, b}));
    EXPECT_EQ(parentOf(c), a);
    EXPECT_TRUE(registry.isParentOf(a, c));
    EXPECT_FALSE(registry.isParentOf(b, c));
}

TEST_F(EntityRegistryTest, ChildrenGoToTheEndOfTheParentsBranch){
    const Entity parent = withTransform();
    const Entity other = withTransform();
    const Entity first = withTransform();
    const Entity second = withTransform();
    registry.addEntityChild(parent, first, false);
    registry.addEntityChild(parent, second, false);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{parent, first, second, other}));
    EXPECT_EQ(registry.findBranchLastIndex(parent), 2u);
}

TEST_F(EntityRegistryTest, GrandchildrenBelongToTheBranch){
    const Entity root = withTransform();
    const Entity sibling = withTransform();
    const Entity child = withTransform();
    const Entity grandchild = withTransform();
    registry.addEntityChild(root, child, false);
    registry.addEntityChild(child, grandchild, false);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{root, child, grandchild, sibling}));
    EXPECT_TRUE(registry.isParentOf(root, grandchild));
    EXPECT_EQ(registry.findOldestParent(grandchild), root);
    EXPECT_EQ(registry.findBranchLastIndex(root), 2u);
}

TEST_F(EntityRegistryTest, ReparentingMovesTheWholeBranch){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity child = withTransform();
    registry.addEntityChild(a, child, false);
    registry.addEntityChild(a, b, false);
    // b moved under a; now move a's first child (with nothing below it) under b
    registry.addEntityChild(b, child, false);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{a, b, child}));
    EXPECT_EQ(parentOf(child), b);
    EXPECT_EQ(parentOf(b), a);
}

TEST_F(EntityRegistryTest, MovingABranchUnderAnotherRootKeepsItsChildren){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity aChild = withTransform();
    registry.addEntityChild(a, aChild, false);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{a, aChild, b}));
    registry.addEntityChild(b, a, false);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{b, a, aChild}));
    EXPECT_TRUE(registry.isParentOf(b, aChild));
}

TEST_F(EntityRegistryTest, UnparentingKeepsTheChildBehindItsOldBranch){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity child = withTransform();
    registry.addEntityChild(a, child, false);
    registry.addEntityChild(NULL_ENTITY, child, false);
    EXPECT_EQ(parentOf(child), Entity(NULL_ENTITY));
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{a, child, b}));
    EXPECT_FALSE(registry.isParentOf(a, child));
}

TEST_F(EntityRegistryTest, AddingTheSameParentAgainChangesNothing){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity child = withTransform();
    registry.addEntityChild(a, child, false);
    const std::vector<Entity> before = transformOrder();
    registry.addEntityChild(a, child, false);
    EXPECT_EQ(transformOrder(), before);
    (void)b;
}

TEST_F(EntityRegistryTest, RemovingATransformGivesItsChildrenTheGrandparent){
    const Entity root = withTransform();
    const Entity middle = withTransform();
    const Entity leaf = withTransform();
    registry.addEntityChild(root, middle, false);
    registry.addEntityChild(middle, leaf, false);
    registry.removeComponent<Transform>(middle);
    EXPECT_EQ(parentOf(leaf), root);
    EXPECT_TRUE(registry.getComponent<Transform>(leaf).needUpdate);
}

// moveChildUp/Down/ToTop/ToBottom step through the siblings (same parent) by Transform index:
// "up" and "to top" toward the end of the array, "down" and "to bottom" toward its start
TEST_F(EntityRegistryTest, MoveChildUpSwapsWithTheNextSibling){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity c = withTransform();
    registry.moveChildUp(a);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{b, a, c}));
}

TEST_F(EntityRegistryTest, MoveChildToTopGoesPastTheLastSibling){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity c = withTransform();
    registry.moveChildToTop(a);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{b, c, a}));
}

TEST_F(EntityRegistryTest, MoveChildDownSwapsWithThePreviousSibling){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity c = withTransform();
    registry.moveChildDown(c);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{a, c, b}));
}

TEST_F(EntityRegistryTest, MoveChildToBottomGoesBeforeTheFirstSibling){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity c = withTransform();
    registry.moveChildToBottom(c);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{c, a, b}));
}

TEST_F(EntityRegistryTest, MovingAParentMovesItsBranch){
    const Entity parent = withTransform();
    const Entity child = withTransform();
    const Entity other = withTransform();
    registry.addEntityChild(parent, child, false);
    registry.moveChildUp(parent);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{other, parent, child}));
}

TEST_F(EntityRegistryTest, MoveChildToIndexReordersRoots){
    const Entity a = withTransform();
    const Entity b = withTransform();
    const Entity c = withTransform();
    registry.moveChildToIndex(c, 0);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{c, a, b}));
    registry.moveChildToIndex(c, 3);
    EXPECT_EQ(transformOrder(), (std::vector<Entity>{a, b, c}));
}

TEST_F(EntityRegistryTest, ComponentsFollowTheTransformOrder){
    const Entity a = withTransform();
    const Entity b = withTransform();
    registry.addComponent<MeshComponent>(a);
    registry.addComponent<MeshComponent>(b);
    registry.moveChildToIndex(b, 0);
    ComponentArray<MeshComponent>* meshes = registry.getComponentArray<MeshComponent>();
    EXPECT_EQ(meshes->getEntity(0), b);
    EXPECT_EQ(meshes->getEntity(1), a);
}

TEST_F(EntityRegistryTest, SortComponentsByEntityOrder){
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.addComponent<ActionComponent>(a);
    registry.addComponent<ActionComponent>(b);
    registry.sortComponentsByEntityOrder<ActionComponent>({b, a});
    EXPECT_EQ(registry.getComponentArray<ActionComponent>()->getEntity(0), b);
}
