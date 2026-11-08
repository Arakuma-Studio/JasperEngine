// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R55, R63: the Terrain window's brush decal and its eyedroppers, the parts that need no window open.

#include "TerrainProjectFixture.h"

#include "window/TerrainEditWindow.h"

using namespace terrainprojecttest;

namespace {

    class TerrainBrushWindowTest : public TerrainProjectFixture {};

}

TEST(TerrainBrushWindow, DecalFadesHalfASecondAfterThePointerRests) {
    EXPECT_EQ(TerrainEditWindow::decalOpacity(0.0), 1.0f);
    EXPECT_EQ(TerrainEditWindow::decalOpacity(0.5), 1.0f) << "whole for half a second";
    EXPECT_NEAR(TerrainEditWindow::decalOpacity(0.625), 0.5f, 1e-4f) << "then fading";
    EXPECT_EQ(TerrainEditWindow::decalOpacity(0.75), 0.0f) << "gone";
    EXPECT_EQ(TerrainEditWindow::decalOpacity(10.0), 0.0f);
}

TEST_F(TerrainBrushWindowTest, EyedroppersReadHeightSlopeAndTheStrongestLayer) {
    giveHeight16(33);
    terrain().surfaceLayers.resize(3);
    // layer 1 (green) at full weight
    giveFileMap(TerrainMapRef(TerrainMapTarget::BlendMap, 0), 33, ColorFormat::RGBA, 4,
                [](size_t i) { return (i % 4) == 1 || (i % 4) == 3 ? 255 : 0; });
    settleFiles();
    const TerrainPickValues values = TerrainEditWindow::pickValues(scene->scene, terrainEntity, 0.0f, 0.0f);
    EXPECT_NEAR(values.height, static_cast<float>(0x4040) / 65535.0f, 1e-3f) << "the fixture's flat height, 0-1";
    EXPECT_NEAR(values.slope, 0.0f, 0.5f) << "flat ground";
    EXPECT_EQ(values.layer, 1) << "the layer that weighs most here";

    giveFileMap(TerrainMapRef(TerrainMapTarget::BlendMap, 0), 33, ColorFormat::RGBA, 4, [](size_t i) { return (i % 4) == 3 ? 255 : 0; });
    settleFiles();
    EXPECT_EQ(TerrainEditWindow::pickValues(scene->scene, terrainEntity, 0.0f, 0.0f).layer, -1) << "the base, where no layer weighs";
}
