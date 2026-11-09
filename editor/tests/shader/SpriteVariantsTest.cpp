// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// library:sprite's variants after S48's Pixel FX (docs/specs/S48-pixel-fx.md R1, R31, R33, AC7-AC9):
//  - LegacyIdentical: every pre-S48 feature combination preprocesses to the same program, and
//    reflects the same uniform blocks and vertex attributes, as the pre-S48 sources
//    (editor/tests/fixtures/shader/sprite_pre_s48.*, sprite.frag and sprite.vert of the merge
//    base, compiled as a project fork): a new feature adds nothing while it is off.
//  - CompileForEveryBackend: each new feature alone and every feature at once compile for the six
//    backend targets within the per-stage budget, 8 fragment textures and a 32-vector custom block.
//  - TapBudget: the texture reads each feature adds to the fragment stage (OpImageSample* and
//    OpImageFetch in the Vulkan SPIR-V, counted statically) within R33's budgets.

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "shader/SpriteTaps.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;

namespace sprite_variants_test {

    constexpr const char* kForkBase = "shaders/PreS48Sprite";

    bool readFixture(const std::string& name, std::string& content){
        std::ifstream file(std::filesystem::path(JASPER_TEST_FIXTURES_DIR) / "shader" / name, std::ios::binary);
        if (!file)
            return false;
        std::ostringstream text;
        text << file.rdbuf();
        content = text.str();
        return true;
    }

    // the merge base's sprite shader as a project fork
    bool readPreS48(const std::string& path, std::string& content){
        if (path == std::string(kForkBase) + ".frag")
            return readFixture("sprite_pre_s48.frag", content);
        if (path == std::string(kForkBase) + ".vert")
            return readFixture("sprite_pre_s48.vert", content);
        return false;
    }

    SubmeshShaderInputs spriteInputs(const std::vector<std::string>& features){
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.tangent = true;
        inputs.vertexColor4 = true;
        inputs.textureRect = true;
        inputs.baseColorTexture = true;
        inputs.normalTexture = true;
        inputs.manifest = ShaderManifests::get("library:sprite");
        inputs.features = inputs.manifest->resolveFeatures(features);
        return inputs;
    }

    struct Built {
        std::string name;
        ShaderKey key;
        CustomShaderSources sources;
    };

    // the variant the renderer would draw for these features, blend mode and environment
    Built spriteVariant(const std::vector<std::string>& features, MaterialBlendMode blend, const MeshShaderEnvironment& environment){
        const SubmeshShaderInputs inputs = spriteInputs(features);
        const std::string reference = ShaderReference::getColorVariantReference("library:sprite", features, blend);
        const uint16_t customId = ShaderPool::registerCustomShader(reference);
        const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
        Built built;
        built.key = ShaderPool::getShaderKey(ShaderType::MESH, variant.properties, customId);
        built.sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, nullptr, ShaderType::MESH);
        built.name = ShaderPool::getShaderStr(ShaderType::MESH, variant.properties, customId);
        return built;
    }

    // the same variant from the pre-S48 sources (same defines, same property bits)
    Built preS48Variant(const Built& current){
        Built built;
        const std::string reference = ShaderReference::withFeatures(kForkBase, current.sources.defines);
        built.key = ShaderPool::getShaderKey(ShaderType::MESH, current.key.properties, ShaderPool::registerCustomShader(reference));
        built.sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, readPreS48, ShaderType::MESH);
        built.name = current.name + " (pre-S48)";
        return built;
    }

    const ShaderStage* stageOf(const ShaderData& data, ShaderStageType type){
        for (const ShaderStage& stage : data.stages){
            if (stage.type == type)
                return &stage;
        }
        return nullptr;
    }

    // what reflection says the stages take: uniform blocks with their members, attributes, textures
    std::string reflection(const ShaderData& data){
        std::ostringstream out;
        for (const ShaderStage& stage : data.stages){
            out << "stage " << int(stage.type) << "\n";
            for (const ShaderAttr& attr : stage.attributes)
                out << "  attr " << attr.name << " " << attr.location << "\n";
            for (const ShaderUniformBlock& block : stage.uniformblocks){
                out << "  block " << block.name << " " << block.sizeBytes << "\n";
                for (const ShaderUniform& uniform : block.uniforms)
                    out << "    " << uniform.name << " " << uniform.offset << " " << int(uniform.type) << " " << uniform.arrayCount << "\n";
            }
            for (const ShaderTexture& texture : stage.textures)
                out << "  texture " << texture.name << "\n";
        }
        return out.str();
    }

    int countTaps(const ShaderBinSource& bytecode){
        return editor::countSpirvTextureReads(bytecode);
    }

    std::vector<std::string> legacyFeatures(){
        return {"OUTLINE", "HUE_SHIFT", "FLASH", "GRAYSCALE", "COLOR_REPLACE", "OVERLAY", "GLOW", "PALETTE_SWAP", "PALETTE_CYCLE"};
    }

    std::vector<std::string> allFeatures(){
        std::vector<std::string> all = legacyFeatures();
        for (const char* feature : {"DITHER_FADE", "DISSOLVE", "RAMP_RECOLOR", "DROP_SHADOW", "OUTLINE_STYLE_PIXEL8"})
            all.push_back(feature);
        return all;
    }

    MeshShaderEnvironment lights2D(){
        MeshShaderEnvironment environment;
        environment.lights2D = true;
        return environment;
    }

}

