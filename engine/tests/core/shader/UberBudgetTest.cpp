// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The S13 uber variants' static budgets (AC12): the SPIR-V of each variant samples no more than
// the spec's Performance table allows beyond the base variant, within 8 samplers and 24 vec4 of
// parameters (every backend's compile, AC11, is editor/tests/shader/UberVariantsTest.cpp: a
// game's runtime compiler has only its own platform's languages). Samples are counted per fragment invocation along the call graph (glslang runs
// without an optimizer, so helpers stay functions: a helper's samples count once per call).

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // SPIR-V opcodes (the unified specification, section 3.49)
    constexpr uint32_t OP_ENTRY_POINT = 15;
    constexpr uint32_t OP_FUNCTION = 54;
    constexpr uint32_t OP_FUNCTION_END = 56;
    constexpr uint32_t OP_FUNCTION_CALL = 57;
    // OpImageSampleImplicitLod .. OpImageDrefGather: every instruction that reads a texture
    constexpr uint32_t OP_IMAGE_SAMPLE_FIRST = 87;
    constexpr uint32_t OP_IMAGE_SAMPLE_LAST = 97;

    // the texture reads of one invocation of the entry point: each function's own reads plus,
    // for every call it makes, the callee's (loops count once: the uber pass samples in none)
    int countSamples(const std::vector<uint32_t>& words){
        std::map<uint32_t, int> own;
        std::map<uint32_t, std::vector<uint32_t>> calls;
        uint32_t entry = 0;
        uint32_t current = 0;
        for (size_t offset = 5; offset < words.size();){
            const uint32_t opcode = words[offset] & 0xffffu;
            const uint32_t count = words[offset] >> 16;
            if (count == 0 || offset + count > words.size())
                return -1;
            if (opcode == OP_ENTRY_POINT && count > 2)
                entry = words[offset + 2];
            else if (opcode == OP_FUNCTION && count > 2)
                current = words[offset + 2];
            else if (opcode == OP_FUNCTION_END)
                current = 0;
            else if (opcode == OP_FUNCTION_CALL && count > 3 && current != 0)
                calls[current].push_back(words[offset + 3]);
            else if (opcode >= OP_IMAGE_SAMPLE_FIRST && opcode <= OP_IMAGE_SAMPLE_LAST && current != 0)
                own[current]++;
            offset += count;
        }
        std::map<uint32_t, int> total;
        // the call graph is acyclic (GLSL has no recursion)
        std::function<int(uint32_t)> visit = [&](uint32_t function) -> int {
            auto found = total.find(function);
            if (found != total.end())
                return found->second;
            int sum = own[function];
            for (uint32_t callee : calls[function])
                sum += visit(callee);
            total[function] = sum;
            return sum;
        };
        return entry ? visit(entry) : -1;
    }

    struct VariantBudget{
        int samples = -1;
        size_t textures = 0;
        size_t samplers = 0;
        unsigned int paramBytes = 0;
    };

    VariantBudget compileUber(uint32_t features, ShaderBackend backend, std::string& error){
        ShaderData data;
        std::string log;
        VariantBudget budget;
        if (!ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::BLIT, features), backend, nullptr, data, &error, &log)){
            error += "\n" + log;
            return budget;
        }
        for (const ShaderStage& stage : data.stages){
            if (stage.type != ShaderStageType::FRAGMENT)
                continue;
            budget.textures = stage.textures.size();
            budget.samplers = stage.samplers.size();
            for (const ShaderUniformBlock& block : stage.uniformblocks)
                budget.paramBytes = std::max(budget.paramBytes, block.sizeBytes);
            if (backend == ShaderBackend::Vulkan && stage.bytecode.data && stage.bytecode.size >= 20){
                std::vector<uint32_t> words(stage.bytecode.size / sizeof(uint32_t));
                std::memcpy(words.data(), stage.bytecode.data, words.size() * sizeof(uint32_t));
                budget.samples = countSamples(words);
            }
        }
        return budget;
    }

    // the S13 bits, each with the bits it needs (the grain types come with UBER_GRAIN)
    struct S13Variant{
        const char* name;
        uint32_t features;
        int extraSamples;   // the Performance table's extra uber samples beyond the base tap
    };
    const S13Variant S13_VARIANTS[] = {
        {"VHS", UBER_VHS, 8},                          // <= 9 scene taps in total, replacing the base tap
        {"Halation", UBER_HALATION, 2},
        {"Film Damage", UBER_FILM_DAMAGE, 0},
        {"Grain Lens", UBER_GRAIN | UBER_GRAIN_LENS, 0},
        {"Grain Filmic", UBER_GRAIN | UBER_GRAIN_FILMIC, 0},
    };

    uint32_t allBits(){
        return (1u << UBER_FEATURE_COUNT) - 1u;
    }

}

