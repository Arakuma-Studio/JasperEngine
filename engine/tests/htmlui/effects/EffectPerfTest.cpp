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
//   A <= 1.5 x B + 1 ms. A2: every element's parameter reads var(--k), changed every frame:
//   nothing compiled, every element follows, and A2 <= 3 x B + 4 ms. A re-instances once per new
//   value text (3.6's decorators are shared by the elements whose value is the same: one per paint
//   effect) and once per filter element: 3 + 16 a frame, not 64. Document C: A's layers without
//   effects (RmlUi's brightness() and blur() in place of the effect filters), reported.
//   What is gated (5.4's deviation, phase-5 spec): on Vulkan the frame's CPU time; on glcore the
//   engine's own work, the frame less the time the graphics backend took on the thread
//   (RenderBackendTime: sokol and the GL driver; both on the steady clock, less the measurement's
//   own clock reads, calibrated). On llvmpipe every render pass is a scene the
//   driver bins and copies its state into on the calling thread (about 125k instructions at
//   1280 x 720), and each filter needs four (its layer, the resolve, the filter, the composite):
//   most of A's frame, a cost a GPU driver does not have. Both measures are printed.
// - Cost table: every built-in effect (the 83 of the catalogue, one row each) fills 1024 x 1024,
//   rendered and read back through the golden harness (its pointer and trail inputs), against a
//   background-color fill of the same size measured with it: rounds of 8 captures of the fill
//   then 8 of the effect, the least of at least 3 rounds for both (the process's CPU time: the GL
//   driver's threads draw). Written to build/linux/htmlui-effects-cost.json
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
#include "render/RenderBackendTime.h"

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

    // The thread time one backend scope's clock reads take outside the backend's total (the
    // clock read before its start, after its end): calibrated once, with empty scopes
    double scopeOverheadMs(){
        static double overhead = -1.0;
        if (overhead < 0.0){
            const bool was = RenderBackendTime::isEnabled();
            RenderBackendTime::setEnabled(true);
            double least = -1.0;
            for (int attempt = 0; attempt < 5; attempt++){
                RenderBackendTime::reset();
                const int n = 20000;
                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < n; i++){
                    RenderBackendTime::Scope scope;
                }
                const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                const double outside = (elapsed - RenderBackendTime::totalMs()) / n;
                least = least < 0.0 ? outside : std::min(least, outside);
            }
            RenderBackendTime::reset();
            RenderBackendTime::setEnabled(was);
            overhead = std::max(least, 0.0);
            std::printf("[ effects  ] a backend scope's clock reads: %.3f us outside its total\n", overhead * 1000.0);
        }
        return overhead;
    }

    struct PerfRun {
        double medianMs = 0.0;
        double updateMs = 0.0, renderMs = 0.0, endMs = 0.0;   // the frame's parts (medians)
        // the frame less the time the graphics backend (sokol and the driver) took on the thread
        // (RenderBackendTime): the engine's own work (median)
        double engineMs = 0.0, backendMs = 0.0;
        EffectStats delta;
    };

    // the parts of the last frame (main-thread CPU ms)
    struct FrameParts {
        double update = 0.0, render = 0.0, end = 0.0;
        double backend = 0.0;   // RenderBackendTime of the frame (steady clock)
        double wall = 0.0;      // the frame on the steady clock
        uint64_t scopes = 0;    // its scopes (their clock reads, outside the backend's time)
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
            RenderBackendTime::reset();
            const auto w0 = std::chrono::steady_clock::now();
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
            const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
            parts = FrameParts{t1 - t0, t2 - t1, t3 - t2, RenderBackendTime::totalMs(), wall, RenderBackendTime::scopeCount()};
        }

        ~Grid(){
            if (document)
                document->Close();
        }
    };


    void printRun(const char* what, const PerfRun& run){
        std::printf("[ effects  ]   %s: %.3f ms (update %.3f, render submission %.3f, end of frame %.3f; engine %.3f, backend %.3f)\n",
                    what, run.medianMs, run.updateMs, run.renderMs, run.endMs, run.engineMs, run.backendMs);
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
        const int warm = 30;
        const int measured = 120;
        for (int i = 0; i < warm; i++)
            frame(i);
        const EffectStats before = EffectRuntime::get().stats();
        std::vector<double> times, updates, renders, ends, engines, backends;
        for (int i = 0; i < measured; i++){
            const double start = threadCpuMs();
            frame(warm + i);
            times.push_back(threadCpuMs() - start);
            updates.push_back(grid.parts.update);
            // the engine's own work: the frame less the backend and the measurement's clock reads,
            // both on the steady clock (a wait for the driver is the driver's)
            engines.push_back(grid.parts.wall - grid.parts.backend - (double)grid.parts.scopes * scopeOverheadMs());
            backends.push_back(grid.parts.backend);
            renders.push_back(grid.parts.render);
            ends.push_back(grid.parts.end);
        }
        const EffectStats after = EffectRuntime::get().stats();
        PerfRun run;
        run.medianMs = median(times);
        run.updateMs = median(updates);
        run.renderMs = median(renders);
        run.endMs = median(ends);
        run.engineMs = median(engines);
        run.backendMs = median(backends);
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

    // The cost of an effect against the fill, interleaved: rounds of a batch of the fill's
    // captures then one of the effect's (the process's CPU time per render + readback, the first
    // capture of each warms up), the least of the rounds for both, so a loaded moment raises one
    // round of both and not the ratio. At least 3 rounds, up to 6 while the ratio is over `bound`
    // (0: no bound).
    struct Cost {
        double fillMs = -1.0, effectMs = -1.0;
        double ratio() const{ return fillMs > 0.0 ? effectMs / fillMs : 0.0; }
    };

    double batchMs(EffectScene& scene, int captures){
        TestImage image;
        const double start = processCpuMs();
        for (int i = 0; i < captures; i++){
            if (!scene.capture(image))
                return -1.0;
        }
        return (processCpuMs() - start) / captures;
    }

    Cost interleavedCost(EffectScene& fill, EffectScene& scene, const EffectManifest* m, double bound){
        Cost cost;
        TestImage image;
        // the golden harness's inputs: its pointer, and the 12-point arc of a trail effect
        if (m && m->pointer != EffectPointer::None)
            setTestPointer(scene, *m);
        if (!fill.capture(image) || !scene.capture(image))
            return cost;
        for (int round = 0; round < 6; round++){
            const double f = batchMs(fill, 8);
            const double e = batchMs(scene, 8);
            if (f <= 0.0 || e <= 0.0)
                return Cost();
            cost.fillMs = cost.fillMs < 0.0 ? f : std::min(cost.fillMs, f);
            cost.effectMs = cost.effectMs < 0.0 ? e : std::min(cost.effectMs, e);
            if (round >= 2 && (bound <= 0.0 || cost.ratio() <= bound))
                break;
        }
        return cost;
    }

}

