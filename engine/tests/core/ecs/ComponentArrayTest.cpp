// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Characterization tests of ComponentArray: a dense array of one component type, kept in
// the order the engine iterates (Transform order for most types).

#include "ecs/ComponentArray.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>

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
