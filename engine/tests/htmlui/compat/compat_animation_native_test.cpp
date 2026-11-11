// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ForkFeature::TimingFunctions, AnimationLonghands and IndividualTransforms (phase 3 p3-animation,
// docs/html-ui/FORK.md P3-A1..P3-A5). With them compat writes transitions, animations, a keyframe's
// own easing, @starting-style and translate / rotate / scale as declared (normalized), and RmlUi
// reads its output to the same computed lists; forced absent, phase 2's rewrites are still there
// (merged shorthands, nearest tweens, mirrored keyframes, -html-animation-fill, merged transform).

#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"

#include "HtmlUI.h"
#include "RmlSystem.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Animation.h>
#include <RmlUi/Core/ComputedValues.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    struct Rewritten {
        std::string rcss;
        std::vector<Diagnostic> diagnostics;

        bool has(const std::string& text) const { return rcss.find(text) != std::string::npos; }
        bool reported(DiagCode code) const {
            for (const Diagnostic& d : diagnostics){
                if (d.code == code)
                    return true;
            }
            return false;
        }
        size_t warnings() const {
            size_t count = 0;
            for (const Diagnostic& d : diagnostics){
                if (d.severity != Severity::Info)
                    count++;
            }
            return count;
        }
    };

    Rewritten rewrite(const std::string& css, const ForkFeatures& fork){
        SourceFiles files;
        const uint32_t file = files.add("ui/animation.css", css);
        DiagnosticSink sink(files);
        CompatOptions options;
        options.fork = fork;
        RewriteContext context;
        context.files.read = [](const std::string&, std::string&){ return false; };
        context.files.exists = [](const std::string&){ return true; };
        CssRewriter rewriter(options, files, sink, context);
        StylesheetInput input;
        input.file = file;
        input.text = files.text(file);
        const RewriteResult result = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
        Rewritten out;
        for (const StylesheetOutput& sheet : result.sheets)
            out.rcss += sheet.rcss;
        out.diagnostics = sink.diagnostics();
        return out;
    }

    ForkFeatures without(ForkFeature feature){
        return ForkFeatures::present().with(feature, false);
    }

    const char* kSheet =
        ".btn { transition: background 0.2s ease-in-out, outline-color 150ms steps(3, jump-start) 50ms, scale .2s cubic-bezier(0.2, 0, 0, 1.4), "
        "display 1s allow-discrete; }\n"
        ".btn:hover { transition-duration: 0s; }\n"
        ".spin { animation: turn 1.5s linear infinite alternate-reverse both paused, fade 300ms 2.5 forwards; }\n"
        ".item:nth-child(2) { animation-delay: 100ms; animation-play-state: running; animation-composition: add; }\n"
        ".card { translate: 10px 20%; rotate: x 45deg; scale: 150% 2; transform: skewX(10deg); }\n"
        "@keyframes turn { from { rotate: 0deg; animation-timing-function: ease-in; } 50% { rotate: 90deg; } to { rotate: 180deg; } }\n"
        "@keyframes fade { from { opacity: 0; } }\n"
        "@starting-style { .panel.open { opacity: 0; } }\n"
        ".panel { transition: opacity 0.3s; @starting-style { translate: 0 20px; } }\n";

}

