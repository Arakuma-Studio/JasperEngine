// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Characterization tests of ComponentArray: a dense array of one component type, kept in
// the order the engine iterates (Transform order for most types).

#include "ecs/ComponentArray.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include "reference/EagerComponentArray.h"
#include <random>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct Value {
        int value = 0;
    };

    // a component that can only move (the array must never copy one)
    struct MoveOnly {
        std::unique_ptr<int> value;
        MoveOnly() = default;
        explicit MoveOnly(int v): value(std::make_unique<int>(v)) {}
        MoveOnly(MoveOnly&&) = default;
        MoveOnly& operator=(MoveOnly&&) = default;
    };

    std::vector<Entity> entities(ComponentArray<Value>& array){
        std::vector<Entity> list;
        for (size_t i = 0; i < array.size(); i++)
            list.push_back(array.getEntity(i));
        return list;
    }

    std::vector<int> values(ComponentArray<Value>& array){
        std::vector<int> list;
        for (size_t i = 0; i < array.size(); i++)
            list.push_back(array.getComponentFromIndex(i).value);
        return list;
    }

    // entities 1..count, each with value entity * 10
    void fill(ComponentArray<Value>& array, Entity count){
        for (Entity e = 1; e <= count; e++)
            array.insert(e, Value{int(e) * 10});
    }

}

TEST(ComponentArray, StartsEmpty){
    ComponentArray<Value> array;
    EXPECT_EQ(array.size(), 0u);
    EXPECT_FALSE(array.hasEntity(1));
    EXPECT_EQ(array.findComponent(1), nullptr);
}

TEST(ComponentArray, InsertAppendsInOrder){
    ComponentArray<Value> array;
    array.insert(7, Value{70});
    array.insert(3, Value{30});
    EXPECT_EQ(array.size(), 2u);
    EXPECT_EQ(entities(array), (std::vector<Entity>{7, 3}));
    EXPECT_EQ(array.getIndex(3), 1u);
    EXPECT_EQ(array.getComponent(7).value, 70);
}

TEST(ComponentArray, DefaultInsertValueInitializes){
    ComponentArray<Value> array;
    array.insert(1);
    EXPECT_EQ(array.getComponent(1).value, 0);
}

TEST(ComponentArray, InsertingTwiceIsAnErrorAndKeepsTheFirst){
    ComponentArray<Value> array;
    array.insert(1, Value{1});
    LogCapture log;
    array.insert(1, Value{2});
    EXPECT_EQ(log.count(D_LOG_ERROR, "more than once"), 1u);
    EXPECT_EQ(array.size(), 1u);
    EXPECT_EQ(array.getComponent(1).value, 1);
}

TEST(ComponentArray, RemoveKeepsTheArrayDenseAndInOrder){
    ComponentArray<Value> array;
    fill(array, 5);
    array.remove(2);
    EXPECT_EQ(array.size(), 4u);
    EXPECT_EQ(entities(array), (std::vector<Entity>{1, 3, 4, 5}));
    EXPECT_EQ(values(array), (std::vector<int>{10, 30, 40, 50}));
    EXPECT_EQ(array.getIndex(5), 3u);
    EXPECT_FALSE(array.hasEntity(2));
}

TEST(ComponentArray, RemovingAMissingEntityIsAnError){
    ComponentArray<Value> array;
    fill(array, 2);
    LogCapture log;
    array.remove(9);
    EXPECT_EQ(log.count(D_LOG_ERROR, "non-existent"), 1u);
    EXPECT_EQ(array.size(), 2u);
}

TEST(ComponentArray, FindComponentReturnsAPointerIntoTheArray){
    ComponentArray<Value> array;
    fill(array, 3);
    Value* value = array.findComponent(2);
    ASSERT_NE(value, nullptr);
    value->value = 99;
    EXPECT_EQ(array.getComponent(2).value, 99);
}

TEST(ComponentArray, GetComponentOfAMissingEntityThrows){
    ComponentArray<Value> array;
    LogCapture log;
    EXPECT_THROW(array.getComponent(4), std::out_of_range);
    EXPECT_THROW(array.getIndex(4), std::out_of_range);
    EXPECT_EQ(log.count(D_LOG_ERROR, "non-existent"), 2u);
}

