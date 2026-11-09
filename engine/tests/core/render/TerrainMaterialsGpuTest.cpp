// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3's material features, drawn on the headless device (docs/specs/T3-terrain-materials-holes.md).
//
// LegacyHighMatchesPreS55 (AC1) holds what a terrain that uses none of T3 drew before T3: its goldens
// were written at the commit before any render change, so nothing may move them.

#include "TerrainGpuFixture.h"
#include "TestPaths.h"

#include "SceneSettings.h"
#include "LogCapture.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "pool/ShaderPool.h"
#include "terrain/TerrainLayers.h"
#include "terrain/TerrainMacroNormal.h"
#include "terrain/TerrainMaterialCost.h"
#include "terrain/TerrainQuality.h"
#include "texture/MaterialPbr.h"
#include "component/TerrainWorldComponent.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TerrainWorldData.h"

#include <filesystem>
#include <fstream>

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
        void TearDown() override{
            terraincore::TerrainQualitySettings::reset();
            TerrainGpuFixture::TearDown();
        }

        // nine layers, PBR or colour only, all overlapping: every texel weighs each of them by its own bump
        void terrainNine(float maxHeight, const std::function<float(float, float)>& shape, bool pbr = true){
            makeTerrain(maxHeight, shape);
            for (unsigned int i = 0; i < 9; i++){
                const unsigned char r = static_cast<unsigned char>(40 + i * 22), g = static_cast<unsigned char>(220 - i * 18), b = static_cast<unsigned char>(90 + i * 7);
                if (pbr){
                    addPbrLayer(i, "nine" + std::to_string(i), r, g, b);
                }else{
                    addColorLayer(i, "ninecolor" + std::to_string(i), r, g, b);
                }
            }
            for (unsigned int m = 0; m < 3; m++){
                setBlendMap(m, [m](float u, float v){
                    const auto weight = [&](int c){
                        const float phase = u * 7.0f + v * 5.0f + static_cast<float>(m * 3 + c) * 0.7f;
                        return static_cast<unsigned char>((0.5f + 0.5f * std::sin(phase)) * 56.0f + 4.0f);
                    };
                    return TerrainGpuColor{weight(0), weight(1), weight(2), 255};
                });
            }
        }

        // the view's count of the frame, per pixel on the terrain (R: the count at 8/255 a step)
        std::vector<int> readCounts(terraincore::TerrainDebugView view){
            // the background black: a count of 0, where the sky's red would read as a count
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            terraincore::TerrainQualitySettings::setDebugView(view);
            EXPECT_TRUE(render());
            std::vector<uint8_t> frame;
            EXPECT_TRUE(readFrame(frame));
            terraincore::TerrainQualitySettings::setDebugView(terraincore::TerrainDebugView::SHADED);
            scene.setBackgroundColor(0.55f, 0.68f, 0.82f, 1.0f);
            std::vector<int> counts(frame.size() / 4);
            for (size_t i = 0; i < counts.size(); i++){
                // the sky's blue channel is its own: only pixels the view wrote count (a ramp's alpha)
                counts[i] = static_cast<int>(std::lround(frame[i * 4] * 255.0 / 255.0 / 8.0));
            }
            return counts;
        }

        int maxFetches(){
            const std::vector<int> counts = readCounts(terraincore::TerrainDebugView::FETCH_COST);
            return *std::max_element(counts.begin(), counts.end());
        }

        void setTier(terraincore::TerrainQuality tier){
            terraincore::TerrainQualitySettings::setGlobalOverride(tier);
        }

        void holesAcross(){
            static std::vector<unsigned char> texels;
            texels.assign(static_cast<size_t>(16) * 16, 255);
            for (int y = 4; y < 12; y++){
                for (int x = 4; x < 12; x++){
                    texels[static_cast<size_t>(y) * 16 + x] = 0;
                }
            }
            component().holeMap.setData(poolId("materials-holes"), TextureData(16, 16, static_cast<unsigned int>(texels.size()), ColorFormat::RED, 1, texels.data()));
            component().needUpdateTexture = true;
        }

        void expectFrameGolden(const std::string& name){
            CompareOptions options;
            if (std::strcmp(HeadlessContext::getBackendName(), "glcore") != 0){
                options.pixelThreshold = 16;
                options.maxBadFraction = 0.03;
                options.maxMeanDiff = 3.0;
            }
            const CompareResult result = compareTargetWithGolden(name, options);
            std::printf("[gpu] %s on %s: largest difference %d, %zu over the threshold\n", name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels);
            EXPECT_TRUE(result.passed) << result.message;
        }

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

// ---------------------------------------------------------------- the tiers (T3 R1-R8)

