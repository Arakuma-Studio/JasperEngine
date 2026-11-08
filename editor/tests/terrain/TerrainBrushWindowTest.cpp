// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R55: the Terrain window's brush decal and the rules behind its tools that need no window to check.

#include "window/TerrainEditWindow.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(TerrainBrushWindow, DecalFadesHalfASecondAfterThePointerRests) {
    EXPECT_EQ(TerrainEditWindow::decalOpacity(0.0), 1.0f);
    EXPECT_EQ(TerrainEditWindow::decalOpacity(0.5), 1.0f) << "whole for half a second";
    EXPECT_NEAR(TerrainEditWindow::decalOpacity(0.625), 0.5f, 1e-4f) << "then fading";
    EXPECT_EQ(TerrainEditWindow::decalOpacity(0.75), 0.0f) << "gone";
    EXPECT_EQ(TerrainEditWindow::decalOpacity(10.0), 0.0f);
}
