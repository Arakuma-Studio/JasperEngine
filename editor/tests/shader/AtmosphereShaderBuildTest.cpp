// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC11 (docs/specs/S12-atmosphere.md): every atmosphere program and the fogged mesh variants
// (which carry the height fog) compile for every backend (GLSL 410, GLSL 300 es, HLSL 5, MSL 2.1 macOS and iOS,
// SPIR-V) with the engine's own shader compiler, and the HLSL build of the marches stays
// within the 10 s budget. An editor test: the editor's build has every back end (a standalone
// engine build only its own API's).

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {
    // the programs AtmosphereRenderer draws (each one variant: the tier is a uniform)
    const ShaderType ATMOSPHERE_TYPES[] = {
        ShaderType::ATMOSPHERE_DEPTH, ShaderType::VOLUMETRIC_FOG, ShaderType::ATMOSPHERE_DENOISE,
        ShaderType::VOLUMETRIC_CLOUDS, ShaderType::SKY_COMPOSITE, ShaderType::VOLUMETRIC_COMPOSITE
    };

    std::vector<ShaderKey> atmosphereKeys(){
        std::vector<ShaderKey> keys;
        for (ShaderType type : ATMOSPHERE_TYPES)
            keys.push_back(ShaderPool::getShaderKey(type, 0));
        // the fog's 'Pnt' variant: a shadowed point fog light (docs/specs/S101-vapor.md R14)
        keys.push_back(ShaderPool::getShaderKey(ShaderType::VOLUMETRIC_FOG, 1));
        // a lit PBR mesh (punctual, shadows, normals, fog, IBL) and an unlit one: the fog carries the height fog
        const uint64_t lit = ShaderPool::getMeshProperties(false, true, false, true, true, true, false, false, false, false, false,
                                                           true, false, false, false, false, false, false, true, false, false, false, false,
                                                           false, false, false, false, false, 8);
        const uint64_t unlit = ShaderPool::getMeshProperties(true, true, false, false, false, false, false, false, false, false, false,
                                                             true, false, false, false, false, false, false, false, false, false, false, false,
                                                             false, false, false, false, false, 8);
        keys.push_back(ShaderPool::getShaderKey(ShaderType::MESH, lit));
        keys.push_back(ShaderPool::getShaderKey(ShaderType::MESH, lit | ShaderPool::getShadowsHQProperty()));
        keys.push_back(ShaderPool::getShaderKey(ShaderType::MESH, unlit));
        return keys;
    }
}

TEST(AtmosphereShaderBuild, EveryProgramEveryBackend){
    if (!ShaderVariantCompiler::isAvailable())
        GTEST_SKIP() << "this build has no shader compiler";
    for (ShaderBackend backend : ShaderPool::getShaderBackends()){
        for (ShaderKey key : atmosphereKeys()){
            const std::string name = ShaderPool::getShaderStr(ShaderPool::getShaderTypeFromKey(key), ShaderPool::getPropertiesFromKey(key));
            ShaderData data;
            std::string error, log;
            const auto start = std::chrono::steady_clock::now();
            const bool ok = ShaderVariantCompiler::compile(key, backend, nullptr, data, &error, &log);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            EXPECT_TRUE(ok) << name << " (" << ShaderPool::getShaderLangStr(backend) << "): " << error << "\n" << log;
            // the marches' HLSL build (R18: <= 10 s)
            const ShaderType type = ShaderPool::getShaderTypeFromKey(key);
            if (backend == ShaderBackend::D3D11 && (type == ShaderType::VOLUMETRIC_FOG || type == ShaderType::VOLUMETRIC_CLOUDS)){
                EXPECT_LE(seconds, 10.0) << name << " HLSL build took " << seconds << " s";
            }
        }
    }
}

// docs/specs/S101-vapor.md R19: on GLES3 (and WebGL2) the fog pass with the point lights' shadows fits
// the 16 fragment texture units and the 224 fragment uniform vectors every device has; without
// a shadowed point light (the plain variant) it declares neither the point block nor its atlas
TEST(AtmosphereShaderBuild, FogPointShadowsFitGles3Limits){
    if (!ShaderVariantCompiler::isAvailable())
        GTEST_SKIP() << "this build has no shader compiler";
    auto fragmentStage = [](const ShaderData& data) -> const ShaderStage*{
        for (const ShaderStage& stage : data.stages){
            if (stage.type == ShaderStageType::FRAGMENT)
                return &stage;
        }
        return nullptr;
    };
    auto hasPair = [](const ShaderStage& stage, const std::string& texture){
        for (const ShaderTextureSamplerPair& pair : stage.textureSamplerPairs){
            if (pair.textureName == texture)
                return true;
        }
        return false;
    };
    for (uint64_t properties : {uint64_t(0), uint64_t(1)}){
        ShaderData data;
        std::string error, log;
        ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::VOLUMETRIC_FOG, properties), ShaderBackend::GLES3,
                                                   nullptr, data, &error, &log)) << error << "\n" << log;
        const ShaderStage* fragment = fragmentStage(data);
        ASSERT_NE(fragment, nullptr);
        // GLES3: each texture-sampler pair is a texture unit
        const size_t units = fragment->textureSamplerPairs.size();
        unsigned int uniformBytes = 0;
        for (const ShaderUniformBlock& block : fragment->uniformblocks)
            uniformBytes += block.sizeBytes;
        const unsigned int vectors = uniformBytes / 16;
        std::printf("[atmosphere] volumetric fog %s on GLES3: %zu texture units, %u uniform vectors\n",
                    properties ? "Pnt" : "plain", units, vectors);
        EXPECT_LE(units, 16u);
        EXPECT_LE(vectors, 224u);
        EXPECT_EQ(hasPair(*fragment, "u_shadowPointAtlas"), properties != 0);
        EXPECT_TRUE(hasPair(*fragment, "u_shadowAtlas"));
        bool pointBlock = false;
        for (const ShaderUniformBlock& block : fragment->uniformblocks)
            pointBlock = pointBlock || block.name == "u_fs_point_shadow_data";
        EXPECT_EQ(pointBlock, properties != 0);
    }
}