// AC6: the budgets, structural and exact, over every pixel of a terrain with cliffs
TEST_F(TerrainMaterialsGpu, FetchBudgets){
    JASPER_REQUIRE_GPU();
    terrainNine(24.0f, cliffBack);
    holesAcross();
    component().macroNormal = true;
    setTier(terraincore::TerrainQuality::HIGH);
    const int high = maxFetches();
    setTier(terraincore::TerrainQuality::MEDIUM);
    const int medium = maxFetches();
    EXPECT_LE(medium, 23) << "Medium with holes and Macro Normal";
    setTier(terraincore::TerrainQuality::LOW);
    const int low = maxFetches();
    EXPECT_LE(low, 7) << "Low with holes";
    EXPECT_GT(high, medium);
    EXPECT_GT(high, low);

    component().holeMap = Texture();
    component().needUpdateTexture = true;
    EXPECT_LE(maxFetches(), 6) << "Low without holes";

    // up to 3 layers with holes: trimmed to one blend map
    component().surfaceLayers.resize(3);
    holesAcross();
    EXPECT_LE(maxFetches(), 5) << "Low, 3 layers, holes";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC7: the formula and the shader's own count, at each tier
TEST_F(TerrainMaterialsGpu, CostMatchesShader){
    JASPER_REQUIRE_GPU();
    terrainNine(24.0f, cliffBack);
    terraincore::TerrainMaterialInputs inputs;
    inputs.layerCount = 9;
    for (int i = 0; i < 9; i++){
        inputs.layers[i].pbr = true;
        inputs.layers[i].normalMap = true;
        inputs.layers[i].surfaceMap = true;
    }
    for (terraincore::TerrainQuality tier : {terraincore::TerrainQuality::HIGH, terraincore::TerrainQuality::MEDIUM, terraincore::TerrainQuality::LOW}){
        setTier(tier);
        const int measured = maxFetches();
        const terraincore::MaterialCost cost = terraincore::materialCost(inputs, tier);
        EXPECT_LE(measured, cost.worst) << terraincore::terrainQualityName(tier) << ": a pixel read more than the formula's worst";
        if (tier != terraincore::TerrainQuality::HIGH){
            EXPECT_EQ(measured, cost.worst) << terraincore::terrainQualityName(tier) << ": the cliff and the blend reach the tier's worst";
        }
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// R53: a new terrain limits its layers, so High with the cap is the variant most terrains draw with, at
// the editor's High Lit Quality: it draws with no layer, and with nine keeps at most four
TEST_F(TerrainMaterialsGpu, LimitedHighDrawsWithoutLayers){
    JASPER_REQUIRE_GPU();
    scene.setLitQuality(LitQuality::HIGH);
    makeTerrain(4.0f, [](float x, float z){ return 0.5f + 0.2f * std::sin(x * 0.1f) * std::cos(z * 0.1f); });
    component().limitLayersPerPixel = true;
    setTier(terraincore::TerrainQuality::HIGH);
    ASSERT_TRUE(render());
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));
    EXPECT_NO_ENGINE_ERRORS();
}

// R53: four painted layers draw the same with the cap as without it: the variant that skips it (no more
// layers than the cap keeps) and the one that runs it (a fifth layer, unpainted, brings it back)
TEST_F(TerrainMaterialsGpu, LimitThatCutsNothingDrawsTheSame){
    JASPER_REQUIRE_GPU();
    makeTerrain(4.0f, cliffBack);
    for (unsigned int i = 0; i < 4; i++){
        addPbrLayer(i, "four" + std::to_string(i), static_cast<unsigned char>(50 + i * 40), static_cast<unsigned char>(200 - i * 30), 90);
    }
    setBlendMap(0, [](float u, float v){
        return TerrainGpuColor{static_cast<unsigned char>(u * 160.0f), static_cast<unsigned char>(v * 120.0f), static_cast<unsigned char>((1.0f - u) * 80.0f), 255};
    });
    setBlendMap(1, [](float u, float v){
        return TerrainGpuColor{static_cast<unsigned char>((1.0f - v) * 70.0f), 0, 0, 255};
    });
    component().limitLayersPerPixel = true;
    setTier(terraincore::TerrainQuality::HIGH);
    const uint64_t limitBit = MaterialPbr::getDefineBit("TERRAIN_LAYER_LIMIT");
    ASSERT_TRUE(render());
    std::vector<uint8_t> skipped;
    ASSERT_TRUE(readFrame(skipped));
    EXPECT_EQ(scene.getComponent<MeshComponent>(terrain->getEntity()).submeshes[0].litDefineBits & limitBit, 0u) << "four layers: no cap";

    addPbrLayer(4, "four4", 220, 220, 220);
    ASSERT_TRUE(render());
    std::vector<uint8_t> capped;
    ASSERT_TRUE(readFrame(capped));
    EXPECT_NE(scene.getComponent<MeshComponent>(terrain->getEntity()).submeshes[0].litDefineBits & limitBit, 0u) << "five layers: the cap";

    ASSERT_EQ(skipped.size(), capped.size());
    int largest = 0;
    for (size_t i = 0; i < skipped.size(); i++){
        largest = std::max(largest, std::abs(static_cast<int>(skipped[i]) - static_cast<int>(capped[i])));
    }
    EXPECT_LE(largest, 2) << "the cap keeps every painted layer";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(TerrainMaterialsGpu, LimitedHighDrawsNineLayers){
    JASPER_REQUIRE_GPU();
    scene.setLitQuality(LitQuality::HIGH);
    terrainNine(4.0f, cliffBack);
    component().limitLayersPerPixel = true;
    setTier(terraincore::TerrainQuality::HIGH);
    ASSERT_TRUE(render());
    const std::vector<int> counts = readCounts(terraincore::TerrainDebugView::LAYER_COUNT);
    EXPECT_LE(*std::max_element(counts.begin(), counts.end()), 4);
    EXPECT_GT(*std::max_element(counts.begin(), counts.end()), 0) << "the layers drew";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC5: how many layers the shader drew, against terraincore::selectLayers at 64 points
TEST_F(TerrainMaterialsGpu, ShaderMatchesCpuSelect){
    JASPER_REQUIRE_GPU();
    terrainNine(0.001f, [](float, float){ return 0.0f; });
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(-64.0f, 64.0f, -36.0f, 36.0f, 1.0f, 400.0f);
    camera.setPosition(0.0f, 100.0f, 0.0f);
    camera.setUp(0.0f, 0.0f, -1.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);

    TerrainComponent& terrainComponent = component();
    // nearest, so a pixel's weights are one texel's, as the CPU reads them here
    for (Texture& map : terrainComponent.blendMaps){
        map.setMinFilter(TextureFilter::NEAREST);
        map.setMagFilter(TextureFilter::NEAREST);
    }
    for (terraincore::TerrainQuality tier : {terraincore::TerrainQuality::MEDIUM, terraincore::TerrainQuality::LOW}){
        setTier(tier);
        const std::vector<int> counts = readCounts(terraincore::TerrainDebugView::LAYER_COUNT);
        const int keep = terraincore::terrainTierInfo(tier).layersPerPixel;
        for (int j = 0; j < 8; j++){
            for (int i = 0; i < 8; i++){
                const int px = 40 + i * 80;
                const int py = 22 + j * 45;
                const float u = (static_cast<float>(px) + 0.5f) / FRAME_WIDTH * 128.0f / TERRAIN_SIZE;
                const float v = ((static_cast<float>(py) + 0.5f) / FRAME_HEIGHT * 72.0f - 36.0f + 64.0f) / TERRAIN_SIZE;
                // the map's value, read as the shader reads it: nearest is close enough away from a tie
                float weights[9];
                for (int layer = 0; layer < 9; layer++){
                    const terraincore::LayerChannel channel = terraincore::terrainLayerChannel(layer);
                    TextureData& data = terrainComponent.blendMaps[channel.map].getData();
                    const int tx = std::clamp(static_cast<int>(u * BLEND_RESOLUTION), 0, BLEND_RESOLUTION - 1);
                    const int ty = std::clamp(static_cast<int>(v * BLEND_RESOLUTION), 0, BLEND_RESOLUTION - 1);
                    weights[layer] = static_cast<const uint8_t*>(data.getData())[(static_cast<size_t>(ty) * BLEND_RESOLUTION + tx) * 4 + channel.channel] / 255.0f;
                }
                const terraincore::LayerSelection selection = terraincore::selectLayers(weights, 9, keep);
                int drawn = 0;
                for (int k = 0; k < selection.count; k++){
                    drawn += selection.weight[k] > 0.0f ? 1 : 0;
                }
                EXPECT_EQ(counts[static_cast<size_t>(py) * FRAME_WIDTH + px], drawn) << terraincore::terrainQualityName(tier) << " at " << px << ", " << py;
            }
        }
    }
}

// AC4: a ramp through 5 layers at each tier: no step between neighbours beyond the High render's own
TEST_F(TerrainMaterialsGpu, LayerCapSeamless){
    JASPER_REQUIRE_GPU();
    makeTerrain(0.001f, [](float, float){ return 0.0f; });
    static const unsigned char colors[5][3] = {{200, 60, 60}, {60, 200, 60}, {60, 60, 200}, {200, 200, 60}, {60, 200, 200}};
    for (unsigned int i = 0; i < 5; i++){
        terrain->setTextureLayer(i, writeMap("ramp" + std::to_string(i), [&](int, int){
            return TerrainGpuColor{colors[i][0], colors[i][1], colors[i][2], 255};
        }));
    }
    const auto weight = [](float u, float at){
        return std::fmax(0.0f, 1.0f - std::fabs(u * 4.0f - at) / 1.5f);
    };
    setBlendMap(0, [&](float u, float){
        return TerrainGpuColor{static_cast<unsigned char>(weight(u, 0.0f) * 120.0f), static_cast<unsigned char>(weight(u, 1.0f) * 120.0f),
                               static_cast<unsigned char>(weight(u, 2.0f) * 120.0f), 255};
    });
    setBlendMap(1, [&](float u, float){
        return TerrainGpuColor{static_cast<unsigned char>(weight(u, 3.0f) * 120.0f), static_cast<unsigned char>(weight(u, 4.0f) * 120.0f), 0, 255};
    });
    camera.setPosition(0.0f, 64.0f, 0.1f);
    camera.setTarget(0.0f, 0.0f, 0.0f);

    const auto largestStep = [&](){
        EXPECT_TRUE(render());
        std::vector<uint8_t> frame;
        EXPECT_TRUE(readFrame(frame));
        int largest = 0;
        const int y = FRAME_HEIGHT / 2;
        for (int x = 40; x + 1 < FRAME_WIDTH - 40; x++){
            for (int c = 0; c < 3; c++){
                largest = std::max(largest, std::abs(frame[(static_cast<size_t>(y) * FRAME_WIDTH + x) * 4 + c] - frame[(static_cast<size_t>(y) * FRAME_WIDTH + x + 1) * 4 + c]));
            }
        }
        return largest;
    };
    setTier(terraincore::TerrainQuality::HIGH);
    const int high = largestStep();
    for (terraincore::TerrainQuality tier : {terraincore::TerrainQuality::MEDIUM, terraincore::TerrainQuality::LOW}){
        setTier(tier);
        EXPECT_LE(largestStep(), high + 6) << terraincore::terrainQualityName(tier);
    }
    expectFrameGolden("terrain_layer_cap_low");
}

// AC8: Low's height from the colour's alpha, the strongest layer's normal fading at the swap
TEST_F(TerrainMaterialsGpu, LowHeightAndNormal){
    JASPER_REQUIRE_GPU();
    makeTerrain(14.0f, rollingHills);
    addPbrLayer(0, "grass", 90, 150, 60);
    addPbrLayer(1, "rock", 140, 130, 120);
    setBlendMap(0, [](float u, float v){ return bands(u, v, 2); });
    setTier(terraincore::TerrainQuality::LOW);
    ASSERT_TRUE(render());
    expectFrameGolden("terrain_low_height");
    EXPECT_NO_ENGINE_ERRORS();
}

// ---------------------------------------------------------------- the features (T3 R9-R15)

// AC12: each define set uploads its reflected size, with no sokol validation error
TEST_F(TerrainMaterialsGpu, BlockSizes){
    JASPER_REQUIRE_GPU();
    makeTerrain(14.0f, rollingHills);
    addPbrLayer(0, "grass", 90, 150, 60);
    addPbrLayer(1, "rock", 140, 130, 120);
    setBlendMap(0, [](float u, float v){ return bands(u, v, 2); });
    const auto blockVectors = [&](){
        EXPECT_TRUE(render());
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain->getEntity());
        unsigned int size = 0;
        mesh.submeshes[0].shader.get()->shaderData.getUniformBlockMembers("u_fs_terrainLayers", size);
        return size / 16u;
    };
    // three vectors a layer and a component of the codes' vectors (GLES3's 224 fragment vectors)
    const unsigned int layers = 3u * 9u + 3u;
    EXPECT_EQ(blockVectors(), layers);
    component().macroMode = TerrainMacroMode::NOISE;
    EXPECT_EQ(blockVectors(), layers + 3u);
    component().resampleEnabled = true;
    EXPECT_EQ(blockVectors(), layers + 3u + 1u);
    component().heightBlendSharpness = 0.5f;
    EXPECT_EQ(blockVectors(), layers + 3u + 1u + 1u);
    // Flat Only is a flag of the layer's code: the cliff variant's block is the same size
    TerrainSurfaceLayer layer = terrain->getSurfaceLayer(0);
    layer.cliffProjection = TerrainCliffProjection::FLAT_ONLY;
    terrain->setSurfaceLayer(0, layer);
    EXPECT_EQ(blockVectors(), layers + 3u + 1u + 1u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC13: a golden per mode; Low's vertex path close to the fragment's; no fetch added
TEST_F(TerrainMaterialsGpu, MacroVariation){
    JASPER_REQUIRE_GPU();
    makeTerrain(14.0f, rollingHills);
    addPbrLayer(0, "grass", 90, 150, 60);
    setBlendMap(0, [](float, float){ return TerrainGpuColor{255, 0, 0, 255}; });
    component().macroMode = TerrainMacroMode::OFF;
    const int before = maxFetches();
    component().macroMode = TerrainMacroMode::NOISE;
    component().macroScale = 32.0f;
    component().macroStrength = 0.9f;
    EXPECT_EQ(maxFetches(), before) << "no texture read";
    ASSERT_TRUE(render());
    std::vector<uint8_t> tinted;
    ASSERT_TRUE(readFrame(tinted));
    expectFrameGolden("terrain_macro_noise");
    // strength 0 draws the layers as they are: the tint is what changed the frame
    component().macroStrength = 0.0f;
    ASSERT_TRUE(render());
    std::vector<uint8_t> untinted;
    ASSERT_TRUE(readFrame(untinted));
    double difference = 0.0;
    for (size_t i = 0; i < tinted.size(); i += 4){
        difference += std::abs(tinted[i + 1] - untinted[i + 1]);
    }
    EXPECT_GT(difference / static_cast<double>(tinted.size() / 4), 2.0) << "the noise tints the ground";
    component().macroStrength = 0.9f;

    component().macroMode = TerrainMacroMode::BASE_MAP;
    Material material = terrain->getMaterial();
    material.baseColorTexture = Texture(writeColorMap("macrobase", 128, 100, 150));
    terrain->setMaterial(material);
    ASSERT_TRUE(render());
    expectFrameGolden("terrain_macro_base_map");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC14: past resampleFar one read per map, near below; a continuous crossfade between
TEST_F(TerrainMaterialsGpu, DistanceResampling){
    JASPER_REQUIRE_GPU();
    makeTerrain(0.001f, [](float, float){ return 0.0f; });
    addPbrLayer(0, "grass", 90, 150, 60);
    setBlendMap(0, [](float, float){ return TerrainGpuColor{255, 0, 0, 255}; });
    camera.setPosition(0.0f, 4.0f, 64.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    component().resampleEnabled = true;
    component().resampleNear = 10.0f;
    component().resampleFar = 40.0f;
    const std::vector<int> counts = readCounts(terraincore::TerrainDebugView::FETCH_COST);
    int smallest = 1000, largest = 0;
    for (int count : counts){
        if (count > 0){
            smallest = std::min(smallest, count);
            largest = std::max(largest, count);
        }
    }
    EXPECT_EQ(smallest, 3 + 3) << "past resampleFar: the blend maps and one read per map";
    EXPECT_EQ(largest, 3 + 6) << "in the crossfade: both";
    ASSERT_TRUE(render());
    expectFrameGolden("terrain_resample");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC15: one plane at Medium and Low within the tier's reads per map; Flat Only reads no side plane
TEST_F(TerrainMaterialsGpu, CliffProjection){
    JASPER_REQUIRE_GPU();
    makeTerrain(24.0f, cliffBack);
    addPbrLayer(0, "rock", 140, 130, 120);
    setBlendMap(0, [](float, float){ return TerrainGpuColor{255, 0, 0, 255}; });
    setTier(terraincore::TerrainQuality::HIGH);
    const int withSides = maxFetches();
    TerrainSurfaceLayer layer = terrain->getSurfaceLayer(0);
    layer.cliffProjection = TerrainCliffProjection::FLAT_ONLY;
    terrain->setSurfaceLayer(0, layer);
    const int flatOnly = maxFetches();
    EXPECT_LT(flatOnly, withSides);
    EXPECT_LE(flatOnly, 3 + 3 * 2) << "near and far, on three maps";

    layer.cliffProjection = TerrainCliffProjection::AUTO;
    terrain->setSurfaceLayer(0, layer);
    setTier(terraincore::TerrainQuality::MEDIUM);
    EXPECT_LE(maxFetches(), 1 + 3 * 2) << "one blend map, 2 reads a map";
    ASSERT_TRUE(render());
    expectFrameGolden("terrain_cliff_medium");
    setTier(terraincore::TerrainQuality::LOW);
    EXPECT_LE(maxFetches(), 1 + 2) << "colour and normal, once each";
    ASSERT_TRUE(render());
    expectFrameGolden("terrain_cliff_low");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC16: 0 is the classic blend, above it Terrain3D's
TEST_F(TerrainMaterialsGpu, HeightBlendSharpness){
    JASPER_REQUIRE_GPU();
    makeTerrain(14.0f, rollingHills);
    addPbrLayer(0, "grass", 90, 150, 60);
    addPbrLayer(1, "rock", 140, 130, 120);
    setBlendMap(0, [](float u, float v){ return bands(u, v, 2); });
    component().heightBlendSharpness = 0.6f;
    ASSERT_TRUE(render());
    expectFrameGolden("terrain_height_blend");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC18: the bake reaches the frame; it does not move with the LOD where vertex normals do
TEST_F(TerrainMaterialsGpu, MacroNormal){
    JASPER_REQUIRE_GPU();
    makeTerrain(20.0f, rollingHills);
    addPbrLayer(0, "grass", 180, 180, 180);
    setBlendMap(0, [](float, float){ return TerrainGpuColor{255, 0, 0, 255}; });
    component().macroNormal = true;
    ASSERT_TRUE(render());
    // the bake is a job: its strength is 0 until it is uploaded, a frame after it is done
    Scene* current = &scene;
    Entity entity = terrain->getEntity();
    GpuEnvironment::runFrames([current, entity](){
        return current->getSystem<RenderSystem>()->isTerrainMacroNormalReady(entity);
    }, 600);
    GpuEnvironment::drawFrames(3);
    EXPECT_TRUE(scene.getSystem<RenderSystem>()->isTerrainMacroNormalReady(entity));
    expectFrameGolden("terrain_macro_normal");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC19: 12 flat layers each draw their colour; an opaque map draws layers 10-12 at 0
// A layer with a Tint and no map draws its Tint, as it does beside a layer that has maps: the terrain
// had no detail array then, so no blend map weighted anything and it drew its base material alone
TEST_F(TerrainMaterialsGpu, TintOnlyLayersDraw){
    JASPER_REQUIRE_GPU();
    makeTerrain(0.001f, [](float, float){ return 0.0f; });
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(-64.0f, 64.0f, -36.0f, 36.0f, 1.0f, 400.0f);
    camera.setPosition(0.0f, 100.0f, 0.0f);
    camera.setUp(0.0f, 0.0f, -1.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    sun.setIntensity(0.0f);
    terrain->setReceiveLights(false);
    const Vector4 tints[3] = {Vector4(0.9f, 0.1f, 0.1f, 1.0f), Vector4(0.1f, 0.8f, 0.1f, 1.0f), Vector4(0.1f, 0.1f, 0.9f, 1.0f)};
    component().surfaceLayers.assign(3, TerrainSurfaceLayer());
    for (int l = 0; l < 3; l++){
        component().surfaceLayers[l].pbr = true;
        component().surfaceLayers[l].colorFactor = tints[l];
    }
    component().needUpdateTerrain = true;
    // the second layer everywhere
    setBlendMap(0, [](float, float){ return TerrainGpuColor{0, 255, 0, 255}; });
    ASSERT_TRUE(render());
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));
    const size_t center = (static_cast<size_t>(FRAME_HEIGHT / 2) * FRAME_WIDTH + FRAME_WIDTH / 2) * 4;
    const int r = frame[center], g = frame[center + 1], b = frame[center + 2];
    EXPECT_GT(g, r + 60) << "the terrain drew (" << r << ", " << g << ", " << b << "), not the green Tint";
    EXPECT_GT(g, b + 60) << "the terrain drew (" << r << ", " << g << ", " << b << "), not the green Tint";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(TerrainMaterialsGpu, TwelveLayers){
    JASPER_REQUIRE_GPU();
    makeTerrain(0.001f, [](float, float){ return 0.0f; });
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(-64.0f, 64.0f, -36.0f, 36.0f, 1.0f, 400.0f);
    camera.setPosition(0.0f, 100.0f, 0.0f);
    camera.setUp(0.0f, 0.0f, -1.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    sun.setIntensity(0.0f);
    terrain->setReceiveLights(false);
    for (unsigned int i = 0; i < 12; i++){
        terrain->setTextureLayer(i, writeMap("twelve" + std::to_string(i), [i](int, int){
            return TerrainGpuColor{static_cast<unsigned char>(20 + i * 20), 128, 64, 255};
        }));
    }
    // the map's opaque: base, RGB 0, alpha 255: layers 10-12 weigh nothing
    for (unsigned int m = 0; m < 3; m++){
        setBlendMap(m, [](float, float){ return TerrainGpuColor{0, 0, 0, 255}; });
    }
    ASSERT_TRUE(render());
    std::vector<uint8_t> opaque;
    ASSERT_TRUE(readFrame(opaque));
    const size_t center = (static_cast<size_t>(FRAME_HEIGHT / 2) * FRAME_WIDTH + FRAME_WIDTH / 2) * 4;
    const int baseRed = opaque[center];

    // layer 11 (index 10) by map 1's alpha, inverted: alpha 0 is its full weight
    setBlendMap(1, [](float, float){ return TerrainGpuColor{0, 0, 0, 0}; });
    ASSERT_TRUE(render());
    std::vector<uint8_t> extended;
    ASSERT_TRUE(readFrame(extended));
    EXPECT_NE(static_cast<int>(extended[center]), baseRed) << "the alpha's layer draws";
    expectFrameGolden("terrain_twelve_layers");
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {

    // Every variant T3 makes, one test each, drawn on a small frame: each layer set (none, nine colour, nine
    // PBR) at each tier, with Limit Layers Per Pixel and without, with holes and without, at each Lit Quality,
    // and each feature, limited, at each tier and Lit Quality. A whole combination no test drew once crashed
    // llvmpipe's JIT (LimitedHighDraws*); one test per variant names each one that fails. Every case sets only
    // what a terrain can set (its layers and fields, the tier, Lit Quality) and the engine's own rule picks the
    // defines (RenderSystem::getTerrainDefineBits), so each variant drawn is one a real terrain gets: with no
    // more layers than the cap, "limited" draws without TERRAIN_LAYER_LIMIT, as such a terrain does.
    enum class VariantLayers { NONE, COLOR, PBR };
    enum class VariantFeature { NONE, MACRO_NOISE, MACRO_BASE_MAP, RESAMPLE, CLIFF, HEIGHT_BLEND, MACRO_NORMAL };

    struct VariantCase {
        VariantLayers layers;
        LitQuality lit;
        terraincore::TerrainQuality tier;
        bool limit;
        bool holes;
        VariantFeature feature;
        terraincore::TerrainDebugView debug = terraincore::TerrainDebugView::SHADED;
        // T4: the same terrain as a Terrain World ('Twd' on every pass), its maps its tiles'
        bool world = false;
    };

    std::vector<VariantCase> allVariantCases(){
        std::vector<VariantCase> cases;
        for (VariantLayers layers : {VariantLayers::NONE, VariantLayers::COLOR, VariantLayers::PBR}){
            for (LitQuality lit : {LitQuality::STANDARD, LitQuality::HIGH}){
                for (terraincore::TerrainQuality tier : {terraincore::TerrainQuality::HIGH, terraincore::TerrainQuality::MEDIUM, terraincore::TerrainQuality::LOW}){
                    for (bool limit : {false, true}){
                        for (bool holes : {false, true}){
                            cases.push_back({layers, lit, tier, limit, holes, VariantFeature::NONE});
                        }
                    }
                    for (int f = static_cast<int>(VariantFeature::MACRO_NOISE); f <= static_cast<int>(VariantFeature::MACRO_NORMAL); f++){
                        cases.push_back({layers, lit, tier, true, false, static_cast<VariantFeature>(f)});
                    }
                    // the Scene view's Terrain views (R42), which the editor draws on any terrain
                    if (lit == LitQuality::STANDARD){
                        for (terraincore::TerrainDebugView view : {terraincore::TerrainDebugView::LAYER_COUNT, terraincore::TerrainDebugView::FETCH_COST, terraincore::TerrainDebugView::HOLES}){
                            cases.push_back({layers, lit, tier, true, true, VariantFeature::NONE, view});
                        }
                    }
                }
            }
        }
        // T4: every one of them again as a Terrain World
        const size_t plain = cases.size();
        for (size_t i = 0; i < plain; i++){
            VariantCase worldCase = cases[i];
            worldCase.world = true;
            cases.push_back(worldCase);
        }
        return cases;
    }

    // A cooked 2 x 2 world over the fixture's 128 m (tiles -1..0 of 64 quads), heights by f, a hole
    // patch in each tile when asked: what an export writes (T4 R53)
    void writeVariantWorld(const std::filesystem::path& folder, float maxHeight, float (*f)(float, float), bool holes){
        using namespace jasperengine::terraincore;
        WorldManifest manifest;
        manifest.guid = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
        manifest.lattice.tileQuads = 64;
        manifest.lattice.spacing = 1.0f;
        manifest.maps.maxHeight = maxHeight;
        manifest.maps.hasHoles = holes;
        WorldOverview overview;
        initOverview(overview, manifest.lattice, TileCoord(-1, -1), TileCoord(0, 0), 0);
        auto write = [](const std::filesystem::path& path, const std::vector<uint8_t>& bytes){
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        };
        for (int z = -1; z <= 0; z++){
            for (int x = -1; x <= 0; x++){
                WorldTile tile;
                initTile(tile, manifest.lattice, TileCoord(x, z), 0);
                for (int j = 0; j < 67; j++){
                    for (int i = 0; i < 67; i++){
                        const float h = std::fmin(std::fmax(f(static_cast<float>(x * 64 + i - 1), static_cast<float>(z * 64 + j - 1)), 0.0f), 1.0f);
                        tile.setSample(i, j, static_cast<uint16_t>(std::lround(h * 65535.0f)));
                    }
                }
                if (holes){
                    tile.holes.allocate(64, 1, 8, 255);
                    for (int hz = 20; hz < 40; hz++){
                        for (int hx = 20; hx < 40; hx++) tile.holes.texel(hx, hz)[0] = 0;
                    }
                }
                const float colours[] = {0.4f, 0.55f, 0.3f, 1.0f};
                buildDerived(tile, manifest.lattice, colours, 0);
                std::vector<uint8_t> bytes;
                encodeTile(tile, TileCodec{}, TileEncodeOptions{}, bytes);
                write(folder / ("tiles/" + std::to_string(x) + "_" + std::to_string(z) + ".jtile"), bytes);
                setOverviewPatch(overview, tile);
                const TileMinMax whole = tileNodeMinMax(tile, manifest.lattice, manifest.lattice.tileLevel(), 0, 0);
                manifest.tiles.push_back(WorldTileInfo{TileCoord(x, z), whole.min, whole.max});
                manifest.skirtDepth = std::fmax(manifest.skirtDepth, tile.skirtDepth);
            }
        }
        std::vector<uint8_t> bytes;
        encodeOverview(overview, manifest.lattice, TileCodec{}, 0, bytes, nullptr);
        write(folder / "overview.jtile", bytes);
        encodeWorldBin(manifest, bytes);
        write(folder / "world.bin", bytes);
    }

    std::string variantName(const ::testing::TestParamInfo<VariantCase>& info){
        static const char* const layerNames[] = {"NoLayers", "ColorLayers", "PbrLayers"};
        static const char* const featureNames[] = {"", "_MacroNoise", "_MacroBaseMap", "_Resample", "_Cliff", "_HeightBlend", "_MacroNormal"};
        static const char* const viewNames[] = {"", "_LayerCountView", "_FetchCostView", "_HolesView"};
        const VariantCase& c = info.param;
        std::string name = std::string(layerNames[static_cast<int>(c.layers)]) + "_" + terraincore::terrainQualityName(c.tier) +
                           (c.lit == LitQuality::HIGH ? "_LitHigh" : "_LitStandard") + (c.limit ? "_Limited" : "") +
                           (c.holes ? "_Holes" : "") + featureNames[static_cast<int>(c.feature)] + viewNames[static_cast<int>(c.debug)] +
                           (c.world ? "_World" : "");
        return name;
    }

    class TerrainVariantGpu : public TerrainMaterialsGpu, public ::testing::WithParamInterface<VariantCase> {};

}

TEST_P(TerrainVariantGpu, Draws){
    JASPER_REQUIRE_GPU();
    const VariantCase& c = GetParam();
    frameWidth = 64;
    frameHeight = 36;
    if (c.layers == VariantLayers::NONE){
        makeTerrain(8.0f, cliffBack);
    }else{
        terrainNine(8.0f, cliffBack, c.layers == VariantLayers::PBR);
    }
    scene.setLitQuality(c.lit);
    setTier(c.tier);
    terraincore::TerrainQualitySettings::setDebugView(c.debug);
    TerrainComponent& terrainComponent = component();
    terrainComponent.limitLayersPerPixel = c.limit;
    if (c.holes && !c.world){
        holesAcross();
    }
    if (c.world){
        // T4 R58: the Terrain becomes a World; its tiles hold the heights and holes
        const std::filesystem::path folder = files.path() / "variant.terrainworld";
        writeVariantWorld(folder, 8.0f, cliffBack, c.holes);
        TerrainWorldComponent world;
        world.worldData.path = folder.generic_string();
        world.useCameraFocus = true;
        scene.addComponent<TerrainWorldComponent>(terrain->getEntity(), world);
    }
    terrainComponent.macroMode = c.feature == VariantFeature::MACRO_NOISE ? TerrainMacroMode::NOISE
                                 : (c.feature == VariantFeature::MACRO_BASE_MAP ? TerrainMacroMode::BASE_MAP : TerrainMacroMode::OFF);
    terrainComponent.resampleEnabled = c.feature == VariantFeature::RESAMPLE;
    terrainComponent.resampleNear = 10.0f;
    terrainComponent.resampleFar = 60.0f;
    terrainComponent.heightBlendSharpness = c.feature == VariantFeature::HEIGHT_BLEND ? 0.5f : 0.0f;
    terrainComponent.macroNormal = c.feature == VariantFeature::MACRO_NORMAL;
    for (size_t i = 0; i < terrainComponent.surfaceLayers.size(); i++){
        terrainComponent.surfaceLayers[i].cliffProjection = c.feature == VariantFeature::CLIFF && i % 2 == 0 ? TerrainCliffProjection::FLAT_ONLY : TerrainCliffProjection::AUTO;
    }

    if (c.world){
        // its overview, then its pages, before the frame is judged
        GpuEnvironment::setView(frameWidth, frameHeight, frameWidth, frameHeight, Scaling::STRETCH);
        ASSERT_TRUE(GpuEnvironment::runFrames([this](){
            const TerrainWorldRuntime* world = component().world;
            return world && world->isReady() && world->tiles().stats().residentPages > 0;
        }, 600)) << "the world never streamed in";
    }
    ASSERT_TRUE(render()) << "never settled";
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));
    // an image: mostly the terrain (not SetUp's background colour), and not black where it is lit
    const int sky[3] = {140, 173, 209};
    size_t ground = 0, black = 0;
    const size_t pixels = frame.size() / 4;
    for (size_t i = 0; i < pixels; i++){
        const uint8_t* p = &frame[i * 4];
        ground += std::abs(p[0] - sky[0]) + std::abs(p[1] - sky[1]) + std::abs(p[2] - sky[2]) > 24 ? 1 : 0;
        black += p[0] < 4 && p[1] < 4 && p[2] < 4 ? 1 : 0;
    }
    EXPECT_GT(ground, pixels / 4) << "drew no terrain";
    EXPECT_LT(black, pixels / 2) << "black (NaN or no light)";
    if (HasFailure()){
        std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::replace(name.begin(), name.end(), '/', '_');
        writePng(jasperengine::test::testPaths().outputDir + "/variant_" + name + ".png", frame, frameWidth, frameHeight);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

INSTANTIATE_TEST_SUITE_P(EveryVariant, TerrainVariantGpu, ::testing::ValuesIn(allVariantCases()), variantName);
