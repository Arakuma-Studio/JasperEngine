// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The uniform budget of the built-in Lit (docs/superpowers/specs/2026-11-04-water-reflections-design.md,
// Follow-ups): every variant of its compile matrix (ShaderLibraryMatrix::buildBuiltinLitVariants,
// as `shaders --library builtin_lit` builds them, with the heavy scenes: sixteen lights per object
// or from screen tiles, S57's capsule and contact shadows, 2D lights and their shadows, fog,
// reflection probes, SSAO and the post colour, on a mesh, with every lobe and on the painted
// terrain), compiled for GLES3, stays within
// - 8 uniform blocks a stage, 16 fragment textures and 16 samplers across the stages, on every
//   backend (ShaderVariantCompiler::checkResourceBudget);
// - the 224 fragment and 256 vertex uniform vectors GLES3 and WebGL2 guarantee, which sokol's GL
//   backends add the stage's blocks up against.

#include "shader/ShaderLibraryMatrix.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace lit_uniform_budget_test {

    size_t litStageVectors(const ShaderData& data, ShaderStageType type, std::string& blocks){
        size_t vectors = 0;
        for (const ShaderStage& stage : data.stages){
            if (stage.type != type)
                continue;
            for (const ShaderUniformBlock& block : stage.uniformblocks){
                vectors += (block.sizeBytes + 15) / 16;
                blocks += (blocks.empty() ? "" : ", ") + block.name + " " + std::to_string((block.sizeBytes + 15) / 16);
            }
        }
        return vectors;
    }

}

using namespace lit_uniform_budget_test;

TEST(LitUniformBudgetSlow, EveryMatrixVariantFits){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const std::vector<ShaderLibraryMatrix::Variant> variants = ShaderLibraryMatrix::buildBuiltinLitVariants();
    size_t heavy = 0;
    for (const ShaderLibraryMatrix::Variant& variant : variants)
        heavy += variant.label.find("heavy scene") != std::string::npos ? 1 : 0;
    ASSERT_GE(heavy, 20u) << "the matrix lost its heavy scenes";

    size_t worst = 0;
    std::string worstName;
    for (const ShaderLibraryMatrix::Variant& variant : variants){
        const bool custom = !variant.reference.empty();
        const uint16_t customId = custom ? ShaderPool::registerCustomShader(variant.reference) : 0;
        CustomShaderSources sources;
        if (custom)
            sources = ShaderVariantCompiler::resolveCustomShaderSources(variant.reference, nullptr, ShaderType::MESH);
        const std::string name = variant.label + " (" + ShaderPool::getShaderStr(ShaderType::MESH, variant.properties, customId) + ")";
        ShaderData data;
        std::string error, log;
        ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, variant.properties, customId), ShaderBackend::GLES3,
                                                   custom ? &sources : nullptr, data, &error, &log)) << name << ": " << error << "\n" << log;
        std::string fragmentBlocks, vertexBlocks;
        const size_t fragment = litStageVectors(data, ShaderStageType::FRAGMENT, fragmentBlocks);
        const size_t vertex = litStageVectors(data, ShaderStageType::VERTEX, vertexBlocks);
        EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name << "; fragment blocks: " << fragmentBlocks;
        EXPECT_LE(fragment, 224u) << name << "; fragment blocks: " << fragmentBlocks;
        EXPECT_LE(vertex, 256u) << name << "; vertex blocks: " << vertexBlocks;
        if (fragment > worst){
            worst = fragment;
            worstName = name;
        }
    }
    std::printf("[lit] %zu matrix variants (%zu in heavy scenes); the most fragment uniform vectors: %zu of 224, %s\n",
                variants.size(), heavy, worst, worstName.c_str());
}
