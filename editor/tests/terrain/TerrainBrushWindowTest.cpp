// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R55, R59, R61, R63, R67: the Terrain window's brush decal and its eyedroppers, the parts that need no window open.

#include "TerrainProjectFixture.h"

#include "command/CommandHistory.h"
#include "terrain/TerrainOverlay.h"
#include "window/HeightImportFields.h"
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

// R59: Resize... resamples a map in place on the terrain, as one undo step
TEST_F(TerrainBrushWindowTest, ResizeKeepsTheSurfaceAsOneStep) {
    constexpr int N = 33;
    giveFileMap(TerrainMapRef(TerrainMapTarget::HeightMap), N, ColorFormat::RED16, 1, [](size_t i) {
        const unsigned value = static_cast<unsigned>((i / 2) % N) * 2000u;
        return static_cast<unsigned char>(i % 2 == 0 ? value & 0xFF : value >> 8);
    });
    settleFiles();
    const float half = terrain().terrainSize * 0.5f;
    const float left = querySurface(terrain(), -half, 0.0f).height;
    const float middle = querySurface(terrain(), 0.0f, 0.0f).height;
    const float right = querySurface(terrain(), half, 0.0f).height;
    CommandHistory* history = CommandHandle::get(scene->id);
    const size_t before = history->position();
    std::string error;
    ASSERT_TRUE(TerrainEditWindow::resizeTerrainMap(project.get(), scene, terrainEntity, TerrainMapRef(TerrainMapTarget::HeightMap), 65, 65, error)) << error;
    EXPECT_EQ(history->position(), before + 1) << "one undo step";
    EXPECT_EQ(TerrainComponentStore(scene->scene, terrainEntity, terrain()).format(HEIGHT_MAP).bounds.w, 65);
    EXPECT_NEAR(querySurface(terrain(), -half, 0.0f).height, left, 0.01f) << "the edges stay where they were";
    EXPECT_NEAR(querySurface(terrain(), 0.0f, 0.0f).height, middle, 0.01f);
    EXPECT_NEAR(querySurface(terrain(), half, 0.0f).height, right, 0.01f);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(TerrainComponentStore(scene->scene, terrainEntity, terrain()).format(HEIGHT_MAP).bounds.w, N) << "undo brings the old size back";
}

// R61: the import preview's hillshade is even on flat ground and lights the slopes that face the top left
TEST(TerrainBrushWindow, HillshadeLightsSlopesFacingTheLight) {
    constexpr int N = 32;
    const std::vector<uint8_t> flat = heightHillshade(std::vector<float>(N * N, 0.5f), N, N, 16);
    ASSERT_EQ(flat.size(), 16u * 16u * 4u);
    EXPECT_EQ(flat[0], flat[(8 * 16 + 8) * 4]) << "flat ground is one shade";
    std::vector<float> east(N * N), west(N * N);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            east[static_cast<size_t>(y) * N + x] = static_cast<float>(x) / N;
            west[static_cast<size_t>(y) * N + x] = 1.0f - static_cast<float>(x) / N;
        }
    }
    const size_t middle = (8 * 16 + 8) * 4;
    EXPECT_GT(heightHillshade(east, N, N, 16)[middle], flat[middle]) << "ground rising away from the light faces it";
    EXPECT_LT(heightHillshade(west, N, N, 16)[middle], flat[middle]) << "ground rising towards the light is in its shade";
}

// R67: Preview bakes the Layer Weights overlay from the rules run on a copy; the terrain's maps stay
TEST_F(TerrainBrushWindowTest, PaintByRulesPreviewLeavesTheMaps) {
    giveHeight16(33);
    terrain().surfaceLayers.resize(3);
    settleFiles();
    TerrainOverlaySettings settings;
    settings.source = "layer_weights";
    auto bake = [&]() {
        std::vector<uint8_t> rgba;
        std::vector<TerrainOverlayLegendStop> legend;
        std::string error;
        EXPECT_TRUE(TerrainOverlay::bake(scene->scene, terrainEntity, settings, 32, 32, rgba, legend, error)) << error;
        return rgba;
    };
    const std::vector<uint8_t> before = bake();
    terrain::Op op;
    op.type = terrain::OpType::PaintByRules;
    terrain::PaintRule everywhere;
    everywhere.layer = 1;
    op.rules.push_back(everywhere);
    std::string error;
    ASSERT_TRUE(TerrainEditWindow::previewPaintByRules(project.get(), scene, terrainEntity, op, 32, error)) << error;
    EXPECT_NE(bake(), before) << "the overlay shows layer 1 painted";
    EXPECT_TRUE(terrain().blendMaps.empty() || terrain().blendMaps[0].empty()) << "the terrain got no blend map";
    TerrainOverlay::clearPreview(scene->scene, terrainEntity);
    EXPECT_EQ(bake(), before) << "cleared, the overlay shows the terrain again";
}
