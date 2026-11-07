// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// PCSS's raw-depth view of the shadow atlas is one more texture and sampler in every 'Shq' variant
// (draw-perf spec, "Follow-ups (2026-11-07)"). On every backend an export targets, the heaviest
// built-in Lit variants stay within what every device binds: 16 samplers across both stages
// (sokol's SG_MAX_SAMPLER_BINDSLOTS, whose slots the stages share) and 16 fragment textures
// (GLES3, WebGL2, macOS GL), the view left out where it would go past them. The editor's shader
// compiler has every language (a game's has its own platform's only).

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // a lit mesh or the painted terrain, textured and normal-mapped, under shadowing lights at a
    // High-filter tier with what takes textures (IBL, SSAO, the post-processing colour) on
    uint64_t heaviest(bool terrain, bool directionalOnly){
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.tangent = true;
        inputs.baseColorTexture = true;
        inputs.normalTexture = true;
        inputs.terrain = terrain;
        inputs.terrainBlendMaps = terrain;
        inputs.terrainSurface = terrain;
        MeshShaderEnvironment environment;
        environment.lights = true;
        environment.shadows = true;
        environment.shadowsHQ = true;
        environment.ibl = true;
        environment.ssao = true;
        environment.postColor = true;
        environment.fog = true;
        environment.directionalLightsOnly = directionalOnly;
        return RenderSystem::getSubmeshShaderVariant(environment, inputs).properties;
    }

}

TEST(ShadowPcssVariants, StayWithinTheBindLimitsOnEveryBackend){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    // three extension maps, the most a Lit variant binds
    const std::string maps = ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT,
        {"PBR_CLEARCOAT", "PBR_CLEARCOAT_MAP", "PBR_SHEEN", "PBR_SHEEN_MAP", "PBR_SPECULAR", "PBR_SPECULAR_MAP"});
    const uint16_t mapsId = ShaderPool::registerCustomShader(maps);
    const CustomShaderSources mapsSources = ShaderVariantCompiler::resolveCustomShaderSources(maps, nullptr);
    ASSERT_TRUE(mapsSources.valid) << mapsSources.error;

    struct Variant { uint64_t properties; bool extensionMaps; };
    const std::vector<Variant> variants = {
        {heaviest(false, false), false}, {heaviest(false, true), false},
        {heaviest(false, false), true}, {heaviest(false, true), true},
        {heaviest(true, false), false}, {heaviest(true, true), false},
    };
    for (ShaderBackend backend : ShaderPool::getShaderBackends()){
        for (const Variant& variant : variants){
            const uint16_t customId = variant.extensionMaps ? mapsId : 0;
            const std::string name = ShaderPool::getShaderStr(ShaderType::MESH, variant.properties, customId) + " (" +
                                     ShaderPool::getShaderLangStr(backend) + ")";
            ShaderData data;
            std::string error, log;
            ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, variant.properties, customId), backend,
                                                       variant.extensionMaps ? &mapsSources : nullptr, data, &error, &log))
                << name << ": " << error << "\n" << log;
            size_t samplers = 0, fragmentTextures = 0;
            for (const ShaderStage& stage : data.stages){
                samplers += stage.samplers.size();
                if (stage.type == ShaderStageType::FRAGMENT)
                    fragmentTextures = stage.textures.size();
            }
            EXPECT_LE(samplers, 16u) << name << ": samplers across both stages";
            EXPECT_LE(fragmentTextures, 16u) << name << ": fragment textures";
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
            // the view is left out only where it would not fit, and the log says so
            const bool raw = data.getTextureIndex(TextureShaderType::SHADOWATLASRAW).first >= 0;
            EXPECT_EQ(!raw, log.find(ShaderVariantCompiler::PCSS_LEFT_OUT) != std::string::npos) << name << ": " << log;
        }
    }
}
