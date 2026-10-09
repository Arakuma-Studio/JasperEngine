// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Performance targets of the HTML stage (phase-2 §3.14), Release builds only: the median of 20
// runs after 3 warm-ups (CPU time). HTML parse + transforms + serialize of 50 KB / 3 500 nodes <= 3 ms, a
// cache hit (hashing every dependency) <= 0.1 ms, fragmentToRml of 2 KB <= 0.2 ms.

#include "CompatTestCommon.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <functional>

using namespace jasperengine::htmlui::compat;

namespace {

    // a shared, loaded build host (several agents compiling) slows every timing down: the
    // targets can be scaled with JASPER_HTMLUI_PERF_SLACK (default 1)
    double slack(){
        const char* value = std::getenv("JASPER_HTMLUI_PERF_SLACK");
        const double factor = value ? std::atof(value) : 1.0;
        return factor > 0.0 ? factor : 1.0;
    }

    double medianMs(const std::function<void()>& work){
        for (int i = 0; i < 3; i++)
            work();
        // the CPU time of the process (single-threaded work): wall time on a shared build host
        // mostly measures the other processes; 10 repetitions per sample for the clock's resolution
        std::vector<double> times;
        for (int i = 0; i < 20; i++){
            const std::clock_t start = std::clock();
            for (int r = 0; r < 10; r++)
                work();
            times.push_back(1000.0 * (double)(std::clock() - start) / CLOCKS_PER_SEC / 10.0);
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    }

    // a settings page of about 50 KB and 3 500 nodes, no stylesheet (the HTML stage alone)
    std::string generatedPage(){
        std::string html = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Settings</title></head><body><main class=\"settings\">";
        int i = 0;
        while (html.size() < 50 * 1024){
            const std::string n = std::to_string(i++);
            html += "<div class=\"row\" id=\"row" + n + "\"><label for=\"opt" + n + "\">Option " + n + "</label>"
                    "<input id=\"opt" + n + "\" type=\"range\" min=\"0\" max=\"100\" value=\"50\"><span class=\"value\">50</span>"
                    "<button type=\"button\" class=\"reset\">Reset</button></div>\n";
        }
        return html + "</main></body></html>";
    }

}

#ifdef NDEBUG

TEST(CompatPerf, HtmlStage){
    const std::string html = generatedPage();
    CompatOptions options;
    std::shared_ptr<const CompatDocument> compiled;
    const double ms = medianMs([&]{ compiled = DocumentLoader::compileString(html, "perf/page.html", options); });
    ASSERT_TRUE(compiled->compiled);
    EXPECT_GE(compiled->sourceMap.elementRefs.size(), 1500u);
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
    const double ms = medianMs([&]{ EXPECT_EQ(DocumentLoader::compile("@htmlui/perf/page.html", options), first); });
    std::printf("[perf] compile() cache hit (100 KB of dependencies hashed): %.3f ms (target 0.1 ms)\n", ms);
    EXPECT_LE(ms, 0.1 * slack());
    DocumentLoader::clearCache();
    VirtualFiles::remove("@htmlui/perf/page.html");
    VirtualFiles::remove("@htmlui/perf/page.css");
}

TEST(CompatPerf, Fragment){
    std::string html;
    while (html.size() < 2048)
        html += "<li class=\"item\"><span class=\"name\">Sword &amp; shield</span><b>12</b><img src=\"img/a.png\" alt=\"\"></li>";
    std::vector<Diagnostic> diagnostics;
    FragmentOptions options;
    options.documentPath = "compat/html/inventory.html";   // its img/a.png exists
    const double ms = medianMs([&]{ diagnostics.clear(); DocumentLoader::fragmentToRml(html, "ui/inventory.html (innerHTML)", diagnostics, options); });
    std::printf("[perf] fragmentToRml of %zu bytes: %.3f ms (target 0.2 ms)\n", html.size(), ms);
    EXPECT_LE(ms, 0.2 * slack());
}

#endif
