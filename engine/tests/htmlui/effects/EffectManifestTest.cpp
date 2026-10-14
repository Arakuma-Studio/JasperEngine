// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Effect manifests (phase-5 spec 3.9, 5.1 EffectManifestTest).

#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/effects/EffectManifest.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    constexpr float kPi = 3.14159265358979f;

    const char* kFull = R"MANIFEST(#version 450
// @effect     glow-test
// @kind       filter
// @category   light
// @summary    A test effect with every directive
// @source     shaders.com Glow (adapted: test)
// @cost       medium
// @backdrop   yes
// @time       speed, drift
// @pointer    ripples
// @prepass    blur size*0.5+2px threshold threshold
// @margin     size*1.5
// @suggest-for bloom halo
// @param      size length 25px 0px 100px -- Blur size
// @param      threshold percentage 50% -- Brightness threshold
// @param      speed number 1 -5 5 -- Rate
// @param      drift number 0.5 0 1 -- Second rate
// @param      angle angle 90deg 0deg 360deg -- An angle
// @param      length time 0.5s 0.1s 2s -- A time
// @param      tint color "rgb(255 0 0 / 50%)" -- A colour
// @param      center position "25% 75%" -- A position
// @param      mode enum soft soft|hard|wild -- An enum
// @param      flag boolean on -- A flag
// @param      count integer 4 1 8 -- An integer
// @param      ramp palette "#000 #fff 50% #f00" -- A palette

void main(){}
#include "effects/lib/main_filter.glsl"
)MANIFEST";

    std::string replaced(const std::string& source, const std::string& from, const std::string& to){
        std::string out = source;
        const size_t at = out.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        if (at != std::string::npos)
            out.replace(at, from.size(), to);
        return out;
    }

    // the diagnostics of parse + validate
    std::vector<EffectDiagnostic> problems(const std::string& source, bool builtin = true){
        EffectManifest m;
        std::vector<EffectDiagnostic> diags;
        if (EffectManifestParser::parse(source, "effects/glow-test.frag", builtin, m, diags))
            EffectManifestParser::validate(m, source, diags);
        return diags;
    }

    ::testing::AssertionResult reportsAt(const std::string& source, int line, const std::string& fragment){
        const std::vector<EffectDiagnostic> diags = problems(source);
        for (const EffectDiagnostic& d : diags){
            if (d.code == hc::codes::EffectManifest && (line == 0 || (int)d.line == line) && d.message.find(fragment) != std::string::npos)
                return ::testing::AssertionSuccess();
        }
        std::string all;
        for (const EffectDiagnostic& d : diags)
            all += std::to_string(d.line) + ": " + d.message + "\n";
        return ::testing::AssertionFailure() << "no effect-manifest error at line " << line << " with '" << fragment << "'; got:\n" << all;
    }

}

TEST(EffectManifestTest, EveryDirectiveAndDefault){
    EffectManifest m;
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectManifestParser::parse(kFull, "effects/glow-test.frag", true, m, diags)) << (diags.empty() ? "" : diags[0].message);
    ASSERT_TRUE(EffectManifestParser::validate(m, kFull, diags)) << (diags.empty() ? "" : diags[0].message);
    EXPECT_EQ(m.name, "glow-test");
    EXPECT_EQ(m.kind, EffectKind::Filter);
    EXPECT_EQ(m.category, "light");
    EXPECT_EQ(m.summary, "A test effect with every directive");
    EXPECT_EQ(m.source, "shaders.com Glow (adapted: test)");
    EXPECT_EQ(m.cost, EffectCost::Medium);
    EXPECT_TRUE(m.backdrop);
    EXPECT_EQ(m.timeParams, (std::vector<std::string>{"speed", "drift"}));
    EXPECT_EQ(m.pointer, EffectPointer::Ripples);
    ASSERT_TRUE(m.hasPrepass);
    EXPECT_EQ(m.prepassSigma.param, "size");
    EXPECT_FLOAT_EQ(m.prepassSigma.scale, 0.5f);
    EXPECT_FLOAT_EQ(m.prepassSigma.addPx, 2.f);
    EXPECT_EQ(m.prepassThreshold, "threshold");
    EXPECT_EQ(m.margin.param, "size");
    EXPECT_FLOAT_EQ(m.margin.scale, 1.5f);
    EXPECT_EQ(m.suggestFor, (std::vector<std::string>{"bloom", "halo"}));
    EXPECT_EQ(m.line, 2);
    ASSERT_EQ(m.params.size(), 12u);
    const EffectParamDef& size = m.params[0];
    EXPECT_EQ(size.name, "size");
    EXPECT_EQ(size.uniform, "size");
    EXPECT_EQ(size.type, EffectParamType::Length);
    EXPECT_EQ(size.defaultText, "25px");
    EXPECT_TRUE(size.hasRange);
    EXPECT_FLOAT_EQ(size.maxValue, 100.f);
    EXPECT_EQ(size.description, "Blur size");
    EXPECT_EQ(size.line, 14);
    EXPECT_FALSE(m.params[1].hasRange);
    EXPECT_NEAR(m.params[4].maxValue, 2.f * kPi, 1e-5);
    EXPECT_FLOAT_EQ(m.params[5].minValue, 0.1f);
    EXPECT_EQ(m.params[6].defaultText, "rgb(255 0 0 / 50%)");
    EXPECT_EQ(m.params[7].type, EffectParamType::Position);
    EXPECT_EQ(m.params[8].options, (std::vector<std::string>{"soft", "hard", "wild"}));
    EXPECT_EQ(m.params[11].type, EffectParamType::Palette);
    EXPECT_EQ(m.findParam("count")->type, EffectParamType::Integer);
    EXPECT_EQ(m.paramIndex("ramp"), 11);
    EXPECT_EQ(m.customShaderName(), "@effects/glow-test");
}

