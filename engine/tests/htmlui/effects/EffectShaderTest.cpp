// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ShaderType::UI_EFFECT (phase-5 spec 3.8): keys, names, specs (5.1 ShaderKeyTest), and every
// effect variant of the library compiled for every backend with its reflection checked against
// the manifest (5.6, the engine side of `ui-effects --validate` and check_effect_shaders.sh).

#include "htmlui/effects/EffectManifest.h"
#include "htmlui/effects/EffectRegistry.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::htmlui;

TEST(ShaderKeyTest, TypeTokensNamesAndMask){
    ShaderType type;
    ASSERT_TRUE(ShaderPool::parseShaderTypeToken("uieffect", type));
    EXPECT_EQ(type, ShaderType::UI_EFFECT);
    EXPECT_EQ(ShaderPool::getShaderTypeName(ShaderType::UI_EFFECT, true), "uieffect");
    EXPECT_EQ(ShaderPool::getShaderTypeName(ShaderType::UI_EFFECT), "UI Effect");
    EXPECT_EQ(ShaderPool::getShaderPropertyCount(ShaderType::UI_EFFECT), 1);
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::UI_EFFECT, 0, true), "Flt");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::UI_EFFECT, 0, false), "Filter form");
    EXPECT_EQ(ShaderPool::getValidPropertyMask(ShaderType::UI_EFFECT), 1u);
}

TEST(ShaderKeyTest, LibraryIdsRoundTrip){
    const uint16_t id = ShaderPool::registerCustomShader("@effects/cursor-trail");
    EXPECT_TRUE(ShaderPool::isLibraryCustomShader(id));
    EXPECT_FALSE(ShaderPool::isLibraryCustomShader(0));
    EXPECT_EQ(ShaderPool::getCustomShaderBasename(id), "fx_cursor_trail");
    const std::string paint = ShaderPool::getShaderStr(ShaderType::UI_EFFECT, 0, id);
    const std::string filter = ShaderPool::getShaderStr(ShaderType::UI_EFFECT, 1, id);
    EXPECT_EQ(paint, "uieffect_fx_cursor_trail");
    EXPECT_EQ(filter, "uieffect_fx_cursor_trail_Flt");
    ShaderKey key = 0;
    ASSERT_TRUE(ShaderPool::parseShaderStr(filter, key));
    EXPECT_EQ(key, ShaderPool::getShaderKey(ShaderType::UI_EFFECT, 1, id));
    ASSERT_TRUE(ShaderPool::parseShaderStr(paint, key));
    EXPECT_EQ(key, ShaderPool::getShaderKey(ShaderType::UI_EFFECT, 0, id));
    std::string cli;
    ASSERT_TRUE(ShaderPool::getShaderCliSpec(filter, cli));
    EXPECT_EQ(cli, "uieffect/cursor-trail:Flt");
    // the internal bright pass keeps its leading '_'
    const uint16_t bright = ShaderPool::registerCustomShader("@effects/_bright");
    EXPECT_EQ(ShaderPool::getCustomShaderBasename(bright), "fx__bright");
    ASSERT_TRUE(ShaderPool::parseShaderStr("uieffect_fx__bright_Flt", key));
    EXPECT_EQ(ShaderPool::getCustomIdFromKey(key), bright);
}

TEST(ShaderKeyTest, ProjectIdsRoundTrip){
    std::vector<EffectDiagnostic> diags;
    const std::string source = "#version 450\n// @effect     key-test\n// @kind       paint\n// @category   pattern\n"
                               "void main(){}\n#include \"effects/lib/main_paint.glsl\"\n";
    ASSERT_TRUE(EffectRegistry::get().declareProjectEffect("key-test", "ui/fx/key-test.frag", "ui/a.css", 1, diags,
        [&](const std::string&, std::string& content){ content = source; return true; })) << (diags.empty() ? "" : diags[0].message);
    const uint16_t id = ShaderPool::registerCustomShader("ui/fx/key-test.frag");
    EXPECT_FALSE(ShaderPool::isLibraryCustomShader(id));
    const std::string name = ShaderPool::getShaderStr(ShaderType::UI_EFFECT, 1, id);
    EXPECT_EQ(name, "uieffect_ui_fx_key_test_frag_Flt");
    ShaderKey key = 0;
    ASSERT_TRUE(ShaderPool::parseShaderStr(name, key));
    EXPECT_EQ(key, ShaderPool::getShaderKey(ShaderType::UI_EFFECT, 1, id));
    ASSERT_TRUE(ShaderPool::parseShaderSpec("uieffect/key-test", key));
    EXPECT_EQ(ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(key)), "ui/fx/key-test.frag");
    EffectRegistry::get().clearProjectEffects();
}