using namespace sprite_variants_test;

// AC7, R1: no new feature on, nothing changes: the preprocessed program of both stages, and the
// reflected blocks and attributes, equal the pre-S48 sources' for each legacy combination (each
// feature alone, all of them, the premultiplied blend, under 2D lights)
TEST(SpriteVariants, LegacyIdentical){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    std::string probe;
    ASSERT_TRUE(readFixture("sprite_pre_s48.frag", probe)) << "the pre-S48 fixture is missing";
    std::vector<std::vector<std::string>> combinations = {{}};
    for (const std::string& feature : legacyFeatures())
        combinations.push_back({feature});
    combinations.push_back(legacyFeatures());
    int compared = 0;
    for (const std::vector<std::string>& features : combinations){
        for (MaterialBlendMode blend : {MaterialBlendMode::AUTO, MaterialBlendMode::PREMULTIPLIED}){
            for (const MeshShaderEnvironment& environment : {MeshShaderEnvironment(), lights2D()}){
                const Built current = spriteVariant(features, blend, environment);
                const Built previous = preS48Variant(current);
                ASSERT_TRUE(current.sources.valid) << current.sources.error;
                ASSERT_TRUE(previous.sources.valid) << previous.sources.error;
                for (ShaderBackend backend : {ShaderBackend::GLCore, ShaderBackend::Vulkan}){
                    std::string vertexNow, fragmentNow, vertexBefore, fragmentBefore, error;
                    ASSERT_TRUE(ShaderVariantCompiler::preprocess(current.key, backend, &current.sources, vertexNow, fragmentNow, &error))
                        << current.name << ": " << error;
                    ASSERT_TRUE(ShaderVariantCompiler::preprocess(previous.key, backend, &previous.sources, vertexBefore, fragmentBefore, &error))
                        << previous.name << ": " << error;
                    EXPECT_EQ(ShaderVariantCompiler::normalizePreprocessed(vertexNow), ShaderVariantCompiler::normalizePreprocessed(vertexBefore))
                        << current.name << ": the vertex stage changed";
                    EXPECT_EQ(ShaderVariantCompiler::normalizePreprocessed(fragmentNow), ShaderVariantCompiler::normalizePreprocessed(fragmentBefore))
                        << current.name << ": the fragment stage changed";
                }
                ShaderData now, before;
                std::string error, log;
                ASSERT_TRUE(ShaderVariantCompiler::compile(current.key, ShaderBackend::GLCore, &current.sources, now, &error, &log))
                    << current.name << ": " << error << "\n" << log;
                ASSERT_TRUE(ShaderVariantCompiler::compile(previous.key, ShaderBackend::GLCore, &previous.sources, before, &error, &log))
                    << previous.name << ": " << error << "\n" << log;
                EXPECT_EQ(reflection(now), reflection(before)) << current.name;
                compared++;
            }
        }
    }
    EXPECT_EQ(compared, int(combinations.size()) * 4);
}

