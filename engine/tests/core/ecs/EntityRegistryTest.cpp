// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// EntityRegistry hierarchy order: the Transform array keeps parents before children through
// every structural operation, the Hierarchy-ordered arrays follow it (lazily re-sorted, also
// for components added late), and sibling order matches a plain tree model.

#include "EntityRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <vector>

using namespace jasperengine;

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

TEST(EntityRegistryTest, HierarchyInvariantFuzz){
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

TEST(EntityRegistryTest, LateHierarchyComponentIsOrdered){
    EntityRegistry registry;
    auto make = [&registry](Entity parent){
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

TEST(EntityRegistryTest, RemoveTransformReparentsItsBranchOnly){
    EntityRegistry registry;
    auto make = [&registry](Entity parent){
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

TEST(EntityRegistryTest, DestroyOneByOneCompactsOnce){
    EntityRegistry registry;
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

TEST(EntityRegistryTest, EntityRefDetectsReuse){
    EntityRegistry registry(EntityPool::System);
    const Entity entity = registry.createEntity();
    const EntityRef ref = registry.makeRef(entity);
    EXPECT_TRUE(registry.isAlive(ref));
    registry.destroyEntity(entity);
    EXPECT_FALSE(registry.isAlive(ref));
    EXPECT_EQ(registry.createEntity(), entity); // the lowest free system id again
    EXPECT_FALSE(registry.isAlive(ref));
    EXPECT_TRUE(registry.isAlive(registry.makeRef(entity)));
    // the editor's recreateEntity (undo) keeps the id with a new serial
    registry.destroyEntity(entity);
    EXPECT_TRUE(registry.recreateEntity(entity));
    EXPECT_FALSE(registry.isAlive(ref));
}

TEST(EntityRegistryTest, ComponentNamesAndOrders){
    EntityRegistry registry;
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
