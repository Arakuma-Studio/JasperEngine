// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The arrays of a component (mesh-memory spec 6.1): they hold what is written, read past their
// end without growing, never move a StableArray's elements, and copy deeply.

#include <gtest/gtest.h>

#include "util/GrowArray.h"
#include "util/StableArray.h"

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    struct Item {
        std::string name = "default";
        int value = 0;
    };

}

TEST(GrowArray, WritingPastTheEndGrowsToTheIndex) {
    GrowArray<int> array;
    EXPECT_EQ(array.stored(), 0u);
    array[4] = 7;
    EXPECT_EQ(array.stored(), 5u);
    EXPECT_EQ(array[4], 7);
    EXPECT_EQ(array[0], 0) << "new elements are value-initialised";
}

TEST(GrowArray, ReadingPastTheEndGivesTheDefaultAndDoesNotGrow) {
    GrowArray<Item> array;
    array[1].name = "one";
    const GrowArray<Item>& readOnly = array;
    EXPECT_EQ(readOnly[10].name, "default");
    EXPECT_EQ(readOnly[10].value, 0);
    EXPECT_EQ(array.stored(), 2u);
}

TEST(GrowArray, ValidIndexIsTheSanityLimit) {
    GrowArray<int> array;
    EXPECT_FALSE(array.validIndex(-1));
    EXPECT_TRUE(array.validIndex(0));
    EXPECT_TRUE(array.validIndex(static_cast<int>(ARRAY_INDEX_LIMIT) - 1));
    EXPECT_FALSE(array.validIndex(static_cast<int>(ARRAY_INDEX_LIMIT)));
    EXPECT_EQ(array.stored(), 0u) << "checking allocates nothing";
}

TEST(GrowArray, ResizeClearAndEdits) {
    GrowArray<int> array;
    array.resize(3);
    array[0] = 1;
    array[1] = 2;
    array[2] = 3;
    array.insert(1, 9);
    EXPECT_EQ((std::vector<int>{array[0], array[1], array[2], array[3]}), (std::vector<int>{1, 9, 2, 3}));
    array.erase(0);
    array.move(0, 2);
    EXPECT_EQ((std::vector<int>{array[0], array[1], array[2]}), (std::vector<int>{2, 3, 9}));
    array.clear();
    EXPECT_EQ(array.stored(), 0u);
    EXPECT_EQ(array.storageBytes(), 0u) << "clear frees the storage";
}

TEST(GrowArray, CopyIsDeepAndMoveTakesTheElements) {
    GrowArray<Item> array;
    array[2].name = "two";
    GrowArray<Item> copy = array;
    copy[2].name = "changed";
    EXPECT_EQ(array[2].name, "two");
    GrowArray<Item> moved = std::move(array);
    EXPECT_EQ(moved[2].name, "two");
    EXPECT_EQ(moved.stored(), 3u);
}

TEST(StableArray, GrowthNeverMovesAnElement) {
    StableArray<Item> array;
    array[0].name = "first";
    const Item* first = &array[0];
    for (size_t i = 1; i < 1000; i++) {
        array[i].value = static_cast<int>(i);
    }
    EXPECT_EQ(&array[0], first);
    EXPECT_EQ(first->name, "first");
    EXPECT_EQ(array.stored(), 1000u);
}

TEST(StableArray, ReadingPastTheEndGivesTheDefaultAndDoesNotGrow) {
    StableArray<Item> array;
    const StableArray<Item>& readOnly = array;
    EXPECT_EQ(readOnly[3].name, "default");
    EXPECT_EQ(array.stored(), 0u);
}

TEST(StableArray, CopyIsDeepAndMoveKeepsTheElements) {
    StableArray<Item> array;
    array[1].name = "one";
    StableArray<Item> copy = array;
    copy[1].name = "changed";
    EXPECT_EQ(array[1].name, "one");
    const Item* one = &array[1];
    StableArray<Item> moved = std::move(array);
    EXPECT_EQ(&moved[1], one) << "a move takes the elements themselves";

    // an assignment keeps the elements both hold in place
    StableArray<Item> target;
    target[0].name = "kept";
    const Item* kept = &target[0];
    target = moved;
    EXPECT_EQ(&target[0], kept);
    EXPECT_EQ(target[1].name, "one");
    EXPECT_EQ(target.stored(), 2u);
}

TEST(StableArray, ReverseSwapsTheElements) {
    StableArray<Item> array;
    for (int i = 0; i < 4; i++) array[i].value = i;
    const Item* zero = &array[0];
    array.reverse(0, 3);
    EXPECT_EQ(array[0].value, 2);
    EXPECT_EQ(array[2].value, 0);
    EXPECT_EQ(array[3].value, 3);
    EXPECT_EQ(&array[2], zero) << "an element keeps its address at its new index";
}

TEST(StableArray, ResizeFreesThePastElements) {
    StableArray<Item> array;
    array.resize(5);
    EXPECT_EQ(array.stored(), 5u);
    array.resize(2);
    EXPECT_EQ(array.stored(), 2u);
    array.clear();
    EXPECT_EQ(array.stored(), 0u);
}
