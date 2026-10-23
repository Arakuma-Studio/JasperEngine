// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HeightIndex (S17 R3, AC1): the Fenwick tree of a virtual view's row heights against a naive
// prefix array.

#include "htmlui/widgets/HeightIndex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

using namespace jasperengine::htmlui;

namespace {

    // the reference: every height, offsets summed on demand
    struct NaiveIndex {
        std::vector<double> heights;
        std::vector<bool> measured;
        double estimate = 0.0;

        double height(int i) const { return measured[i] ? heights[i] : estimate; }
        double offset(int i) const{
            double sum = 0.0;
            for (int k = 0; k < i; k++)
                sum += height(k);
            return sum;
        }
        int indexAt(double y) const{
            if (heights.empty())
                return -1;
            double sum = 0.0;
            for (int k = 0; k < (int)heights.size(); k++){
                sum += height(k);
                if (y < sum)
                    return k;
            }
            return (int)heights.size() - 1;
        }
    };

}

TEST(HeightIndex, UniformRowsAreArithmetic){
    HeightIndex index;
    index.reset(10000, 48.0f);
    EXPECT_EQ(index.count(), 10000);
    EXPECT_FLOAT_EQ(index.total(), 480000.0f);
    EXPECT_FLOAT_EQ(index.offsetOf(0), 0.0f);
    EXPECT_FLOAT_EQ(index.offsetOf(97), 97.0f * 48.0f);
    EXPECT_EQ(index.indexAt(0.0f), 0);
    EXPECT_EQ(index.indexAt(47.9f), 0);
    EXPECT_EQ(index.indexAt(48.0f), 1);
    EXPECT_EQ(index.indexAt(-10.0f), 0);
    EXPECT_EQ(index.indexAt(1.0e9f), 9999);
    EXPECT_EQ(index.measuredCount(), 0);
}

TEST(HeightIndex, EmptyIndex){
    HeightIndex index;
    index.reset(0, 20.0f);
    EXPECT_EQ(index.indexAt(5.0f), -1);
    EXPECT_FLOAT_EQ(index.total(), 0.0f);
    EXPECT_FLOAT_EQ(index.offsetOf(3), 0.0f);
}

TEST(HeightIndex, MeasuredRowsMoveTheRowsAfterThem){
    HeightIndex index;
    index.reset(5, 10.0f);
    index.set(1, 30.0f);
    index.set(3, 0.0f);
    EXPECT_FLOAT_EQ(index.offsetOf(1), 10.0f);
    EXPECT_FLOAT_EQ(index.offsetOf(2), 40.0f);
    EXPECT_FLOAT_EQ(index.offsetOf(4), 50.0f);
    EXPECT_FLOAT_EQ(index.total(), 60.0f);
    EXPECT_EQ(index.indexAt(39.9f), 1);
    EXPECT_EQ(index.indexAt(40.0f), 2);
    // a zero-height row holds no offset: y at its top is the next row's
    EXPECT_EQ(index.indexAt(50.0f), 4);
    index.clear(1);
    EXPECT_FALSE(index.isMeasured(1));
    EXPECT_FLOAT_EQ(index.total(), 40.0f);
}

TEST(HeightIndex, EstimateChangeMovesUnmeasuredRowsOnly){
    HeightIndex index;
    index.reset(4, 10.0f);
    index.set(0, 100.0f);
    index.setEstimate(20.0f);
    EXPECT_FLOAT_EQ(index.heightOf(0), 100.0f);
    EXPECT_FLOAT_EQ(index.heightOf(1), 20.0f);
    EXPECT_FLOAT_EQ(index.total(), 160.0f);
    EXPECT_FLOAT_EQ(index.offsetOf(2), 120.0f);
}

TEST(HeightIndex, AppendAndTruncateKeepPrefixSums){
    HeightIndex index;
    index.reset(3, 10.0f);
    index.set(2, 50.0f);
    index.append(13);   // across several power-of-two nodes
    EXPECT_EQ(index.count(), 16);
    EXPECT_FLOAT_EQ(index.offsetOf(16), 10.0f * 15 + 50.0f);
    index.set(15, 1.0f);
    EXPECT_FLOAT_EQ(index.total(), 10.0f * 14 + 50.0f + 1.0f);
    index.truncate(2);
    EXPECT_EQ(index.count(), 2);
    EXPECT_EQ(index.measuredCount(), 0);
    EXPECT_FLOAT_EQ(index.total(), 20.0f);
    index.resize(4);
    EXPECT_FLOAT_EQ(index.offsetOf(4), 40.0f);
}

// AC1: 100k random set/append/truncate/clear and queries against the naive prefix array
TEST(HeightIndex, RandomOperationsMatchNaivePrefixArray){
    std::mt19937 random(1234);
    HeightIndex index;
    NaiveIndex naive;
    index.reset(300, 24.0f);
    naive.heights.assign(300, 0.0);
    naive.measured.assign(300, false);
    naive.estimate = 24.0;

    for (int op = 0; op < 100000; op++){
        const int count = index.count();
        const int kind = (int)(random() % 100);
        if (kind < 55 && count > 0){
            const int i = (int)(random() % count);
            const float h = (float)(random() % 1600) / 10.0f;
            index.set(i, h);
            naive.heights[i] = h;
            naive.measured[i] = true;
        }else if (kind < 62 && count > 0){
            const int i = (int)(random() % count);
            index.clear(i);
            naive.measured[i] = false;
        }else if (kind < 66){
            const int n = 1 + (int)(random() % 40);
            index.append(n);
            naive.heights.resize(naive.heights.size() + n, 0.0);
            naive.measured.resize(naive.measured.size() + n, false);
        }else if (kind < 68 && count > 1){
            const int keep = std::max(1, count - 1 - (int)(random() % 30));
            index.truncate(keep);
            naive.heights.resize(keep);
            naive.measured.resize(keep);
        }else if (kind < 69){
            const float estimate = 8.0f + (float)(random() % 120);
            index.setEstimate(estimate);
            naive.estimate = estimate;
        }else if (count > 0){
            // queries
            const int i = (int)(random() % (count + 1));
            ASSERT_NEAR(index.offsetOf(i), naive.offset(i), 0.05) << "op " << op;
            const double total = naive.offset((int)naive.heights.size());
            const float y = (float)(std::uniform_real_distribution<double>(-10.0, total + 10.0)(random));
            const int at = index.indexAt(y);
            const int expected = naive.indexAt(y);
            if (at != expected){
                // a float y on a row boundary may land on either side: the rows must touch it
                ASSERT_NEAR(index.offsetOf(std::max(at, expected)), y, 0.05) << "op " << op << " y " << y;
            }
        }
    }
    ASSERT_NEAR(index.total(), naive.offset((int)naive.heights.size()), 0.05);
}

// R34: heights + tree within 24 bytes per item (keys and selection are not per item)
TEST(HeightIndex, MemoryPerItem){
    HeightIndex index;
    index.reset(10000, 32.0f);
    for (int i = 0; i < 10000; i += 3)
        index.set(i, 40.0f);
    EXPECT_LE(index.memoryBytes(), (size_t)10001 * 12);
}
