// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The effects' performance smoke and cost table (phase-5 spec 5.4, 3.14), Release builds only.
//
// - Document A: a 1280 x 720 canvas, 64 elements of 140 x 90 in an 8 x 8 grid, 48 of them paint
//   effects (stripes, spotlight, cursor-trail in turn) and 16 filters (12 pixelate, 4 frosted-glass
//   backdrops). Document B: the same layout with background-color only. 30 warm-up frames, then
//   120 measured frames with a moving clock; the metric is the median CPU time of the main thread
//   for a frame of the document (the compat runtime, Context::Update and the render submission,
//   FixtureRunner::frame, as HtmlUIContext does it each engine frame). In the steady state no
//   effect instance is created, no variant is compiled and no pipeline is created, and
//   CPU(A) <= 1.5 x CPU(B) + 1 ms. A2: every element's parameter reads var(--k), changed every
//   frame: exactly 64 re-instances a frame, nothing compiled, CPU(A2) <= 3 x CPU(B) + 4 ms.
// - Cost table: every registered effect fills 1024 x 1024 and is rendered and read back 20 times
//   through the golden harness, against a background-color fill of the same size (the process's
//   CPU time: the GL driver's threads draw). Written to build/linux/htmlui-effects-cost.json
//   (htmlui-effects-cost-vulkan.json on Vulkan) and printed; the ratio is at most 6 for a `low`
//   effect and 24 for a `medium` one (3.14's classes x 1.5); `high` is unbounded.
//
// The timings are gated like the compat performance tests (PerfFixtures.h): the least of up to 5
// measurements is compared, since a loaded host only ever adds time to one.

#include "EffectTestCommon.h"

#include "htmlui/HtmlUIClock.h"
#include "htmlui/compat/runtime/RuntimeRegistry.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/DocumentLoader.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>

