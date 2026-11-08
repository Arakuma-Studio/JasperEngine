// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// PCSS on GLES3/WebGL2 (draw-perf spec, "Follow-ups (2026-11-07)"): the raw-depth view of the
// shadow atlas PCSS reads is one more fragment texture and sampler, of the 16 fragment units GLES3
// guarantees and the 16 samplers sokol has for both stages. The directional-only Lit ('Dlo', two
// textures fewer) always has room for it; a general variant (the painted terrain's too) has it when
// its textures and samplers leave one free, and is built without it otherwise on every backend
// (ShaderVariantCompiler::compile), within the limits either way. Each case
// prints what the GLES3 fragment stage costs ([pcss-gles] lines: textures, uniform vectors,
// lookups). Every backend's budget: DirectionalOnlyVariants (the editor's tests, which have them all).

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // GLES3's minimum fragment uniform vectors (GL_MAX_FRAGMENT_UNIFORM_VECTORS)
    constexpr size_t GLES3_FRAGMENT_UNIFORM_VECTORS = 224;

    struct Compiled {
        ShaderData data;
        std::string log;
        std::string name;
    };

    // a textured, normal-mapped Lit mesh under a shadowing sun at a High-filter tier, with what
    // takes fragment textures (IBL, SSAO, the post-processing colour) on
    uint64_t heavyLit(bool directionalOnly){
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.tangent = true;
        inputs.baseColorTexture = true;
        inputs.normalTexture = true;
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

    // its material normal-mapped, the worst case (16 fragment textures and 16 samplers without PCSS)
    uint64_t paintedTerrain(bool directionalOnly){
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.tangent = true;
        inputs.baseColorTexture = true;
        inputs.normalTexture = true;
        inputs.terrain = true;
        inputs.terrainBlendMaps = true;
        inputs.terrainSurface = true;
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

    // three extension maps (the most a variant binds) in the "builtin:lit#..." reference
    const std::string& threeExtensionMaps(){
        static const std::string reference = ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT,
            {"PBR_CLEARCOAT", "PBR_CLEARCOAT_MAP", "PBR_SHEEN", "PBR_SHEEN_MAP", "PBR_SPECULAR", "PBR_SPECULAR_MAP"});
        return reference;
    }

    Compiled compile(uint64_t properties, ShaderBackend backend, const std::string& reference = std::string()){
        Compiled out;
        uint16_t customId = 0;
        CustomShaderSources sources;
        if (!reference.empty()){
            customId = ShaderPool::registerCustomShader(reference);
            sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, nullptr);
            EXPECT_TRUE(sources.valid) << sources.error;
        }
        out.name = ShaderPool::getShaderStr(ShaderType::MESH, properties, customId) + (reference.empty() ? "" : " " + reference) +
                   " (" + ShaderPool::getShaderLangStr(backend) + ")";
        std::string error;
        EXPECT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, properties, customId), backend,
                                                   reference.empty() ? nullptr : &sources, out.data, &error, &out.log))
            << out.name << ": " << error << "\n" << out.log;
        return out;
    }

    const ShaderStage* fragmentOf(const ShaderData& data){
        for (const ShaderStage& stage : data.stages){
            if (stage.type == ShaderStageType::FRAGMENT)
                return &stage;
        }
        return nullptr;
    }

    bool hasRawView(Compiled& compiled){
        return compiled.data.getTextureIndex(TextureShaderType::SHADOWATLASRAW).first >= 0;
    }

    size_t countOf(const std::string& text, const std::string& what){
        size_t count = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size()))
            count++;
        return count;
    }

    // what the variant's GLES3 fragment stage takes: its textures, samplers and (uniforms) uniform
    // vectors within GLES3's guarantees; prints them with its size and texture lookups
    void expectWithinGles3Limits(const Compiled& compiled, bool uniforms = true){
        EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(compiled.data), "") << compiled.name;
        const ShaderStage* fragment = fragmentOf(compiled.data);
        ASSERT_NE(fragment, nullptr) << compiled.name;
        size_t vectors = 0;
        for (const ShaderUniformBlock& block : fragment->uniformblocks)
            vectors += (block.sizeBytes + 15) / 16;
        if (uniforms){
            EXPECT_LE(vectors, GLES3_FRAGMENT_UNIFORM_VECTORS) << compiled.name;
        }
        size_t samplers = 0;
        for (const ShaderStage& stage : compiled.data.stages)
            samplers += stage.samplers.size();
        EXPECT_LE(samplers, 16u) << compiled.name << ": sokol's sampler slots are shared by the stages";
        std::printf("[pcss-gles] %s: %zu textures, %zu samplers (%zu with the vertex stage), %zu uniform vectors, %zu bytes, %zu texture( / %zu textureLod( call sites\n",
                    compiled.name.c_str(), fragment->textures.size(), fragment->samplers.size(), samplers, vectors, fragment->source.size(),
                    countOf(fragment->source, "texture("), countOf(fragment->source, "textureLod("));
    }

}

