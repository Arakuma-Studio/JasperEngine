// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CSS rows of the phase-2 performance targets (§3.14): tokenize + parse + rewrite of a 50 KB
// stylesheet with 1 200 rules in at most 4 ms, the user-agent sheet in at most 1 ms. Release
// builds only. The sheet is PerfFixtures.h's representativeSheet() (shaped like the author
// sheets of the golden pages); each step is measured as PerfFixtures.h's gatedCpuMs().

#include "PerfFixtures.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/ua/UserAgentStyle.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    void rewriteOnce(const std::string& css){
        SourceFiles files;
        const uint32_t file = files.add("ui/perf.css", css);
        DiagnosticSink sink(files);
        CompatOptions options;
        options.fork = ForkFeatures::none();
        RewriteContext context;
        context.files.read = [](const std::string&, std::string&){ return false; };
        context.files.exists = [](const std::string&){ return true; };
        CssRewriter rewriter(options, files, sink, context);
        StylesheetInput input;
        input.file = file;
        input.text = files.text(file);
        rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
    }

}

TEST(CompatCssPerf, FiftyKilobyteSheet){
    SheetShape shape;
    const std::string css = representativeSheet(&shape);
    EXPECT_GE(css.size(), 50u * 1024u);
    EXPECT_LE(css.size(), 60u * 1024u);
    EXPECT_EQ(shape.rules, 1200u);
    // as the author sheets of the golden pages: 41% of the declarations repeat an earlier one
    EXPECT_NEAR(100.0 * (double)shape.repeated / (double)shape.declarations, 41.0, 1.5);
    std::printf("[ perf ] the sheet: %zu bytes, %zu rules (%zu in @media), %zu declarations, %.0f%% repeat an earlier one, "
                "%.1f%% of the background/transition/animation/outline/text-shadow families, %.1f%% read a custom property\n",
                css.size(), shape.rules, shape.mediaRules, shape.declarations, 100.0 * (double)shape.repeated / (double)shape.declarations,
                100.0 * (double)shape.familyDeclarations / (double)shape.declarations, 100.0 * (double)shape.varReads / (double)shape.declarations);
#ifdef NDEBUG
    const double ms = gatedCpuMs([&](){ rewriteOnce(css); }, 1, 4.0 * perfSlack());
    std::printf("[ perf ] CSS tokenize + parse + rewrite, %zu bytes: %.3f ms\n", css.size(), ms);
    EXPECT_LE(ms, 4.0 * perfSlack());
#else
    rewriteOnce(css);
    GTEST_SKIP() << "performance targets are measured in Release builds";
#endif
}

TEST(CompatCssPerf, UserAgentSheet){
    const std::string css(UserAgentStyle::source());
#ifdef NDEBUG
    const double ms = gatedCpuMs([&](){
        SourceFiles files;
        const uint32_t file = files.add("useragent.css", css);
        DiagnosticSink sink(files);
        CssRewriter rewriter(CompatOptions(), files, sink);
        StylesheetInput input;
        input.kind = StylesheetInput::Kind::UserAgent;
        input.file = file;
        input.text = files.text(file);
        rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
    }, 1, 1.0 * perfSlack());
    std::printf("[ perf ] user-agent sheet rewrite, %zu bytes: %.3f ms\n", css.size(), ms);
    EXPECT_LE(ms, 1.0 * perfSlack());
#else
    GTEST_SKIP() << "performance targets are measured in Release builds";
#endif
}
