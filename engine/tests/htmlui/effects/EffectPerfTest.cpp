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
//   frame: nothing compiled, every element follows, and CPU(A2) <= 3 x CPU(B) + 4 ms. A
//   re-instances once per new value text (3.6's decorators are shared by the elements whose value
//   is the same: one per paint effect) and once per filter element: 3 + 16 a frame, not 64.
//   Document C: A's layers without effects (RmlUi's brightness() and blur() in place of the effect
//   filters): CPU(A) <= 1.5 x CPU(C) + 1 ms, the effect system's own work. On llvmpipe the 16
//   filter layers alone cost the main thread far more than B (the driver's render-to-texture
//   flushes in RmlRenderer's layer path, whoever the filter is), see the phase-5 spec's
//   "p5-tooling as implemented".
// - Cost table: every built-in effect fills 1024 x 1024 and is rendered and read back 20 times
//   through the golden harness (its pointer and trail inputs), against a background-color fill of the same size (the process's
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
#include "htmlui/effects/EffectInstancers.h"
#include "htmlui/effects/EffectValue.h"

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
    // document C: document A's layers without effects, RmlUi's own filters in place of the
    // effect filters (the 12 filters brightness(), the 4 backdrops blur()), the paints colours.
    // What A costs above it is the effect system's own work
    std::string layersDocument(){
        std::string css = "html, body { margin: 0; width: 1280px; height: 720px; background-color: #101218; }\n"
                          "#grid div { position: absolute; width: 140px; height: 90px; background-color: #3366aa; }\n";
        std::string body;
        for (int i = 0; i < 64; i++){
            const int col = i % 8, row = i / 8;
            const std::string id = "e" + std::to_string(i);
            std::string rule = "#" + id + " { left: " + std::to_string(10 + col * 158) + "px; top: " + std::to_string(row * 90) + "px;";
            if (i >= 48 && i < 60)
                rule += " filter: brightness(1.05);";
            else if (i >= 60)
                rule += " background-color: transparent; backdrop-filter: blur(6px);";
            css += rule + " }\n";
            body += "<div id=\"" + id + "\"></div>";
        }
        return "<!DOCTYPE html><html><head><style>" + css + "</style></head><body><div id=\"grid\">" + body + "</div></body></html>";
    }

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

    struct PerfRun {
        double medianMs = 0.0;
        double updateMs = 0.0, renderMs = 0.0, endMs = 0.0;   // the frame's parts (medians)
        EffectStats delta;
    };

    // the parts of the last frame (main-thread CPU ms)
    struct FrameParts {
        double update = 0.0, render = 0.0, end = 0.0;
    };

    // A 1280 x 720 context and its loaded document; a frame is what HtmlUIContext does each
    // engine frame: the compat runtime, Context::Update, the render submission
    struct Grid {
        bool gpu = GpuEnvironment::ensure();   // the headless device first (this may be the first test)
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

        FrameParts parts;

        // RenderTestContext::renderFrame without the composite into its screen target, timed in parts
        void frame(){
            beginTestFrame();
            if (!r.target.isCreated())
                r.target.ensure(r.targetSize.x, r.targetSize.y, 4);
            const double t0 = threadCpuMs();
            const bool begun = r.renderer.beginFrame(r.target, r.contextSize);
            Rml::SetGeometryFeathering(false);
            Rml::SetGeometryPixelSize(1.0f);
            hc::updateRuntime(r.context);
            r.context->Update();
            const double t1 = threadCpuMs();
            if (begun)
                r.context->Render();
            const double t2 = threadCpuMs();
            r.renderer.endFrame();
            SystemRender::commit();
            const double t3 = threadCpuMs();
            parts = FrameParts{t1 - t0, t2 - t1, t3 - t2};
        }

        ~Grid(){
            if (document)
                document->Close();
        }
    };

    void printRun(const char* what, const PerfRun& run){
        std::printf("[ effects  ]   %s: %.3f ms (update %.3f, render submission %.3f, end of frame %.3f)\n", what, run.medianMs,
                    run.updateMs, run.renderMs, run.endMs);
    }

    // 30 warm-up frames, then 120 measured ones, the clock 1/60 s further each frame; perFrame
    // runs before each frame (A2's var change)
    PerfRun measure(Grid& grid, const std::function<void(int)>& perFrame = {}){
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
        std::vector<double> times, updates, renders, ends;
        for (int i = 0; i < 120; i++){
            const double start = threadCpuMs();
            frame(30 + i);
            times.push_back(threadCpuMs() - start);
            updates.push_back(grid.parts.update);
            renders.push_back(grid.parts.render);
            ends.push_back(grid.parts.end);
        }
        const EffectStats after = EffectRuntime::get().stats();
        PerfRun run;
        run.medianMs = median(times);
        run.updateMs = median(updates);
        run.renderMs = median(renders);
        run.endMs = median(ends);
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
        // the golden harness's inputs: its pointer, and the 12-point arc of a trail effect
        if (m && m->pointer != EffectPointer::None)
            setTestPointer(scene, *m);
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
    if (!grid.gpu || !grid.r.isValid())
        GTEST_SKIP() << "no GPU";
    ASSERT_TRUE(warmUpReferenceEffects());
    HtmlUIClock::setFixedTime(10.0);

    // the least of up to 5 runs, stopping once within `target`; check() sees each run
    auto gated = [&](const char* what, double target, const std::function<PerfRun(int)>& run, const std::function<void(const PerfRun&)>& check){
        double least = -1.0;
        for (int attempt = 0; attempt < 5; attempt++){
            const PerfRun r = run(attempt);
            printRun(what, r);
            if (check)
                check(r);
            least = least < 0.0 ? r.medianMs : std::min(least, r.medianMs);
            if (target > 0.0 && least <= target)
                break;
        }
        return least;
    };

    ASSERT_TRUE(grid.load(gridDocument(false, false))) << "document B";
    const double b = gated("B", 0.0, [&](int){ return measure(grid); }, {});
    ASSERT_TRUE(grid.load(layersDocument())) << "document C";
    const double c = gated("C", 0.0, [&](int){ return measure(grid); }, {});

    ASSERT_TRUE(grid.load(gridDocument(true, false))) << "document A";
    const double a = gated("A", 1.5 * b + 1.0, [&](int){ return measure(grid); }, [](const PerfRun& run){
        EXPECT_EQ(run.delta.instancesCreated, 0u) << "no effect instance is created in the steady state";
        EXPECT_EQ(run.delta.compileCalls, 0u) << "no variant is compiled in the steady state";
        EXPECT_EQ(run.delta.pipelinesCreated, 0u) << "no pipeline is created in the steady state";
        EXPECT_EQ(run.delta.reinstances, 0u) << "a moving clock re-instances nothing";
        EXPECT_EQ(run.delta.paintDraws, 48u * 120u) << "one draw per paint effect per frame";
        EXPECT_EQ(run.delta.filterPasses, 16u * 120u) << "one pass per filter per frame";
    });

    ASSERT_TRUE(grid.load(gridDocument(true, true))) << "document A2";
    Rml::Element* parent = grid.document->GetElementById("grid");
    ASSERT_NE(parent, nullptr);
    std::string last;
    const double a2 = gated("A2", 3.0 * b + 4.0, [&](int attempt){
        return measure(grid, [&](int index){
            // a value no earlier frame had (750 frames at most): no decorator comes from the cache
            char value[32];
            std::snprintf(value, sizeof(value), "%.4f", 0.05 + 0.85 * (double)(((attempt * 150 + index) * 37) % 1000) / 1000.0);
            last = value;
            parent->SetProperty("--k", value);
        });
    }, [&](const PerfRun& run){
        // 3.6: decorators are shared by the elements whose effect value is the same (one per paint
        // effect here), filters are instanced per element: 3 + 16 a frame
        EXPECT_EQ(run.delta.reinstances, (3u + 16u) * 120u) << "each new value re-instances once a frame";
        EXPECT_EQ(run.delta.compileCalls, 0u) << "a var() change compiles nothing";
        EXPECT_EQ(run.delta.pipelinesCreated, 0u) << "a var() change creates no pipeline";
        // every element follows the value
        const std::string expected = EffectValueParser::canonical(EffectParamDef{"k", "k", EffectParamType::Number}, [&]{
            EffectValue v; v.type = EffectParamType::Number; v.f = (float)std::atof(last.c_str()); return v; }());
        int following = 0;
        for (int i = 0; i < 64; i++){
            for (const EffectUse& use : collectEffectUses(grid.document->GetElementById("e" + std::to_string(i)))){
                for (const auto& param : use.params){
                    if ((param.first == "offset" || param.first == "intensity" || param.first == "shrink" || param.first == "gap" ||
                         param.first == "saturation") && param.second == expected)
                        following++;
                }
            }
        }
        EXPECT_EQ(following, 64) << "every element's parameter is the last --k (" << expected << ")";
    });

    std::printf("[ effects  ] frame CPU (median of 120, least of the runs): B %.3f ms, C (A's layers, CSS filters) %.3f ms, "
                "A %.3f ms (budget %.3f; over C %.3f), A2 %.3f ms (budget %.3f)\n", b, c, a, 1.5 * b + 1.0, a - c, a2, 3.0 * b + 4.0);
    EXPECT_LE(a, 1.5 * b + 1.0) << "document A's frame (5.4)";
    EXPECT_LE(a2, 3.0 * b + 4.0) << "document A2's frame (5.4)";
    // the effect system's own work: A against the same layers drawn by RmlUi's own filters
    EXPECT_LE(a, 1.5 * c + 1.0) << "document A's frame against the same layers without effects";
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
    for (const EffectManifest* m : EffectRegistry::get().builtins()){
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
