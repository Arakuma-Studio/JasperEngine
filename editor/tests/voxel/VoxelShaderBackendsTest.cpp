// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R49 (AC39): every tier through the bundled SPIRV-Cross, for every backend. Metal and D3D11 are
// translated here, not compiled by a driver.

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"

#include <string>
#include <vector>

using namespace jasperengine;

TEST(VoxelShader, CrossCompilesForEveryBackend) {
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable());
    const uint64_t properties = ShaderPool::getMeshProperties(false, false, false, true, false, true, false, false, false, false, false,
                                                              false, false, false, false, false, false, false, false, false);
    const ShaderBackend backends[] = {ShaderBackend::GLCore, ShaderBackend::GLES3, ShaderBackend::D3D11, ShaderBackend::MetalMacOS, ShaderBackend::MetalIOS, ShaderBackend::Vulkan};
    for (const char* tier : {"VOXEL_TIER_HIGH", "VOXEL_TIER_MEDIUM", "VOXEL_TIER_LOW"}) {
        const std::string reference = ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, {"HAS_VOXEL", tier});
        const uint16_t id = ShaderPool::registerCustomShader(reference);
        const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, {}, ShaderType::MESH);
        for (const ShaderBackend backend : backends) {
            ShaderData data;
            std::string error, log;
            EXPECT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::MESH, properties, id), backend, &sources, data, &error, &log))
                << tier << " on backend " << static_cast<int>(backend) << ": " << error << "\n" << log;
        }
    }
}
