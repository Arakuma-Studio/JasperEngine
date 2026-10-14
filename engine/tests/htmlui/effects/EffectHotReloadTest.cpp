// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Hot reload of a project effect (phase-5 spec 3.10, p5-tooling): a changed .frag is read again
// and its variants dropped (the editor's listener is told); a .frag that is gone or whose manifest
// no longer parses forgets the declaration, so the next @effect reports why
// (effect-file-missing, effect-manifest at the .frag line) and declares it again once fixed.
// The editor side (UIDocumentSystem::reloadChangedFiles, the auto-sync, write_file) is covered by
// tools/mcp/tests/test_html_ui_effects.py.

#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/effects/EffectManifest.h"
#include "htmlui/effects/EffectRegistry.h"

#include "Log.h"
#include "harness/TestSystem.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::htmlui;
using namespace jasperengine::test;
namespace hc = jasperengine::htmlui::compat;

namespace {

    const char* kPath = "@htmlui/test/effects/fx/hot-glow.frag";

    std::string frag(const std::string& defaultIntensity){
        return "#version 450\n"
               "// @effect     hot-glow\n"
               "// @kind       paint\n"
               "// @category   light\n"
               "// @summary    A project effect of the hot reload tests\n"
               "// @source     original\n"
               "// @cost       low\n"
               "// @param      intensity number " + defaultIntensity + " 0 1 -- Strength\n"
               "\n"
               "#include \"effects/lib/jfx_common.glsl\"\n"
               "uniform u_fs_effectParams { float intensity; } params;\n"
               "vec4 jfx_paint(JfxContext c){ return vec4(1.0, 1.0, 1.0, params.intensity); }\n"
               "#include \"effects/lib/main_paint.glsl\"\n";
    }

    bool hasCode(const std::vector<EffectDiagnostic>& diags, hc::DiagCode code, uint32_t line = 0){
        for (const EffectDiagnostic& d : diags){
            if (d.code == code && (line == 0 || d.line == line))
                return true;
        }
        return false;
    }

    struct EffectHotReload : ::testing::Test {
        std::vector<std::string> told;
        void SetUp() override{
            hc::VirtualFiles::add(kPath, frag("0.5"));
            EffectRegistry::get().setProjectEffectListener([this](const std::string& path){ told.push_back(path); });
        }
        void TearDown() override{
            EffectRegistry::get().setProjectEffectListener(nullptr);
            EffectRegistry::get().clearProjectEffects();
            hc::VirtualFiles::remove(kPath);
        }
        bool declare(std::vector<EffectDiagnostic>& diags){
            return EffectRegistry::get().declareProjectEffect("hot-glow", kPath, "ui/hot.css", 1, diags);
        }
    };

}

TEST_F(EffectHotReload, ChangedFileIsReadAgainAndTheEditorIsTold){
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(declare(diags));
    ASSERT_NE(EffectRegistry::get().find("hot-glow"), nullptr);
    EXPECT_EQ(EffectRegistry::get().find("hot-glow")->params.at(0).defaultText, "0.5");
    hc::VirtualFiles::add(kPath, frag("0.25"));
    EXPECT_FALSE(EffectRegistry::get().onProjectFileChanged("@htmlui/test/effects/fx/other.frag")) << "not a declared effect";
    EXPECT_TRUE(told.empty());
    ASSERT_TRUE(EffectRegistry::get().onProjectFileChanged(kPath));
    ASSERT_NE(EffectRegistry::get().find("hot-glow"), nullptr);
    EXPECT_EQ(EffectRegistry::get().find("hot-glow")->params.at(0).defaultText, "0.25") << "the manifest is read again";
    EXPECT_EQ(told, std::vector<std::string>{kPath}) << "the listener (the editor's compiled cache) is told";
}

TEST_F(EffectHotReload, BrokenManifestIsForgottenAndReportedByTheNextDeclaration){
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(declare(diags));
    TestSystem::get().clearLogs();
    hc::VirtualFiles::add(kPath, frag("3"));   // out of its range
    ASSERT_TRUE(EffectRegistry::get().onProjectFileChanged(kPath));
    EXPECT_EQ(EffectRegistry::get().find("hot-glow"), nullptr) << "a broken effect is forgotten";
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 1u) << "the Console says why";
    EXPECT_EQ(told.size(), 1u);
    // the document reloading declares it again: effect-manifest at the @param line
    diags.clear();
    EXPECT_FALSE(declare(diags));
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectManifest, 8)) << "effect-manifest at the @param line (8)";
    // fixed: declared again
    hc::VirtualFiles::add(kPath, frag("0.75"));
    diags.clear();
    EXPECT_TRUE(declare(diags));
    ASSERT_NE(EffectRegistry::get().find("hot-glow"), nullptr);
    EXPECT_EQ(EffectRegistry::get().find("hot-glow")->params.at(0).defaultText, "0.75");
}

TEST_F(EffectHotReload, DeletedFileIsForgottenAndReportedMissing){
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(declare(diags));
    hc::VirtualFiles::remove(kPath);
    ASSERT_TRUE(EffectRegistry::get().onProjectFileChanged(kPath));
    EXPECT_EQ(EffectRegistry::get().find("hot-glow"), nullptr) << "a deleted effect is forgotten";
    diags.clear();
    EXPECT_FALSE(declare(diags));
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectFileMissing)) << "effect-file-missing";
    // back (a rename undone): declared again
    hc::VirtualFiles::add(kPath, frag("0.5"));
    diags.clear();
    EXPECT_TRUE(declare(diags));
    // a changed file nothing declares any more is not an effect's
    EffectRegistry::get().clearProjectEffects();
    EXPECT_FALSE(EffectRegistry::get().onProjectFileChanged(kPath));
}

TEST_F(EffectHotReload, StaleDeclarationIsReplaced){
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(declare(diags));
    const char* renamed = "@htmlui/test/effects/fx/hot-glow-2.frag";
    hc::VirtualFiles::add(renamed, frag("0.5"));
    // both files exist: another src is a conflict, the first declaration is used
    diags.clear();
    EXPECT_FALSE(EffectRegistry::get().declareProjectEffect("hot-glow", renamed, "ui/hot.css", 1, diags));
    EXPECT_TRUE(hasCode(diags, hc::codes::EffectDeclareConflict));
    // renamed (the first file is gone) and the CSS follows: the new src replaces the stale one
    hc::VirtualFiles::remove(kPath);
    diags.clear();
    EXPECT_TRUE(EffectRegistry::get().declareProjectEffect("hot-glow", renamed, "ui/hot.css", 1, diags));
    ASSERT_NE(EffectRegistry::get().find("hot-glow"), nullptr);
    EXPECT_EQ(EffectRegistry::get().find("hot-glow")->sourcePath, renamed);
    EffectRegistry::get().clearProjectEffects();
    hc::VirtualFiles::remove(renamed);
}

TEST_F(EffectHotReload, MissingFilesAreForgotten){
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(declare(diags));
    EXPECT_EQ(EffectRegistry::get().forgetMissingProjectEffects(), 0u) << "the file exists";
    ASSERT_NE(EffectRegistry::get().find("hot-glow"), nullptr);
    hc::VirtualFiles::remove(kPath);
    EXPECT_EQ(EffectRegistry::get().forgetMissingProjectEffects(), 1u);
    EXPECT_EQ(EffectRegistry::get().find("hot-glow"), nullptr) << "a declaration whose file is gone is forgotten";
}
