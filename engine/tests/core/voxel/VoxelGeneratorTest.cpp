// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R33, R35 (AC32, AC33): bases are pure functions of a block's positions

#include "VoxelTestSupport.h"
#include "voxel/VoxelGenerator.h"
#include "voxel/VoxelGeneratorNoise.h"

#include <gtest/gtest.h>

using namespace voxeltest;

namespace {

    class HillSource final : public TerrainSource {
    public:
        float height(float x, float z) const override { return 20.0f + 4.0f * std::sin(x * 0.05f) * std::cos(z * 0.04f); }
        void gradient(float x, float z, float& dhdx, float& dhdz) const override {
            dhdx = 4.0f * 0.05f * std::cos(x * 0.05f) * std::cos(z * 0.04f);
            dhdz = -4.0f * 0.04f * std::sin(x * 0.05f) * std::sin(z * 0.04f);
        }
        TerrainLayerSample layers(float, float) const override { return TerrainLayerSample{2, 3, 64}; }
    };

}

TEST(VoxelGenerator, CoarseBlockWithoutFine) {
    Store store(desc(128, 128, 128, 3));
    store.setBase(wavyBase(60.0));
    const BlockKey coarse{2, 0, 0, 0};
    const BlockContent content = store.content(coarse);
    EXPECT_EQ(store.slot(BlockKey{0, 0, 0, 0}).kind(), Slot::Base);
    EXPECT_EQ(store.slot(BlockKey{0, 1, 3, 2}).kind(), Slot::Base);
    // It equals the decimation of the fine blocks, sample for sample
    for (int z = 0; z < 16; z += 3) {
        for (int y = 0; y < 16; y += 3) {
            for (int x = 0; x < 16; x += 3) {
                EXPECT_EQ(content.sdfAt(sampleIndex(x, y, z)), store.sdfAt(0, x * 4, y * 4, z * 4));
            }
        }
    }
}

TEST(VoxelGenerator, SolidGroundClipsWithoutEvaluating) {
    const std::shared_ptr<const BaseSource> base = makeSolidGroundBase(40.0, 1.0, 2);
    int16_t sdf = 0;
    uint16_t material = 0;
    EXPECT_TRUE(base->evaluateUniform(Int3{0, 128, 0}, 1, sdf, material));
    EXPECT_EQ(sdf, SDF_AIR);
    EXPECT_TRUE(base->evaluateUniform(Int3{0, 0, 0}, 1, sdf, material));
    EXPECT_EQ(sdf, -SDF_MAX);
    EXPECT_EQ(material, packMaterial(2, 2, 0));
    EXPECT_FALSE(base->evaluateUniform(Int3{0, 32, 0}, 1, sdf, material));
}

TEST(VoxelGenerator, TerrainSeedFlush) {
    Store store(desc(128, 64, 128, 2, BoundsMode::Open));
    auto source = std::make_shared<HillSource>();
    TerrainBaseOptions options;
    options.voxelSize = 1.0;
    options.overlapDepth = 0.02;
    store.setBase(makeTerrainBase(source, options));
    // Over the surface the seed's own distance stays within overlapDepth + cutTolerance of the rule's value
    for (int z = 8; z < 120; z += 7) {
        for (int x = 8; x < 120; x += 5) {
            const double h = source->height(static_cast<float>(x), static_cast<float>(z));
            float dhdx = 0.0f, dhdz = 0.0f;
            source->gradient(static_cast<float>(x), static_cast<float>(z), dhdx, dhdz);
            const double d = valueAt(store, Vec3d{static_cast<double>(x), h, static_cast<double>(z)});
            EXPECT_NEAR(d, 0.02 * slopeFactor(dhdx, dhdz), 0.02 + 0.25);
        }
    }
    // The materials come from the terrain's layers
    const uint16_t material = store.materialAt(0, 32, 20, 32);
    EXPECT_EQ(materialSlotA(material), 2);
    EXPECT_EQ(materialSlotB(material), 3);
    EXPECT_EQ(materialWeightB(material), 64);
}

TEST(VoxelGenerator, BlendReachesTerrainAtEdge) {
    auto source = std::make_shared<HillSource>();
    TerrainBaseOptions options;
    const std::shared_ptr<const BaseSource> terrain = makeTerrainBase(source, options);
    // A cave everywhere: all air, faded to exact terrain at the footprint's edge
    const std::shared_ptr<const BaseSource> blended = makeBlendedBase(makeEmptyBase(), terrain, Int3{128, 64, 128}, 16.0);
    int16_t sdf[BLOCK_SAMPLES], direct[BLOCK_SAMPLES];
    uint16_t material[BLOCK_SAMPLES], directMaterial[BLOCK_SAMPLES];
    blended->evaluate(Int3{0, 16, 0}, 1, sdf, material);
    terrain->evaluate(Int3{0, 16, 0}, 1, direct, directMaterial);
    EXPECT_EQ(sdf[sampleIndex(0, 4, 8)], direct[sampleIndex(0, 4, 8)]);
    blended->evaluate(Int3{64, 16, 64}, 1, sdf, material);
    EXPECT_EQ(sdf[sampleIndex(0, 4, 0)], SDF_AIR);
}

