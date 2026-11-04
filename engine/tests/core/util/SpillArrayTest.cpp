// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SpillArray in a build without the editor (where it is its own type): the inline case allocates
// nothing and costs a branch; past Size every element is on the heap, in one block, in order.

#include "util/HybridArray.h"

#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>

using namespace jasperengine;

namespace {

    struct Element {
        std::string text;
        int number = 0;
    };

    using Array = SpillArray<Element, 4>;

}

static_assert(!std::is_same_v<Array, HybridArray<Element, 4>>, "this suite tests a build without the editor");

TEST(SpillArray, TheInlineCaseAllocatesNothing){
    testsupport::ThreadAllocationCount allocations;
    Array array;
    for (size_t i = 0; i < 4; i++){
        array[i].number = (int)i;
    }
    EXPECT_EQ(allocations.count(), 0u);
    EXPECT_EQ(array.size(), 4u);
    EXPECT_EQ(array.data(), &array.values[0]);
    EXPECT_TRUE(array.spilled.empty());
}

TEST(SpillArray, PastItsSizeEveryElementIsKeptInOneBlock){
    Array array;
    for (size_t i = 0; i < 4; i++){
        array[i] = {"inline " + std::to_string(i), (int)i};
    }
    array[9] = {"past", 9};

    EXPECT_EQ(array.size(), 10u);
    EXPECT_NE(array.data(), &array.values[0]);
    for (size_t i = 0; i < 4; i++){
        EXPECT_EQ(array[i].text, "inline " + std::to_string(i));
        EXPECT_EQ(array[i].number, (int)i);
        EXPECT_EQ(&array[i], array.data() + i);
    }
    EXPECT_EQ(array[9].number, 9);
    EXPECT_TRUE(array[8].text.empty()) << "the ones between are default ones";

    // what it moved leaves nothing behind inline
    for (const Element& element : array.values){
        EXPECT_TRUE(element.text.empty());
    }
}

TEST(SpillArray, EveryIndexIsValid){
    Array array;
    EXPECT_FALSE(array.validIndex(-1));
    EXPECT_TRUE(array.validIndex(0));
    EXPECT_TRUE(array.validIndex(1000));
    EXPECT_EQ(array.size(), 4u) << "asking does not grow it";
}

TEST(SpillArray, ACopyHoldsItsOwnElements){
    Array array;
    array[7].text = "seven";
    Array copy = array;
    copy[7].text = "changed";
    EXPECT_EQ(array[7].text, "seven");
    EXPECT_EQ(copy.size(), 8u);

    Array assigned;
    assigned[0].text = "gone";
    assigned = array;
    EXPECT_EQ(assigned[7].text, "seven");
    EXPECT_EQ(assigned.size(), 8u);
}

TEST(SpillArray, AMoveTakesTheHeapBlock){
    Array array;
    array[7].text = "seven";
    const Element* block = array.data();
    Array moved = std::move(array);
    EXPECT_EQ(moved.data(), block);
    EXPECT_EQ(moved[7].text, "seven");
}

TEST(SpillArray, ConstReadsSeeEitherStorage){
    Array array;
    array[1].number = 11;
    const Array& inlineRead = array;
    EXPECT_EQ(inlineRead[1].number, 11);
    array[5].number = 55;
    const Array& spilledRead = array;
    EXPECT_EQ(spilledRead[1].number, 11);
    EXPECT_EQ(spilledRead[5].number, 55);
}
