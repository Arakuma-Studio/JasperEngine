// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83's effect programs on every shader target (docs/specs/S83-title-effects-generators.md R29-R31,
// R16, AC20, AC26), an editor test (the editor's compiler has every back end):
// - TitleFxShaderBuild.EveryProgramEveryBackend: every TFX program of titleart_fx.frag, in each
//   variant a device can need (the float path, the RGBA8 encodings) and its TFX_LIVE form,
//   compiles for GLSL 410, GLSL ES 300, HLSL 5, MSL macOS and iOS and SPIR-V within the program
//   limits (at most 12 samplers, 4 uniform blocks, no storage buffers).
// - TitleFxShaderBuild.LoopsSampleWithoutDerivatives: a static scan of titleart_fx.frag's includes
//   finds no `texture(` inside a loop (D3D11 needs textureLod or texelFetch there) and
//   `precision highp float` in the program.
// - TitleFxShaderBuild.NoClock: no effect shader or definition reads time (R16): no time or clock
//   uniform in titleart/fx, no clock call in the definitions.

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "titleart/effects/TfxPrograms.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    std::string readFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // the repository root: the editor tests run from inside the build tree
    std::filesystem::path repoRoot() {
        std::filesystem::path p = std::filesystem::path(__FILE__).parent_path();
        for (int i = 0; i < 6 && !p.empty(); i++) {
            if (std::filesystem::exists(p / "engine" / "shaderlib" / "titleart_fx.frag")) return p;
            p = p.parent_path();
        }
        return {};
    }

    // the lines of each loop body (a `for` or `while` with a brace on its line, to the matching
    // brace) of a GLSL text
    std::vector<std::string> loopBodies(const std::string& text) {
        std::vector<std::string> bodies;
        const std::regex loop(R"(\b(for|while)\s*\()");
        for (std::sregex_iterator it(text.begin(), text.end(), loop), end; it != end; ++it) {
            size_t open = text.find('{', (size_t)it->position());
            if (open == std::string::npos) continue;
            int depth = 0;
            size_t i = open;
            for (; i < text.size(); i++) {
                if (text[i] == '{') depth++;
                else if (text[i] == '}' && --depth == 0) break;
            }
            bodies.push_back(text.substr(open, i - open));
        }
        return bodies;
    }

}

TEST(TitleFxShaderBuild, EveryProgramEveryBackend) {
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
    std::set<std::string> names;
    for (int p = 1; p <= tfx::PROGRAM_LAST; p++) {
        const tfx::Program program = (tfx::Program)p;
        EXPECT_TRUE(names.insert(tfx::programName(program)).second) << "a program name is used twice";
        std::set<uint64_t> variants;
        for (bool live : {false, true}) {
            variants.insert(tfx::programVariant(program, floatCaps, live));
            variants.insert(tfx::programVariant(program, fixedCaps, live));
        }
        for (uint64_t variant : variants) {
            const ShaderKey key = tfx::programKey(program, variant);
            EXPECT_EQ(ShaderPool::getShaderTypeFromKey(key), ShaderType::TITLE_ART);
            for (ShaderBackend backend : backends) {
                SCOPED_TRACE(std::string(tfx::programName(program)) + " " + ShaderPool::getShaderStr(ShaderType::TITLE_ART, key.properties) + " " +
                             ShaderPool::getShaderLangStr(backend));
                ShaderData data;
                std::string error, log;
                ASSERT_TRUE(ShaderVariantCompiler::compile(key, backend, nullptr, data, &error, &log)) << error << "\n" << log;
                EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "");
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
    EXPECT_GE(compiled, tfx::PROGRAM_LAST * 2 * 6);
}

TEST(TitleFxShaderBuild, LoopsSampleWithoutDerivatives) {
    const std::filesystem::path root = repoRoot();
    ASSERT_FALSE(root.empty()) << "the repository's shaders are not found";
    const std::string frag = readFile(root / "engine/shaderlib/titleart_fx.frag");
    EXPECT_NE(frag.find("precision highp float;"), std::string::npos);
    int scanned = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "engine/shaderlib/titleart/fx")) {
        if (entry.path().extension() != ".glsl") continue;
        const std::string text = readFile(entry.path());
        for (const std::string& body : loopBodies(text)) {
            EXPECT_EQ(body.find("texture("), std::string::npos) << entry.path().filename().string() << ": a loop samples with texture(:\n" << body.substr(0, 400);
        }
        scanned++;
    }
    EXPECT_GE(scanned, 9);
}

TEST(TitleFxShaderBuild, NoClock) {
    const std::filesystem::path root = repoRoot();
    ASSERT_FALSE(root.empty());
    const std::regex time(R"(\b(u?[Tt]ime|iTime|uClock|clock)\b)");
    for (const auto& entry : std::filesystem::directory_iterator(root / "engine/shaderlib/titleart/fx")) {
        const std::string text = readFile(entry.path());
        EXPECT_FALSE(std::regex_search(text, time)) << entry.path().filename().string() << " reads a clock";
    }
    const std::regex clock(R"(std::chrono|\bclock\(\)|\btime\(|getTime|Engine::get(Delta|Frame|Time))");
    for (const auto& entry : std::filesystem::directory_iterator(root / "engine/core/titleart/effects")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("Tfx", 0) != 0 && name.find("Effect") == std::string::npos) continue;
        const std::string text = readFile(entry.path());
        EXPECT_FALSE(std::regex_search(text, clock)) << name << " reads a clock";
    }
}
