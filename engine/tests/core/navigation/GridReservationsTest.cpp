// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "navigation/GridPathfinder.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

using namespace jasperengine::nav;

TEST(GridReservations, ReplacesClaimsAndKeepsLongerReservations){
    GridReservations reservations;
    reservations.reset(8);
    reservations.reserve(1, {-1, 0, 1, 1, 8}, 10);
    reservations.reserve(2, {1, 2}, 9);
    EXPECT_EQ(reservations.holder(1, 9), 1u);
    EXPECT_EQ(reservations.holder(2, 9), 2u);
    EXPECT_TRUE(reservations.isReservedByOther(0, 2, 10));
    EXPECT_FALSE(reservations.isReservedByOther(0, 1, 10));
    EXPECT_FALSE(reservations.isReservedByOther(0, 2, 11));

    reservations.reserve(1, {3}, 20);
    EXPECT_EQ(reservations.holder(0, 0), 0u);
    EXPECT_EQ(reservations.holder(1, 0), 0u);
    EXPECT_EQ(reservations.holder(2, 9), 2u);
    EXPECT_EQ(reservations.holder(3, 20), 1u);
    EXPECT_EQ(reservations.holder(3, 21), 0u);
    reservations.reserve(1, {}, 30);
    EXPECT_EQ(reservations.holder(3, 0), 0u);
}

TEST(GridReservations, ReleasePreservesTransferredClaims){
    GridReservations reservations;
    reservations.reset(4);
    reservations.reserve(1, {0, 1, 1}, 10);
    reservations.reserve(2, {1, 2}, 11);
    reservations.release(1);
    reservations.release(1);
    EXPECT_EQ(reservations.holder(0, 0), 0u);
    EXPECT_EQ(reservations.holder(1, 11), 2u);
    EXPECT_EQ(reservations.holder(2, 11), 2u);
    reservations.release(2);
    EXPECT_EQ(reservations.holder(1, 0), 0u);
    EXPECT_EQ(reservations.holder(2, 0), 0u);
}

TEST(GridReservations, ResetDropsAgentsAndRejectsInvalidCells){
    GridReservations reservations;
    reservations.reset(4);
    reservations.reserve(0, {0}, 10);
    EXPECT_EQ(reservations.holder(0, 0), 0u);
    reservations.reserve(UINT32_MAX, {3}, UINT32_MAX);
    EXPECT_EQ(reservations.holder(3, UINT32_MAX), UINT32_MAX);
    reservations.reset(1);
    reservations.reserve(UINT32_MAX, {0, 3}, 1);
    EXPECT_EQ(reservations.holder(0, 1), UINT32_MAX);
    EXPECT_EQ(reservations.holder(-1, 0), 0u);
    EXPECT_EQ(reservations.holder(1, 0), 0u);
    EXPECT_FALSE(reservations.isReservedByOther(-1, 1, 0));
    reservations.reset(-1);
    reservations.release(UINT32_MAX);
    EXPECT_EQ(reservations.getCellCount(), 0);
}

TEST(GridReservations, ReusesStorageForSteadyStateUpdates){
    GridReservations reservations;
    reservations.reset(64);
    const std::vector<int> first{0, 1, 2}, second{3, 4};
    reservations.reserve(1, first, 1);
    reservations.reserve(2, second, 1);
    testsupport::ThreadAllocationCount allocations;
    for (uint32_t tick = 2; tick < 102; tick++){
        reservations.reserve(1, first, tick);
        reservations.reserve(2, second, tick);
    }
    const uint64_t count = allocations.count();
    if (testsupport::countsAllocations()){
        EXPECT_EQ(count, 0u);
    }
    EXPECT_EQ(reservations.holder(0, 101), 1u);
    EXPECT_EQ(reservations.holder(3, 101), 2u);
}
