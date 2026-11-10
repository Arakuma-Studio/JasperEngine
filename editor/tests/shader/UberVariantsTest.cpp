// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The S13 uber variants on every backend an export targets (docs/specs/S13-post-fx.md AC11):
// each new bit alone (the grain types with the grain) and all bits on compile to GLSL 410,
// GLSL 300 es, HLSL 5, MSL (macOS, iOS) and SPIR-V within the engine's resource budget. The
// editor's shader compiler has every language (a game's has its own platform's only).

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <string>
#include <vector>

using namespace jasperengine;

TEST(UberVariants, CompileForEveryBackend) {
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const std::vector<uint32_t> variants = {
        UBER_VHS, UBER_HALATION, UBER_FILM_DAMAGE, UBER_GRAIN | UBER_GRAIN_LENS, UBER_GRAIN | UBER_GRAIN_FILMIC,
        UBER_VHS | UBER_HALATION | UBER_FILM_DAMAGE | UBER_GRAIN | UBER_GRAIN_LENS | UBER_BLOOM | UBER_LUT | UBER_DITHER,
        // S48's Glitch alone and with the pixel-art bits it meets (Stretch Fill, Palette Quantize)
        UBER_GLITCH, UBER_GLITCH | UBER_SHARP_BILINEAR | UBER_PALETTE | UBER_PIXELATE,
        (1u << UBER_FEATURE_COUNT) - 1u,
    };
    for (ShaderBackend backend : ShaderPool::getShaderBackends()) {
        for (uint32_t features : variants) {
            ShaderData data;
            std::string error, log;
            const std::string name = ShaderPool::getShaderStr(ShaderType::BLIT, features, 0) + " (" + ShaderPool::getShaderLangStr(backend) + ")";
            EXPECT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::BLIT, features), backend, nullptr, data, &error, &log))
                << name << ": " << error << "\n" << log;
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
        }
    }
}