TEST(CompatAnimationNative, WrittenAsDeclared){
    const ForkFeatures present = ForkFeatures::present();
    ASSERT_TRUE(present.has(ForkFeature::TimingFunctions));
    ASSERT_TRUE(present.has(ForkFeature::AnimationLonghands));
    ASSERT_TRUE(present.has(ForkFeature::IndividualTransforms));
    const Rewritten out = rewrite(kSheet, present);

    // transitions: CSS easings and allow-discrete kept; names compat writes as another property mapped
    // (background and outline-color are the fork's own with BackgroundLayers and Outline)
    EXPECT_TRUE(out.has("transition: background 0.2s ease-in-out, outline-color 0.15s steps(3, jump-start) 0.05s, "
                        "scale 0.2s cubic-bezier(0.2, 0, 0, 1.4), display 1s allow-discrete;")) << out.rcss;
    // a longhand alone is itself (phase 2 merged it with a base rule)
    EXPECT_TRUE(out.has("transition-duration: 0s;")) << out.rcss;
    // animations: every component written, the keyframes name last; no mirrored keyframes, no fill emulation
    EXPECT_TRUE(out.has("animation: 1.5s linear 0s infinite alternate-reverse both paused turn, 0.3s ease 0s 2.5 normal forwards running fade;")) << out.rcss;
    EXPECT_TRUE(out.has("animation-delay: 0.1s;")) << out.rcss;
    EXPECT_TRUE(out.has("animation-play-state: running;")) << out.rcss;
    EXPECT_TRUE(out.has("animation-composition: add;")) << out.rcss;
    EXPECT_FALSE(out.has("turn-rev")) << out.rcss;
    EXPECT_FALSE(out.has("-html-animation-fill")) << out.rcss;
    // a keyframe's own easing
    EXPECT_TRUE(out.has("animation-timing-function: ease-in;")) << out.rcss;
    // translate / rotate / scale next to transform
    EXPECT_TRUE(out.has("translate: 10px 20%;")) << out.rcss;
    EXPECT_TRUE(out.has("rotate: x 45deg;")) << out.rcss;
    EXPECT_TRUE(out.has("scale: 150% 2;")) << out.rcss;
    EXPECT_TRUE(out.has("transform: skewX(10deg);")) << out.rcss;
    // @starting-style: top level and nested
    EXPECT_TRUE(out.has("@starting-style {\n#htmlui-root .panel.open {\nopacity: 0;\n}\n}")) << out.rcss;
    EXPECT_TRUE(out.has("@starting-style {\n#htmlui-root .panel {\ntranslate: 0 20px;\n}\n}")) << out.rcss;

    // the only note: animation-composition: add composites as replace
    EXPECT_EQ(out.warnings(), 0u) << out.rcss;
    size_t notes = 0;
    for (const Diagnostic& d : out.diagnostics){
        if (d.code == codes::NestingFlattened)
            continue;
        notes++;
        EXPECT_EQ(d.code, codes::AnimationApproximated) << toText(d);
    }
    EXPECT_EQ(notes, 1u);
}

TEST(CompatAnimationNative, RmlUiReadsTheOutput){
    ASSERT_TRUE(HtmlUI::acquire());
    const Rewritten out = rewrite(kSheet, ForkFeatures::present());
    const unsigned warningsBefore = HtmlUI::getSystemInterface().getWarningCount();
    Rml::SharedPtr<Rml::StyleSheetContainer> sheet = Rml::Factory::InstanceStyleSheetString(out.rcss);
    ASSERT_NE(sheet, nullptr);
    EXPECT_EQ(HtmlUI::getSystemInterface().getWarningCount(), warningsBefore) << out.rcss;
    HtmlUI::release();
}

TEST(CompatAnimationNative, TimingFunctionsPassThroughWithoutApproximation){
    ASSERT_TRUE(HtmlUI::acquire());
    const char* values[] = {
        "linear", "ease", "ease-in", "ease-out", "ease-in-out", "step-start", "step-end",
        "cubic-bezier(0.16, 1, 0.3, 1)", "cubic-bezier(0.68, -0.55, 0.265, 1.55)",
        "steps(4, jump-start)", "steps(4, jump-end)", "steps(4, jump-none)", "steps(4, jump-both)",
        "linear(0, 0.25 25% 75%, 1)", "linear(0 0%, 0.5 0%, 1)", "linear(0.4)"
    };
    for (const char* value : values){
        SCOPED_TRACE(value);
        const Rewritten out = rewrite(std::string(".box { transition: translate 1s ") + value +
                                      "; animation-timing-function: " + value + "; }", ForkFeatures::present());
        EXPECT_FALSE(out.reported(codes::EasingApproximated));
        EXPECT_FALSE(out.reported(codes::StepsApproximated));
        EXPECT_EQ(out.warnings(), 0u) << out.rcss;
        EXPECT_TRUE(out.has(std::string("animation-timing-function: ") + value + ";")) << out.rcss;
        const unsigned warningsBefore = HtmlUI::getSystemInterface().getWarningCount();
        EXPECT_NE(Rml::Factory::InstanceStyleSheetString(out.rcss), nullptr) << out.rcss;
        EXPECT_EQ(HtmlUI::getSystemInterface().getWarningCount(), warningsBefore) << out.rcss;
    }
    HtmlUI::release();
}

