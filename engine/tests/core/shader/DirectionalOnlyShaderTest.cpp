// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The built-in Lit's directional-only variant (MESH bit 44 'Dlo', draw-perf spec 6.5): named, given
// up first when a build lacks it (the general variant draws the same scene), and compiled without
// the spot mask atlas, the point shadow block and the point shadow atlas (two textures and a block
// fewer to bind). A lit variant without the bit is untouched (ShaderZeroCost holds its hashes).

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

namespace {

    // MESH 'Dlo', by its number
    constexpr uint64_t DIRECTIONAL_ONLY_BIT = 1ull << 44;

    // a lit, shadowed built-in Lit variant
    uint64_t litShadowed(){
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.tangent = true;
        inputs.baseColorTexture = true;
        MeshShaderEnvironment environment;
        environment.lights = true;
        environment.shadows = true;
        return RenderSystem::getSubmeshShaderVariant(environment, inputs).properties;
    }

    std::string fragmentOf(uint64_t properties, ShaderBackend backend){
        std::string vertex, fragment, error;
        EXPECT_TRUE(ShaderVariantCompiler::preprocess(ShaderPool::getShaderKey(ShaderType::MESH, properties, 0), backend, nullptr,
                                                      vertex, fragment, &error)) << error;
        return fragment;
    }

}

TEST(DirectionalOnlyShader, IsMeshBit44){
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 44), "Dlo");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 44, false), "Directional Lights Only");
    EXPECT_EQ(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, DIRECTIONAL_ONLY_BIT | 8u)).properties,
              DIRECTIONAL_ONLY_BIT | 8u) << "the bit is a property a key keeps";
}

TEST(DirectionalOnlyShader, FallbackGivesItUpFirst){
    const uint64_t properties = litShadowed() | DIRECTIONAL_ONLY_BIT;
    const std::vector<uint64_t> candidates = ShaderPool::getFallbackProperties(ShaderType::MESH, properties);
    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates.front(), properties & ~DIRECTIONAL_ONLY_BIT);
}

TEST(DirectionalOnlyShader, LeavesOutPointAndSpotResources){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no embedded shader library";
    const uint64_t general = litShadowed();
    for (ShaderBackend backend : {ShaderBackend::GLCore, ShaderBackend::GLES3}){
        const std::string withAll = fragmentOf(general, backend);
        const std::string directional = fragmentOf(general | DIRECTIONAL_ONLY_BIT, backend);
        for (const char* name : {"u_spotMaskAtlas", "u_shadowPointAtlas", "pointShadowData"}){
            EXPECT_NE(withAll.find(name), std::string::npos) << name << " in the general variant";
            EXPECT_EQ(directional.find(name), std::string::npos) << name << " in the 'Dlo' variant";
        }
    }
}
