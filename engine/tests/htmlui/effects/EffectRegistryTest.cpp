// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The effect registry (phase-5 spec 3.6, 3.10; 5.1 EffectRegistryTest).

#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/effects/EffectManifest.h"
#include "htmlui/effects/EffectRegistry.h"

#include "HtmlUI.h"
#include "harness/TestSystem.h"
#include "pool/ShaderPool.h"

#include <gtest/gtest.h>

#include <set>

using namespace jasperengine;
using namespace jasperengine::htmlui;
using namespace jasperengine::test;
namespace hc = jasperengine::htmlui::compat;

namespace {

    const char* kProject = "effects/fx/tint-wave.frag";

    bool hasCode(const std::vector<EffectDiagnostic>& diags, hc::DiagCode code){
        for (const EffectDiagnostic& d : diags){
            if (d.code == code)
                return true;
        }
        return false;
    }

    struct ProjectEffects : ::testing::Test {
        void TearDown() override{
            EffectRegistry::get().clearProjectEffects();
        }
    };

}

TEST(EffectRegistryTest, BuiltinsAreUniqueAndSorted){
    const std::vector<const EffectManifest*> all = EffectRegistry::get().builtins();
    ASSERT_GE(all.size(), 5u);
    std::set<std::string> names;
    for (const EffectManifest* m : all){
        EXPECT_TRUE(names.insert(m->name).second) << m->name;
        EXPECT_TRUE(m->builtin);
        EXPECT_EQ(EffectRegistry::get().find(m->name), m);
    }
    for (const char* name : {"stripes", "spotlight", "cursor-trail", "pixelate", "frosted-glass"})
        EXPECT_NE(EffectRegistry::get().find(name), nullptr) << name;
    EXPECT_TRUE(std::is_sorted(all.begin(), all.end(), [](const EffectManifest* a, const EffectManifest* b){ return a->name < b->name; }));
    EXPECT_EQ(EffectRegistry::get().find("_bright"), nullptr);
}

TEST(EffectRegistryTest, Suggestions){
    const std::vector<std::string> glass = EffectRegistry::get().suggestions("glassmorphism");
    ASSERT_FALSE(glass.empty());
    EXPECT_EQ(glass.front(), "frosted-glass");
    const std::vector<std::string> typo = EffectRegistry::get().suggestions("stripse");
    ASSERT_FALSE(typo.empty());
    EXPECT_EQ(typo.front(), "stripes");
    EXPECT_TRUE(EffectRegistry::get().suggestions("zzzzzzzzzzzz").empty());
}

TEST_F(ProjectEffects, DeclareFromAFile){
    TestSystem::get();   // the fixtures are the assets
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectRegistry::get().declareProjectEffect("tint-wave", kProject, "ui/a.css", 3, diags))
        << (diags.empty() ? "" : diags[0].message);
    const EffectManifest* m = EffectRegistry::get().find("tint-wave");
    ASSERT_NE(m, nullptr);
    EXPECT_FALSE(m->builtin);
    EXPECT_EQ(m->sourcePath, kProject);
    EXPECT_EQ(m->customShaderName(), kProject);
    EXPECT_EQ(EffectRegistry::get().findByCustomShader(kProject), m);
    // listed with the built-in ones, in name order
    const std::vector<const EffectManifest*> all = EffectRegistry::get().all();
    EXPECT_NE(std::find(all.begin(), all.end(), m), all.end());
    // the same declaration again is fine
    EXPECT_TRUE(EffectRegistry::get().declareProjectEffect("tint-wave", kProject, "ui/b.css", 7, diags));
    EXPECT_TRUE(diags.empty());
}

TEST_F(ProjectEffects, ConflictsMissingFilesAndManifests){
    std::vector<EffectDiagnostic> diags;
    // a built-in name
    EXPECT_FALSE(EffectRegistry::get().declareProjectEffect("stripes", kProject, "ui/a.css", 1, diags));
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectDeclareConflict));
    // another src for a declared name
    diags.clear();
    ASSERT_TRUE(EffectRegistry::get().declareProjectEffect("tint-wave", kProject, "ui/a.css", 1, diags));
    EXPECT_FALSE(EffectRegistry::get().declareProjectEffect("tint-wave", "effects/fx/other.frag", "ui/b.css", 2, diags));
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectDeclareConflict));
    // a missing file
    diags.clear();
    EXPECT_FALSE(EffectRegistry::get().declareProjectEffect("ghost", "effects/fx/ghost.frag", "ui/a.css", 1, diags));
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectFileMissing));
    // a manifest error, at the .frag's line
    diags.clear();
    EXPECT_FALSE(EffectRegistry::get().declareProjectEffect("bad-manifest", "effects/fx/bad-manifest.frag", "ui/a.css", 1, diags));
    ASSERT_TRUE(hasCode(diags, hc::codes::EffectManifest));
    EXPECT_EQ(diags[0].file, "effects/fx/bad-manifest.frag");
    EXPECT_EQ(diags[0].line, 5u);
    // the manifest names another effect
    diags.clear();
    EXPECT_FALSE(EffectRegistry::get().declareProjectEffect("tint-wave-2", kProject, "ui/a.css", 1, diags));
    ASSERT_TRUE(hasCode(diags, hc::codes::EffectManifest));
    EXPECT_NE(diags.back().message.find("@effect tint-wave"), std::string::npos) << diags.back().message;
    EXPECT_EQ(diags.back().line, 2u);
    EXPECT_EQ(EffectRegistry::get().find("tint-wave-2"), nullptr);
}

TEST_F(ProjectEffects, ChangedFileKeepsTheOldManifestAlive){
    ASSERT_TRUE(HtmlUI::acquire());
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectRegistry::get().declareProjectEffect("tint-wave", kProject, "ui/a.css", 1, diags));
    const EffectManifest* before = EffectRegistry::get().find("tint-wave");
    EXPECT_FALSE(EffectRegistry::get().onProjectFileChanged("ui/unrelated.css"));
    ASSERT_TRUE(EffectRegistry::get().onProjectFileChanged(kProject));
    const EffectManifest* after = EffectRegistry::get().find("tint-wave");
    ASSERT_NE(after, nullptr);
    EXPECT_NE(after, before);
    // the old manifest and the replaced instancers are kept until shutdown (RmlUi may hold them)
    EXPECT_EQ(before->name, "tint-wave");
    HtmlUI::release();
}

TEST(EffectRegistryTest, ShaderKeysFollowTheCustomShaderGeneration){
    const EffectManifest* m = EffectRegistry::get().find("stripes");
    ASSERT_NE(m, nullptr);
    const ShaderKey paint = EffectRegistry::get().shaderKey(*m, false);
    const ShaderKey filter = EffectRegistry::get().shaderKey(*m, true);
    EXPECT_EQ(ShaderPool::getShaderTypeFromKey(paint), ShaderType::UI_EFFECT);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(paint), 0u);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(filter), 1u);
    EXPECT_EQ(ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(paint)), "@effects/stripes");
    // a reset registry (another project): the id is registered again
    const uint32_t generation = ShaderPool::getCustomShaderGeneration();
    ShaderPool::clearUnused();
    EXPECT_GT(ShaderPool::getCustomShaderGeneration(), generation);
    EXPECT_EQ(ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(paint)), "");
    const ShaderKey again = EffectRegistry::get().shaderKey(*m, false);
    EXPECT_EQ(ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(again)), "@effects/stripes");
    const ShaderKey bright = EffectRegistry::get().brightPassKey();
    EXPECT_EQ(ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(bright)), "@effects/_bright");
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(bright), 1u);
}
