// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Title Art's programs on every shader target (docs/specs/S80-title-art-core.md R37; AC33), in
// the editor's tests (the editor's compiler has every back end; a game's has its own):
// each of the renderer's programs (ShaderType::TITLE_ART, the program number in the property
// bits) compiles for GLSL 410, GLSL ES 300, HLSL 5, MSL macOS and iOS and SPIR-V, within the
// engine's per-stage budget, with at most 12 samplers and 4 uniform blocks, its one block
// u_fs_titleart the size render/TitlePrograms.h writes, and the slots the GPU layer binds.

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/render/TitlePrograms.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    const char* backendName(ShaderBackend backend) {
        switch (backend) {
            case ShaderBackend::GLCore: return "glsl410";
            case ShaderBackend::GLES3: return "glsl300es";
            case ShaderBackend::D3D11: return "hlsl5";
            case ShaderBackend::MetalMacOS: return "msl macOS";
            case ShaderBackend::MetalIOS: return "msl iOS";
            case ShaderBackend::Vulkan: return "spirv";
        }
        return "";
    }

}

TEST(TitleArtShadersTest, CompileAllTargets) {
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable());
    const ShaderBackend backends[] = {ShaderBackend::GLCore, ShaderBackend::GLES3, ShaderBackend::D3D11,
                                      ShaderBackend::MetalMacOS, ShaderBackend::MetalIOS, ShaderBackend::Vulkan};
    for (uint32_t program = TITLE_RENDER_PROGRAM_FIRST; program < TITLE_RENDER_PROGRAM_FIRST + TITLE_RENDER_PROGRAM_COUNT; program++) {
        ASSERT_STRNE(titleRenderProgramName(program), "") << "program " << program << " has no name";
        const ShaderKey key = titleRenderProgramKey(program);
        EXPECT_EQ(ShaderPool::getShaderTypeFromKey(key), ShaderType::TITLE_ART);
        EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), program);
        for (ShaderBackend backend : backends) {
            SCOPED_TRACE(std::string(titleRenderProgramName(program)) + " on " + backendName(backend));
            ShaderData data;
            std::string error, log;
            if (!ShaderVariantCompiler::compile(key, backend, nullptr, data, &error, &log)) {
                ADD_FAILURE() << error << "\n" << log;
                continue;
            }
            EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "");
            for (const ShaderStage& stage : data.stages) {
                if (stage.type != ShaderStageType::FRAGMENT) continue;
                EXPECT_LE(stage.samplers.size(), 12u);
                EXPECT_LE(stage.uniformblocks.size(), 4u);
                ASSERT_EQ(stage.uniformblocks.size(), 1u);
                EXPECT_EQ(stage.uniformblocks[0].name, "u_fs_titleart");
                EXPECT_EQ(stage.uniformblocks[0].sizeBytes, (unsigned)(TITLE_PARAMS * 16));
                for (const ShaderTexture& t : stage.textures) {
                    const bool filtered = t.name.rfind("u_taTex", 0) == 0, raw = t.name.rfind("u_taRaw", 0) == 0;
                    EXPECT_TRUE(filtered || raw) << t.name;
                    const int slot = std::stoi(t.name.substr(7));
                    EXPECT_LT(slot, filtered ? TITLE_TEXTURE_SLOTS : TITLE_RAW_SLOTS) << t.name;
                }
            }
        }
    }
}

TEST(TitleArtShadersTest, UnknownProgramFails) {
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable());
    // no program 0, and none past the renderer's last (titleart_render.frag says #error)
    for (uint32_t program : {0u, TITLE_RENDER_PROGRAM_LAST + 1}) {
        ShaderData data;
        std::string error;
        EXPECT_FALSE(ShaderVariantCompiler::compile(titleRenderProgramKey(program), ShaderBackend::Vulkan, nullptr, data, &error)) << program;
    }
}
