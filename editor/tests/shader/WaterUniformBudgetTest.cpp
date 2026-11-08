// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The uniform budget of library:water (docs/superpowers/specs/2026-11-04-water-reflections-design.md,
// Follow-ups): every variant it can draw with, compiled for GLES3, stays within
// - the 8 uniform blocks a stage every backend takes (ShaderVariantCompiler::checkResourceBudget);
// - the 224 fragment and 256 vertex uniform vectors GLES3 and WebGL2 guarantee
//   (GL_MAX_FRAGMENT_UNIFORM_VECTORS, GL_MAX_VERTEX_UNIFORM_VECTORS): sokol's GL backends upload
//   each block as plain uniforms, so a stage's blocks add up against them.
// The variants are the library's compile matrix (ShaderLibraryMatrix, as `shaders --library water`
// builds them) and, at each light capacity, the scene with everything that adds a block or vectors.

#include "shader/ShaderLibraryMatrix.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace water_uniform_budget_test {

    constexpr size_t FRAGMENT_VECTORS = 224;
    constexpr size_t VERTEX_VECTORS = 256;

    size_t stageVectors(const ShaderData& data, ShaderStageType type){
        size_t vectors = 0;
        for (const ShaderStage& stage : data.stages){
            if (stage.type == type){
                for (const ShaderUniformBlock& block : stage.uniformblocks)
                    vectors += (block.sizeBytes + 15) / 16;
            }
        }
        return vectors;
    }

    // the stage's blocks with their vectors, for the failure message
    std::string stageBlocks(const ShaderData& data, ShaderStageType type){
        std::string list;
        for (const ShaderStage& stage : data.stages){
            if (stage.type != type)
                continue;
            for (const ShaderUniformBlock& block : stage.uniformblocks)
                list += (list.empty() ? "" : ", ") + block.name + " " + std::to_string((block.sizeBytes + 15) / 16);
        }
        return list;
    }

    struct Budget {
        size_t compiled = 0;
        size_t worstFragment = 0;
        std::string worstName;
    };

    // compiles one variant for GLES3 as the library's compile matrix does
    // (ShaderLibraryMatrix::compileVariants) and checks it against both limits
    void checkVariant(const std::string& label, const std::string& reference, ShaderType type, uint64_t properties, Budget& budget){
        const uint16_t customId = ShaderPool::registerCustomShader(reference);
        const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, nullptr, type);
        const std::string name = label + " (" + ShaderPool::getShaderStr(type, properties, customId) + ")";
        ShaderData data;
        std::string error, log;
        ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(type, properties, customId), ShaderBackend::GLES3,
                                                   &sources, data, &error, &log)) << name << ": " << error << "\n" << log;
        budget.compiled++;
        EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name << "; fragment blocks: " << stageBlocks(data, ShaderStageType::FRAGMENT);
        const size_t fragment = stageVectors(data, ShaderStageType::FRAGMENT);
        const size_t vertex = stageVectors(data, ShaderStageType::VERTEX);
        EXPECT_LE(fragment, FRAGMENT_VECTORS) << name << "; fragment blocks: " << stageBlocks(data, ShaderStageType::FRAGMENT);
        EXPECT_LE(vertex, VERTEX_VECTORS) << name << "; vertex blocks: " << stageBlocks(data, ShaderStageType::VERTEX);
        if (fragment > budget.worstFragment){
            budget.worstFragment = fragment;
            budget.worstName = name;
        }
    }

}

using namespace water_uniform_budget_test;

TEST(WaterUniformBudgetSlow, EveryMatrixVariantFits){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get("library:water");
    ASSERT_TRUE(manifest);
    const std::vector<ShaderLibraryMatrix::Variant> variants = ShaderLibraryMatrix::buildVariants("water", *manifest);
    ASSERT_GT(variants.size(), 100u) << "the matrix lost its variants";
    Budget budget;
    for (const ShaderLibraryMatrix::Variant& variant : variants)
        checkVariant(variant.label, variant.reference, variant.type, variant.properties, budget);
    EXPECT_EQ(budget.compiled, variants.size());
    std::printf("[water] %zu matrix variants; the most fragment uniform vectors: %zu of %zu, %s\n",
                budget.compiled, budget.worstFragment, FRAGMENT_VECTORS, budget.worstName.c_str());
}

// lit per pixel with every feature, at each light capacity, from the object's lights and from
// screen tiles, in the scene with every option that adds to the fragment stage's uniforms: the
// shadows with the High filters, S57's capsule and contact shadows, fog (the height fog's block),
// the post colour, the reflection environment, 2D lights and their shadows, Water Excluders and
// Wakes; on a mesh, a lattice-bound mesh and a terrain
TEST(WaterUniformBudgetSlow, WorstScenesFitAtEveryLightCapacity){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    SubmeshShaderInputs inputs;
    inputs.normal = true;
    inputs.tangent = true;
    inputs.texCoord1 = true;
    inputs.vertexColor4 = true;
    inputs.builtinLit = false;
    inputs.manifest = ShaderManifests::get("library:water");
    ASSERT_TRUE(inputs.manifest);
    inputs.lighting = MaterialLighting::PIXEL;
    std::vector<std::string> all;
    for (const ShaderManifestFeature& feature : inputs.manifest->features)
        all.push_back(feature.define);
    inputs.features = inputs.manifest->resolveFeatures(all);
    const std::string reference = ShaderReference::withFeatures("library:water", inputs.features);

    MeshShaderEnvironment worst;
    worst.lights = true;
    worst.shadows = true;
    worst.shadowsHQ = true;
    worst.capsuleShadows = true;
    worst.contactShadows = true;
    worst.ibl = true;
    worst.reflectionProbes = true;
    worst.fog = true;
    worst.postColor = true;
    worst.lights2D = true;
    worst.shadows2D = true;
    worst.sceneDepth = true;
    worst.waterExclusion = true;
    worst.waterWake = true;

    SubmeshShaderInputs lattice = inputs;
    lattice.lattice = true;
    SubmeshShaderInputs terrain = inputs;
    terrain.terrain = true;
    terrain.texCoord1 = false;
    terrain.tangent = false;

    Budget budget;
    for (int capacity : {4, 8, 16}){
        for (bool tiled : {false, true}){
            MeshShaderEnvironment environment = worst;
            environment.lightCapacity = capacity;
            environment.tiledLights = tiled;
            const std::string label = std::to_string(capacity) + " lights" + (tiled ? ", tiled" : "");
            for (const auto& mesh : {std::make_pair("mesh", &inputs), std::make_pair("lattice", &lattice), std::make_pair("terrain", &terrain)}){
                const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, *mesh.second);
                ASSERT_TRUE(variant.properties & ShaderPool::getWaterExclusionProperty()) << label;
                checkVariant(label + ", " + mesh.first, reference, ShaderType::MESH, variant.properties, budget);
            }
        }
    }
    std::printf("[water] worst scenes; the most fragment uniform vectors: %zu of %zu, %s\n",
                budget.worstFragment, FRAGMENT_VECTORS, budget.worstName.c_str());
}