#if defined(__linux__) || defined(__APPLE__)
#include <time.h>
#endif

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    // the calling thread's CPU time in ms (the process's where the platform has no thread clock)
    double threadCpuMs(){
#if defined(__linux__) || defined(__APPLE__)
        timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#else
        return 1000.0 * (double)std::clock() / (double)CLOCKS_PER_SEC;
#endif
    }

    double processCpuMs(){
        return 1000.0 * (double)std::clock() / (double)CLOCKS_PER_SEC;
    }

    double median(std::vector<double> values){
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    const char* const kPaints[] = {"stripes", "spotlight", "cursor-trail"};

    // the 8 x 8 grid: `effects` false = background-color only (document B); `vars` = every
    // effect parameter reads var(--k) (A2)
    std::string gridDocument(bool effects, bool vars){
        std::string css = "html, body { margin: 0; width: 1280px; height: 720px; background-color: #101218; }\n"
                          "#grid { position: absolute; left: 0; top: 0; width: 1280px; height: 720px; --k: 0.5; }\n"
                          "#grid div { position: absolute; width: 140px; height: 90px; background-color: #3366aa; }\n";
        std::string body;
        for (int i = 0; i < 64; i++){
            const int col = i % 8, row = i / 8;
            const std::string id = "e" + std::to_string(i);
            std::string rule = "#" + id + " { left: " + std::to_string(10 + col * 158) + "px; top: " + std::to_string(row * 90) + "px;";
            if (effects){
                if (i < 48){
                    const std::string name = kPaints[i % 3];
                    std::string param;
                    if (vars)
                        param = name == "stripes" ? ", offset var(--k)" : name == "spotlight" ? ", intensity var(--k)" : ", shrink var(--k)";
                    rule += " background-image: effect(" + name + param + ");";
                }else if (i < 60){
                    rule += std::string(" filter: effect(pixelate, size 6px") + (vars ? ", gap var(--k)" : "") + ");";
                }else{
                    rule += std::string(" background-color: transparent; backdrop-filter: effect(frosted-glass, blur 6px") +
                            (vars ? ", saturation var(--k)" : "") + ");";
                }
            }
            css += rule + " }\n";
            body += "<div id=\"" + id + "\"></div>";
        }
        return "<!DOCTYPE html><html><head><style>" + css + "</style></head><body><div id=\"grid\">" + body + "</div></body></html>";
    }

    struct Run {
        double medianMs = 0.0;
        EffectStats delta;
    };

    // A 1280 x 720 context and its loaded document; a frame is what HtmlUIContext does each
    // engine frame: the compat runtime, Context::Update, the render submission
    struct Grid {
        RenderTestContext r{1280, 720, 0, 0, 4};
        Rml::ElementDocument* document = nullptr;
        std::shared_ptr<const hc::CompatDocument> compiled;
        int counter = 0;

        bool load(const std::string& html){
            if (document){
                document->Close();
                document = nullptr;
                compiled.reset();
                r.context->Update();
            }
            const std::string path = "@htmlui/test/effects/perf" + std::to_string(++counter) + ".html";
            hc::VirtualFiles::add(path, html);
            hc::CompatOptions options;
            options.viewport = hc::CompatSize{1280, 720};
            options.dpRatio = 1.0f;
            document = hc::DocumentLoader::load(r.context, path, options, &compiled);
            hc::VirtualFiles::remove(path);
            if (!document)
                return false;
            document->Show();
            return r.renderUntilDrawn(60);
        }

        void frame(){
            hc::updateRuntime(r.context);
            r.renderFrame();
        }

        ~Grid(){
            if (document)
                document->Close();
        }
    };

    // 30 warm-up frames, then 120 measured ones, the clock 1/60 s further each frame; perFrame
    // runs before each frame (A2's var change)
    Run measure(Grid& grid, const std::function<void(int)>& perFrame = {}){
        double clock = HtmlUIClock::now();
        auto frame = [&](int index){
            clock += 1.0 / 60.0;
            HtmlUIClock::setFixedTime(clock);
            if (perFrame)
                perFrame(index);
            grid.frame();
        };
        for (int i = 0; i < 30; i++)
            frame(i);
        const EffectStats before = EffectRuntime::get().stats();
        std::vector<double> times;
        for (int i = 0; i < 120; i++){
            const double start = threadCpuMs();
            frame(30 + i);
            times.push_back(threadCpuMs() - start);
        }
        const EffectStats after = EffectRuntime::get().stats();
        Run run;
        run.medianMs = median(times);
        run.delta.instancesCreated = after.instancesCreated - before.instancesCreated;
        run.delta.compileCalls = after.compileCalls - before.compileCalls;
        run.delta.pipelinesCreated = after.pipelinesCreated - before.pipelinesCreated;
        run.delta.reinstances = after.reinstances - before.reinstances;
        run.delta.paintDraws = after.paintDraws - before.paintDraws;
        run.delta.filterPasses = after.filterPasses - before.filterPasses;
        run.delta.prepassPasses = after.prepassPasses - before.prepassPasses;
        return run;
    }

    bool warmUpReferenceEffects(){
        for (const char* name : {"stripes", "spotlight", "cursor-trail", "pixelate", "frosted-glass"}){
            const EffectManifest* m = EffectRegistry::get().find(name);
            if (!m || !warmUpEffect(*m))
                return false;
        }
        return true;
    }

    std::string costFile(){
        // build/linux/htmlui-tests-<backend>/tests/htmlui -> build/linux
        const std::filesystem::path out = std::filesystem::path(JASPER_HTMLUI_TEST_BINARY_DIR).parent_path().parent_path().parent_path();
#if defined(SOKOL_VULKAN)
        return (out / "htmlui-effects-cost-vulkan.json").string();
#else
        return (out / "htmlui-effects-cost.json").string();
#endif
    }

    // the cost table's document: #fx fills 1024 x 1024 (a filter over a test-card-like content,
    // a backdrop over content behind it)
    std::string costDocument(const EffectManifest* m){
        std::string css = "html, body { margin: 0; width: 1024px; height: 1024px; background-color: #101218; }\n"
                          "#fx, #under { position: absolute; left: 0; top: 0; width: 1024px; height: 1024px; }\n"
                          "#under div, #fx div { position: absolute; width: 50%; height: 50%; }\n"
                          ".a { left: 0; top: 0; background-color: #e74c3c; } .b { left: 50%; top: 0; background-color: #f1c40f; }\n"
                          ".c { left: 0; top: 50%; background-color: #2ecc71; } .d { left: 50%; top: 50%; background-color: #3498db; }\n";
        const std::string cells = "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c\"></div><div class=\"d\"></div>";
        std::string body;
        if (!m){
            css += "#fx { background-color: #336699; }\n";
            body = "<div id=\"fx\"></div>";
        }else if (m->kind == EffectKind::Paint){
            css += "#fx { background-image: effect(" + m->name + "); }\n";
            body = "<div id=\"fx\"></div>";
        }else if (m->backdrop){
            css += "#fx { backdrop-filter: effect(" + m->name + "); }\n";
            body = "<div id=\"under\">" + cells + "</div><div id=\"fx\"></div>";
        }else{
            css += "#fx { filter: effect(" + m->name + "); }\n";
            body = "<div id=\"fx\">" + cells + "</div>";
        }
        return "<!DOCTYPE html><html><head><style>" + css + "</style></head><body>" + body + "</body></html>";
    }

    // the CPU time of the process for one render + readback of the document, the least of up to
    // 5 averages of 20 (the first capture warms up)
    double costMs(EffectScene& scene, const EffectManifest* m, double boundMs){
        TestImage image;
        if (m && m->pointer != EffectPointer::None){
            const Rml::Rectanglef fx = scene.box("fx");
            EffectRuntime::get().setTestPointer(scene.context(), fx.Position() + Rml::Vector2f(fx.Width() * 0.7f, fx.Height() * 0.4f), false, 0.3f);
        }
        if (!scene.capture(image))
            return -1.0;
        double least = -1.0;
        for (int attempt = 0; attempt < 5; attempt++){
            const double start = processCpuMs();
            for (int i = 0; i < 20; i++){
                if (!scene.capture(image))
                    return -1.0;
            }
            const double ms = (processCpuMs() - start) / 20.0;
            least = least < 0.0 ? ms : std::min(least, ms);
            if (boundMs <= 0.0 || least <= boundMs)
                break;
        }
        return least;
    }

}

