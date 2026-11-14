// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The gradient ambient's variant (MESH bit 55 'Agr', docs/specs/S94-rendering-anim-gaps.md F15):
// only lit variants of a Gradient scene that take the scene ambient carry it and AMBIENT_GRADIENT;
// a Color scene's variants have neither (ShaderZeroCost holds their programs), and a build missing
// one falls back to the variant without it.

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

namespace {

    constexpr uint64_t AMBIENT_GRADIENT_BIT = 1ull << 55;

    SubmeshShaderInputs litInputs(){
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.baseColorTexture = true;
        return inputs;
    }

    uint64_t variant(bool gradient, bool lights, bool ibl){
        MeshShaderEnvironment environment;
        environment.lights = lights;
        environment.ibl = ibl;
        environment.ambientGradient = gradient;
        SubmeshShaderInputs inputs = litInputs();
        inputs.receiveIBL = true;
        return RenderSystem::getSubmeshShaderVariant(environment, inputs).properties;
    }

    std::string fragmentOf(uint64_t properties){
        std::string vertex, fragment, error;
        EXPECT_TRUE(ShaderVariantCompiler::preprocess(ShaderPool::getShaderKey(ShaderType::MESH, properties, 0), ShaderBackend::GLCore, nullptr,
                                                      vertex, fragment, &error)) << error;
        return fragment;
    }

}

TEST(AmbientGradientShader, IsMeshBit55){
    EXPECT_EQ(ShaderPool::getAmbientGradientProperty(), AMBIENT_GRADIENT_BIT);
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 55), "Agr");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 55, false), "Ambient Gradient");
    EXPECT_EQ(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, AMBIENT_GRADIENT_BIT | 8u)).properties,
              AMBIENT_GRADIENT_BIT | 8u) << "the bit is a property a key keeps";
}

TEST(AmbientGradientShader, ColorAmbientHasNoGradientDefine){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no embedded shader library";
    // Color: no bit, no block
    const uint64_t color = variant(false, true, false);
    EXPECT_EQ(color & AMBIENT_GRADIENT_BIT, 0u);
    const std::string colorFragment = fragmentOf(color);
    EXPECT_EQ(colorFragment.find("u_fs_ambientGradient"), std::string::npos);
    EXPECT_EQ(colorFragment.find("ambientGradient("), std::string::npos);

    // Gradient: the lit variant takes it, and reads the gradient
    const uint64_t gradient = variant(true, true, false);
    EXPECT_EQ(gradient, color | AMBIENT_GRADIENT_BIT) << "the bit alone";
    const std::string gradientFragment = fragmentOf(gradient);
    EXPECT_NE(gradientFragment.find("u_fs_ambientGradient"), std::string::npos);
    EXPECT_NE(gradientFragment.find("ambientGradient("), std::string::npos);
}

TEST(AmbientGradientShader, OnlyVariantsTakingTheSceneAmbient){
    // the built-in Lit lit by the sky's IBL alone: the IBL replaces the scene ambient
    EXPECT_EQ(variant(true, false, true) & AMBIENT_GRADIENT_BIT, 0u);
    // with lights too: the IBL still is its ambient (the SH's share is the gradient's average)
    EXPECT_EQ(variant(true, true, true) & AMBIENT_GRADIENT_BIT, 0u);
    // unlit
    MeshShaderEnvironment environment;
    environment.ambientGradient = true;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(environment, litInputs()).properties & AMBIENT_GRADIENT_BIT, 0u);
}

TEST(AmbientGradientShader, FallbackGivesItUp){
    const uint64_t properties = variant(true, true, false);
    ASSERT_NE(properties & AMBIENT_GRADIENT_BIT, 0u);
    bool without = false;
    for (uint64_t candidate : ShaderPool::getFallbackProperties(ShaderType::MESH, properties))
        without = without || candidate == (properties & ~AMBIENT_GRADIENT_BIT);
    EXPECT_TRUE(without) << "a build missing 'Agr' draws the variant without it (the gradient's average)";
}