TEST(ComponentArray, IndexAccessorsCheckTheRange){
    ComponentArray<Value> array;
    fill(array, 2);
    EXPECT_EQ(array.findComponentFromIndex(1)->value, 20);
    EXPECT_EQ(array.findComponentFromIndex(2), nullptr);
    LogCapture log;
    EXPECT_THROW(array.getComponentFromIndex(5), std::out_of_range);
    EXPECT_EQ(array.getEntity(5), Entity(NULL_ENTITY));
    EXPECT_EQ(log.errors().size(), 2u);
}

TEST(ComponentArray, EntityDestroyedRemovesItsComponent){
    ComponentArray<Value> array;
    fill(array, 3);
    array.entityDestroyed(1);
    EXPECT_EQ(entities(array), (std::vector<Entity>{2, 3}));
}

TEST(ComponentArray, EntityDestroyedWithoutAComponentIsSilent){
    ComponentArray<Value> array;
    fill(array, 3);
    LogCapture log;
    array.entityDestroyed(8);
    EXPECT_TRUE(log.errors().empty());
    EXPECT_EQ(array.size(), 3u);
}

TEST(ComponentArray, EntitiesDestroyedCompactsOnceKeepingTheOrder){
    ComponentArray<Value> array;
    fill(array, 6);
    array.entitiesDestroyed({2, 5, 9});
    EXPECT_EQ(entities(array), (std::vector<Entity>{1, 3, 4, 6}));
    EXPECT_EQ(values(array), (std::vector<int>{10, 30, 40, 60}));
    for (size_t i = 0; i < array.size(); i++)
        EXPECT_EQ(array.getIndex(array.getEntity(i)), i);
    EXPECT_FALSE(array.hasEntity(5));
}

TEST(ComponentArray, EntitiesDestroyedOfNoneOfItsEntitiesChangesNothing){
    ComponentArray<Value> array;
    fill(array, 3);
    array.entitiesDestroyed({7, 8});
    EXPECT_EQ(entities(array), (std::vector<Entity>{1, 2, 3}));
}

TEST(ComponentArray, EntitiesDestroyedOfEveryEntityEmptiesIt){
    ComponentArray<Value> array;
    fill(array, 3);
    array.entitiesDestroyed({1, 2, 3});
    EXPECT_EQ(array.size(), 0u);
}

TEST(ComponentArray, MoveEntityToALowerIndex){
    ComponentArray<Value> array;
    fill(array, 5);
    array.moveEntityToIndex(4, 1);
    EXPECT_EQ(entities(array), (std::vector<Entity>{1, 4, 2, 3, 5}));
    EXPECT_EQ(array.getIndex(4), 1u);
    EXPECT_EQ(array.getComponent(4).value, 40);
}

TEST(ComponentArray, MoveEntityToAHigherIndex){
    ComponentArray<Value> array;
    fill(array, 5);
    array.moveEntityToIndex(1, 3);
    EXPECT_EQ(entities(array), (std::vector<Entity>{2, 3, 4, 1, 5}));
    EXPECT_EQ(array.getIndex(1), 3u);
}

TEST(ComponentArray, MoveEntityRangeKeepsTheRangeTogether){
    ComponentArray<Value> array;
    fill(array, 6);
    array.moveEntityRangeToIndex(4, 5, 0);
    EXPECT_EQ(entities(array), (std::vector<Entity>{4, 5, 1, 2, 3, 6}));
    EXPECT_EQ(values(array), (std::vector<int>{40, 50, 10, 20, 30, 60}));
}

TEST(ComponentArray, MoveEntityRangePastTheEndIsAnError){
    ComponentArray<Value> array;
    fill(array, 4);
    LogCapture log;
    array.moveEntityRangeToIndex(1, 2, 3);
    EXPECT_EQ(log.count(D_LOG_ERROR, "out of array"), 1u);
    EXPECT_EQ(entities(array), (std::vector<Entity>{1, 2, 3, 4}));
}