TEST(ShadowPcssGles, DirectionalOnlyVariantsReadRawDepthsWithinTheLimits){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    // the 'Dlo' Lit with three extension maps, and the normal-mapped painted terrain
    ASSERT_NE(heavyLit(true) & ShaderPool::getDirectionalOnlyProperty(), 0u);
    ASSERT_NE(paintedTerrain(true) & ShaderPool::getDirectionalOnlyProperty(), 0u);
    ASSERT_NE(heavyLit(true) & ShaderPool::getShadowsHQProperty(), 0u);
    std::vector<Compiled> variants;
    variants.push_back(compile(heavyLit(true), ShaderBackend::GLES3, threeExtensionMaps()));
    variants.push_back(compile(paintedTerrain(true), ShaderBackend::GLES3));
    for (Compiled& variant : variants){
        EXPECT_TRUE(hasRawView(variant)) << variant.name << " has no raw-depth view: no PCSS on GLES3";
        EXPECT_EQ(variant.log.find(ShaderVariantCompiler::PCSS_LEFT_OUT), std::string::npos) << variant.name;
        expectWithinGles3Limits(variant);
    }
}

TEST(ShadowPcssGles, GeneralVariantsTakeItWhereTheTexturesLeaveAUnit){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    // the same mesh with point and spot lights possible: the spot mask and point atlas leave room
    // without extension maps...
    Compiled plain = compile(heavyLit(false), ShaderBackend::GLES3);
    EXPECT_TRUE(hasRawView(plain)) << plain.name << " has a free texture unit and no PCSS";
    EXPECT_EQ(plain.log.find(ShaderVariantCompiler::PCSS_LEFT_OUT), std::string::npos) << plain.name;
    expectWithinGles3Limits(plain);

    // ...and none with three, nor the normal-mapped painted terrain: built without the view (the
    // Vogel disk) on every backend, and saying so. (The terrain's uniform vectors are over GLES3's
    // guaranteed 224 with or without it: the terrain layers' blocks, not PCSS's)
    for (ShaderBackend backend : {ShaderBackend::GLES3, ShaderBackend::GLCore, ShaderBackend::Vulkan}){
        Compiled full = compile(heavyLit(false), backend, threeExtensionMaps());
        EXPECT_FALSE(hasRawView(full)) << full.name << " is over the 16 fragment texture units";
        EXPECT_NE(full.log.find(ShaderVariantCompiler::PCSS_LEFT_OUT), std::string::npos) << full.name << ": " << full.log;
        expectWithinGles3Limits(full);
        Compiled terrain = compile(paintedTerrain(false), backend);
        EXPECT_FALSE(hasRawView(terrain)) << terrain.name << " is over the 16 samplers";
        EXPECT_NE(terrain.log.find(ShaderVariantCompiler::PCSS_LEFT_OUT), std::string::npos) << terrain.name << ": " << terrain.log;
        expectWithinGles3Limits(terrain, false);
    }
}
