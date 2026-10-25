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
