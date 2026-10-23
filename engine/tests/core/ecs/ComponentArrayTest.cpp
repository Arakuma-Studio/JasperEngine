// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ComponentArray: O(1) removal with lazy stable compaction must look, to every index-based
// observer, exactly like the eager-erase array it replaced (reference/EagerComponentArray.h),
// release a removed component's resources at once, keep references across adds and free
// its index pages.

#include "ecs/ComponentArray.h"
#include "reference/EagerComponentArray.h"

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using namespace jasperengine;

namespace {

    struct Payload {
        std::string name;
        std::vector<int> values;
        std::shared_ptr<int> shared;

        Payload() = default;
        explicit Payload(int value): name("payload-" + std::to_string(value)), values{value, value + 1}, shared(std::make_shared<int>(value)) {}
    };

    // the same operations on the new array and the reference, observations compared
    class Differential {
    public:
        ComponentArray<Payload> array;
        reference::EagerComponentArray<Payload> model;
        // the arrays sortByComponent orders by (kept equal too)
        ComponentArray<int> other;
        reference::EagerComponentArray<int> otherModel;

        std::mt19937 random;
        std::vector<Entity> present;
        int nextValue = 0;

        explicit Differential(unsigned seed): random(seed) {}

        Entity anyEntity(){
            return 1001 + (Entity)(random() % 400);
        }

        size_t pick(size_t count){
            return count ? random() % count : 0;
        }

        void refreshPresent(){
            present.clear();
            for (size_t i = 0; i < model.size(); i++){
                present.push_back(model.getEntity(i));
            }
        }

        void observe(){
            ASSERT_EQ(array.size(), model.size());
            for (size_t i = 0; i < model.size(); i++){
                const Entity entity = model.getEntity(i);
                ASSERT_EQ(array.getEntity(i), entity) << "index " << i;
                ASSERT_EQ(array.getIndex(entity), model.getIndex(entity));
                ASSERT_EQ(array.getComponentFromIndex(i).name, model.getComponentFromIndex(i).name);
                ASSERT_EQ(array.findComponentFromIndex(i)->values, model.findComponentFromIndex(i)->values);
                ASSERT_EQ(array.getComponent(entity).name, model.getComponent(entity).name);
            }
            ASSERT_EQ(array.findComponentFromIndex(model.size()), nullptr);
            ASSERT_EQ(other.size(), otherModel.size());
            for (size_t i = 0; i < otherModel.size(); i++){
                ASSERT_EQ(other.getEntity(i), otherModel.getEntity(i));
            }
        }

        void step(){
            refreshPresent();
            const unsigned op = random() % 100;
            if (op < 30){
                const Entity entity = anyEntity();
                if (!model.hasEntity(entity)){
                    Payload payload(nextValue++);
                    array.insert(entity, payload);
                    model.insert(entity, payload);
                }
            }else if (op < 50){
                if (!present.empty()){
                    const Entity entity = present[pick(present.size())];
                    array.remove(entity);
                    model.remove(entity);
                }
            }else if (op < 55){
                std::unordered_set<Entity> doomed;
                const size_t count = 1 + pick(8);
                for (size_t i = 0; i < count; i++){
                    doomed.insert(present.empty() || random() % 4 == 0 ? anyEntity() : present[pick(present.size())]);
                }
                array.entitiesDestroyed(doomed);
                model.entitiesDestroyed(doomed);
                other.entitiesDestroyed(doomed);
                otherModel.entitiesDestroyed(doomed);
            }else if (op < 58){
                for (size_t i = 0; i < 5 && !present.empty(); i++){
                    const Entity entity = present[pick(present.size())];
                    if (array.hasEntity(entity)){
                        array.entityDestroyed(entity);
                        model.entityDestroyed(entity);
                    }
                }
            }else if (op < 68){
                if (present.size() > 1){
                    const Entity entity = present[pick(present.size())];
                    const size_t index = pick(present.size());
                    array.moveEntityToIndex(entity, index);
                    model.moveEntityToIndex(entity, index);
                }
            }else if (op < 78){
                if (present.size() > 2){
                    size_t first = pick(present.size());
                    size_t last = pick(present.size());
                    if (first > last) std::swap(first, last);
                    const size_t length = last - first + 1;
                    const size_t destination = pick(present.size() - length + 1);
                    array.moveEntityRangeToIndex(present[first], present[last], destination);
                    model.moveEntityRangeToIndex(present[first], present[last], destination);
                }
            }else if (op < 84){
                // the order source changes as Transforms would: adds, removals, moves
                for (int i = 0; i < 6; i++){
                    const Entity entity = anyEntity();
                    if (otherModel.hasEntity(entity)){
                        if (random() % 2){
                            other.remove(entity);
                            otherModel.remove(entity);
                        }
                    }else{
                        other.insert(entity, (int)entity);
                        otherModel.insert(entity, (int)entity);
                    }
                }
                if (otherModel.size() > 1){
                    const Entity entity = otherModel.getEntity(pick(otherModel.size()));
                    const size_t index = pick(otherModel.size());
                    other.moveEntityToIndex(entity, index);
                    otherModel.moveEntityToIndex(entity, index);
                }
            }else if (op < 90){
                array.sortByComponent(&other);
                model.sortByComponent(&otherModel);
            }else if (op < 94){
                std::vector<Entity> order;
                for (int i = 0; i < 60; i++){
                    order.push_back(random() % 3 == 0 || present.empty() ? anyEntity() : present[pick(present.size())]);
                }
                array.sortByEntityOrder(order);
                model.sortByEntityOrder(order);
            }else{
                observe();
            }
        }
    };

}