#ifdef NDEBUG

TEST(EffectPerfTest, GridSteadyStateAndBudgets){
    Grid grid;
    if (!grid.gpu || !grid.r.isValid())
        GTEST_SKIP() << "no GPU";
    ASSERT_TRUE(warmUpReferenceEffects());
    HtmlUIClock::setFixedTime(10.0);

    // What is gated (5.4 as implemented, p5-tooling): on Vulkan the frame's CPU time; on glcore the
    // engine's own work, the frame less the backend's share (sokol and the GL driver, which on
    // llvmpipe sets up every render pass on this thread: most of a frame with layers, a cost a GPU
    // driver does not have). The full frame is printed on both.
#if defined(SOKOL_VULKAN)
    const bool gateEngine = false;
#else
    const bool gateEngine = true;
#endif
    RenderBackendTime::setEnabled(true);
    struct BackendOff {
        ~BackendOff(){ RenderBackendTime::setEnabled(false); }
    } backendOff;
    struct Least {
        double frame = -1.0, engine = -1.0;
        double gate(bool engineGate) const{ return engineGate ? engine : frame; }
    };
    // the least of up to 5 runs, stopping once the gated measure is within `target`; check()
    // sees each run
    auto gated = [&](const char* what, double target, const std::function<PerfRun(int)>& run, const std::function<void(const PerfRun&)>& check){
        Least least;
        for (int attempt = 0; attempt < 5; attempt++){
            const PerfRun r = run(attempt);
            printRun(what, r);
            if (check)
                check(r);
            least.frame = least.frame < 0.0 ? r.medianMs : std::min(least.frame, r.medianMs);
            least.engine = least.engine < 0.0 ? r.engineMs : std::min(least.engine, r.engineMs);
            if (target > 0.0 && least.gate(gateEngine) <= target)
                break;
        }
        return least;
    };

    ASSERT_TRUE(grid.load(gridDocument(false, false))) << "document B";
    const Least b = gated("B", 0.0, [&](int){ return measure(grid); }, {});
    ASSERT_TRUE(grid.load(layersDocument())) << "document C";
    const Least c = gated("C", 0.0, [&](int){ return measure(grid); }, {});

    ASSERT_TRUE(grid.load(gridDocument(true, false))) << "document A";
    const double budgetA = 1.5 * b.gate(gateEngine) + 1.0;
    const double budgetA2 = 3.0 * b.gate(gateEngine) + 4.0;
    const Least a = gated("A", budgetA, [&](int){ return measure(grid); }, [](const PerfRun& run){
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
    const Least a2 = gated("A2", budgetA2, [&](int attempt){
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

    std::printf("[ effects  ] frame CPU (median of 120, least of the runs): B %.3f ms, C (A's layers, CSS filters) %.3f ms, A %.3f ms, "
                "A2 %.3f ms\n", b.frame, c.frame, a.frame, a2.frame);
    std::printf("[ effects  ] engine's own work (the frame less the backend): B %.3f ms, C %.3f ms, A %.3f ms, A2 %.3f ms\n", b.engine, c.engine,
                a.engine, a2.engine);
    std::printf("[ effects  ] gated (%s): A %.3f ms (budget %.3f), A2 %.3f ms (budget %.3f)\n", gateEngine ? "the engine's own work" : "the frame",
                a.gate(gateEngine), budgetA, a2.gate(gateEngine), budgetA2);
    EXPECT_LE(a.gate(gateEngine), budgetA) << "document A (5.4; " << (gateEngine ? "the engine's own work" : "the frame") << ")";
    EXPECT_LE(a2.gate(gateEngine), budgetA2) << "document A2 (5.4; " << (gateEngine ? "the engine's own work" : "the frame") << ")";
    // (C is reported, not gated: A against B, with no layers, is the stricter check of the same
    // work, so A <= 1.5 x C + 1 ms follows from it whenever C costs at least B)
    HtmlUIClock::setFixedTime(0.0);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST(EffectPerfTest, CostTable){
    EffectScene fill(1024, 1024, 4);
    EffectScene scene(1024, 1024, 4);
    if (!fill.valid() || !scene.valid())
        GTEST_SKIP() << "no GPU";
    FixedEffectClock clock(1.5);
    std::string error;
    ASSERT_TRUE(fill.loadHtml(costDocument(nullptr), &error)) << error;

    nlohmann::json effects = nlohmann::json::array();
    std::printf("[ effects  ] cost at 1024 x 1024 (render + readback, process CPU), each against the fill measured with it\n");
    // every effect of the catalogue, one row each: an effect that does not render is a failure of
    // its own row, the others are still measured
    const std::vector<const EffectManifest*> catalogue = EffectRegistry::get().builtins();
    std::vector<std::string> measured;
    double highestLow = 0.0, highestMedium = 0.0;
    for (const EffectManifest* m : catalogue){
        if (!warmUpEffect(*m)){
            ADD_FAILURE() << m->name << ": its variants do not compile";
            continue;
        }
        if (!scene.loadHtml(costDocument(m), &error)){
            ADD_FAILURE() << m->name << ": " << error;
            continue;
        }
        const double bound = m->cost == EffectCost::Low ? 6.0 : m->cost == EffectCost::Medium ? 24.0 : 0.0;
        const Cost cost = interleavedCost(fill, scene, m, bound);
        if (cost.effectMs <= 0.0){
            ADD_FAILURE() << m->name << " does not render";
            continue;
        }
        measured.push_back(m->name);
        const double ratio = cost.ratio();
        if (m->cost == EffectCost::Low)
            highestLow = std::max(highestLow, ratio);
        else if (m->cost == EffectCost::Medium)
            highestMedium = std::max(highestMedium, ratio);
        std::printf("[ effects  ]   %-24s %-6s %-6s %8.3f ms  fill %7.3f ms  x%.2f%s\n", m->name.c_str(), effectKindName(m->kind),
                    effectCostName(m->cost), cost.effectMs, cost.fillMs, ratio,
                    bound > 0.0 ? (" (bound " + std::to_string((int)bound) + ")").c_str() : "");
        effects.push_back({{"name", m->name}, {"kind", effectKindName(m->kind)}, {"cost", effectCostName(m->cost)}, {"builtin", m->builtin},
                           {"ms", cost.effectMs}, {"fill_ms", cost.fillMs}, {"ratio", ratio},
                           {"bound", bound > 0.0 ? nlohmann::json(bound) : nlohmann::json(nullptr)}});
        if (bound > 0.0)
            EXPECT_LE(ratio, bound) << m->name << " (" << effectCostName(m->cost) << ") costs " << ratio << " x the fill";
    }
    std::printf("[ effects  ] %zu of the %zu effects of the catalogue measured; the highest low x%.2f, medium x%.2f\n", measured.size(),
                catalogue.size(), highestLow, highestMedium);
    EXPECT_EQ(measured.size(), catalogue.size()) << "every effect of the catalogue is in the table";
    EXPECT_GE(catalogue.size(), 83u) << "the phase-5 catalogue (Appendix A) has 83 effects";
    nlohmann::json table = {{"size", 1024}, {"captures_per_round", 8}, {"effects", effects},
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
