// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "registry/PhysicsLayers.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

TEST(LayerCollisionMatrix, EverythingCollidesAtFirst){
    LayerCollisionMatrix matrix;
    EXPECT_TRUE(matrix.isAllEnabled());
    EXPECT_TRUE(matrix.collides(0, 31));
    EXPECT_EQ(matrix.getMask(3), LAYER_MASK_EVERYTHING);
}

TEST(LayerCollisionMatrix, IgnoreIsSymmetric){
    LayerCollisionMatrix matrix;
    const uint32_t version = matrix.getVersion();
    EXPECT_TRUE(matrix.setIgnore(1, 8, true));
    EXPECT_TRUE(matrix.isIgnored(1, 8));
    EXPECT_TRUE(matrix.isIgnored(8, 1));
    EXPECT_FALSE(matrix.isAllEnabled());
    EXPECT_GT(matrix.getVersion(), version);
    matrix.setIgnore(8, 1, false);
    EXPECT_FALSE(matrix.isIgnored(1, 8));
    EXPECT_TRUE(matrix.isAllEnabled());
}

TEST(LayerCollisionMatrix, InvalidLayersAreRefused){
    LayerCollisionMatrix matrix;
    EXPECT_FALSE(matrix.setIgnore(-1, 3, true));
    EXPECT_FALSE(matrix.setMask(32, 0));
    EXPECT_FALSE(matrix.isIgnored(40, 0));
    EXPECT_EQ(matrix.getMask(-5), LAYER_MASK_EVERYTHING);
}

TEST(LayerCollisionMatrix, SetMaskUpdatesTheOtherRows){
    LayerCollisionMatrix matrix;
    matrix.setMask(2, (1u << 2) | (1u << 6));
    EXPECT_EQ(matrix.getMask(2), (1u << 2) | (1u << 6));
    EXPECT_TRUE(matrix.collides(6, 2));
    EXPECT_FALSE(matrix.collides(7, 2));
    EXPECT_FALSE(matrix.collides(2, 7));
}

TEST(LayerCollisionMatrix, SetRowsKeepsOnlyPairsBothRowsAllow){
    LayerCollisionMatrix matrix;
    uint32_t rows[MAX_LAYERS];
    for (int i = 0; i < MAX_LAYERS; i++)
        rows[i] = LAYER_MASK_EVERYTHING;
    rows[0] = ~(1u << 9);
    matrix.setRows(rows);
    EXPECT_TRUE(matrix.isIgnored(0, 9));
    EXPECT_TRUE(matrix.isIgnored(9, 0));
    uint32_t back[MAX_LAYERS];
    matrix.getRows(back);
    EXPECT_EQ(back[9], ~1u);
}

TEST(LayerCollisionMatrix, ResetEnablesEverything){
    LayerCollisionMatrix matrix;
    matrix.setIgnore(3, 4, true);
    matrix.reset();
    EXPECT_TRUE(matrix.isAllEnabled());
    EXPECT_FALSE(matrix.isIgnored(3, 4));
}

TEST(Physics, IgnoreLayerCollisionChecksTheLayers){
    Physics::resetLayerCollisionMatrix();
    Physics::ignoreLayerCollision(10, 11);
    EXPECT_TRUE(Physics::getIgnoreLayerCollision(11, 10));
    EXPECT_EQ(Physics::getLayerCollisionMask(10) & (1u << 11), 0u);
    LogCapture log;
    Physics::ignoreLayerCollision(10, 99);
    EXPECT_EQ(log.count(D_LOG_ERROR, "between 0 and 31"), 1u);
    Physics::resetLayerCollisionMatrix();
    EXPECT_FALSE(Physics::getIgnoreLayerCollision(10, 11));
}
