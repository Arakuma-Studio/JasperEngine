// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// R37, AC33 (docs/specs/S80-title-art-core.md): every TITLE_ART program the effects and generators
// bind, in every variant a device can need (the float path and the RGBA8 encodings), compiles for
// every shader target (GLSL 410, GLSL ES 300, HLSL 5, MSL macOS and iOS, SPIR-V) with the engine's
// own shader compiler, within the program limits: at most 12 samplers and 4 uniform blocks, no
// storage buffers. An editor test: the editor's build has every back end.

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/render/TitlePasses.h"

#include <gtest/gtest.h>

#include <cctype>
#include <set>
#include <string>
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

// S83 R29, AC20: every generator program (titleart_gen.vert / .frag, the instanced quads and
// meshes pulled from the vertex stage's data texture included), on the float path and the RGBA8
// encodings, for every shader target; within the same limits, no storage buffers, and no
// `texture(` call inside a loop of the generator includes (R30: loops use textureLod/texelFetch)
TEST(TitleArtShaderBuild, EveryGeneratorProgramEveryBackend) {
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const std::vector<ShaderBackend> backends = ShaderPool::getShaderBackends();
    TitleArtCaps floatCaps;
    floatCaps.buffers[(int)TitleBuffer::Distance].encoding = TitleEncoding::Native;
    TitleArtCaps fixedCaps;
    fixedCaps.buffers[(int)TitleBuffer::Distance].encoding = TitleEncoding::Fixed16;
    int compiled = 0;
    for (gen::GenProgram program : gen::genPrograms()) {
        std::set<uint64_t> variants = {gen::genProgramVariant(program, floatCaps), gen::genProgramVariant(program, fixedCaps)};
        for (uint64_t variant : variants) {
            const ShaderKey key = gen::genProgramKey(program, variant);
            for (ShaderBackend backend : backends) {
                SCOPED_TRACE(std::string(gen::genProgramName(program)) + " " + ShaderPool::getShaderStr(ShaderType::TITLE_ART, key.properties) + " " +
                             ShaderPool::getShaderLangStr(backend));
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
    EXPECT_GE(compiled, (int)gen::genPrograms().size() * (int)backends.size());
    // R30's static scan: a data-dependent loop samples with textureLod or texelFetch
    const auto& sources = ShaderVariantCompiler::getLibrarySources();
    int scanned = 0;
    for (const auto& [name, text] : sources) {
        if (name.rfind("titleart/gen/", 0) != 0 && name != "titleart_gen.frag" && name != "titleart_gen.vert") continue;
        scanned++;
        // comments out: their words are not code
        std::string source;
        for (size_t i = 0; i < text.size(); i++) {
            if (text.compare(i, 2, "//") == 0) {
                while (i < text.size() && text[i] != '\n') i++;
            }
            if (i < text.size()) source += text[i];
        }
        int depth = 0;
        std::vector<int> loopDepths;
        for (size_t i = 0; i < source.size(); i++) {
            if (source.compare(i, 4, "for ") == 0 || source.compare(i, 4, "for(") == 0) loopDepths.push_back(depth + 1);
            if (source[i] == '{') depth++;
            if (source[i] == '}') {
                if (!loopDepths.empty() && loopDepths.back() == depth) loopDepths.pop_back();
                depth--;
            }
            if (!loopDepths.empty() && source.compare(i, 8, "texture(") == 0 && (i == 0 || !std::isalnum((unsigned char)source[i - 1]))) {
                ADD_FAILURE() << name << ": texture() inside a loop at byte " << i;
            }
        }
        if (name.rfind("titleart/gen/", 0) == 0) EXPECT_NE(source.find("#ifndef TITLEART_GEN"), std::string::npos) << name << ": every generator include is guarded";
    }
    EXPECT_GE(scanned, 5);
}
