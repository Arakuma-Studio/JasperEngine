// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 R23: the particle variant bits of pixel particles stay where saved shader keys and caches put
// them: PARTICLES 12 'Pxl' (PARTICLE_PIXEL) and 13 'Ana' (PARTICLE_ANALYTIC), the last two of the
// type (14 bits), set only by effects that use them

#include "pool/ShaderPool.h"
#include "shader/ShaderBuildTypes.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(ParticlePixelShader, VariantBitsArePinned){
    ParticlesShaderFlags none;
    ParticlesShaderFlags pixel;
    pixel.pixel = true;
    ParticlesShaderFlags analytic;
    analytic.analytic = true;
    EXPECT_EQ(ShaderPool::getParticlesProperties(none) & (shaderPropertyBit(12) | shaderPropertyBit(13)), 0u) << "set only when used";
    EXPECT_EQ(ShaderPool::getParticlesProperties(pixel) ^ ShaderPool::getParticlesProperties(none), shaderPropertyBit(12));
    EXPECT_EQ(ShaderPool::getParticlesProperties(analytic) ^ ShaderPool::getParticlesProperties(none), shaderPropertyBit(13));
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::PARTICLES, 12, true), "Pxl");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::PARTICLES, 13, true), "Ana");
    EXPECT_EQ(ShaderPool::getShaderPropertyCount(ShaderType::PARTICLES), 14);
}