TEST(EffectManifestTest, ScaledGrammar){
    EffectScaledParam s;
    ASSERT_TRUE(EffectManifestParser::parseScaled("blur", s));
    EXPECT_EQ(s.param, "blur");
    EXPECT_FLOAT_EQ(s.scale, 1.f);
    EXPECT_FLOAT_EQ(s.addPx, 0.f);
    ASSERT_TRUE(EffectManifestParser::parseScaled("blur*3", s));
    EXPECT_FLOAT_EQ(s.scale, 3.f);
    ASSERT_TRUE(EffectManifestParser::parseScaled("thickness*0.5+8px", s));
    EXPECT_EQ(s.param, "thickness");
    EXPECT_FLOAT_EQ(s.scale, 0.5f);
    EXPECT_FLOAT_EQ(s.addPx, 8.f);
    ASSERT_TRUE(EffectManifestParser::parseScaled("width + 2px", s));
    EXPECT_FLOAT_EQ(s.addPx, 2.f);
    ASSERT_TRUE(EffectManifestParser::parseScaled("12px", s));
    EXPECT_TRUE(s.param.empty());
    EXPECT_FLOAT_EQ(s.addPx, 12.f);
    EXPECT_FALSE(EffectManifestParser::parseScaled("", s));
    EXPECT_FALSE(EffectManifestParser::parseScaled("blur*", s));
    EXPECT_FALSE(EffectManifestParser::parseScaled("blur+2", s));
    EXPECT_FALSE(EffectManifestParser::parseScaled("Blur*2", s));
}

TEST(EffectManifestTest, ErrorsWithLineNumbers){
    const std::string full = kFull;
    EXPECT_TRUE(reportsAt(replaced(full, "// @cost       medium", "// @colour     medium"), 7, "unknown manifest directive"));
    EXPECT_TRUE(reportsAt(replaced(full, "count integer 4", "count int 4"), 24, "unknown parameter type"));
    EXPECT_TRUE(reportsAt(replaced(full, "count integer 4 1 8", "count integer 9 1 8"), 24, "outside its range"));
    EXPECT_TRUE(reportsAt(replaced(full, "// @kind       filter\n", ""), 2, "no @kind"));
    EXPECT_TRUE(reportsAt(replaced(full, "main_filter.glsl", "main_paint.glsl"), 28, "exactly one"));
    EXPECT_TRUE(reportsAt(replaced(full, "// @param      drift", "// @param      speed"), 17, "declared twice"));
    EXPECT_TRUE(reportsAt(replaced(full, "@effect     glow-test", "@effect     blur"), 2, "CSS function"));
    EXPECT_TRUE(reportsAt(replaced(full, "@effect     glow-test", "@effect     _glow"), 2, "not an effect name"));
    EXPECT_TRUE(reportsAt(replaced(full, "angle angle 90deg", "angle angle ninety"), 18, "default of 'angle'"));
    EXPECT_TRUE(reportsAt(replaced(full, "mode enum soft soft|hard|wild", "opacity number 1"), 22, "universal parameter"));
    EXPECT_TRUE(reportsAt(replaced(full, "// @time       speed, drift", "// @time       speed, missing"), 0, "'missing'"));
    EXPECT_TRUE(reportsAt(replaced(full, "// @category   light", "// @category   shiny"), 0, "unknown @category"));
    EXPECT_TRUE(reportsAt(replaced(full, "// @prepass    blur size*0.5+2px threshold threshold", "// @prepass    glow size"), 11, "@prepass is"));
    // a paint effect cannot have filter directives
    std::string paint = replaced(full, "// @kind       filter", "// @kind       paint");
    paint = replaced(paint, "main_filter.glsl", "main_paint.glsl");
    EXPECT_TRUE(reportsAt(paint, 0, "@backdrop is for filter effects"));
    EXPECT_TRUE(reportsAt(paint, 0, "@prepass is for filter effects"));
    // a project effect needs no @source, a built-in one does
    const std::string noSource = replaced(full, "// @source     shaders.com Glow (adapted: test)\n", "");
    EXPECT_TRUE(reportsAt(noSource, 0, "no @source"));
    EXPECT_TRUE(problems(noSource, false).empty());
}

TEST(EffectManifestTest, BlockAndDetection){
    EXPECT_TRUE(EffectManifestParser::hasEffectManifest(kFull));
    EXPECT_FALSE(EffectManifestParser::hasEffectManifest("#version 450\n// a post-process fork\nvoid main(){}\n"));
    EXPECT_FALSE(EffectManifestParser::hasEffectManifest("// @effect x\n"));
    const std::string block = EffectManifestParser::manifestBlock(kFull);
    EXPECT_EQ(block.find("// @effect     glow-test\n"), 0u);
    EXPECT_NE(block.find("// @param      ramp"), std::string::npos);
    EXPECT_EQ(block.find("void main"), std::string::npos);
    EffectManifest m;
    std::vector<EffectDiagnostic> diags;
    EXPECT_FALSE(EffectManifestParser::parse("void main(){}", "x.frag", true, m, diags));
    ASSERT_FALSE(diags.empty());
    EXPECT_EQ(diags[0].file, "x.frag");
}