TEST(UberBudget, SampleCounts){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    std::string error;
    // the base: the plain upscale (one scene tap); grain alone samples nothing more
    const VariantBudget base = compileUber(UBER_UPSCALE, ShaderBackend::Vulkan, error);
    ASSERT_GE(base.samples, 1) << error;
    const VariantBudget grain = compileUber(UBER_GRAIN, ShaderBackend::Vulkan, error);
    ASSERT_GE(grain.samples, 1) << error;

    for (const S13Variant& variant : S13_VARIANTS){
        const VariantBudget budget = compileUber(variant.features, ShaderBackend::Vulkan, error);
        ASSERT_GE(budget.samples, 0) << variant.name << ": " << error;
        const VariantBudget& reference = (variant.features & UBER_GRAIN) ? grain : base;
        std::printf("[bench] uber %s: %d texture reads (base %d), %zu samplers\n", variant.name, budget.samples, reference.samples,
                    budget.samplers);
        EXPECT_LE(budget.samples - reference.samples, variant.extraSamples) << variant.name;
        EXPECT_LE(budget.samplers, 8u) << variant.name;
    }

    // every bit on (combinations the tiers never make included): within 8 samplers (scene, LUT,
    // bloom, blur, halation x2, Palette Quantize's one for its two tables) and 24 vec4 of
    // parameters; 8 textures with Palette Quantize's cells and row (S43)
    const VariantBudget all = compileUber(allBits(), ShaderBackend::Vulkan, error);
    ASSERT_GE(all.samples, 0) << error;
    std::printf("[bench] uber all bits: %d texture reads, %zu textures, %zu samplers, %u parameter bytes\n", all.samples, all.textures,
                all.samplers, all.paramBytes);
    EXPECT_LE(all.textures, 8u);
    EXPECT_LE(all.samplers, 8u);
    EXPECT_EQ(all.textures, 8u);
    // Palette Quantize reads its cell and its colour: 2 texture reads more than the upscale (R30)
    const VariantBudget palette = compileUber(UBER_PALETTE, ShaderBackend::Vulkan, error);
    ASSERT_GE(palette.samples, 0) << error;
    EXPECT_LE(palette.samples - base.samples, 2);
    EXPECT_LE(all.paramBytes, 24u * 16u);
}

// S43 AC12 (e): the anti-banding dither is not compiled with Palette Quantize: the fragment
// program with UBER_DITHER on as well is the same bytes
TEST(UberBudget, PaletteQuantizeCompilesNoDither){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    auto fragmentCode = [](uint32_t features, std::string& error){
        ShaderData data;
        std::string log;
        std::vector<uint8_t> code;
        if (!ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::BLIT, features), ShaderBackend::Vulkan, nullptr, data, &error, &log))
            return code;
        for (const ShaderStage& stage : data.stages){
            if (stage.type == ShaderStageType::FRAGMENT && stage.bytecode.data)
                code.assign(static_cast<const uint8_t*>(stage.bytecode.data), static_cast<const uint8_t*>(stage.bytecode.data) + stage.bytecode.size);
        }
        return code;
    };
    std::string error;
    const std::vector<uint8_t> palette = fragmentCode(UBER_PALETTE, error);
    ASSERT_FALSE(palette.empty()) << error;
    const std::vector<uint8_t> withDither = fragmentCode(UBER_PALETTE | UBER_DITHER, error);
    ASSERT_FALSE(withDither.empty()) << error;
    EXPECT_EQ(palette, withDither);
    // (while the dither alone does change the program)
    EXPECT_NE(fragmentCode(UBER_DITHER, error), fragmentCode(UBER_UPSCALE, error));
}
