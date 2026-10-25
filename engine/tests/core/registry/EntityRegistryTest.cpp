// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Characterization tests of EntityRegistry: components and signatures, entity lifetime, the
// metadata (names, tags, layers, active flags) and the Transform hierarchy, whose order is the
// order of the Transform array (a parent, then its whole branch).

#include "registry/EntityRegistry.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
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

// ---------------------------------------------------------------------------- S07: hierarchy order
// The Transform array keeps parents before children through every structural operation, the
// Hierarchy-ordered arrays follow it (lazily re-sorted, also for components added late), and
// sibling order matches a plain tree model.

namespace {

    // the hierarchy as a tree: children in sibling order, NULL_ENTITY holds the roots
    struct TreeModel {
        std::map<Entity, std::vector<Entity>> children;
        std::map<Entity, Entity> parents;

        std::vector<Entity>& siblingsOf(Entity entity){
            return children[parents[entity]];
        }

        void add(Entity entity, Entity parent){
            parents[entity] = parent;
            children[parent].push_back(entity);
        }

        void detach(Entity entity){
            auto& siblings = siblingsOf(entity);
            siblings.erase(std::find(siblings.begin(), siblings.end(), entity));
        }

        bool inSubtree(Entity root, Entity entity){
            for (Entity e = entity; e != NULL_ENTITY; e = parents[e]){
                if (e == root) return true;
            }
            return false;
        }

        Entity oldest(Entity entity){
            while (parents[entity] != NULL_ENTITY) entity = parents[entity];
            return entity;
        }

        void subtree(Entity root, std::vector<Entity>& out){
            out.push_back(root);
            for (Entity child : children[root]) subtree(child, out);
        }

        void erase(Entity root){
            std::vector<Entity> doomed;
            subtree(root, doomed);
            detach(root);
            for (Entity entity : doomed){
                children.erase(entity);
                parents.erase(entity);
            }
        }

        // removeComponent<Transform>: the children take its place below its parent
        void dissolve(Entity entity){
            auto& siblings = siblingsOf(entity);
            auto at = std::find(siblings.begin(), siblings.end(), entity);
            std::vector<Entity> moved = children[entity];
            for (Entity child : moved) parents[child] = parents[entity];
            at = siblings.erase(at);
            siblings.insert(at, moved.begin(), moved.end());
            children.erase(entity);
            parents.erase(entity);
        }
    };

    class HierarchyFuzz {
    public:
        EntityRegistry registry;
        TreeModel model;
        std::mt19937 random;

        explicit HierarchyFuzz(unsigned seed): random(seed) {}

        std::vector<Entity> treeEntities(){
            std::vector<Entity> list;
            for (const auto& [entity, parent] : model.parents) list.push_back(entity);
            return list;
        }

        Entity anyTreeEntity(){
            auto list = treeEntities();
            return list.empty() ? NULL_ENTITY : list[random() % list.size()];
        }

        void create(){
            const Entity parent = random() % 3 == 0 ? NULL_ENTITY : anyTreeEntity();
            const Entity entity = registry.createEntity();
            registry.addComponent<Transform>(entity);
            if (random() % 3 == 0) registry.addComponent<MeshComponent>(entity);
            if (parent != NULL_ENTITY){
                registry.addEntityChild(parent, entity, false);
            }
            model.add(entity, parent);
        }

        void reparent(){
            const Entity child = anyTreeEntity();
            if (child == NULL_ENTITY) return;
            const Entity parent = random() % 4 == 0 ? NULL_ENTITY : anyTreeEntity();
            if (parent != NULL_ENTITY && model.inSubtree(child, parent)) return;
            if (model.parents[child] == parent) return;
            registry.addEntityChild(parent, child, false);
            if (parent == NULL_ENTITY){
                const Entity root = model.oldest(child);
                model.detach(child);
                model.parents[child] = NULL_ENTITY;
                auto& roots = model.children[NULL_ENTITY];
                roots.insert(std::find(roots.begin(), roots.end(), root) + 1, child);
            }else{
                model.detach(child);
                model.add(child, parent);
            }
        }

        void moveSibling(){
            const Entity entity = anyTreeEntity();
            if (entity == NULL_ENTITY) return;
            auto& siblings = model.siblingsOf(entity);
            const size_t at = (size_t)(std::find(siblings.begin(), siblings.end(), entity) - siblings.begin());
            switch (random() % 4){
                case 0: // after the next sibling
                    registry.moveChildUp(entity);
                    if (at + 1 < siblings.size()) std::swap(siblings[at], siblings[at + 1]);
                    break;
                case 1: // last
                    registry.moveChildToTop(entity);
                    siblings.erase(siblings.begin() + (ptrdiff_t)at);
                    siblings.push_back(entity);
                    break;
                case 2: // before the previous sibling
                    registry.moveChildDown(entity);
                    if (at > 0) std::swap(siblings[at], siblings[at - 1]);
                    break;
                default: // first
                    registry.moveChildToBottom(entity);
                    siblings.erase(siblings.begin() + (ptrdiff_t)at);
                    siblings.insert(siblings.begin(), entity);
                    break;
            }
        }