TEST(ComponentArray, SortByEntityOrderFollowsTheList){
    ComponentArray<Value> array;
    fill(array, 4);
    array.sortByEntityOrder({3, 1, 4, 2});
    EXPECT_EQ(entities(array), (std::vector<Entity>{3, 1, 4, 2}));
    EXPECT_EQ(values(array), (std::vector<int>{30, 10, 40, 20}));
    EXPECT_EQ(array.getIndex(2), 3u);
}

TEST(ComponentArray, SortByEntityOrderPutsTheUnlistedLastInTheirOrder){
    ComponentArray<Value> array;
    fill(array, 5);
    array.sortByEntityOrder({4, 2});
    EXPECT_EQ(entities(array), (std::vector<Entity>{4, 2, 1, 3, 5}));
}

TEST(ComponentArray, SortByComponentFollowsTheOtherArray){
    ComponentArray<Value> array;
    fill(array, 4);
    ComponentArray<Value> order;
    order.insert(3);
    order.insert(1);
    order.insert(2);
    array.sortByComponent(&order);
    // entity 4 has no component in the other array: last
    EXPECT_EQ(entities(array), (std::vector<Entity>{3, 1, 2, 4}));
}

TEST(ComponentArray, GrowingMovesComponentsInsteadOfCopying){
    ComponentArray<MoveOnly> array;
    for (Entity e = 1; e <= 100; e++)
        array.insert(e, MoveOnly(int(e)));
    ASSERT_EQ(array.size(), 100u);
    for (Entity e = 1; e <= 100; e++)
        EXPECT_EQ(*array.getComponent(e).value, int(e));
}

TEST(ComponentArray, IndicesStayConsistentAfterMixedOperations){
    ComponentArray<Value> array;
    fill(array, 8);
    array.remove(3);
    array.moveEntityToIndex(8, 0);
    array.entitiesDestroyed({1, 6});
    array.insert(20, Value{200});
    array.sortByEntityOrder({20, 2, 4, 5, 7, 8});
    EXPECT_EQ(entities(array), (std::vector<Entity>{20, 2, 4, 5, 7, 8}));
    for (size_t i = 0; i < array.size(); i++){
        EXPECT_EQ(array.getIndex(array.getEntity(i)), i);
        EXPECT_EQ(array.getComponentFromIndex(i).value, array.getEntity(i) == 20 ? 200 : int(array.getEntity(i)) * 10);
    }
}

// ---------------------------------------------------------------------------- S07: the paged array with O(1) removal and lazy stable compaction
// ComponentArray: O(1) removal with lazy stable compaction must look, to every index-based
// observer, exactly like the eager-erase array it replaced (reference/EagerComponentArray.h),
// release a removed component's resources at once, keep references across adds and free
// its index pages.

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

// findNextAfter steps over runs of holes through skip links it shortens as it goes: whatever the
// order of appends, removals (the last slot's too) and lookups, with no index read to close the
// holes, it answers the next live component in slot order
TEST(ComponentArray, FindNextAfterStepsOverHolesInAnyOrder){
    std::mt19937 rng(7);
    ComponentArray<Value> array;
    std::vector<Entity> live; // slot order
    Entity nextId = 1;
    size_t mostHoles = 0;
    for (int step = 0; step < 20000; step++){
        const unsigned op = rng() % 10;
        if (op < 3 || live.empty()){
            array.insert(nextId, Value{int(nextId)});
            live.push_back(nextId++);
        }else if (op < 6){
            // newest first half the time: the runs of holes behind a slot grow
            const size_t i = (rng() % 2) ? live.size() - 1 - rng() % std::min<size_t>(live.size(), 3) : rng() % live.size();
            array.remove(live[i]);
            live.erase(live.begin() + (ptrdiff_t)i);
        }else{
            const size_t i = rng() % live.size();
            const Value* next = array.findNextAfter(live[i]);
            if (i + 1 < live.size()){
                ASSERT_NE(next, nullptr) << "step " << step;
                ASSERT_EQ(next->value, int(live[i + 1])) << "step " << step;
            }else{
                ASSERT_EQ(next, nullptr) << "step " << step;
            }
        }
        mostHoles = std::max(mostHoles, array.getHoleCount());
    }
    EXPECT_GT(mostHoles, 100u) << "the lookups never met a long run of holes";
    EXPECT_EQ(array.size(), live.size());
}
