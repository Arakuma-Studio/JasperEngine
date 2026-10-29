// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ForkFeature::LineClamp (phase 6, p6-line-clamp: Chromium 153's legacy line clamping in the fork,
// docs/html-ui/FORK.md P6-L1). With the feature compat writes -webkit-line-clamp, -webkit-box-orient
// and display: -webkit-box natively, reports the clamps that do nothing in their own rule, and drops
// the standard line-clamp shorthand as Chromium 153 does; without it phase 2's cut stays.

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    struct Rewritten {
        std::string rcss;
        std::vector<Diagnostic> diagnostics;

        // the value written for `property` in the first rule that has it ("" when none)
        std::string value(const std::string& property) const {
            const std::string key = "\n" + property + ": ";
            const size_t at = rcss.find(key);
            if (at == std::string::npos)
                return std::string();
            const size_t begin = at + key.size();
            const size_t end = rcss.find(";\n", begin);
            return rcss.substr(begin, end - begin);
        }
        const Diagnostic* find(DiagCode code) const {
            for (const Diagnostic& d : diagnostics){
                if (d.code == code)
                    return &d;
            }
            return nullptr;
        }
        std::string text() const {
            std::string out = rcss;
            for (const Diagnostic& d : diagnostics)
                out += "\n" + toText(d);
            return out;
        }
    };

    Rewritten rewrite(const std::string& css, const ForkFeatures& fork = ForkFeatures::present()){
        SourceFiles files;
        const uint32_t file = files.add("ui/clamp.css", css);
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

    const char* kPattern = ".desc {\n"
                           "  display: -webkit-box;\n"
                           "  -webkit-box-orient: vertical;\n"
                           "  -webkit-line-clamp: 3;\n"
                           "  overflow: hidden;\n"
                           "}\n";

}

TEST(CompatLineClamp, TheLegacyPatternIsNative){
    ASSERT_TRUE(ForkFeatures::present().has(ForkFeature::LineClamp));
    const Rewritten out = rewrite(kPattern);
    EXPECT_EQ(out.value("display"), "-webkit-box") << out.text();
    EXPECT_EQ(out.value("-webkit-box-orient"), "vertical") << out.text();
    EXPECT_EQ(out.value("flex-direction"), "column") << out.text();   // the legacy box without a clamp is a column flexbox
    EXPECT_EQ(out.value("-webkit-line-clamp"), "3") << out.text();
    EXPECT_EQ(out.value("max-height"), "") << out.text();
    EXPECT_TRUE(out.diagnostics.empty()) << out.text();

    // the aliases, the inline box, none
    const Rewritten other = rewrite(".a { display: -webkit-inline-box; -webkit-box-orient: block-axis; -webkit-line-clamp: none; }");
    EXPECT_EQ(other.value("display"), "-webkit-inline-box") << other.text();
    EXPECT_EQ(other.value("-webkit-box-orient"), "vertical") << other.text();
    EXPECT_EQ(other.value("-webkit-line-clamp"), "none") << other.text();
    EXPECT_TRUE(other.diagnostics.empty()) << other.text();
}

TEST(CompatLineClamp, WithoutTheFeaturePhaseTwoCuts){
    const Rewritten out = rewrite(kPattern, ForkFeatures::present().with(ForkFeature::LineClamp, false));
    EXPECT_EQ(out.value("display"), "block") << out.text();
    EXPECT_EQ(out.value("max-height"), "3.6em") << out.text();
    EXPECT_EQ(out.value("-webkit-line-clamp"), "") << out.text();
    EXPECT_EQ(out.value("-webkit-box-orient"), "") << out.text();
    EXPECT_NE(out.find(codes::ApproximatedVisible), nullptr) << out.text();
}