        void removeTransform(){
            const Entity entity = anyTreeEntity();
            if (entity == NULL_ENTITY) return;
            registry.removeComponent<Transform>(entity);
            model.dissolve(entity);
        }

        void destroy(){
            const Entity entity = anyTreeEntity();
            if (entity == NULL_ENTITY) return;
            registry.destroyHierarchy(entity);
            model.erase(entity);
        }

        // PasteEntitiesCmd: a run of consecutive top-level branches moved to the end at once
        void pasteMove(){
            auto& roots = model.children[NULL_ENTITY];
            if (roots.size() < 2) return;
            const size_t first = random() % roots.size();
            const size_t last = std::min(roots.size() - 1, first + random() % 3);
            auto transforms = registry.getComponentArray<Transform>();
            const size_t start = transforms->getIndex(roots[first]);
            const size_t end = registry.findBranchLastIndex(roots[last]);
            const size_t count = end - start + 1;
            Signature moved;
            for (size_t i = start; i <= end; i++){
                moved |= registry.getSignature(transforms->getEntity(i));
            }
            transforms->moveEntityRangeToIndex(transforms->getEntity(start), transforms->getEntity(end), transforms->size() - count);
            registry.sortComponentsByTransform(moved);
            std::vector<Entity> block(roots.begin() + (ptrdiff_t)first, roots.begin() + (ptrdiff_t)last + 1);
            roots.erase(roots.begin() + (ptrdiff_t)first, roots.begin() + (ptrdiff_t)last + 1);
            roots.insert(roots.end(), block.begin(), block.end());
        }

        // a Hierarchy component added to or removed from any entity, mid-hierarchy included
        void toggleHierarchyComponent(){
            const Entity entity = anyTreeEntity();
            if (entity == NULL_ENTITY) return;
            if (random() % 2){
                if (registry.findComponent<UIComponent>(entity)) registry.removeComponent<UIComponent>(entity);
                else registry.addComponent<UIComponent>(entity);
            }else{
                if (registry.findComponent<MeshComponent>(entity)) registry.removeComponent<MeshComponent>(entity);
                else registry.addComponent<MeshComponent>(entity);
            }
        }

        template<typename T>
        void expectTransformOrder(const char* type){
            auto transforms = registry.getComponentArray<Transform>();
            auto array = registry.getComponentArray<T>();
            size_t previous = 0;
            for (size_t i = 0; i < array->size(); i++){
                const Entity entity = array->getEntity(i);
                const size_t key = transforms->hasEntity(entity) ? transforms->getIndex(entity) : SIZE_MAX;
                ASSERT_LE(previous, key) << type << " index " << i;
                previous = key;
            }
        }

        void check(){
            auto transforms = registry.getComponentArray<Transform>();
            std::map<Entity, std::vector<Entity>> derived;
            for (size_t i = 0; i < transforms->size(); i++){
                const Entity entity = transforms->getEntity(i);
                const Entity parent = transforms->getComponentFromIndex(i).parent;
                if (parent != NULL_ENTITY && transforms->hasEntity(parent)){
                    ASSERT_LT(transforms->getIndex(parent), i) << "parent after child " << entity;
                }
                derived[parent].push_back(entity);
            }
            expectTransformOrder<MeshComponent>("MeshComponent");
            expectTransformOrder<UIComponent>("UIComponent");
            for (auto& [parent, list] : model.children){
                if (list.empty()) continue;
                ASSERT_EQ(derived[parent], list) << "children of " << parent;
            }
            size_t modelCount = model.parents.size();
            ASSERT_EQ(transforms->size(), modelCount);
        }
    };

}

// over 5 s: a Slow suite (docs/testing.md)
TEST(EntityRegistryTestSlow, HierarchyInvariantFuzz){
    for (unsigned seed = 1; seed <= 100; seed++){
        HierarchyFuzz fuzz(seed);
        for (int step = 0; step < 400; step++){
            const unsigned op = fuzz.random() % 100;
            if (op < 30) fuzz.create();
            else if (op < 45) fuzz.reparent();
            else if (op < 65) fuzz.moveSibling();
            else if (op < 70) fuzz.removeTransform();
            else if (op < 77) fuzz.destroy();
            else if (op < 83) fuzz.pasteMove();
            else fuzz.toggleHierarchyComponent();

            fuzz.check();
            if (::testing::Test::HasFatalFailure()){
                FAIL() << "seed " << seed << ", step " << step;
            }
        }
    }
}

