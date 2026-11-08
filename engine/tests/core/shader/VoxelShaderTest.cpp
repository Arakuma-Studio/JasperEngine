// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R49, R51, R83 (AC36, AC39, AC55): every tier's fetches, on every backend's translation

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "texture/MaterialPbr.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr uint32_t OP_ENTRY_POINT = 15;
    constexpr uint32_t OP_FUNCTION = 54;
    constexpr uint32_t OP_FUNCTION_END = 56;
    constexpr uint32_t OP_FUNCTION_CALL = 57;
    constexpr uint32_t OP_IMAGE_SAMPLE_FIRST = 87;
    constexpr uint32_t OP_IMAGE_SAMPLE_LAST = 97;

    // The reads of one invocation: each function's own, plus its callees' per call. All branches
    // count, so this is the worst case.
    int countSamples(const std::vector<uint32_t>& words) {
        std::map<uint32_t, int> own;
        std::map<uint32_t, std::vector<uint32_t>> calls;
        uint32_t entry = 0, current = 0;
        for (size_t offset = 5; offset < words.size();) {
            const uint32_t opcode = words[offset] & 0xffffu;
            const uint32_t count = words[offset] >> 16;
            if (count == 0 || offset + count > words.size()) return -1;
            if (opcode == OP_ENTRY_POINT && count > 2) entry = words[offset + 2];
            else if (opcode == OP_FUNCTION && count > 2) current = words[offset + 2];
            else if (opcode == OP_FUNCTION_END) current = 0;
            else if (opcode == OP_FUNCTION_CALL && count > 3 && current != 0) calls[current].push_back(words[offset + 3]);
            else if (opcode >= OP_IMAGE_SAMPLE_FIRST && opcode <= OP_IMAGE_SAMPLE_LAST && current != 0) own[current]++;
            offset += count;
        }
        std::map<uint32_t, int> total;
        std::function<int(uint32_t)> visit = [&](uint32_t function) -> int {
            const auto found = total.find(function);
            if (found != total.end()) return found->second;
            int sum = own[function];
            for (uint32_t callee : calls[function]) sum += visit(callee);
            total[function] = sum;
            return sum;
        };
        return entry ? visit(entry) : -1;
    }

    uint64_t chunkProperties() {
        // what a chunk draws with: lit, normals, no UVs, no vertex colours
        return ShaderPool::getMeshProperties(false, false, false, true, false, true, false, false, false, false, false,
                                             false, false, false, false, false, false, false, false, false);
    }

    int compileSamples(const std::vector<std::string>& defines, ShaderBackend backend, std::string& error) {
        const std::string reference = defines.empty() ? std::string() : ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, defines);
        const uint16_t id = reference.empty() ? 0 : ShaderPool::registerCustomShader(reference);
        const CustomShaderSources sources = reference.empty() ? CustomShaderSources{} : ShaderVariantCompiler::resolveCustomShaderSources(reference, {}, ShaderType::MESH);
        ShaderData data;
        std::string log;
        if (!ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, chunkProperties(), id), backend, reference.empty() ? nullptr : &sources, data, &error, &log)) {
            error += "\n" + log;
            return -1;
        }
        for (const ShaderStage& stage : data.stages) {
            if (stage.type != ShaderStageType::FRAGMENT || backend != ShaderBackend::Vulkan || !stage.bytecode.data) continue;
            std::vector<uint32_t> words(stage.bytecode.size / sizeof(uint32_t));
            std::memcpy(words.data(), stage.bytecode.data, words.size() * sizeof(uint32_t));
            return countSamples(words);
        }
        return 0;
    }

}

TEST(VoxelShader, FetchBudgetPerTier) {
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    std::string error;
    const int base = compileSamples({}, ShaderBackend::Vulkan, error);
    ASSERT_GE(base, 0) << error;
    const struct {
        const char* tier;
        int budget;
    } tiers[] = {{"VOXEL_TIER_HIGH", 32}, {"VOXEL_TIER_MEDIUM", 21}, {"VOXEL_TIER_LOW", 6}};
    int previous = 1000;
    for (const auto& tier : tiers) {
        const int total = compileSamples({"HAS_VOXEL", tier.tier}, ShaderBackend::Vulkan, error);
        ASSERT_GE(total, 0) << tier.tier << ": " << error;
        const int reads = total - base;
        std::printf("[bench] voxel %s: %d reads of u_voxelDetail, budget %d\n", tier.tier, reads, tier.budget);
        EXPECT_LE(reads, tier.budget) << tier.tier;
        EXPECT_GT(reads, 0) << tier.tier;
        EXPECT_LT(reads, previous);
        previous = reads;
    }
}

TEST(VoxelShader, GBufferVariantCompiles) {
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable());
    const uint64_t properties = ShaderPool::getGBufferMeshProperties(false, true, false, false, false, false, false, false, false) | ShaderPool::getGBufferVoxelProperty();
    ShaderData data;
    std::string error, log;
    EXPECT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::GBUFFER, properties, 0), ShaderBackend::Vulkan, nullptr, data, &error, &log)) << error << log;
}

TEST(ShaderKey, VoxelBitIsPinned) {
    // Appended at landing, and pinned once there (T3 D2, R83)
    EXPECT_EQ(ShaderPool::getGBufferVoxelProperty(), static_cast<uint64_t>(1) << 23);
    EXPECT_EQ(MaterialPbr::getDefineBit(MaterialPbr::VOXEL_DEFINE), static_cast<uint64_t>(1) << 20);
    EXPECT_EQ(MaterialPbr::getDefineBit(MaterialPbr::VOXEL_TIER_HIGH_DEFINE), static_cast<uint64_t>(1) << 21);
    EXPECT_EQ(MaterialPbr::getDefineBit(MaterialPbr::VOXEL_TIER_MEDIUM_DEFINE), static_cast<uint64_t>(1) << 22);
    EXPECT_EQ(MaterialPbr::getDefineBit(MaterialPbr::VOXEL_TIER_LOW_DEFINE), static_cast<uint64_t>(1) << 23);
}
