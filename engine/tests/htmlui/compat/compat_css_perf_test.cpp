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
#include <chrono>
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
        const char* templates[] = {
            ".panel-%d { background: rgba(255,255,255,.08); border: 1px solid rgba(255,255,255,.15); border-radius: var(--radius); padding: 1.5rem; }\n",
            ".btn-%d { display: inline-flex; align-items: center; gap: 8px; transition: all .3s ease; color: #fff; background: linear-gradient(90deg, #ff6b6b, #feca57); }\n",
            ".btn-%d:hover { transform: translateY(-2px) scale(1.02); box-shadow: 0 4px 12px rgba(0,0,0,.4); }\n",
            ".title-%d { font: 700 2rem/1.2 'Orbitron', sans-serif; text-shadow: 0 0 10px var(--accent); letter-spacing: .1em; }\n",
            "@media (max-width: 768px) { .grid-%d { flex-direction: column; padding: calc(1rem + 2px); } }\n",
            ".item-%d:nth-child(2n+1) > .icon { width: 24px; height: 24px; border-radius: 50%%; animation: fadeIn .4s ease forwards; }\n",
            ".card-%d { & .header { color: hsl(200 80%% 60%%); } &:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; } }\n",
            ".hud-%d { position: fixed; top: 16px; right: 16px; z-index: 10; backdrop-filter: blur(8px); }\n",
        };
        size_t written = 0;
        int index = 0;
        while (written < rules){
            for (const char* t : templates){
                char buffer[512];
                std::snprintf(buffer, sizeof(buffer), t, index);
                css += buffer;
                written++;
            }
            index++;
        }
        return css;
    }

    double medianMilliseconds(const std::function<void()>& run){
        for (int i = 0; i < 3; i++)
            run();
        std::vector<double> times;
        for (int i = 0; i < 20; i++){
            const auto start = std::chrono::steady_clock::now();
            run();
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
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
    std::string css = generatedSheet(1200);
    while (css.size() < 50 * 1024)
        css += ".pad { color: red; }\n";
    EXPECT_GE(css.size(), 50u * 1024u);
#ifdef NDEBUG
    const double ms = medianMilliseconds([&](){ rewriteOnce(css); });
    std::printf("[ perf ] CSS tokenize + parse + rewrite, %zu bytes: %.3f ms\n", css.size(), ms);
    EXPECT_LE(ms, 4.0);
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
    EXPECT_LE(ms, 1.0);
#else
    GTEST_SKIP() << "performance targets are measured in Release builds";
#endif
}