TEST(CompatLineClamp, InvalidCountsAreDropped){
    // Chromium 153: none or an integer from 1
    for (const char* value : {"0", "-2", "2.5", "3px", "auto"}){
        SCOPED_TRACE(value);
        const Rewritten out = rewrite(std::string(".a { display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: ") + value + "; }");
        EXPECT_EQ(out.value("-webkit-line-clamp"), "") << out.text();
        const Diagnostic* d = out.find(codes::UnsupportedValue);
        ASSERT_NE(d, nullptr) << out.text();
        EXPECT_EQ(d->severity, Severity::Warning);
    }
    // a var(): its static value, or the fork's to resolve
    const Rewritten out = rewrite(".a { --n: 2; display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: var(--n); }");
    EXPECT_FALSE(out.value("-webkit-line-clamp").empty()) << out.text();
    EXPECT_EQ(out.find(codes::UnsupportedValue), nullptr) << out.text();
}

TEST(CompatLineClamp, AClampWithoutTheLegacyBoxIsReported){
    // its own rule shows it does nothing in Chromium: another display, or no vertical orient (Info)
    {
        const Rewritten out = rewrite(".a { display: block; -webkit-line-clamp: 2; overflow: hidden; }");
        const Diagnostic* d = out.find(codes::IgnoredProperty);
        ASSERT_NE(d, nullptr) << out.text();
        EXPECT_EQ(d->severity, Severity::Info);
        EXPECT_NE(d->suggestion.find("-webkit-box-orient: vertical"), std::string::npos) << toText(*d);
        EXPECT_EQ(out.value("-webkit-line-clamp"), "2") << out.text();
    }
    {
        const Rewritten out = rewrite(".a { display: -webkit-box; -webkit-line-clamp: 2; }");
        const Diagnostic* d = out.find(codes::IgnoredProperty);
        ASSERT_NE(d, nullptr) << out.text();
        EXPECT_EQ(d->severity, Severity::Info);
    }
    // another rule may give the box: nothing to say
    EXPECT_TRUE(rewrite(".a { -webkit-line-clamp: 2; }").diagnostics.empty());
}

TEST(CompatLineClamp, TheStandardShorthandIsDroppedAsChromiumDrops){
    // alone: a warning with the legacy pattern as the fix
    {
        const Rewritten out = rewrite(".a { line-clamp: 2; }");
        const Diagnostic* d = out.find(codes::UnsupportedProperty);
        ASSERT_NE(d, nullptr) << out.text();
        EXPECT_EQ(d->severity, Severity::Warning);
        EXPECT_NE(d->suggestion.find("-webkit-line-clamp"), std::string::npos) << toText(*d);
        EXPECT_EQ(out.value("max-height"), "") << out.text();
    }
    // beside -webkit-line-clamp (for other browsers): info
    {
        std::string sheet = kPattern;
        sheet.insert(sheet.find("overflow"), "line-clamp: 3;\n  ");
        const Rewritten out = rewrite(sheet);
        const Diagnostic* d = out.find(codes::UnsupportedProperty);
        ASSERT_NE(d, nullptr) << out.text();
        EXPECT_EQ(d->severity, Severity::Info);
        EXPECT_EQ(out.value("-webkit-line-clamp"), "3") << out.text();
    }
    // its longhands: Chromium 153 has none of them
    for (const char* declaration : {"max-lines: 2", "block-ellipsis: auto", "continue: discard"}){
        SCOPED_TRACE(declaration);
        const Rewritten out = rewrite(std::string(".a { ") + declaration + "; }");
        const Diagnostic* d = out.find(codes::UnsupportedProperty);
        ASSERT_NE(d, nullptr) << out.text();
        EXPECT_NE(d->suggestion.find("-webkit-line-clamp"), std::string::npos) << toText(*d);
    }
}

TEST(CompatLineClamp, TransitionsNameTheForkProperty){
    const Rewritten out = rewrite(".a { transition: -webkit-line-clamp 1s linear allow-discrete; }");
    EXPECT_NE(out.value("transition").find("-webkit-line-clamp"), std::string::npos) << out.text();
    EXPECT_EQ(out.value("transition").find("max-height"), std::string::npos) << out.text();
}
