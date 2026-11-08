// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The uniform budget of the built-in Lit's painted Terrain ('Tpb'; docs/superpowers/specs/
// 2026-11-04-water-reflections-design.md, Follow-ups): its variants, compiled for GLES3, stay within
// the 8 uniform blocks and 16 textures a stage every backend takes (checkResourceBudget) and the 224
// fragment and 256 vertex uniform vectors GLES3 and WebGL2 guarantee, which sokol's GL backends add
// the stage's blocks up against. Its nine layers' block takes 45 vectors, so its lights read their
// records from u_lightRecords (includes/lights.glsl) as library:water's do.

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

namespace terrain_uniform_budget_test {

    size_t terrainStageVectors(const ShaderData& data, ShaderStageType type, std::string& blocks){
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

using namespace terrain_uniform_budget_test;

// the Lit matrix's painted terrains, and the painted terrain at each light capacity in the scene
// with everything that adds to its fragment uniforms: shadows with the High filters, the sky's and
// a probe's reflections, SSAO, the post colour, with and without fog (the height fog's block); and
// its material normal-mapped (16 fragment textures and 16 samplers before its lights' records),
// with point and spot lights possible or directional only, at the High and the Medium tier, with
// S57's capsule shadows and 2D lights with their shadows or without
TEST(TerrainUniformBudgetSlow, PaintedTerrainFitsAtEveryLightCapacity){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    std::vector<ShaderLibraryMatrix::Variant> variants;
    for (const ShaderLibraryMatrix::Variant& variant : ShaderLibraryMatrix::buildBuiltinLitVariants()){
        if (variant.properties & shaderPropertyBit(27))
            variants.push_back(variant);
    }
    ASSERT_FALSE(variants.empty()) << "the Lit matrix lost its painted terrain";

    SubmeshShaderInputs terrain;
    terrain.terrain = true;
    terrain.terrainBlendMaps = true;
    terrain.terrainSurface = true;
    terrain.normal = true;
    terrain.baseColorTexture = true;
    for (int capacity : {4, 8, 16}){
        for (bool fog : {false, true}){
            MeshShaderEnvironment environment;
            environment.lights = true;
            environment.shadows = true;
            environment.shadowsHQ = true;
            environment.ibl = true;
            environment.reflectionProbes = true;
            environment.ssao = true;
            environment.postColor = true;
            environment.fog = fog;
            environment.lightCapacity = capacity;
            const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, terrain);
            ASSERT_TRUE(variant.properties & shaderPropertyBit(27)) << "not the painted terrain";
            // (no Lit define: the engine's own variant, no reference)
            variants.push_back({std::to_string(capacity) + " lights" + (fog ? ", fog" : ""),
                                variant.litDefines.empty() ? std::string() : ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, variant.litDefines),
                                ShaderType::MESH, variant.properties});
        }
    }

    SubmeshShaderInputs normalMapped = terrain;
    normalMapped.texCoord1 = true;
    normalMapped.tangent = true;
    normalMapped.normalTexture = true;
    for (int capacity : {8, 16}){
        for (bool high : {true, false}){
            for (int extra = 0; extra < 3; extra++){
                const bool directionalOnly = extra == 1;
                const bool heavy = extra == 2;
                MeshShaderEnvironment environment;
                environment.lights = true;
                environment.shadows = true;
                environment.shadowsHQ = high;
                environment.ibl = true;
                environment.reflectionProbes = true;
                environment.ssao = true;
                environment.postColor = true;
                environment.fog = true;
                environment.directionalLightsOnly = directionalOnly;
                environment.lightCapacity = capacity;
                environment.capsuleShadows = heavy;
                environment.contactShadows = heavy;
                environment.lights2D = heavy;
                environment.shadows2D = heavy;
                const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, normalMapped);
                ASSERT_TRUE(variant.properties & shaderPropertyBit(27)) << "not the painted terrain";
                variants.push_back({"normal-mapped, " + std::to_string(capacity) + " lights, fog" + (high ? ", high filters" : "") +
                                    (directionalOnly ? ", directional only" : "") + (heavy ? ", capsule shadows, 2d lights and shadows" : ""),
                                    variant.litDefines.empty() ? std::string() : ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, variant.litDefines),
                                    ShaderType::MESH, variant.properties});
            }
        }
    }

    size_t worst = 0;
    std::string worstName;
    size_t shadows2DLeftOut = 0;
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
        const size_t fragment = terrainStageVectors(data, ShaderStageType::FRAGMENT, fragmentBlocks);
        const size_t vertex = terrainStageVectors(data, ShaderStageType::VERTEX, vertexBlocks);
        // (16 samplers across the stages, 16 fragment textures: checkResourceBudget)
        EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
        // the spot lights' masks left out only where the records would not fit otherwise, and the
        // log says so
        // the 2D lights' shadows left out (the last fallback), said by the build and at runtime
        if (log.find(ShaderVariantCompiler::SHADOWS_2D_LEFT_OUT) != std::string::npos){
            shadows2DLeftOut++;
            EXPECT_EQ(data.getTextureIndexByName("u_shadow2DAtlas").first, -1) << name;
            EXPECT_NE(RenderSystem::describeLeftOutLighting(variant.properties, data), "") << name;
        }else{
            EXPECT_EQ(RenderSystem::describeLeftOutLighting(variant.properties, data), "") << name;
        }
        if ((variant.properties & shaderPropertyBit(3)) && data.getTextureIndex(TextureShaderType::SPOTMASKATLAS).first == -1 &&
                !(variant.properties & ShaderPool::getDirectionalOnlyProperty())){
            EXPECT_NE(log.find(ShaderVariantCompiler::SPOT_MASKS_LEFT_OUT), std::string::npos) << name << ": " << log;
        }
        EXPECT_LE(fragment, 224u) << name << "; fragment blocks: " << fragmentBlocks;
        EXPECT_LE(vertex, 256u) << name << "; vertex blocks: " << vertexBlocks;
        if (fragment > worst){
            worst = fragment;
            worstName = name;
        }
    }
    // the normal-mapped painted terrain lit by shadowed 2D lights reaches the 2D shadows' fallback
    EXPECT_GT(shadows2DLeftOut, 0u) << "no variant left the 2D shadows out: the fallback has no case";
    std::printf("[terrain] %zu painted terrain variants; the most fragment uniform vectors: %zu of 224, %s\n",
                variants.size(), worst, worstName.c_str());
}