#ifdef NDEBUG

TEST(EffectPerfTest, GridSteadyStateAndBudgets){
    Grid grid;
    if (!grid.r.isValid())
        GTEST_SKIP() << "no GPU";
    ASSERT_TRUE(warmUpReferenceEffects());
    HtmlUIClock::setFixedTime(10.0);

    ASSERT_TRUE(grid.load(gridDocument(false, false))) << "document B";
    double b = -1.0;
    for (int attempt = 0; attempt < 5; attempt++){
        const Run run = measure(grid);
        b = b < 0.0 ? run.medianMs : std::min(b, run.medianMs);
    }

    ASSERT_TRUE(grid.load(gridDocument(true, false))) << "document A";
    double a = -1.0;
    for (int attempt = 0; attempt < 5; attempt++){
        const Run run = measure(grid);
        a = a < 0.0 ? run.medianMs : std::min(a, run.medianMs);
        EXPECT_EQ(run.delta.instancesCreated, 0u) << "no effect instance is created in the steady state";
        EXPECT_EQ(run.delta.compileCalls, 0u) << "no variant is compiled in the steady state";
        EXPECT_EQ(run.delta.pipelinesCreated, 0u) << "no pipeline is created in the steady state";
        EXPECT_EQ(run.delta.reinstances, 0u) << "a moving clock re-instances nothing";
        EXPECT_EQ(run.delta.paintDraws, 48u * 120u) << "one draw per paint effect per frame";
        EXPECT_EQ(run.delta.filterPasses, 16u * 120u) << "one pass per filter per frame";
        if (a <= 1.5 * b + 1.0)
            break;
    }

    ASSERT_TRUE(grid.load(gridDocument(true, true))) << "document A2";
    Rml::Element* parent = grid.document->GetElementById("grid");
    ASSERT_NE(parent, nullptr);
    double a2 = -1.0;
    for (int attempt = 0; attempt < 5; attempt++){
        const Run run = measure(grid, [&](int index){
            // a new value every frame (never one an earlier frame of the run had: no cached decorator)
            char value[32];
            std::snprintf(value, sizeof(value), "%.4f", 0.05 + 0.85 * (double)((index * 37 + attempt * 11) % 1000) / 1000.0);
            parent->SetProperty("--k", value);
        });
        a2 = a2 < 0.0 ? run.medianMs : std::min(a2, run.medianMs);
        EXPECT_EQ(run.delta.reinstances, 64u * 120u) << "every element re-instances its effect once a frame";
        EXPECT_EQ(run.delta.compileCalls, 0u) << "a var() change compiles nothing";
        EXPECT_EQ(run.delta.pipelinesCreated, 0u) << "a var() change creates no pipeline";
        if (a2 <= 3.0 * b + 4.0)
            break;
    }

    std::printf("[ effects  ] frame CPU (median of 120, least of the runs): B %.3f ms, A %.3f ms (budget %.3f), A2 %.3f ms (budget %.3f)\n",
                b, a, 1.5 * b + 1.0, a2, 3.0 * b + 4.0);
    EXPECT_LE(a, 1.5 * b + 1.0) << "document A's frame";
    EXPECT_LE(a2, 3.0 * b + 4.0) << "document A2's frame";
    HtmlUIClock::setFixedTime(0.0);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST(EffectPerfTest, CostTable){
    EffectScene scene(1024, 1024, 4);
    if (!scene.valid())
        GTEST_SKIP() << "no GPU";
    FixedEffectClock clock(1.5);
    std::string error;
    ASSERT_TRUE(scene.loadHtml(costDocument(nullptr), &error)) << error;
    const double baseline = costMs(scene, nullptr, 0.0);
    ASSERT_GT(baseline, 0.0) << "the baseline renders";

    nlohmann::json effects = nlohmann::json::array();
    std::printf("[ effects  ] cost at 1024 x 1024 (render + readback, process CPU), baseline %.3f ms\n", baseline);
    for (const EffectManifest* m : EffectRegistry::get().all()){
        ASSERT_TRUE(warmUpEffect(*m)) << m->name;
        ASSERT_TRUE(scene.loadHtml(costDocument(m), &error)) << m->name << ": " << error;
        const double bound = m->cost == EffectCost::Low ? 6.0 : m->cost == EffectCost::Medium ? 24.0 : 0.0;
        const double ms = costMs(scene, m, bound * baseline);
        ASSERT_GT(ms, 0.0) << m->name << " renders";
        const double ratio = ms / baseline;
        std::printf("[ effects  ]   %-24s %-6s %-6s %8.3f ms  x%.2f%s\n", m->name.c_str(), effectKindName(m->kind), effectCostName(m->cost), ms, ratio,
                    bound > 0.0 ? (" (bound " + std::to_string((int)bound) + ")").c_str() : "");
        effects.push_back({{"name", m->name}, {"kind", effectKindName(m->kind)}, {"cost", effectCostName(m->cost)}, {"builtin", m->builtin},
                           {"ms", ms}, {"ratio", ratio}, {"bound", bound > 0.0 ? nlohmann::json(bound) : nlohmann::json(nullptr)}});
        if (bound > 0.0)
            EXPECT_LE(ratio, bound) << m->name << " (" << effectCostName(m->cost) << ") costs " << ratio << " x the fill";
    }
    nlohmann::json table = {{"size", 1024}, {"runs", 20}, {"baseline_ms", baseline}, {"effects", effects},
#if defined(SOKOL_VULKAN)
                            {"backend", "vulkan"}
#else
                            {"backend", "glcore"}
#endif
    };
    std::ofstream out(costFile());
    out << table.dump(1) << "\n";
    EXPECT_TRUE((bool)out) << "written: " << costFile();
}

#endif