TEST(ComponentArrayTest, DifferentialAgainstEagerModel){
    for (unsigned seed = 1; seed <= 200; seed++){
        Differential test(seed);
        for (int op = 0; op < 2000; op++){
            test.step();
            if (::testing::Test::HasFatalFailure()){
                FAIL() << "seed " << seed << ", operation " << op;
            }
        }
        test.observe();
        if (::testing::Test::HasFatalFailure()){
            FAIL() << "seed " << seed << ", final observation";
        }
    }
}

TEST(ComponentArrayTest, RemoveReleasesResourcesAtOnce){
    ComponentArray<Payload> array;
    std::vector<std::weak_ptr<int>> held;
    for (Entity entity = 1001; entity <= 1010; entity++){
        array.insert(entity, Payload((int)entity));
        held.push_back(array.getComponent(entity).shared);
    }

    // no index observation in between: the holes are still there, the resources are not
    array.remove(1003);
    array.entityDestroyed(1007);
    EXPECT_TRUE(held[2].expired());
    EXPECT_EQ(held[2].use_count(), 0);
    EXPECT_TRUE(held[6].expired());
    EXPECT_FALSE(held[0].expired());
    EXPECT_EQ(array.getHoleCount(), 2u);
    EXPECT_EQ(array.size(), 8u);

    // the first index observation compacts once
    const size_t compactions = array.getCompactionCount();
    EXPECT_EQ(array.getEntity(2), (Entity)1004);
    EXPECT_EQ(array.getCompactionCount(), compactions + 1);
    EXPECT_EQ(array.getHoleCount(), 0u);
    EXPECT_FALSE(held[3].expired());
}

TEST(ComponentArrayTest, LookupsByEntityDoNotCompact){
    ComponentArray<Payload> array;
    for (Entity entity = 1001; entity <= 1100; entity++){
        array.insert(entity, Payload((int)entity));
    }
    for (Entity entity = 1001; entity <= 1100; entity += 2){
        array.remove(entity);
    }
    EXPECT_EQ(array.getHoleCount(), 50u); // none of them was the last slot
    EXPECT_NE(array.findComponent(1002), nullptr);
    EXPECT_EQ(array.findComponent(1001), nullptr);
    EXPECT_TRUE(array.hasEntity(1050));
    EXPECT_EQ(array.getComponent(1050).name, "payload-1050");
    EXPECT_EQ(array.getCompactionCount(), 0u);
}

TEST(ComponentArrayTest, ReferencesStableAcrossAdds){
    ComponentArray<Payload> array;
    array.insert(1001, Payload(1));
    Payload* first = &array.getComponent(1001);
    const std::string* name = &first->name;
    for (Entity entity = 1002; entity < 1002 + 10000; entity++){
        array.insert(entity, Payload((int)entity));
    }
    EXPECT_EQ(&array.getComponent(1001), first);
    EXPECT_EQ(&array.getComponent(1001).name, name);
    EXPECT_EQ(array.getComponent(1001).name, "payload-1");
    // an argument naming a component of the same array (the add that grows a page)
    ComponentArray<Payload> copies;
    copies.insert(1, Payload(7));
    for (Entity entity = 2; entity < 2 + 2 * ComponentArray<Payload>::pageCapacity(); entity++){
        copies.insert(entity, copies.getComponent(1));
    }
    EXPECT_EQ(copies.getComponent(1 + ComponentArray<Payload>::pageCapacity()).name, "payload-7");
}

TEST(ComponentArrayTest, SparseIndexPagesFreed){
    ComponentArray<int> array;
    for (Entity entity = 1001; entity <= 200000; entity++){
        array.insert(entity, (int)entity);
    }
    EXPECT_GT(array.indexPageCount(), 40u);
    for (Entity entity = 1001; entity <= 200000; entity++){
        array.remove(entity);
    }
    EXPECT_EQ(array.indexPageCount(), 0u);
    EXPECT_EQ(array.size(), 0u);
}

TEST(ComponentArrayTest, UnorderedSwapAndPop){
    ComponentArray<int> array;
    array.setOrder(ComponentOrder::Unordered);
    for (Entity entity = 1; entity <= 5; entity++){
        array.insert(entity, (int)entity * 10);
    }
    array.remove(2);
    EXPECT_EQ(array.getHoleCount(), 0u);
    EXPECT_EQ(array.size(), 4u);
    EXPECT_EQ(array.getEntity(1), (Entity)5);
    EXPECT_EQ(array.getComponent(5), 50);
    EXPECT_EQ(array.getIndex(5), 1u);
}

TEST(ComponentArrayTest, FindOwnerOfComponentAddress){
    ComponentArray<Payload> array;
    for (Entity entity = 1001; entity < 1001 + 300; entity++){
        array.insert(entity, Payload((int)entity));
    }
    EXPECT_EQ(array.findOwner(&array.getComponent(1250)), (Entity)1250);
    Payload outside;
    EXPECT_EQ(array.findOwner(&outside), NULL_ENTITY);
}
