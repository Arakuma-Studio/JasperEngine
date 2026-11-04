// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3's material features, drawn on the headless device (docs/specs/T3-terrain-materials-holes.md).
//
// LegacyHighMatchesPreS55 (AC1) holds what a terrain that uses none of T3 drew before T3: its goldens
// were written at the commit before any render change, so nothing may move them.

#include "TerrainGpuFixture.h"

#include "SceneSettings.h"

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    float rollingHills(float x, float z){
        return 0.32f + 0.1f * std::sin(x * 0.09f) * std::cos(z * 0.07f);
    }

    // A flat shelf that climbs a steep bank at the back, so that the side planes draw
    float cliffBack(float x, float z){
        const float bank = std::fmin(std::fmax((-z - 8.0f) / 10.0f, 0.0f), 1.0f);
        return 0.1f + 0.75f * bank + 0.02f * std::sin(x * 0.2f);
    }

    TerrainGpuColor bands(float u, float v, int count){
        const float at = (u + v * 0.35f) * static_cast<float>(count);
        unsigned char channel[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++){
            const float distance = std::fabs(at - (static_cast<float>(i) + 0.5f));
            channel[i] = static_cast<unsigned char>(std::fmax(0.0f, 1.0f - distance) * 255.0f);
        }
        return TerrainGpuColor{channel[0], channel[1], channel[2], 255};
    }

    // AC1: glcore holds 0 pixels off by more than 1/255 and at most 0.01 % by 1/255; Vulkan holds the
    // difference measured between its own frame and the goldens on the pre-T3 commit (printed by each
    // run). A pixel over the threshold of 1 is a pixel off by 2 or more.
    CompareOptions legacyOptions(){
        CompareOptions options;
        if (std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0){
            options.pixelThreshold = 1;
            options.maxBadFraction = 0.0;
            options.maxMeanDiff = 0.0001 + 0.01 / 100.0;
        }else{
            options.pixelThreshold = 16;
            options.maxBadFraction = 0.03;
            options.maxMeanDiff = 3.0;
        }
        return options;
    }

    class TerrainMaterialsGpu : public TerrainGpuFixture {
    protected:
        void expectLegacyGolden(const std::string& name){
            // a golden of an empty frame would hold nothing: most of the frame must be the terrain
            std::vector<uint8_t> frame;
            ASSERT_TRUE(readFrame(frame));
            size_t ground = 0;
            for (size_t i = 0; i + 3 < frame.size(); i += 4){
                ground += std::abs(frame[i] - frame[0]) + std::abs(frame[i + 1] - frame[1]) + std::abs(frame[i + 2] - frame[2]) > 24 ? 1 : 0;
            }
            EXPECT_GT(ground, frame.size() / 4 / 4) << name << " drew no terrain";

            const CompareResult result = compareTargetWithGolden(name, legacyOptions());
            std::printf("[gpu] %s on %s: largest difference %d, %zu pixels over the threshold, mean %.5f\n",
                        name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
            EXPECT_TRUE(result.passed) << result.message;
        }

        void addColorLayer(unsigned int index, const std::string& name, unsigned char r, unsigned char g, unsigned char b){
            terrain->setTextureLayer(index, writeColorMap(name, r, g, b));
        }

        void addPbrLayer(unsigned int index, const std::string& name, unsigned char r, unsigned char g, unsigned char b){
            TerrainSurfaceLayer layer;
            layer.pbr = true;
            layer.colorTexture = Texture(writeColorMap(name, r, g, b));
            layer.normalTexture = Texture(writeNormalMap(name + "_normal"));
            const Texture surface(writeSurfaceMap(name + "_surface"));
            layer.occlusionTexture = surface;
            layer.roughnessTexture = surface;
            layer.metallicTexture = surface;
            layer.heightTexture = surface;
            layer.uvScale = Vector2(1.0f + static_cast<float>(index) * 0.25f, 1.0f);
            terrain->setSurfaceLayer(index, layer);
        }
    };

}

TEST_F(TerrainMaterialsGpu, LegacyHighThreeColorLayers){
    JASPER_REQUIRE_GPU();
    makeTerrain(14.0f, rollingHills);
    addColorLayer(0, "grass", 90, 150, 60);
    addColorLayer(1, "rock", 140, 130, 120);
    addColorLayer(2, "sand", 210, 190, 130);
    setBlendMap(0, [](float u, float v){ return bands(u, v, 3); });
    ASSERT_TRUE(render());
    expectLegacyGolden("terrain_legacy_color");
}

TEST_F(TerrainMaterialsGpu, LegacyHighPbrWithCliff){
    JASPER_REQUIRE_GPU();
    makeTerrain(24.0f, cliffBack);
    addPbrLayer(0, "grass", 90, 150, 60);
    addPbrLayer(1, "rock", 140, 130, 120);
    addPbrLayer(2, "sand", 210, 190, 130);
    addPbrLayer(3, "snow", 235, 235, 240);
    setBlendMap(0, [](float u, float v){ return bands(u, v, 3); });
    setBlendMap(1, [](float u, float){
        return TerrainGpuColor{static_cast<unsigned char>(std::fmax(0.0f, 2.0f * u - 1.0f) * 255.0f), 0, 0, 255};
    });
    ASSERT_TRUE(render());
    expectLegacyGolden("terrain_legacy_pbr_cliff");
}

TEST_F(TerrainMaterialsGpu, LegacyHighHexAndStochastic){
    JASPER_REQUIRE_GPU();
    makeTerrain(14.0f, rollingHills);
    addPbrLayer(0, "grass", 90, 150, 60);
    addPbrLayer(1, "rock", 140, 130, 120);
    addPbrLayer(2, "sand", 210, 190, 130);
    TerrainSurfaceLayer hexed = terrain->getSurfaceLayer(0);
    hexed.hexTiling = true;
    terrain->setSurfaceLayer(0, hexed);
    TerrainSurfaceLayer stochastic = terrain->getSurfaceLayer(1);
    stochastic.stochasticTiling = true;
    terrain->setSurfaceLayer(1, stochastic);
    setBlendMap(0, [](float u, float v){ return bands(u, v, 3); });
    ASSERT_TRUE(render());
    expectLegacyGolden("terrain_legacy_hex_stochastic");
}

TEST_F(TerrainMaterialsGpu, LegacyHighPostColorWithShadows){
    JASPER_REQUIRE_GPU();
    makeTerrain(24.0f, cliffBack);
    addPbrLayer(0, "grass", 90, 150, 60);
    addPbrLayer(1, "rock", 140, 130, 120);
    setBlendMap(0, [](float u, float v){ return bands(u, v, 2); });
    sun.setShadows(true);
    PostProcessingSettings post = scene.getPostProcessing();
    post.exposure = 0.25f;
    post.tonemap = TonemapMode::ACES;
    scene.setPostProcessing(post);
    ASSERT_TRUE(render());
    expectLegacyGolden("terrain_legacy_post_shadow");
}
