// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The variants a missing precompiled one falls back to (ShaderPool::getFallbackProperties):
// nearest first, only optional features given up, never a bit that changes a uniform block
// the draw uploads by the key's own bits.

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"

#include <set>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    uint64_t bits(std::initializer_list<int> list) {
        uint64_t mask = 0;
        for (int bit : list)
            mask |= shaderPropertyBit(bit);
        return mask;
    }

    // MESH: Sao Fog S2d Shw Mir Ibl Nmp Tan Uv2 Uv1
    const uint64_t meshDroppable = bits({21, 12, 23, 4, 20, 19, 7, 8, 2, 1});
    // DEPTH: Ams Tex; GBUFFER: Occ Uv2 Mrt Bct
    const uint64_t depthDroppable = bits({7, 0});
    const uint64_t gbufferDroppable = bits({16, 17, 8, 0});

    // every candidate a subset of properties, the bits outside droppable kept, none repeated,
    // one per non-empty subset of the droppable bits present
    void expectWellFormed(ShaderType type, uint64_t properties, uint64_t droppable) {
        const std::vector<uint64_t> candidates = ShaderPool::getFallbackProperties(type, properties);
        std::set<uint64_t> seen;
        for (uint64_t candidate : candidates) {
            EXPECT_EQ(candidate & ~properties, 0u) << std::hex << candidate;
            EXPECT_EQ(candidate & ~droppable, properties & ~droppable) << std::hex << candidate;
            EXPECT_NE(candidate, properties);
            EXPECT_TRUE(seen.insert(candidate).second) << std::hex << candidate;
        }
        int present = 0;
        for (int bit = 0; bit < SHADER_PROPERTY_BITS; bit++) {
            if (properties & droppable & shaderPropertyBit(bit))
                present++;
        }
        EXPECT_EQ(candidates.size(), (size_t(1) << present) - 1);
        // fewest dropped first
        int previous = 0;
        for (uint64_t candidate : candidates) {
            int dropped = 0;
            for (int bit = 0; bit < SHADER_PROPERTY_BITS; bit++) {
                if ((properties & ~candidate) & shaderPropertyBit(bit))
                    dropped++;
            }
            EXPECT_GE(dropped, previous);
            previous = dropped;
        }
    }

}

TEST(ShaderFallback, NearestFirstInTheGiveUpOrder) {
    // Uv1 Puc Shw Nor Fog Ibl
    const uint64_t properties = bits({1, 3, 4, 6, 12, 19});
    const std::vector<uint64_t> candidates = ShaderPool::getFallbackProperties(ShaderType::MESH, properties);
    ASSERT_EQ(candidates.size(), 15u);
    // one feature: Fog, Shw, Ibl, Uv1
    EXPECT_EQ(candidates[0], properties & ~bits({12}));
    EXPECT_EQ(candidates[1], properties & ~bits({4}));
    EXPECT_EQ(candidates[2], properties & ~bits({19}));
    EXPECT_EQ(candidates[3], properties & ~bits({1}));
    // then two, the earlier ones first: Fog+Shw
    EXPECT_EQ(candidates[4], properties & ~bits({12, 4}));
    // and last all of them
    EXPECT_EQ(candidates.back(), bits({3, 6}));
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::MESH, candidates[0]), "mesh_Uv1PucShwNorIbl");
    expectWellFormed(ShaderType::MESH, properties, meshDroppable);
}

