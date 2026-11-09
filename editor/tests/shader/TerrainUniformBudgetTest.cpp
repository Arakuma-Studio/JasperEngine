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

// T3's painted terrain variants (12 layers, holes, the macro maps, distance resampling, cliffs,
// height blend, stochastic tiling, every tier at High Lit Quality) in the heaviest scene above stay
// within GLES3's limits as upstream's lit-uniforms work requires of every built-in terrain variant:
// 8 blocks, 16 textures and samplers (checkResourceBudget), 224 fragment and 256 vertex vectors.
// T4's Terrain World ('Twd') widens u_vs_terrainParams by its page, page size and tile: each
// variant as a World too, its depth program with it
TEST(TerrainUniformBudgetSlow, TerrainProgrammeVariantsFitGles3){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    SubmeshShaderInputs terrain;
    terrain.terrain = true;
    terrain.terrainBlendMaps = true;
    terrain.terrainSurface = true;
    terrain.normal = true;
    terrain.baseColorTexture = true;
    terrain.terrainVariant.layerCount = 12;

    size_t compiled = 0;
    size_t worst = 0;
    std::string worstName;
    for (jasperengine::terrain::TerrainQuality tier : {jasperengine::terrain::TerrainQuality::HIGH, jasperengine::terrain::TerrainQuality::MEDIUM,
                                                      jasperengine::terrain::TerrainQuality::LOW}){
        for (bool holes : {false, true}){
            for (bool features : {false, true}){
                for (int both = 0; both < 4; both++){
                    const bool heavy = (both & 1) != 0;
                    const bool world = (both & 2) != 0;
                    SubmeshShaderInputs inputs = terrain;
                    inputs.terrainHoles = holes;
                    inputs.terrainWorld = world;
                    inputs.terrainVariant.limitLayers = features;
                    inputs.terrainVariant.macro = features;
                    inputs.terrainVariant.macroNormal = features;
                    inputs.terrainVariant.resample = features;
                    inputs.terrainVariant.cliff = features;
                    inputs.terrainVariant.heightBlend = features;
                    inputs.terrainHex = features;
                    inputs.terrainHexStochastic = features;
                    inputs.terrainVariant.hex = features;
                    inputs.terrainVariant.hexStochastic = features;
                    MeshShaderEnvironment environment;
                    environment.lights = true;
                    environment.shadows = true;
                    environment.shadowsHQ = true;
                    environment.ibl = true;
                    environment.reflectionProbes = true;
                    environment.ssao = true;
                    environment.postColor = true;
                    environment.fog = true;
                    environment.litQualityHigh = true;
                    environment.lightCapacity = 16;
                    environment.terrainTier = tier;
                    environment.capsuleShadows = heavy;
                    environment.contactShadows = heavy;
                    environment.lights2D = heavy;
                    environment.shadows2D = heavy;
                    const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
                    const bool custom = !variant.litDefines.empty();
                    const std::string reference = custom ? ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, variant.litDefines) : std::string();
                    const uint16_t customId = custom ? ShaderPool::registerCustomShader(reference) : 0;
                    CustomShaderSources sources;
                    if (custom)
                        sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, nullptr, ShaderType::MESH);
                    const std::string name = std::string(jasperengine::terrain::terrainQualityName(tier)) + (holes ? ", holes" : "") +
                                             (features ? ", every feature" : "") + (heavy ? ", capsule shadows, 2d lights and shadows" : "") +
                                             (world ? ", Terrain World" : "") + " (" + ShaderPool::getShaderStr(ShaderType::MESH, variant.properties, customId) + ")";
                    ShaderData data;
                    std::string error, log;
                    ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, variant.properties, customId), ShaderBackend::GLES3,
                                                               custom ? &sources : nullptr, data, &error, &log)) << name << ": " << error << "\n" << log;
                    compiled++;
                    std::string fragmentBlocks, vertexBlocks;
                    const size_t fragment = terrainStageVectors(data, ShaderStageType::FRAGMENT, fragmentBlocks);
                    const size_t vertex = terrainStageVectors(data, ShaderStageType::VERTEX, vertexBlocks);
                    EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
                    EXPECT_LE(fragment, 224u) << name << "; fragment blocks: " << fragmentBlocks;
                    EXPECT_LE(vertex, 256u) << name << "; vertex blocks: " << vertexBlocks;
                    if (fragment > worst){
                        worst = fragment;
                        worstName = name;
                    }
                    if (world){
                        ASSERT_NE(variant.properties & ShaderPool::getTerrainWorldProperty(ShaderType::MESH), 0u) << name;
                        ASSERT_NE(variant.depthProperties & ShaderPool::getTerrainWorldProperty(ShaderType::DEPTH), 0u) << name;
                        const std::string depthName = name + ", depth (" + ShaderPool::getShaderStr(ShaderType::DEPTH, variant.depthProperties) + ")";
                        ShaderData depth;
                        ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::DEPTH, variant.depthProperties), ShaderBackend::GLES3,
                                                                   nullptr, depth, &error, &log)) << depthName << ": " << error << "\n" << log;
                        compiled++;
                        std::string depthFragmentBlocks, depthVertexBlocks;
                        EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(depth), "") << depthName;
                        EXPECT_LE(terrainStageVectors(depth, ShaderStageType::FRAGMENT, depthFragmentBlocks), 224u) << depthName << "; fragment blocks: " << depthFragmentBlocks;
                        EXPECT_LE(terrainStageVectors(depth, ShaderStageType::VERTEX, depthVertexBlocks), 256u) << depthName << "; vertex blocks: " << depthVertexBlocks;
                    }
                }
            }
        }
    }
    // 24 terrain variants, each a World too, with its depth program
    EXPECT_EQ(compiled, 72u);
    std::printf("[terrain] %zu terrain programme variants; the most fragment uniform vectors: %zu of 224, %s\n",
                compiled, worst, worstName.c_str());
}

