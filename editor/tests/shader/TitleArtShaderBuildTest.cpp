// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// R37, AC33 (docs/specs/S80-title-art-core.md): every TITLE_ART program the effects and generators
// bind, in every variant a device can need (the float path and the RGBA8 encodings), compiles for
// every shader target (GLSL 410, GLSL ES 300, HLSL 5, MSL macOS and iOS, SPIR-V) with the engine's
// own shader compiler, within the program limits: at most 12 samplers and 4 uniform blocks, no
// storage buffers. An editor test: the editor's build has every back end.

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "titleart/render/TitlePasses.h"

#include <gtest/gtest.h>

#include <set>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;

TEST(TitleArtShaderBuild, EveryProgramEveryBackend) {
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const std::vector<ShaderBackend> backends = ShaderPool::getShaderBackends();
    ASSERT_EQ(backends.size(), 6u) << "GLSL 410, GLSL ES 300, HLSL 5, MSL macOS and iOS, SPIR-V";
    TitleArtCaps floatCaps;
    floatCaps.buffers[(int)TitleBuffer::Seeds].encoding = TitleEncoding::Native;
    floatCaps.buffers[(int)TitleBuffer::Distance].encoding = TitleEncoding::Native;
    TitleArtCaps fixedCaps;
    fixedCaps.buffers[(int)TitleBuffer::Seeds].encoding = TitleEncoding::Fixed12_4;
    fixedCaps.buffers[(int)TitleBuffer::Distance].encoding = TitleEncoding::Fixed16;
    int compiled = 0;
    for (int p = 1; p <= TITLE_PROGRAM_LAST; p++) {
        const TitleProgram program = (TitleProgram)p;
        std::set<uint64_t> variants = {0, titleProgramVariant(program, floatCaps), titleProgramVariant(program, fixedCaps)};
        for (uint64_t variant : variants) {
            const ShaderKey key = titleProgramKey(program, variant);
            for (ShaderBackend backend : backends) {
                SCOPED_TRACE(ShaderPool::getShaderStr(ShaderType::TITLE_ART, key.properties) + " " + ShaderPool::getShaderLangStr(backend));
                ShaderData data;
                std::string error, log;
                ASSERT_TRUE(ShaderVariantCompiler::compile(key, backend, nullptr, data, &error, &log)) << error << "\n" << log;
                for (const ShaderStage& stage : data.stages) {
                    EXPECT_LE(stage.samplers.size(), 12u);
                    EXPECT_LE(stage.textures.size(), 12u);
                    EXPECT_LE(stage.uniformblocks.size(), 4u);
                    EXPECT_TRUE(stage.storagebuffers.empty());
                }
                compiled++;
            }
        }
    }
    EXPECT_GE(compiled, TITLE_PROGRAM_LAST * 6);
}