TEST(ShaderFallback, LayoutBitsAreNeverDropped) {
    // the lighting model and per-object lights block, the u_fs_pbrParams layout, the post
    // colour, the geometry: kept whatever else is given up
    const uint64_t kept[] = {
        bits({0}),       // Ult
        bits({3}),       // Puc
        bits({5}),       // Sto
        bits({24}),      // Ams
        bits({28}),      // Vtl
        bits({29}),      // Lc4
        bits({30}),      // L16
        bits({31}),      // Pcl
        bits({13, 14, 15, 16, 17, 18, 11, 27}), // Ski Mta Mnr Mtg Ter Ist Txr Tpb
    };
    for (uint64_t keep : kept) {
        const uint64_t properties = keep | meshDroppable;
        for (uint64_t candidate : ShaderPool::getFallbackProperties(ShaderType::MESH, properties)) {
            EXPECT_EQ(candidate & keep, keep) << std::hex << candidate;
        }
    }

    // every MESH bit set: only the droppable ones ever go
    expectWellFormed(ShaderType::MESH, ShaderPool::getValidPropertyMask(ShaderType::MESH), meshDroppable);
    // a key with none of them has no fallback (Puc Nor Vtl Lc4 Pcl)
    EXPECT_TRUE(ShaderPool::getFallbackProperties(ShaderType::MESH, bits({3, 6, 28, 29, 31})).empty());
}

TEST(ShaderFallback, DepthAndGBufferGiveUpTheirTextures) {
    // Tex Ski Ams Sto: the alpha mask, then the texture, then both; skinning and Stochastic
    // Tiling stay (the depth material is uploaded at its reflected size)
    const uint64_t depth = bits({0, 1, 7, 14});
    const std::vector<uint64_t> depthCandidates = ShaderPool::getFallbackProperties(ShaderType::DEPTH, depth);
    ASSERT_EQ(depthCandidates.size(), 3u);
    EXPECT_EQ(depthCandidates[0], bits({0, 1, 14}));
    EXPECT_EQ(depthCandidates[1], bits({1, 7, 14}));
    EXPECT_EQ(depthCandidates[2], bits({1, 14}));
    expectWellFormed(ShaderType::DEPTH, ShaderPool::getValidPropertyMask(ShaderType::DEPTH), depthDroppable);

    // Bct Nor Mrt Sto
    const uint64_t gbuffer = bits({0, 1, 8, 10});
    const std::vector<uint64_t> gbufferCandidates = ShaderPool::getFallbackProperties(ShaderType::GBUFFER, gbuffer);
    ASSERT_EQ(gbufferCandidates.size(), 3u);
    EXPECT_EQ(gbufferCandidates[0], bits({0, 1, 10}));
    EXPECT_EQ(gbufferCandidates[1], bits({1, 8, 10}));
    EXPECT_EQ(gbufferCandidates[2], bits({1, 10}));
    expectWellFormed(ShaderType::GBUFFER, ShaderPool::getValidPropertyMask(ShaderType::GBUFFER), gbufferDroppable);
}

TEST(ShaderFallback, GBufferGivesUpOcclusionFirst) {
    // Bct Nor Occ: the SSR occlusion map goes before the base colour
    const uint64_t gbuffer = bits({0, 1, 16});
    const std::vector<uint64_t> candidates = ShaderPool::getFallbackProperties(ShaderType::GBUFFER, gbuffer);
    ASSERT_EQ(candidates.size(), 3u);
    EXPECT_EQ(candidates[0], bits({0, 1}));
    EXPECT_EQ(candidates[1], bits({1, 16}));
    EXPECT_EQ(candidates[2], bits({1}));
    expectWellFormed(ShaderType::GBUFFER, bits({0, 1, 8, 16, 17}), gbufferDroppable);
}

TEST(ShaderFallback, OtherTypesHaveNone) {
    // their bits select passes or the output encoding (uber features, bloom passes, the
    // post colour, exclusive HTML UI variants)
    EXPECT_TRUE(ShaderPool::getFallbackProperties(ShaderType::SKYBOX, bits({0})).empty());
    EXPECT_TRUE(ShaderPool::getFallbackProperties(ShaderType::BLIT, bits({0, 7, 10})).empty());
    EXPECT_TRUE(ShaderPool::getFallbackProperties(ShaderType::BLOOM, bits({0, 1})).empty());
    EXPECT_TRUE(ShaderPool::getFallbackProperties(ShaderType::POINTS, bits({0, 4})).empty());
    EXPECT_TRUE(ShaderPool::getFallbackProperties(ShaderType::HTMLUI_POST, bits({6})).empty());
    EXPECT_TRUE(ShaderPool::getFallbackProperties(ShaderType::SSR, 0).empty());
}
