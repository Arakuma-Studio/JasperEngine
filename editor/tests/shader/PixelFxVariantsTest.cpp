// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S48's screen transition program and Glitch variant collection (docs/specs/S48-pixel-fx.md R19,
// R30, R32): the transition compiles for every backend an export targets with no texture and
// within the resource budget, and a project whose scripts set post-processing ships the Glitch
// bit (getScriptableEffectFields names glitchEnabled).

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCollector.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <string>

using namespace jasperengine;

TEST(TransitionVariants, CompileForEveryBackend) {
    ASSERT_TRUE(ShaderVariantCompiler::isAvailable()) << "this build has no shader compiler";
    for (ShaderBackend backend : ShaderPool::getShaderBackends()) {
        ShaderData data;
        std::string error, log;
        const std::string name = std::string("transition (") + ShaderPool::getShaderLangStr(backend) + ")";
        ASSERT_TRUE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::TRANSITION, 0), backend, nullptr, data, &error, &log))
            << name << ": " << error << "\n" << log;
        EXPECT_EQ(ShaderVariantCompiler::checkResourceBudget(data), "") << name;
        size_t textures = 0;
        for (const ShaderStage& stage : data.stages) textures += stage.textures.size();
        EXPECT_EQ(textures, 0u) << name << ": the transition reads no texture (R32)";
        EXPECT_GE(data.getUniformBlockIndexByName("u_fs_transition"), 0) << name;
    }
}

TEST(GlitchCollector, ScriptsSettingPostProcessingShipTheGlitchBit) {
    const auto& fields = editor::ShaderVariantCollector::getScriptableEffectFields();
    EXPECT_NE(std::find(fields.begin(), fields.end(), "glitchEnabled"), fields.end());
    // a script that sets post-processing without naming fields: each switch flipped alone
    const std::set<uint32_t> any = editor::ShaderVariantCollector::plausibleUberFeatures(PostProcessingSettings(), true, {});
    EXPECT_EQ(any.count(UBER_GLITCH), 1u);
    // the scene's own settings ship it without scripts
    PostProcessingSettings on;
    on.glitchEnabled = true;
    on.glitchIntensity = 0.0f;
    EXPECT_EQ(editor::ShaderVariantCollector::plausibleUberFeatures(on, false, {}).count(UBER_GLITCH), 1u);
}
