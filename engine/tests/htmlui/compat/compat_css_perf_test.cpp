// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CSS rows of the phase-2 performance targets (§3.14): tokenize + parse + rewrite of a 50 KB
// stylesheet with 1 200 rules in at most 4 ms, the user-agent sheet in at most 1 ms (the budgets
// the tests gate on are those or what a cloud GPU host's CPU measures: see below). Release
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

    // The budgets. The row's targets are 4 ms (a 50 KB sheet) and 1 ms (the user-agent sheet);
    // the slower CPUs of the cloud GPU hosts miss some of them, and the owner chose to raise
    // those budgets to what such a CPU measures (2026-10-27). Measured on the NVIDIA GTX 1080 test
    // host (56 cores, Release, each test alone, nothing else running): 30 gate measurements
    // (gatedCpuMs with every one of its 15 attempts, JASPER_HTMLUI_PERF_SLACK=0.0001) of each
    // test, min / median / max:
    //   user-agent sheet          0.873 / 1.155 / 1.204 ms  -> 1.33 ms (the max plus 10 %)
    //   50 KB sheet, 1 200 rules  3.102 / 3.216 / 3.510 ms  -> 4 ms, the target (covers the max plus 14 %)
    //   50 KB sheet of page rules 2.992 / 3.164 / 4.164 ms  -> 4.6 ms (the max plus 10 %)
    // (the CPU's speed is bimodal: the user-agent sheet takes 0.87-0.92 ms or 1.15-1.20 ms)
    // A second cloud CPU (Xeon E5-2630 v4, 2.2 GHz, the RTX 3060 Ti box), 13 runs: 50 KB sheet
    // 4.094 / 4.162 / 4.304 ms (failed the 4 ms target), page rules 3.961 / 4.048 / 4.188 ms,
    // user-agent sheet 1.146 / 1.195 / 1.212 ms: the 50 KB budget becomes 4.75 ms (that max plus
    // 10 %); the other two budgets already cover it.
    // A third cloud CPU (Xeon E5-2695 v3, 2.3 GHz, the RTX 3060 Ti box of 2026-10-28, a host shared
    // with other tenants: load average 23-30), 12 runs: 50 KB sheet 3.173-4.732 ms, page rules
    // 3.111-4.520 ms, user-agent sheet 0.891-1.285 ms: every budget is the slowest run seen on any
    // of these machines plus 10 % (the owner's rule).
    constexpr double kFiftyKilobyteBudgetMs = 5.2;
    constexpr double kPageRulesBudgetMs = 5.0;
    constexpr double kUserAgentBudgetMs = 1.42;

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
    const double ms = gatedCpuMs([&](){ rewriteOnce(css); }, 1, kFiftyKilobyteBudgetMs * perfSlack());
    std::printf("[ perf ] CSS tokenize + parse + rewrite, %zu bytes: %.3f ms\n", css.size(), ms);
    EXPECT_LE(ms, kFiftyKilobyteBudgetMs * perfSlack());
#else
    rewriteOnce(css);
    GTEST_SKIP() << "performance targets are measured in Release builds";
#endif
}

// The same row on a sheet of the same size with the rules of a real one (3.9 declarations a
// rule, about 450 rules): FiftyKilobyteSheet has the row's 1 200 rules, which is more
// selectors than 55 KB of a real sheet hold.
TEST(CompatCssPerf, FiftyKilobyteSheetOfPageRules){
    SheetShape shape;
    const std::string css = representativeSheet(&shape, true);
    EXPECT_GE(css.size(), 50u * 1024u);
    EXPECT_LE(css.size(), 60u * 1024u);
    EXPECT_NEAR((double)shape.declarations / (double)shape.rules, 3.9, 0.3);
    EXPECT_NEAR(100.0 * (double)shape.repeated / (double)shape.declarations, 41.0, 1.5);
    std::printf("[ perf ] the sheet of page rules: %zu bytes, %zu rules (%zu in @media), %zu declarations (%.1f a rule), %.0f%% repeat an earlier one, "
                "%.1f%% of the background/transition/animation/outline/text-shadow families, %.1f%% read a custom property\n",
                css.size(), shape.rules, shape.mediaRules, shape.declarations, (double)shape.declarations / (double)shape.rules,
                100.0 * (double)shape.repeated / (double)shape.declarations,
                100.0 * (double)shape.familyDeclarations / (double)shape.declarations, 100.0 * (double)shape.varReads / (double)shape.declarations);
#ifdef NDEBUG
    const double ms = gatedCpuMs([&](){ rewriteOnce(css); }, 1, kPageRulesBudgetMs * perfSlack());
    std::printf("[ perf ] CSS tokenize + parse + rewrite (page rules), %zu bytes: %.3f ms\n", css.size(), ms);
    EXPECT_LE(ms, kPageRulesBudgetMs * perfSlack());
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
    }, 1, kUserAgentBudgetMs * perfSlack());
    std::printf("[ perf ] user-agent sheet rewrite, %zu bytes: %.3f ms\n", css.size(), ms);
    EXPECT_LE(ms, kUserAgentBudgetMs * perfSlack());
#else
    GTEST_SKIP() << "performance targets are measured in Release builds";
#endif
}