// the packed layer block (includes/terrain_fs.glsl: 12-bit pairs, the layer codes read by a mask)
// through the bundled SPIRV-Cross for every backend, at each tier with every feature; Metal and
// D3D11 are translated here, not compiled by a driver
TEST(TerrainUniformBudgetSlow, PackedLayersCrossCompileForEveryBackend){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    SubmeshShaderInputs inputs;
    inputs.terrain = true;
    inputs.terrainBlendMaps = true;
    inputs.terrainSurface = true;
    inputs.normal = true;
    inputs.baseColorTexture = true;
    inputs.terrainHoles = true;
    inputs.terrainHex = true;
    inputs.terrainHexStochastic = true;
    inputs.terrainVariant.hex = true;
    inputs.terrainVariant.hexStochastic = true;
    inputs.terrainVariant.layerCount = 12;
    inputs.terrainVariant.limitLayers = true;
    inputs.terrainVariant.macro = true;
    inputs.terrainVariant.macroNormal = true;
    inputs.terrainVariant.resample = true;
    inputs.terrainVariant.cliff = true;
    inputs.terrainVariant.heightBlend = true;
    const ShaderBackend backends[] = {ShaderBackend::GLCore, ShaderBackend::GLES3, ShaderBackend::D3D11, ShaderBackend::MetalMacOS,
                                      ShaderBackend::MetalIOS, ShaderBackend::Vulkan};
    for (jasperengine::terrain::TerrainQuality tier : {jasperengine::terrain::TerrainQuality::HIGH, jasperengine::terrain::TerrainQuality::MEDIUM,
                                                      jasperengine::terrain::TerrainQuality::LOW}){
        MeshShaderEnvironment environment;
        environment.lights = true;
        environment.litQualityHigh = true;
        environment.terrainTier = tier;
        const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
        ASSERT_FALSE(variant.litDefines.empty()) << "the terrain's features are Lit defines";
        const std::string reference = ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, variant.litDefines);
        const uint16_t customId = ShaderPool::registerCustomShader(reference);
        const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, nullptr, ShaderType::MESH);
        const std::string name = ShaderPool::getShaderStr(ShaderType::MESH, variant.properties, customId);
        for (const ShaderBackend backend : backends){
            ShaderData data;
            std::string error, log;
            EXPECT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, variant.properties, customId), backend, &sources, data, &error, &log))
                << name << " on backend " << static_cast<int>(backend) << ": " << error << "\n" << log;
        }
    }
}
