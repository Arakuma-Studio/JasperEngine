// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// effect() and @effect through the compatibility layer (phase-5 spec 3.2, 3.5, 3.13; 5.1
// EffectCompatTest): the exact RCSS of each place effect() goes, with the fork's native
// background layers and without them, gradient text, blend modes, and every diagnostic.

#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/effects/EffectRegistry.h"

#include "harness/TestSystem.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::htmlui;
using namespace jasperengine::htmlui::compat;

namespace {

    struct Rewritten {
        std::string rcss;
        std::vector<Diagnostic> diagnostics;
        std::vector<std::string> dependencies;

        bool has(const std::string& text) const { return rcss.find(text) != std::string::npos; }
        const Diagnostic* find(DiagCode code) const {
            for (const Diagnostic& d : diagnostics){
                if (d.code == code)
                    return &d;
            }
            return nullptr;
        }
        std::string text() const {
            std::string all;
            for (const Diagnostic& d : diagnostics)
                all += toText(d) + "\n";
            return all;
        }
    };

    // the fixtures directory (project effects of the tests)
    bool readFixture(const std::string& path, std::string& content){
        std::ifstream file(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/" + path, std::ios::binary);
        if (!file)
            return false;
        std::ostringstream stream;
        stream << file.rdbuf();
        content = stream.str();
        return true;
    }

    Rewritten rewrite(const std::string& css, const ForkFeatures& fork = ForkFeatures::present()){
        SourceFiles files;
        const uint32_t file = files.add("effects/menu.css", css);
        DiagnosticSink sink(files);
        CompatOptions options;
        options.fork = fork;
        RewriteContext context;
        context.files.read = readFixture;
        context.files.exists = [](const std::string& path){ std::string content; return readFixture(path, content); };
        CssRewriter rewriter(options, files, sink, context);
        StylesheetInput input;
        input.file = file;
        input.text = files.text(file);
        const RewriteResult result = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
        Rewritten out;
        for (const StylesheetOutput& sheet : result.sheets)
            out.rcss += sheet.rcss;
        out.diagnostics = sink.diagnostics();
        out.dependencies = result.dependencies;
        return out;
    }

    ForkFeatures legacy(){
        return ForkFeatures::present().with(ForkFeature::BackgroundLayers, false);
    }

    const char* kStripes = "jfx-stripes(v(#000000ff) v(#ffffffff) v(45deg) v(5) v(0.5) v(0) v(0.2) v(0) v(linear) v(100%) v(1) v(0s) v(normal))";

    struct EffectCompatTest : ::testing::Test {
        void TearDown() override{
            EffectRegistry::get().clearProjectEffects();
        }
    };

}

TEST_F(EffectCompatTest, BackgroundImageNative){
    const Rewritten r = rewrite(".a { background-image: effect(stripes); }");
    EXPECT_TRUE(r.has(std::string("background-image: ") + kStripes + ";")) << r.rcss;
    EXPECT_EQ(r.find(codes::UnsupportedFunction), nullptr) << r.text();
    EXPECT_TRUE(r.diagnostics.empty()) << r.text();
}

TEST_F(EffectCompatTest, LayersKeepTheirOrder){
    const Rewritten r = rewrite(".a { background: effect(stripes, speed 0), linear-gradient(#000, #fff), url(\"img.png\") #101218; }");
    const size_t effect = r.rcss.find("jfx-stripes(");
    const size_t gradient = r.rcss.find("linear-gradient(");
    const size_t image = r.rcss.find("url(");
    ASSERT_NE(effect, std::string::npos) << r.rcss;
    ASSERT_NE(gradient, std::string::npos) << r.rcss;
    ASSERT_NE(image, std::string::npos) << r.rcss;
    EXPECT_LT(effect, gradient);
    EXPECT_LT(gradient, image);
    EXPECT_TRUE(r.has("background-color: #101218ff;")) << r.rcss;
}

TEST_F(EffectCompatTest, LegacyDecoratorsWithPaintAreas){
    const Rewritten r = rewrite(".a { background: effect(stripes), linear-gradient(#000, #fff); background-clip: padding-box, border-box; }", legacy());
    EXPECT_TRUE(r.has(std::string("decorator: ") + kStripes + " padding-box, linear-gradient(")) << r.rcss;
    const Rewritten single = rewrite(".b { background-image: effect(spotlight); }", legacy());
    EXPECT_TRUE(single.has(") border-box")) << single.rcss;
}

TEST_F(EffectCompatTest, BlendModes){
    Rewritten r = rewrite(".a { background-image: effect(stripes); background-blend-mode: screen; }");
    EXPECT_TRUE(r.has("v(0s) v(screen))")) << r.rcss;
    EXPECT_EQ(r.find(codes::UnsupportedProperty), nullptr) << r.text();
    r = rewrite(".a { background-image: effect(stripes), effect(spotlight); background-blend-mode: plus-lighter, multiply; }");
    EXPECT_TRUE(r.has("v(0s) v(add)), jfx-spotlight(")) << r.rcss;
    EXPECT_TRUE(r.has("v(0s) v(multiply))")) << r.rcss;
    r = rewrite(".a { background-image: effect(stripes); background-blend-mode: difference; }");
    const Diagnostic* d = r.find(codes::EffectUnsupportedBlend);
    ASSERT_NE(d, nullptr) << r.text();
    EXPECT_EQ(d->severity, Severity::Warning);
    EXPECT_TRUE(r.has("v(normal))")) << r.rcss;
    // an explicit blend argument wins
    r = rewrite(".a { background-image: effect(stripes, blend multiply); background-blend-mode: screen; }");
    EXPECT_TRUE(r.has("v(multiply))")) << r.rcss;
    // phase 2's report stays for a rule without effect layers
    r = rewrite(".a { background-image: linear-gradient(#000, #fff); background-blend-mode: screen; }");
    EXPECT_NE(r.find(codes::UnsupportedProperty), nullptr) << r.text();
}

TEST_F(EffectCompatTest, GradientTextFill){
    Rewritten r = rewrite(".t { color: transparent; background-image: effect(stripes), effect(spotlight); background-clip: text; }");
    // both layers fill the text, bottom layer first; the text is drawn white for the fill
    const size_t fill = r.rcss.find("-jfx-text-fill: jfx-spotlight(");
    ASSERT_NE(fill, std::string::npos) << r.rcss;
    EXPECT_NE(r.rcss.find(" jfx-stripes(", fill), std::string::npos) << r.rcss;
    EXPECT_EQ(r.rcss.find("-jfx-text-fill: jfx-stripes(v(#000000ff) v(#ffffffff) v(45deg) v(5) v(0.5) v(0) v(0.2) v(0) v(linear) v(1) v(1) v(0s) v(normal)) border-box"),
              std::string::npos);
    EXPECT_TRUE(r.has("color: #ffffffff;")) << r.rcss;
    EXPECT_EQ(r.rcss.find("color: #00000000"), std::string::npos) << r.rcss;
    EXPECT_TRUE(r.has("background-image: none, none;")) << r.rcss;
    const Diagnostic* info = r.find(codes::EffectTextFill);
    ASSERT_NE(info, nullptr) << r.text();
    EXPECT_EQ(info->severity, Severity::Info);
    EXPECT_EQ(r.find(codes::ApproximatedVisible), nullptr) << r.text();
    // -webkit-text-fill-color and the shorthand's text keyword do the same
    r = rewrite(".t { -webkit-text-fill-color: transparent; background: effect(stripes) text; }");
    EXPECT_TRUE(r.has("-jfx-text-fill: jfx-stripes(")) << r.rcss;
    // without the fork's text fill: phase 2's solid colour
    r = rewrite(".t { color: transparent; background-image: effect(stripes); background-clip: text; }",
                ForkFeatures::present().with(ForkFeature::TextFillFilter, false));
    EXPECT_FALSE(r.has("-jfx-text-fill")) << r.rcss;
    EXPECT_NE(r.find(codes::ApproximatedVisible), nullptr) << r.text();
    // an opaque text colour: the clip is dropped, as in CSS
    r = rewrite(".t { color: #fff; background-image: effect(stripes); background-clip: text; }");
    EXPECT_FALSE(r.has("-jfx-text-fill")) << r.rcss;
    EXPECT_TRUE(r.has("background-image: jfx-stripes(")) << r.rcss;
}

TEST_F(EffectCompatTest, MaskImage){
    const Rewritten r = rewrite(".m { mask-image: effect(spotlight, radius 40px); }\n.n { -webkit-mask-image: effect(stripes); }");
    EXPECT_TRUE(r.has("mask-image: jfx-spotlight(v(#ffffffff) v(40px)")) << r.rcss;
    EXPECT_TRUE(r.has("mask-image: jfx-stripes(")) << r.rcss << r.text();
    const Rewritten wrong = rewrite(".m { mask-image: effect(pixelate); }");
    EXPECT_NE(wrong.find(codes::EffectWrongContext), nullptr) << wrong.text();
}

TEST_F(EffectCompatTest, FilterLists){
    Rewritten r = rewrite(".f { filter: blur(2px) effect(pixelate, size 4px) drop-shadow(0 2px 4px #0008); }");
    EXPECT_TRUE(r.has("filter: blur(2px) jfx-pixelate(v(4px) v(0) v(0) v(100%) v(1) v(0s)) drop-shadow(#00000088 0px 2px 4px);")) << r.rcss;
    r = rewrite(".p { backdrop-filter: effect(frosted-glass, blur 14px, tint #ffffff22); }");
    EXPECT_TRUE(r.has("backdrop-filter: jfx-frosted-glass(v(14px) v(#ffffff22) v(1.2) v(1) v(0.04) v(0.35) v(300deg) v(100%) v(1) v(0s));")) << r.rcss;
    // a paint effect in filter form keeps its blend slot
    r = rewrite(".f { filter: effect(stripes, blend screen); }");
    EXPECT_TRUE(r.has("filter: jfx-stripes(")) << r.rcss;
    EXPECT_TRUE(r.has("v(0s) v(screen));")) << r.rcss;
    EXPECT_TRUE(r.diagnostics.empty()) << r.text();
}

TEST_F(EffectCompatTest, Transitions){
    Rewritten r = rewrite(".b { background-image: effect(stripes, speed 0); transition: background-image 300ms; }");
    // the fork's background layers transition natively: the property keeps its name
    EXPECT_TRUE(r.has("transition: background-image 0.3s;")) << r.rcss;
    r = rewrite(".b { background-image: effect(stripes, speed 0); transition-property: background-image; transition-duration: 0.3s; }", legacy());
    EXPECT_TRUE(r.has("decorator")) << r.rcss;
    EXPECT_EQ(r.rcss.find("transition-property: background-image"), std::string::npos) << r.rcss;
}

TEST_F(EffectCompatTest, AtEffectIsHoisted){
    const Rewritten r = rewrite(".a { background-image: effect(tint-wave, speed 2); }\n"
                                "@effect tint-wave { src: url(\"fx/tint-wave.frag\"); }\n");
    EXPECT_TRUE(r.has("background-image: jfx-tint-wave(v(#3388ffff) v(2) v(100%) v(1) v(0s) v(normal));")) << r.rcss << r.text();
    EXPECT_EQ(r.rcss.find("@effect"), std::string::npos) << r.rcss;
    EXPECT_NE(std::find(r.dependencies.begin(), r.dependencies.end(), "effects/fx/tint-wave.frag"), r.dependencies.end());
    EXPECT_TRUE(r.diagnostics.empty()) << r.text();
}

TEST_F(EffectCompatTest, DiagnosticsWithPositions){
    const std::string css =
        ".a { background-image: effect(glassmorphism); }\n"       // 1
        ".b { background-image: effect(stripes, speeed 2); }\n"    // 2
        ".c { color: effect(stripes); }\n"                         // 3
        ".d { background-image: effect(stripes, 3); }\n"           // 4
        ".e { background-image: effect(stripes, density x); }\n"   // 5
        ".f { background-image: effect(stripes, density 99); }\n"  // 6
        ".g { background-image: effect(stripes, speed 1, speed 2); }\n"   // 7
        ".h { filter: effect(pixelate, blend add); }\n"            // 8
        ".i { background-image: effect(stripes); background-size: 10px; background-position: 5px 5px; background-repeat: no-repeat; }\n"   // 9
        ".j { background-image: effect(pixelate); }\n";            // 10
    const Rewritten r = rewrite(css);
    auto at = [&](DiagCode code, uint32_t line, uint32_t column, Severity severity) -> const Diagnostic* {
        for (const Diagnostic& d : r.diagnostics){
            if (d.code == code && d.line == line){
                EXPECT_EQ(d.column, column) << toText(d);
                EXPECT_EQ(d.severity, severity) << toText(d);
                EXPECT_EQ(d.file, "effects/menu.css");
                return &d;
            }
        }
        ADD_FAILURE() << code.id() << " not reported on line " << line << "\n" << r.text();
        return nullptr;
    };
    const Diagnostic* unknown = at(codes::EffectUnknown, 1, 31, Severity::Error);
    if (unknown){
        EXPECT_NE(unknown->suggestion.find("frosted-glass"), std::string::npos) << unknown->suggestion;
        EXPECT_NE(unknown->suggestion.find("jasper://docs/html-ui-effects"), std::string::npos);
    }
    const Diagnostic* param = at(codes::EffectUnknownParam, 2, 40, Severity::Warning);
    if (param){
        EXPECT_EQ(param->suggestion, "speed");
    }
    const Diagnostic* wrong = at(codes::EffectWrongContext, 3, 13, Severity::Error);
    if (wrong){
        EXPECT_NE(wrong->suggestion.find("background-image"), std::string::npos);
    }
    at(codes::EffectPositionalArg, 4, 40, Severity::Error);
    at(codes::EffectBadValue, 5, 48, Severity::Warning);
    at(codes::EffectOutOfRange, 6, 48, Severity::Warning);
    at(codes::EffectDuplicateParam, 7, 40, Severity::Warning);
    at(codes::EffectBlendOnFilter, 8, 31, Severity::Warning);
    at(codes::EffectIgnoredBackgroundProp, 9, 31, Severity::Warning);
    at(codes::EffectWrongContext, 10, 31, Severity::Error);
    // the text format (check --ui, the Console)
    if (param){
        EXPECT_EQ(toText(*param), "effects/menu.css:2:40: warning: 'stripes' has no parameter 'speeed'; it is ignored (fix: speed) [HTMLUI-FX-0004]");
    }
}

TEST_F(EffectCompatTest, WrongContextNamesTheCounterpart){
    const Rewritten r = rewrite("@effect glints-test { src: url(\"fx/glints-test.frag\"); }\n"
                                "@effect sparkle-test { src: url(\"fx/sparkle-test.frag\"); }\n"
                                ".a { background-image: effect(sparkle-test); }\n"
                                ".b { filter: effect(sparkle-test, amount 0.2) effect(glints-test); }\n");
    const Diagnostic* d = r.find(codes::EffectWrongContext);
    ASSERT_NE(d, nullptr) << r.text();
    EXPECT_EQ(d->line, 3u);
    EXPECT_NE(d->suggestion.find("glints-test"), std::string::npos) << d->suggestion;
    EXPECT_TRUE(r.has("filter: jfx-sparkle-test(v(0.2) v(100%) v(1) v(0s)) jfx-glints-test(")) << r.rcss;
}

TEST_F(EffectCompatTest, AtEffectDiagnostics){
    const Rewritten r = rewrite("@effect stripes { src: url(\"fx/tint-wave.frag\"); }\n"           // 1: a built-in name
                                "@effect ghost { src: url(\"fx/ghost.frag\"); }\n"                 // 2: missing
                                "@effect bad-manifest { src: url(\"fx/bad-manifest.frag\"); }\n"   // 3: manifest error
                                "@effect tint-wave { src: url(\"fx/tint-wave.frag\"); }\n"
                                "@effect tint-wave { src: url(\"fx/glints-test.frag\"); }\n"       // 5: another src
                                "@effect Bad_Name { src: url(\"fx/tint-wave.frag\"); }\n");        // 6: not a name
    auto lineOf = [&](DiagCode code, const std::string& file, uint32_t line){
        for (const Diagnostic& d : r.diagnostics){
            if (d.code == code && d.file == file && d.line == line)
                return true;
        }
        ADD_FAILURE() << code.id() << " not at " << file << ":" << line << "\n" << r.text();
        return false;
    };
    lineOf(codes::EffectDeclareConflict, "effects/menu.css", 1);
    lineOf(codes::EffectFileMissing, "effects/menu.css", 2);
    lineOf(codes::EffectManifest, "effects/fx/bad-manifest.frag", 5);
    lineOf(codes::EffectDeclareConflict, "effects/menu.css", 5);
    lineOf(codes::EffectDeclareConflict, "effects/menu.css", 6);
}

TEST_F(EffectCompatTest, ManyHighCostEffects){
    std::string css = "@effect heavy-test { src: url(\"fx/heavy-test.frag\"); }\n";
    for (int i = 0; i < 5; i++)
        css += ".h" + std::to_string(i) + " { background-image: effect(heavy-test); }\n";
    const Rewritten r = rewrite(css);
    const Diagnostic* d = r.find(codes::EffectCost);
    ASSERT_NE(d, nullptr) << r.text();
    EXPECT_EQ(d->severity, Severity::Info);
    EXPECT_EQ(d->line, 6u);
    const Rewritten four = rewrite("@effect heavy-test { src: url(\"fx/heavy-test.frag\"); }\n.a { background-image: effect(heavy-test); }\n");
    EXPECT_EQ(four.find(codes::EffectCost), nullptr);
}

TEST_F(EffectCompatTest, PaintSuggestion){
    const Rewritten r = rewrite(".a { background-image: paint(stripes); }");
    const Diagnostic* d = r.find(codes::UnsupportedFunction);
    ASSERT_NE(d, nullptr) << r.text();
    EXPECT_EQ(d->suggestion, "write effect(stripes)");
    const Rewritten other = rewrite(".a { background-image: paint(checkerboard); }");
    ASSERT_NE(other.find(codes::UnsupportedFunction), nullptr);
    EXPECT_EQ(other.find(codes::UnsupportedFunction)->suggestion, "use url() or a gradient");
}

TEST_F(EffectCompatTest, CustomPropertiesTypedThroughTheHandler){
    const Rewritten r = rewrite(":root { --bg: effect(stripes, speed 0); }\n.a { background: var(--bg); }\n");
    EXPECT_TRUE(r.has("jfx-stripes(")) << r.rcss;
    EXPECT_EQ(r.rcss.find("effect(stripes"), std::string::npos) << r.rcss;
}

TEST_F(EffectCompatTest, LuaStylePath){
    SourceFiles files;
    DiagnosticSink sink(files);
    const std::string rcss = CssRewriter::rewriteDeclarations("background-image: effect(stripes, speed 0.5); filter: effect(pixelate)", &sink);
    EXPECT_NE(rcss.find("background-image: jfx-stripes("), std::string::npos) << rcss;
    EXPECT_NE(rcss.find("filter: jfx-pixelate("), std::string::npos) << rcss;
    const std::string bad = CssRewriter::rewriteDeclarations("background-image: effect(stripes, speeed 2)", &sink);
    bool reported = false;
    for (const Diagnostic& d : sink.diagnostics())
        reported = reported || d.code == codes::EffectUnknownParam;
    EXPECT_TRUE(reported);
}

TEST_F(EffectCompatTest, EveryCodeIsRegistered){
    // HTMLUI-FX-0001 ... 0016 in the order of the spec's table, with their names
    const char* names[] = {"effect-unknown", "effect-wrong-context", "effect-positional-arg", "effect-unknown-param", "effect-bad-value",
                           "effect-out-of-range", "effect-duplicate-param", "effect-blend-on-filter", "effect-ignored-background-prop",
                           "effect-unsupported-blend", "effect-text-fill", "effect-declare-conflict", "effect-file-missing",
                           "effect-manifest", "effect-shader-compile", "effect-cost"};
    for (int i = 0; i < 16; i++){
        const DiagCodeInfo* info = findDiagnosticCode(names[i]);
        ASSERT_NE(info, nullptr) << names[i];
        EXPECT_EQ(info->code.area, DiagArea::FX);
        EXPECT_EQ(info->code.number, i + 1);
        EXPECT_NE(std::string(info->help).size(), 0u);
    }
    EXPECT_EQ(findDiagnosticCode("HTMLUI-FX-0001")->code, codes::EffectUnknown);
    EXPECT_EQ(std::string(diagAreaName(DiagArea::FX)), "FX");
}