TEST(CompatAnimationNative, PhaseTwoRewritesWithoutTheFeatures){
    // AnimationLonghands absent: merged shorthands, mirrored keyframes, the fill emulation, dropped keyframe easing
    const Rewritten merged = rewrite(kSheet, without(ForkFeature::AnimationLonghands));
    EXPECT_TRUE(merged.has("turn-rev")) << merged.rcss;
    EXPECT_TRUE(merged.has("@keyframes turn-rev")) << merged.rcss;
    EXPECT_TRUE(merged.has("-html-animation-fill: forwards;")) << merged.rcss;
    EXPECT_FALSE(merged.has("animation-delay:")) << merged.rcss;
    EXPECT_FALSE(merged.has("transition-duration:")) << merged.rcss;
    EXPECT_FALSE(merged.has("@starting-style")) << merged.rcss;
    EXPECT_FALSE(merged.has("animation-timing-function")) << merged.rcss;
    EXPECT_TRUE(merged.reported(codes::AnimationApproximated));
    EXPECT_TRUE(merged.reported(codes::UnsupportedAtRule));

    // TimingFunctions absent: the nearest RmlUi tween, with its diagnostics
    const Rewritten tweens = rewrite(kSheet, without(ForkFeature::TimingFunctions));
    EXPECT_FALSE(tweens.has("cubic-bezier")) << tweens.rcss;
    EXPECT_FALSE(tweens.has("steps(")) << tweens.rcss;
    EXPECT_TRUE(tweens.has("quadratic-in-out")) << tweens.rcss;
    EXPECT_TRUE(tweens.reported(codes::EasingApproximated));
    EXPECT_TRUE(tweens.reported(codes::StepsApproximated));

    // IndividualTransforms absent: merged into transform, and transitioned as transform
    const Rewritten transform = rewrite(kSheet, without(ForkFeature::IndividualTransforms));
    EXPECT_TRUE(transform.has("transform: translate(10px, 20%) rotateX(45deg) scale(1.5, 2) skewX(10deg);")) << transform.rcss;
    EXPECT_FALSE(transform.has("\nscale:")) << transform.rcss;
    EXPECT_TRUE(transform.has("transform 0.2s cubic-bezier(0.2, 0, 0, 1.4)")) << transform.rcss;

    // BackgroundLayers absent: background transitions as the colour and the decorator; Outline absent: outline-color as
    // box-shadow (phase 3, p3-paint)
    const Rewritten layers = rewrite(kSheet, without(ForkFeature::BackgroundLayers));
    EXPECT_TRUE(layers.has("background-color 0.2s ease-in-out, decorator 0.2s ease-in-out")) << layers.rcss;
    EXPECT_TRUE(layers.has("outline-color 0.15s")) << layers.rcss;
    const Rewritten outline = rewrite(kSheet, without(ForkFeature::Outline));
    EXPECT_TRUE(outline.has("box-shadow 0.15s steps(3, jump-start) 0.05s")) << outline.rcss;
    EXPECT_TRUE(outline.has("transition: background 0.2s")) << outline.rcss;
}
