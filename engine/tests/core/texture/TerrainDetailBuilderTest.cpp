// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R6, R16 (AC8, AC20's CPU half): the array's slices: a PBR colour slice keeps its RGB and takes its
// height in alpha; cooked slices concatenate level by level, all of one format.

#include "ImageCompare.h"
#include "TempDir.h"
#include "component/TerrainComponent.h"
#include "texture/TerrainDetailBuilder.h"
#include "texture/MipChain.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    std::string writeImage(const test::TempDir& dir, const std::string& name, int side, uint8_t r, uint8_t g, uint8_t b, uint8_t a){
        std::vector<uint8_t> rgba(static_cast<size_t>(side) * side * 4);
        for (int y = 0; y < side; y++){
            for (int x = 0; x < side; x++){
                const size_t at = (static_cast<size_t>(y) * side + x) * 4;
                const uint8_t shade = (x + y) % 2 == 0 ? 255 : 128;
                rgba[at] = static_cast<uint8_t>(r * shade / 255);
                rgba[at + 1] = static_cast<uint8_t>(g * shade / 255);
                rgba[at + 2] = static_cast<uint8_t>(b * shade / 255);
                rgba[at + 3] = a;
            }
        }
        const std::string path = dir.file(name).generic_string();
        EXPECT_TRUE(test::writePng(path, rgba, side, side));
        return path;
    }

}

TEST(TerrainDetailBuilder, PbrColourRgbUnchanged){
    test::TempDir dir;
    const std::string color = writeImage(dir, "color.png", 16, 200, 100, 50, 255);
    const std::string height = writeImage(dir, "height.png", 16, 40, 40, 40, 255);

    TerrainComponent plain;
    plain.surfaceLayers.resize(1);
    plain.surfaceLayers[0].pbr = true;
    plain.surfaceLayers[0].colorTexture = Texture(color);

    TerrainComponent packed = plain;
    packed.surfaceLayers[0].heightTexture = Texture(height);

    DetailPixels plainPixels;
    DetailPixels packedPixels;
    ASSERT_TRUE(buildDetailPixels(plain, planDetailArray(plain), DetailBuildOptions(), plainPixels));
    ASSERT_TRUE(buildDetailPixels(packed, planDetailArray(packed), DetailBuildOptions(), packedPixels));
    ASSERT_EQ(plainPixels.levels.size(), packedPixels.levels.size());
    ASSERT_GT(plainPixels.levels.size(), 1u);

    // the first slice of each level: the colour bytes of every level as they were
    for (size_t level = 0; level < plainPixels.levels.size(); level++){
        const size_t texels = static_cast<size_t>(plainPixels.levelWidths[level]) * plainPixels.levelHeights[level];
        for (size_t t = 0; t < texels; t++){
            for (int c = 0; c < 3; c++){
                ASSERT_EQ(plainPixels.levels[level][t * 4 + c], packedPixels.levels[level][t * 4 + c]) << "level " << level << " texel " << t;
            }
        }
    }
    // the height is in its alpha, at every level: the mean of 40 and 20, and no white left
    EXPECT_LT(packedPixels.levels[0][3], 60);
    EXPECT_LT(packedPixels.levels.back()[3], 60);
    EXPECT_EQ(plainPixels.levels[0][3], 255) << "no height map: full height";
}

TEST(TerrainDetailBuilder, PlanCountsEverySlice){
    TerrainComponent terrain;
    terrain.surfaceLayers.resize(4);
    terrain.surfaceLayers[0].colorTexture.setPath("a.png");
    terrain.surfaceLayers[1].colorTexture.setPath("b.png");
    terrain.surfaceLayers[1].pbr = true;
    terrain.surfaceLayers[1].normalTexture.setPath("n.png");
    terrain.surfaceLayers[1].roughnessTexture.setPath("r.png");
    const DetailPlan plan = planDetailArray(terrain);
    EXPECT_EQ(plan.colorSlices, 6) << "colours come in whole blend maps";
    EXPECT_EQ(plan.sliceKeys.size(), 8u);
    EXPECT_EQ(plan.normalSlice[1], 6);
    EXPECT_EQ(plan.surfaceSlice[1], 7);
    EXPECT_EQ(detailSliceKind(plan, 0), DetailSliceKind::COLOR);
    EXPECT_EQ(detailSliceKind(plan, 6), DetailSliceKind::NORMAL);
    EXPECT_EQ(detailSliceKind(plan, 7), DetailSliceKind::SURFACE);
}

TEST(TerrainDetailBuilder, CookedSlicesConcatenate){
    std::vector<CookedSlice> slices(2);
    for (int s = 0; s < 2; s++){
        slices[s].format = GpuTextureFormat::BC7;
        slices[s].width = 8;
        slices[s].height = 8;
        slices[s].levels = {std::vector<uint8_t>(16, static_cast<uint8_t>(s)), std::vector<uint8_t>(16, static_cast<uint8_t>(10 + s))};
    }
    DetailPixels out;
    std::string error;
    ASSERT_TRUE(concatenateCookedSlices(slices, 0, out, &error)) << error;
    EXPECT_EQ(out.slices, 2);
    EXPECT_EQ(out.format, GpuTextureFormat::BC7);
    ASSERT_EQ(out.levels.size(), 2u);
    EXPECT_EQ(out.levels[0].size(), 32u);
    EXPECT_EQ(out.levels[0][16], 1);
    EXPECT_EQ(out.levels[1][0], 10);

    // the runtime shrink skips the top levels
    DetailPixels small;
    ASSERT_TRUE(concatenateCookedSlices(slices, 1, small, &error));
    EXPECT_EQ(small.width, 4);
    ASSERT_EQ(small.levels.size(), 1u);
    EXPECT_EQ(small.levels[0][0], 10);

    // a mix of formats is refused, with an error
    slices[1].format = GpuTextureFormat::RGBA8;
    EXPECT_FALSE(concatenateCookedSlices(slices, 0, out, &error));
    EXPECT_NE(error.find("format"), std::string::npos);
}

TEST(TerrainDetailBuilder, CookedKeyIsAPathOfItsOwn){
    const std::string key = cookedSliceIndexKey("surface|a.png|b.png||");
    EXPECT_EQ(key.rfind("__terrain_slices/", 0), 0u);
    EXPECT_NE(key, cookedSliceIndexKey("surface|a.png|b.png|c.png|"));
    EXPECT_EQ(key, cookedSliceIndexKey("surface|a.png|b.png||")) << "shared maps dedupe";
}