TEST(VoxelGenerator, GeneratorJsonIsCanonical) {
    GeneratorDesc desc;
    desc.kind = "terrain";
    desc.sourceGuid = "abc";
    desc.sourceHash = 77;
    desc.blendWidth = 8.0;
    desc.otherKind = "noise_graph";
    const std::string json = generatorToJson(desc);
    EXPECT_EQ(json, "{\"blend_width\":8.0,\"kind\":\"terrain\",\"other_kind\":\"noise_graph\",\"source_guid\":\"abc\",\"source_hash\":77}");
    GeneratorDesc back;
    ASSERT_TRUE(generatorFromJson(json, back));
    EXPECT_EQ(generatorToJson(back), json);
    GeneratorDesc bad;
    EXPECT_FALSE(generatorFromJson("{nope", bad));
}

// T6 R34: only through T2's runtime. Hashes are not pinned while T2's kernel table may change, so these
// hold the properties R5 and R34 ask of it.
namespace {

    const char* const SDF_GRAPH = R"({
      "formatVersion": 1, "kind": "noise", "kindVersion": 1, "nextId": 8,
      "nodes": [
        {"id": "n1", "type": "position", "position": [0.0, 0.0], "params": {}},
        {"id": "n2", "type": "scale_offset", "position": [0.0, 0.0], "params": {"scale": 1.0, "offset": -32.0}},
        {"id": "n3", "type": "fbm", "position": [0.0, 0.0], "params": {"basis": "perlin", "dimensions": "3d", "frequency": 0.05, "octaves": 3, "lacunarity": 2.0, "gain": 0.5, "seed_offset": 0}},
        {"id": "n4", "type": "scale_offset", "position": [0.0, 0.0], "params": {"scale": 6.0, "offset": 0.0}},
        {"id": "n5", "type": "math", "position": [0.0, 0.0], "params": {"op": "add"}},
        {"id": "n6", "type": "output_value", "position": [0.0, 0.0], "params": {"name": "sdf"}}
      ],
      "edges": [
        {"id": "e1", "from": {"node": "n1", "port": "y"}, "to": {"node": "n2", "port": "in"}},
        {"id": "e2", "from": {"node": "n3", "port": "out"}, "to": {"node": "n4", "port": "in"}},
        {"id": "e3", "from": {"node": "n2", "port": "out"}, "to": {"node": "n5", "port": "a"}},
        {"id": "e4", "from": {"node": "n4", "port": "out"}, "to": {"node": "n5", "port": "b"}},
        {"id": "e5", "from": {"node": "n5", "port": "out"}, "to": {"node": "n6", "port": "in"}}
      ],
      "variables": [], "groups": [], "viewport": {"offset": [0.0, 0.0], "zoom": 1.0},
      "metadata": {"noise": {"space": "world", "dimensions": "3d"}}
    })";

    std::shared_ptr<const BaseSource> noiseBaseOf(uint32_t seed) {
        std::string error;
        NoiseBaseOptions options;
        options.graph = compileVoxelGraph(SDF_GRAPH, error);
        EXPECT_NE(options.graph, nullptr) << error;
        options.seed = seed;
        return options.graph ? makeNoiseBase(options) : nullptr;
    }

}

TEST(VoxelGenerator, NoiseCoarseSampleIsTheFineSample) {
    const std::shared_ptr<const BaseSource> base = noiseBaseOf(7);
    ASSERT_NE(base, nullptr);
    Store store(desc(128, 128, 128, 3));
    store.setBase(base);
    const BlockContent coarse = store.content(BlockKey{2, 0, 0, 0});
    EXPECT_EQ(store.slot(BlockKey{0, 0, 0, 0}).kind(), Slot::Base);
    int checked = 0;
    for (int z = 1; z < 16; z += 2) {
        for (int y = 1; y < 16; y += 2) {
            for (int x = 1; x < 16; x += 2) {
                ASSERT_EQ(coarse.sdfAt(sampleIndex(x, y, z)), store.sdfAt(0, x * 4, y * 4, z * 4)) << x << " " << y << " " << z;
                checked++;
            }
        }
    }
    EXPECT_GT(checked, 100);
}

TEST(VoxelGenerator, NoiseIsDeterministicBySeedAndClipsByRange) {
    Store a(desc(64, 64, 64));
    Store b(desc(64, 64, 64));
    Store other(desc(64, 64, 64));
    a.setBase(noiseBaseOf(7));
    b.setBase(noiseBaseOf(7));
    other.setBase(noiseBaseOf(8));
    EXPECT_EQ(contentHash(a), contentHash(b));
    EXPECT_NE(contentHash(a), contentHash(other));
    // Far above the band the range alone says air, with no evaluation
    int16_t sdf = 0;
    uint16_t material = 0;
    const std::shared_ptr<const BaseSource> base = noiseBaseOf(7);
    EXPECT_TRUE(base->evaluateUniform(Int3{0, 1600, 0}, 1, sdf, material));
    EXPECT_EQ(sdf, SDF_AIR);
    EXPECT_FALSE(base->evaluateUniform(Int3{0, 32, 0}, 1, sdf, material));
}

TEST(VoxelGenerator, AGraphWithNoSdfOutputIsRefused) {
    std::string json = SDF_GRAPH;
    json.replace(json.find("\"name\": \"sdf\""), 13, "\"name\": \"elevation\"");
    std::string error;
    EXPECT_EQ(compileVoxelGraph(json, error), nullptr);
    EXPECT_NE(error.find("sdf"), std::string::npos);
}
