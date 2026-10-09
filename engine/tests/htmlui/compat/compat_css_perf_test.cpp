// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CSS rows of the phase-2 performance targets (§3.14): tokenize + parse + rewrite of a 50 KB
// stylesheet with 1 200 rules in at most 4 ms, the user-agent sheet in at most 1 ms. Release
// builds only; each target is the median of 20 runs after 3 warm-ups.

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

namespace {

    // a game-UI stylesheet the way an LLM writes one, repeated with varied names
    std::string generatedSheet(size_t rules){
        std::string css = ":root { --accent: rgba(0, 200, 255, .8); --bg: linear-gradient(135deg, #1e1e2e, #11111b); --radius: 12px; --dur: 200ms; }\n";
        css += "* { box-sizing: border-box; margin: 0; padding: 0; }\n";
        css += "@keyframes fadeIn { from { opacity: 0; transform: translateY(8px); } to { opacity: 1; transform: none; } }\n";
        // 12 rule shapes, ~43 bytes each on average: 1 200 rules make the 50 KB of the target
        const char* templates[] = {
            ".panel-%d { padding: 12px; border-radius: 8px; }\n",
            ".btn-%d:hover { color: #fff; }\n",
            ".title-%d { font: 700 18px/1.2 sans-serif; }\n",
            "@media (max-width: 768px) { .g-%d { flex-direction: column; } }\n",
            ".item-%d > .icon { width: 24px; height: 24px; }\n",
            ".card-%d { background: rgba(255,255,255,.08); }\n",
            ".fade-%d { transition: opacity .3s ease; }\n",
            ".hud-%d { position: fixed; top: 16px; }\n",
            ".row-%d { display: flex; gap: 8px; }\n",
            ".x-%d { box-shadow: 0 4px 12px rgba(0,0,0,.4); }\n",
            ".t-%d { text-shadow: 0 0 10px var(--accent); }\n",
            ".a-%d { & .b { animation: fadeIn .4s ease; } }\n",
        };
        size_t written = 0;
        int index = 0;
        while (written < rules){
            for (const char* t : templates){
                if (written == rules)
                    break;
                char buffer[512];
                std::snprintf(buffer, sizeof(buffer), t, index);
                css += buffer;
                written++;
            }
            index++;
        }
        return css;
    }

    // a shared, loaded build host (several agents compiling) slows every timing down: the
    // targets can be scaled with JASPER_HTMLUI_PERF_SLACK (default 1), as in compat_perf_test.cpp
    double slack(){
        const char* value = std::getenv("JASPER_HTMLUI_PERF_SLACK");
        const double factor = value ? std::atof(value) : 1.0;
        return factor > 0.0 ? factor : 1.0;
    }

    // the median CPU time of the process (the work is single-threaded): wall time on a shared
    // host mostly measures the other processes
    double medianMilliseconds(const std::function<void()>& run){
        for (int i = 0; i < 3; i++)
            run();
        std::vector<double> times;
        for (int i = 0; i < 20; i++){
            const std::clock_t start = std::clock();
            run();
            times.push_back(1000.0 * (double)(std::clock() - start) / (double)CLOCKS_PER_SEC);
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    }

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
    const std::string css = generatedSheet(1200);
    EXPECT_GE(css.size(), 45u * 1024u);
    EXPECT_LE(css.size(), 60u * 1024u);
#ifdef NDEBUG
    const double ms = medianMilliseconds([&](){ rewriteOnce(css); });
    std::printf("[ perf ] CSS tokenize + parse + rewrite, %zu bytes: %.3f ms\n", css.size(), ms);
    EXPECT_LE(ms, 4.0 * slack());
#else
    rewriteOnce(css);
    GTEST_SKIP() << "performance targets are measured in Release builds";
#endif
}

TEST(CompatCssPerf, UserAgentSheet){
    const std::string css(UserAgentStyle::source());
#ifdef NDEBUG
    const double ms = medianMilliseconds([&](){
        SourceFiles files;
        const uint32_t file = files.add("useragent.css", css);
        DiagnosticSink sink(files);
        CssRewriter rewriter(CompatOptions(), files, sink);
        StylesheetInput input;
        input.kind = StylesheetInput::Kind::UserAgent;
        input.file = file;
        input.text = files.text(file);
        rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
    });
    std::printf("[ perf ] user-agent sheet rewrite, %zu bytes: %.3f ms\n", css.size(), ms);
    EXPECT_LE(ms, 1.0 * slack());
#else
    GTEST_SKIP() << "performance targets are measured in Release builds";
#endif
}