TEST_F(EntityRegistryTest, LateHierarchyComponentIsOrdered){
    auto make = [this](Entity parent){
        const Entity entity = registry.createEntity();
        registry.addComponent<Transform>(entity);
        if (parent != NULL_ENTITY) registry.addEntityChild(parent, entity, false);
        return entity;
    };
    const Entity a = make(NULL_ENTITY);
    const Entity b = make(a);
    const Entity c = make(a);
    const Entity d = make(NULL_ENTITY);
    (void)c;

    registry.addComponent<MeshComponent>(d);
    registry.addComponent<MeshComponent>(a);
    registry.addComponent<MeshComponent>(b); // mid-hierarchy, after the others

    auto meshes = registry.getComponentArray<MeshComponent>();
    ASSERT_EQ(meshes->size(), 3u);
    EXPECT_EQ(meshes->getEntity(0), a);
    EXPECT_EQ(meshes->getEntity(1), b);
    EXPECT_EQ(meshes->getEntity(2), d);
    EXPECT_FALSE(meshes->isOrderDirty());
}

TEST_F(EntityRegistryTest, RemoveTransformReparentsItsBranchOnly){
    auto make = [this](Entity parent){
        const Entity entity = registry.createEntity();
        registry.addComponent<Transform>(entity);
        if (parent != NULL_ENTITY) registry.addEntityChild(parent, entity, false);
        return entity;
    };
    const Entity root = make(NULL_ENTITY);
    const Entity middle = make(root);
    const Entity child = make(middle);
    const Entity grandchild = make(child);
    const Entity other = make(NULL_ENTITY);
    registry.removeComponent<Transform>(middle);
    EXPECT_EQ(registry.getComponent<Transform>(child).parent, root);
    EXPECT_EQ(registry.getComponent<Transform>(grandchild).parent, child);
    EXPECT_EQ(registry.getComponent<Transform>(other).parent, NULL_ENTITY);
}

TEST_F(EntityRegistryTest, DestroyOneByOneCompactsOnce){
    std::vector<Entity> entities;
    for (int i = 0; i < 100; i++){
        const Entity entity = registry.createEntity();
        registry.addComponent<Transform>(entity);
        entities.push_back(entity);
    }
    auto transforms = registry.getComponentArray<Transform>();
    const size_t compactions = transforms->getCompactionCount();
    for (size_t i = 0; i < entities.size(); i += 2){
        registry.destroyEntity(entities[i]);
    }
    EXPECT_EQ(transforms->getCompactionCount(), compactions);
    EXPECT_EQ(transforms->getEntity(0), entities[1]);
    EXPECT_EQ(transforms->getCompactionCount(), compactions + 1);
    EXPECT_EQ(transforms->size(), 50u);
}

TEST_F(EntityRegistryTest, EntityRefDetectsReuse){
    EntityRegistry systemRegistry(EntityPool::System);
    const Entity entity = systemRegistry.createEntity();
    const EntityRef ref = systemRegistry.makeRef(entity);
    EXPECT_TRUE(systemRegistry.isAlive(ref));
    systemRegistry.destroyEntity(entity);
    EXPECT_FALSE(systemRegistry.isAlive(ref));
    EXPECT_EQ(systemRegistry.createEntity(), entity); // the lowest free system id again
    EXPECT_FALSE(systemRegistry.isAlive(ref));
    EXPECT_TRUE(systemRegistry.isAlive(systemRegistry.makeRef(entity)));
    // the editor's recreateEntity (undo) keeps the id with a new serial
    systemRegistry.destroyEntity(entity);
    EXPECT_TRUE(systemRegistry.recreateEntity(entity));
    EXPECT_FALSE(systemRegistry.isAlive(ref));
}

TEST_F(EntityRegistryTest, ComponentNamesAndOrders){
    ComponentId id;
    ASSERT_TRUE(registry.findComponentId("MeshComponent", id));
    EXPECT_EQ(id, registry.getComponentId<MeshComponent>());
    EXPECT_EQ(registry.getComponentName(registry.getComponentId<Transform>()), "Transform");
    EXPECT_FALSE(registry.findComponentId("NoSuchComponent", id));
    EXPECT_EQ(registry.getComponentArray<MeshComponent>()->getOrder(), ComponentOrder::Hierarchy);
    EXPECT_EQ(registry.getComponentArray<AudioSourceComponent>()->getOrder(), ComponentOrder::Hierarchy);
    EXPECT_EQ(registry.getComponentArray<SkyboxComponent>()->getOrder(), ComponentOrder::Manual);
    EXPECT_EQ(registry.getComponentArray<Transform>()->getOrder(), ComponentOrder::Insertion);
    EXPECT_EQ(registry.getComponentArray<SpriteComponent>()->getOrder(), ComponentOrder::Insertion);
}
