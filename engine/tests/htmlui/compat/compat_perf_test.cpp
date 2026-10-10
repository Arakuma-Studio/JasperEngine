// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Performance targets of the HTML stage (phase-2 §3.14), Release builds only: the median of 20
// runs after 3 warm-ups (CPU time). HTML parse + transforms + serialize of 50 KB / 3 500 nodes <= 3 ms, a
// cache hit (hashing every dependency) <= 0.1 ms, fragmentToRml of 2 KB <= 0.2 ms.

#include "CompatTestCommon.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"

#include "PerfFixtures.h"

#include <gtest/gtest.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <functional>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    double slack(){
        return perfSlack();
    }

    // PerfFixtures.h: the least of up to 15 medians of the CPU time of 10 runs
    double gatedMs(const std::function<void()>& work, double targetMs){
        return gatedCpuMs(work, 10, targetMs);
    }

    // a settings page of about 50 KB and 3 500 nodes (elements and text), no stylesheet (the HTML stage alone)
    std::string generatedPage(){
        std::string html = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Settings</title></head><body><main class=\"settings\">";
        int i = 0;
        while (html.size() < 50 * 1024){
            const std::string n = std::to_string(i++);
            html += "<div class=\"row\" id=\"r" + n + "\"><label>Opt " + n + "</label><input type=\"range\" value=\"50\"><span>50</span></div>";
        }
        return html + "</main></body></html>";
    }

}

#ifdef NDEBUG

TEST(CompatPerf, HtmlStage){
    const std::string html = generatedPage();
    CompatOptions options;
    std::shared_ptr<const CompatDocument> compiled;
    const double ms = gatedMs([&]{ compiled = DocumentLoader::compileString(html, "perf/page.html", options); }, 3.0 * slack());
    ASSERT_TRUE(compiled->compiled);
    EXPECT_GE(compiled->sourceMap.elementRefs.size(), 2000u);   // + 1 600 text nodes: about 3 600 nodes
    std::printf("[perf] HTML stage: %zu bytes, %zu elements, %.3f ms (target 3 ms)\n", html.size(), compiled->sourceMap.elementRefs.size(), ms);
    EXPECT_LE(ms, 3.0 * slack());
}

TEST(CompatPerf, CacheHit){
    VirtualFiles::add("@htmlui/perf/page.html", generatedPage() + "<link rel=\"stylesheet\" href=\"page.css\">");
    std::string css;
    for (int i = 0; css.size() < 50 * 1024; i++)
        css += ".rule" + std::to_string(i) + " { color: #ff0000; margin: 1px 2px; }\n";
    VirtualFiles::add("@htmlui/perf/page.css", css);
    CompatOptions options;
    std::shared_ptr<const CompatDocument> first = DocumentLoader::compile("@htmlui/perf/page.html", options);
    ASSERT_TRUE(first->compiled);
    const double ms = gatedMs([&]{ EXPECT_EQ(DocumentLoader::compile("@htmlui/perf/page.html", options), first); }, 0.1 * slack());
    std::printf("[perf] compile() cache hit (100 KB of dependencies hashed): %.3f ms (target 0.1 ms)\n", ms);
    EXPECT_LE(ms, 0.1 * slack());
    DocumentLoader::clearCache();
    VirtualFiles::remove("@htmlui/perf/page.html");
    VirtualFiles::remove("@htmlui/perf/page.css");
}

// "Whole compile() (cold), 50 KB HTML + 50 KB CSS, <= 8 ms": the page of HtmlStage linking the
// sheet of CompatCssPerf.FiftyKilobyteSheet, nothing cached (the user-agent sheet, rewritten
// once per process, is)
TEST(CompatPerf, ColdCompile){
    VirtualFiles::add("@htmlui/perf/cold.html", generatedPage() + "<link rel=\"stylesheet\" href=\"cold.css\">");
    const std::string css = representativeSheet();
    VirtualFiles::add("@htmlui/perf/cold.css", css);
    CompatOptions options;
    std::shared_ptr<const CompatDocument> compiled;
    const double ms = gatedCpuMs([&]{
        DocumentLoader::clearCache();
        compiled = DocumentLoader::compile("@htmlui/perf/cold.html", options);
    }, 1, 8.0 * slack());
    ASSERT_TRUE(compiled->compiled);
    ASSERT_EQ(compiled->stylesheets.size(), 1u);
    EXPECT_GT(compiled->stylesheets[0].rcss.size(), css.size() / 2);
    std::printf("[perf] cold compile() of %zu bytes of HTML and %zu of CSS: %.3f ms (target 8 ms)\n", generatedPage().size(), css.size(), ms);
    EXPECT_LE(ms, 8.0 * slack());
    compiled.reset();
    DocumentLoader::clearCache();
    VirtualFiles::remove("@htmlui/perf/cold.html");
    VirtualFiles::remove("@htmlui/perf/cold.css");
}

#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 33)
// "Memory kept per compiled document <= 4 x (HTML + CSS size)": the heap in use while the
// document of ColdCompile is held, over the heap in use without it (what the thread keeps for
// its next compiles, the arena blocks and the freed HTML nodes, is there in both)
TEST(CompatPerf, MemoryKeptPerDocument){
    const std::string html = generatedPage() + "<link rel=\"stylesheet\" href=\"kept.css\">";
    const std::string css = representativeSheet();
    VirtualFiles::add("@htmlui/perf/kept.html", html);
    VirtualFiles::add("@htmlui/perf/kept.css", css);
    CompatOptions options;
    // (a first compile leaves what a thread keeps between compiles)
    DocumentLoader::compile("@htmlui/perf/kept.html", options);
    DocumentLoader::clearCache();
    const auto inUse = [](){
        const struct mallinfo2 info = mallinfo2();
        return (size_t)info.uordblks + (size_t)info.hblkhd;
    };
    const size_t before = inUse();
    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compile("@htmlui/perf/kept.html", options);
    ASSERT_TRUE(compiled->compiled);
    DocumentLoader::clearCache();   // the cache's reference: the test holds the only one
    const size_t held = inUse();
    const size_t kept = held > before ? held - before : 0;
    std::printf("[perf] memory kept by a compiled document: %zu bytes for %zu of HTML and CSS (%.2f x, target 4 x)\n", kept, html.size() + css.size(),
                (double)kept / (double)(html.size() + css.size()));
    EXPECT_GT(kept, compiled->rml.size());
    EXPECT_LE(kept, 4 * (html.size() + css.size()));
    compiled.reset();
    VirtualFiles::remove("@htmlui/perf/kept.html");
    VirtualFiles::remove("@htmlui/perf/kept.css");
}
#endif

TEST(CompatPerf, Fragment){
    std::string html;
    while (html.size() < 2048)
        html += "<li class=\"item\"><span class=\"name\">Sword &amp; shield</span><b>12</b><img src=\"img/a.png\" alt=\"\"></li>";
    std::vector<Diagnostic> diagnostics;
    FragmentOptions options;
    options.documentPath = "compat/html/inventory.html";   // its img/a.png exists
    const double ms = gatedMs([&]{ diagnostics.clear(); DocumentLoader::fragmentToRml(html, "ui/inventory.html (innerHTML)", diagnostics, options); },
                              0.2 * slack());
    std::printf("[perf] fragmentToRml of %zu bytes: %.3f ms (target 0.2 ms)\n", html.size(), ms);
    EXPECT_LE(ms, 0.2 * slack());
}

#endif