TEST(ShaderKeyTest, SpecsAndExpansion){
    ShaderKey key = 0;
    std::string error;
    ASSERT_TRUE(ShaderPool::parseShaderSpec("uieffect/stripes:Flt", key, &error)) << error;
    EXPECT_EQ(ShaderPool::getShaderTypeFromKey(key), ShaderType::UI_EFFECT);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), 1u);
    EXPECT_EQ(ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(key)), "@effects/stripes");
    ASSERT_TRUE(ShaderPool::parseShaderSpec("uieffect/pixelate:Filter form", key, &error)) << error;
    EXPECT_FALSE(ShaderPool::parseShaderSpec("uieffect/no-such-effect", key, &error));
    EXPECT_NE(error.find("Unknown UI effect"), std::string::npos);
    EXPECT_FALSE(ShaderPool::parseShaderSpec("uieffect", key, &error));
    EXPECT_FALSE(ShaderPool::parseShaderSpec("mesh/stripes", key, &error));
    EXPECT_FALSE(ShaderPool::parseShaderSpec("uieffect/all", key, &error));

    std::vector<ShaderKey> keys;
    ASSERT_TRUE(ShaderPool::expandShaderSpec("uieffect/all", keys, &error)) << error;
    size_t paint = 0, filter = 0;
    for (const EffectManifest* m : EffectRegistry::get().builtins())
        (m->kind == EffectKind::Paint ? paint : filter)++;
    // 2 x paint + filter + the internal bright pass
    EXPECT_EQ(keys.size(), 2 * paint + filter + 1);
    std::set<ShaderKey> unique(keys.begin(), keys.end());
    EXPECT_EQ(unique.size(), keys.size());
    keys.clear();
    ASSERT_TRUE(ShaderPool::expandShaderSpec("mesh:Uv1", keys, &error));
    ASSERT_EQ(keys.size(), 1u);
}

TEST(ShaderKeyTest, FragmentOnlyCustomShaders){
    std::string vert, frag;
    ShaderVariantCompiler::getCustomShaderPaths("ui/fx/glow.frag", vert, frag);
    EXPECT_TRUE(vert.empty());
    EXPECT_EQ(frag, "ui/fx/glow.frag");
    EXPECT_EQ(ShaderVariantCompiler::getCustomShaderIncludeRoot("ui/fx/glow.frag"), "ui/fx");
    // a fork (two entry points) is unchanged
    ShaderVariantCompiler::getCustomShaderPaths("shaders/myMesh", vert, frag);
    EXPECT_EQ(vert, "shaders/myMesh.vert");
    EXPECT_EQ(frag, "shaders/myMesh.frag");
    // the sources of a fragment-only shader: no vertex entry point, includes resolved
    const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources("ui/fx/glow.frag",
        [](const std::string& path, std::string& content){
            if (path == "ui/fx/glow.frag"){
                content = "#version 450\n#include \"local.glsl\"\n#include \"effects/lib/jfx_noise.glsl\"\n";
                return true;
            }
            if (path == "ui/fx/local.glsl"){
                content = "float local(){ return 1.0; }\n";
                return true;
            }
            return false;
        });
    ASSERT_TRUE(sources.valid) << sources.error;
    EXPECT_TRUE(sources.vertFile.empty());
    EXPECT_TRUE(sources.files.count("local.glsl"));
    EXPECT_NE(sources.signature.find("engine:effects/lib/jfx_noise.glsl"), std::string::npos);
}

// 5.6: every variant of uieffect/all for every backend (GLSL 410, GLSL 300 es, HLSL 5, MSL 2.1
// macOS and iOS, SPIR-V), the effects' reflection checked against their manifests
TEST(EffectShaderBuildTest, EveryVariantEveryBackend){
    if (!ShaderVariantCompiler::isAvailable())
        GTEST_SKIP() << "this build has no shader compiler";
    std::vector<ShaderKey> keys;
    std::string error;
    ASSERT_TRUE(ShaderPool::expandShaderSpec("uieffect/all", keys, &error)) << error;
    for (ShaderBackend backend : ShaderPool::getShaderBackends()){
        for (ShaderKey key : keys){
            const std::string name = ShaderPool::getShaderStr(ShaderPool::getShaderTypeFromKey(key), ShaderPool::getPropertiesFromKey(key),
                                                              ShaderPool::getCustomIdFromKey(key));
            ShaderData data;
            std::string compileError, log;
            const bool ok = ShaderVariantCompiler::compile(key, backend, nullptr, data, &compileError, &log);
            EXPECT_TRUE(ok) << name << " (" << ShaderPool::getShaderLangStr(backend) << "): " << compileError << "\n" << log;
            if (!ok || backend != ShaderBackend::GLCore)
                continue;
            const std::string custom = ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(key));
            const EffectManifest* m = EffectRegistry::get().find(ShaderPool::getUiEffectName(custom));
            if (!m)
                continue;   // the internal bright pass
            std::vector<std::string> problems;
            EXPECT_TRUE(EffectManifestParser::checkReflection(*m, data, problems)) << name << ": " << (problems.empty() ? "" : problems[0]);
        }
    }
}

TEST(EffectShaderBuildTest, UnknownLibraryEffectFails){
    if (!ShaderVariantCompiler::isAvailable())
        GTEST_SKIP() << "this build has no shader compiler";
    const uint16_t id = ShaderPool::registerCustomShader("@effects/not-there");
    ShaderData data;
    std::string error;
    EXPECT_FALSE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::UI_EFFECT, 0, id), ShaderBackend::GLCore, nullptr, data, &error));
    EXPECT_NE(error.find("Unknown UI effect"), std::string::npos) << error;
    EXPECT_FALSE(ShaderVariantCompiler::compile(ShaderPool::getShaderKey(ShaderType::UI_EFFECT, 0, 0), ShaderBackend::GLCore, nullptr, data, &error));
}