// AC8, R31: each new feature alone and every feature at once, plain and under 2D lights, compile for
// the six backend targets, within the stage budget, 8 fragment textures and a custom block of at
// most 32 vectors
TEST(SpriteVariants, CompileForEveryBackend){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    const std::vector<std::vector<std::string>> combinations = {
        {"DITHER_FADE"}, {"DISSOLVE"}, {"RAMP_RECOLOR"}, {"DROP_SHADOW"},
        {"OUTLINE", "OUTLINE_STYLE_PIXEL4"}, {"OUTLINE", "OUTLINE_STYLE_PIXEL8"},
        {"OUTLINE", "DISSOLVE"}, {"DROP_SHADOW", "OUTLINE", "DITHER_FADE"},
        allFeatures(),
    };
    for (const std::vector<std::string>& features : combinations){
        for (const MeshShaderEnvironment& environment : {MeshShaderEnvironment(), lights2D()}){
            const Built built = spriteVariant(features, MaterialBlendMode::AUTO, environment);
            ASSERT_TRUE(built.sources.valid) << built.sources.error;
            for (ShaderBackend backend : ShaderPool::getShaderBackends()){
                const std::string name = built.name + " (" + ShaderPool::getShaderLangStr(backend) + ")";
                ShaderData data;
                std::string error, log;
                ASSERT_TRUE(ShaderVariantCompiler::compile(built.key, backend, &built.sources, data, &error, &log))
                    << name << ": " << error << "\n" << log;
                EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
                const ShaderStage* fragment = stageOf(data, ShaderStageType::FRAGMENT);
                ASSERT_TRUE(fragment) << name;
                EXPECT_LE(fragment->textures.size(), 8u) << name << ": over 8 fragment texture bindings";
                for (const ShaderUniformBlock& block : fragment->uniformblocks){
                    if (block.name.find("customParams") != std::string::npos) {
                        EXPECT_LE(block.sizeBytes, 32u * 16u) << name << ": the custom block is over 32 vectors";
                    }
                }
            }
        }
    }
    // every feature at once under 2D lights with a normal map: base, normal map, replace mask,
    // overlay, ramp and the palette's two textures
    const Built worst = spriteVariant(allFeatures(), MaterialBlendMode::AUTO, lights2D());
    ShaderData data;
    std::string error, log;
    ASSERT_TRUE(ShaderVariantCompiler::compile(worst.key, ShaderBackend::GLCore, &worst.sources, data, &error, &log)) << error;
    const ShaderStage* fragment = stageOf(data, ShaderStageType::FRAGMENT);
    ASSERT_TRUE(fragment);
    std::printf("[sprite] every feature: %zu fragment textures\n", fragment->textures.size());
    EXPECT_EQ(fragment->textures.size(), 7u);
}

// AC9, R33: the texture reads each S48 feature adds to the base sprite's fragment stage (static
// count in the Vulkan SPIR-V) within the budget
// the count is static (SPIR-V call sites): a read inside a shader loop counts once, as the
// frame_cost lint says in its message
TEST(SpriteVariants, TapBudget){
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    auto taps = [](const std::vector<std::string>& features){
        const Built built = spriteVariant(features, MaterialBlendMode::AUTO, MeshShaderEnvironment());
        ShaderData data;
        std::string error, log;
        EXPECT_TRUE(ShaderVariantCompiler::compile(built.key, ShaderBackend::Vulkan, &built.sources, data, &error, &log)) << error;
        const ShaderStage* fragment = stageOf(data, ShaderStageType::FRAGMENT);
        return fragment ? countTaps(fragment->bytecode) : -1;
    };
    const int base = taps({});
    ASSERT_GE(base, 1) << "no texture read in the base sprite's SPIR-V";
    struct Budget { std::vector<std::string> features; int extra; };
    const std::vector<Budget> budgets = {
        {{"DITHER_FADE"}, 0},
        {{"DISSOLVE"}, 1},
        {{"RAMP_RECOLOR"}, 3},
        {{"DROP_SHADOW"}, 1 + 1},     // the caster tap, and the ramp's darkest for its colour
        {{"OUTLINE", "OUTLINE_STYLE_PIXEL4"}, 4 + 8},
        {{"OUTLINE", "OUTLINE_STYLE_PIXEL8"}, 8 + 8},
        {{"DITHER_FADE", "DISSOLVE", "RAMP_RECOLOR", "DROP_SHADOW", "OUTLINE", "OUTLINE_STYLE_PIXEL4"}, 24},
    };
    for (const Budget& budget : budgets){
        std::string name;
        for (const std::string& feature : budget.features)
            name += (name.empty() ? "" : ",") + feature;
        const int count = taps(budget.features);
        ASSERT_GE(count, 0) << name;
        std::printf("[sprite] %s: %d texture reads beyond the base's %d\n", name.c_str(), count - base, base);
        EXPECT_LE(count - base, budget.extra) << name;
    }
}
