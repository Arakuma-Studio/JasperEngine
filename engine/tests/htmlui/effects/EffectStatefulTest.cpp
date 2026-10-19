// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Stateful effects (phase 6, docs/html-ui/phase-6-stateful-effects.md 4): the manifest's state
// grammar and its checks, the state images and their pool, resizes, pause and resume, 100 load
// and unload cycles, the fixed step and its substep cap, the pinned clock's replay (byte-identical
// captures), splats, resets and reseeds, the budgets (FX-0018) and unsupported formats (FX-0017).
// fixtures/effects/fx/sim-probe.frag counts its steps and the splat strength it took on the GPU and
// shows them (R = steps / 255, G = splats / 255), so the CPU's account and the images agree.

#include "EffectTestCommon.h"

#include "HtmlUIClock.h"
#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/effects/EffectSimulator.h"

#include <array>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    // ── the manifest ──

    std::string probeManifest(const std::string& states, const std::string& passes, const std::string& cost = "stateful",
                              const std::string& extra = std::string(), bool stepInclude = true){
        return "#version 450\n"
               "// @effect     sim-test\n"
               "// @kind       paint\n"
               "// @category   interactive\n"
               "// @summary    A stateful manifest of the parser tests\n"
               "// @source     original\n"
               "// @cost       " + cost + "\n" + states + passes + extra +
               "\n#include \"effects/lib/jfx_common.glsl\"\n"
               "#include \"effects/lib/main_paint.glsl\"\n" +
               (stepInclude ? "#include \"effects/lib/main_step.glsl\"\n" : "");
    }

    std::string diagnosticsOf(const std::string& source, EffectManifest* out = nullptr){
        EffectManifest m;
        std::vector<EffectDiagnostic> diags;
        if (EffectManifestParser::parse(source, "effects/sim-test.frag", true, m, diags))
            EffectManifestParser::validate(m, source, diags);
        if (out)
            *out = m;
        std::string all;
        for (const EffectDiagnostic& d : diags)
            all += std::to_string(d.line) + ": " + d.message + "\n";
        return all;
    }

    const char* kStates = "// @state      vel rgba16f 128x64 linear\n"
                          "// @state      dye rgba16f box*0.5 max 512 resample\n";
    const char* kPasses = "// @step       move vel\n"
                          "// @step       carry dye\n";

    // ── the scenes ──

    const char* kProbe = "fixtures/effects/fx/sim-probe.frag";

    std::string document(const std::string& elements, const std::string& extraCss = std::string()){
        return "<!DOCTYPE html><html><head><style>\n"
               "html, body { margin: 0; width: 360px; height: 240px; overflow: hidden; background: #000; }\n"
               ".p { position: absolute; top: 20px; width: 160px; height: 80px; background-image: effect(sim-probe); }\n"
               ".p.seeded { background-image: effect(sim-probe, seed 5); }\n"
               ".p.wide { width: 240px; }\n"
               ".p.fast { background-image: effect(sim-probe, time-scale 2); }\n"
               ".p.frozen { background-image: effect(sim-probe, time-scale 0); }\n"
               ".p.plain { background-image: effect(stripes); }\n" +
               extraCss +
               "</style></head><body>" + elements + "</body></html>";
    }

    std::string probes(int count){
        std::string out;
        for (int i = 0; i < count; i++){
            const int left = 10 + (i % 2) * 180;
            const int top = 20 + (i / 2) * 100;
            out += "<div class=\"p\" id=\"p" + std::to_string(i) + "\" style=\"left: " + std::to_string(left) + "px; top: " +
                   std::to_string(top) + "px\"></div>";
        }
        return out;
    }

    // a pixel of a capture (RGBA)
    std::array<int, 4> pixel(const TestImage& image, int x, int y){
        const size_t at = ((size_t)y * (size_t)image.width + (size_t)x) * 4;
        return {image.rgba[at], image.rgba[at + 1], image.rgba[at + 2], image.rgba[at + 3]};
    }

    size_t countDiagnostics(const std::string& code){
        size_t n = 0;
        for (const auto& d : EffectRuntime::get().statefulDiagnostics())
            n += d.first == code ? 1 : 0;
        return n;
    }

    struct EffectStateful : ::testing::Test {
        std::unique_ptr<EffectScene> scene;
        const EffectManifest* probe = nullptr;

        void SetUp() override{
            TestSystem::get().clearLogs();
            EffectRuntime::get().clearStatefulDiagnostics();
            scene = std::make_unique<EffectScene>(360, 240, 1);
            if (!scene->valid())
                GTEST_SKIP() << "no GPU";
            std::vector<EffectDiagnostic> diags;
            ASSERT_TRUE(EffectRegistry::get().declareProjectEffect("sim-probe", kProbe, "EffectStatefulTest", 1, diags))
                << (diags.empty() ? std::string() : diags[0].message);
            probe = EffectRegistry::get().find("sim-probe");
            ASSERT_NE(probe, nullptr);
            ASSERT_TRUE(probe->stateful());
            ASSERT_TRUE(warmUpEffect(*probe)) << "sim-probe.frag does not compile in every form";
        }

        void TearDown() override{
            EffectClock::setFixedTime(std::nullopt);
            EffectRuntime::get().clearTestInput();
            EffectRuntime::get().setStatefulBudget(EffectStatefulBudget());
            EffectRuntime::get().clearStatefulDiagnostics();
            EffectStatePool::setUnsupportedFormatsForTesting(0);
            scene.reset();
            EffectRegistry::get().clearProjectEffects();
        }

        // the live clock: the UI clock at `t` (the effect clock follows it; a load sets it to 0)
        void at(double t){
            HtmlUIClock::setFixedTime(t);
        }

        bool load(const std::string& html){
            std::string error;
            const bool ok = scene->loadHtml(html, &error);
            EXPECT_TRUE(ok) << error;
            return ok;
        }

        TestImage capture(){
            TestImage image;
            EXPECT_TRUE(scene->capture(image));
            return image;
        }

        EffectSimulationInfo info(const std::string& id){
            return EffectRuntime::get().simulationInfo(scene->element(id), "sim-probe", 0);
        }

        // R and G of an element's centre: (steps, splat strength) as the GPU counted them
        std::pair<int, int> counted(const TestImage& image, const std::string& id){
            const Rml::Rectanglef box = scene->box(id);
            const auto c = pixel(image, (int)(box.Left() + box.Width() * 0.5f), (int)(box.Top() + box.Height() * 0.5f));
            return {c[0], c[1]};
        }
    };

}

// ── the manifest grammar and its checks ──

TEST(EffectStatefulManifest, ParsesStatesPassesAndClock){
    EffectManifest m;
    const std::string source = probeManifest("// @state      vel rgba16f 128x64 linear\n"
                                             "// @state      dye rgba16f box*0.5 max 512 resample\n"
                                             "// @state      place rgba32f 64x64\n"
                                             "// @state      acc rgba16f box\n",
                                             "// @init       spawn vel\n"
                                             "// @step       move vel x4\n"
                                             "// @step       carry dye xiterations\n"
                                             "// @step       place place\n"
                                             "// @step       splat acc scatter place\n",
                                             "stateful",
                                             "// @warmup     30 0.05s\n"
                                             "// @rate       30 max 2\n"
                                             "// @replay     3s\n"
                                             "// @reseed     iterations\n"
                                             "// @param      iterations integer 4 1 16 -- Solver iterations\n");
    EXPECT_EQ(diagnosticsOf(source, &m), "");
    ASSERT_EQ(m.states.size(), 4u);
    EXPECT_TRUE(m.states[0].fixedSize);
    EXPECT_EQ(m.states[0].width, 128);
    EXPECT_EQ(m.states[0].height, 64);
    EXPECT_TRUE(m.states[0].linear);
    EXPECT_FALSE(m.states[1].fixedSize);
    EXPECT_FLOAT_EQ(m.states[1].boxScale, 0.5f);
    EXPECT_EQ(m.states[1].boxMax, 512);
    EXPECT_TRUE(m.states[1].resample);
    EXPECT_FALSE(m.states[2].linear) << "32-bit float states are nearest by default";
    // ping-pong unless every writer is additive
    EXPECT_FALSE(m.states[0].single);
    EXPECT_TRUE(m.states[3].single);
    ASSERT_EQ(m.passes.size(), 5u);
    EXPECT_TRUE(m.passes[0].init);
    EXPECT_EQ(m.passes[1].repeat, 4);
    EXPECT_EQ(m.passes[2].repeatParam, "iterations");
    EXPECT_EQ(m.passes[4].scatterState, 2);
    EXPECT_TRUE(m.passes[4].additive);
    EXPECT_TRUE(m.hasScatter());
    EXPECT_EQ(m.warmupSteps, 30);
    EXPECT_FLOAT_EQ(m.warmupDt, 0.05f);
    EXPECT_FLOAT_EQ(m.rateHz, 30.f);
    EXPECT_EQ(m.maxSubsteps, 2);
    EXPECT_FLOAT_EQ(m.replaySeconds, 3.f);
    ASSERT_EQ(m.reseedParams.size(), 1u);
    EXPECT_EQ(effectPassMacro("advect-vel"), "JFX_PASS_ADVECT_VEL");
    EXPECT_EQ(effectStateFormatBytes(EffectStateFormat::RGBA32F), 16);
    EXPECT_EQ(effectStateFormatBytes(EffectStateFormat::R32F), 4);
}

TEST(EffectStatefulManifest, ReportsEveryMistake){
    auto has = [](const std::string& diags, const std::string& fragment){
        return diags.find(fragment) != std::string::npos;
    };
    // the cost class goes with the states, both ways
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, kPasses, "high")), "stateful")) << diagnosticsOf(probeManifest(kStates, kPasses, "high"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest("", "", "stateful", "", false)), "stateful"));
    // the grammar
    EXPECT_TRUE(has(diagnosticsOf(probeManifest("// @state      vel rgba12 64x64\n", kPasses)), "unknown state format"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest("// @state      vel rgba16f 0x64\n", kPasses)), "a state size is"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest("// @state      vel rgba16f box*9\n", kPasses)), "a box size is"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest("// @state      vel rgba16f 64x64 fuzzy\n", kPasses)), "unexpected 'fuzzy'"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, "// @step       move nothing\n")), "not a @state declared before it"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, "// @step       move vel x99\n// @step       carry dye\n")), "1 to 64 times"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, "// @step       move vel\n// @step       move dye\n")), "declared twice"));
    // the checks
    EXPECT_TRUE(has(diagnosticsOf(probeManifest("// @state      vel rgba32f 64x64 linear\n", "// @step       move vel\n")), "nearest"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, "// @step       move vel\n")), "dye"));   // never written
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, "// @init       spawn vel\n// @init       fill dye\n")), "@step"))
        << "no @step pass";
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, "// @step       move vel scatter vel\n// @step       carry dye\n")), "scatter"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, kPasses, "stateful", "// @reseed     nothing\n")), "nothing"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, kPasses, "stateful", "// @warmup     30 0.05s\n")), "@init"));
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(kStates, kPasses, "stateful", "", false)), "main_step"));
    std::string nine;
    for (int i = 0; i < 9; i++)
        nine += "// @state      s" + std::to_string(i) + " rgba8 4x4\n";
    std::string nineWrites;
    for (int i = 0; i < 9; i++)
        nineWrites += "// @step       w" + std::to_string(i) + " s" + std::to_string(i) + "\n";
    EXPECT_TRUE(has(diagnosticsOf(probeManifest(nine, nineWrites)), "8"));
    // a valid manifest stays clean
    EXPECT_EQ(diagnosticsOf(probeManifest(kStates, kPasses)), "");
}

TEST(EffectStatefulManifest, PassFormsAndShaderKeys){
    const EffectManifest* fluid = EffectRegistry::get().find("fluid-smoke");
    ASSERT_NE(fluid, nullptr);
    EXPECT_TRUE(fluid->stateful());
    EXPECT_EQ(fluid->cost, EffectCost::Stateful);
    const ShaderKey key = EffectRegistry::get().shaderKey(*fluid, false);
    const std::vector<uint32_t> forms = EffectManifestParser::passForms(*fluid, ShaderPool::getPropertiesFromKey(key));
    ASSERT_EQ(forms.size(), 1u) << "fluid-smoke has no scatter pass";
    EXPECT_EQ(forms[0], 2u);
    const EffectManifest* boids = EffectRegistry::get().find("boids");
    ASSERT_NE(boids, nullptr);
    const std::vector<uint32_t> boidForms = EffectManifestParser::passForms(*boids, 0u);
    ASSERT_EQ(boidForms.size(), 2u);
    EXPECT_EQ(boidForms[1], 4u);
    // the step and scatter variants' strings round-trip
    for (uint32_t form : {2u, 4u, 3u, 5u}){
        const ShaderKey variant = ShaderPool::getShaderKey(ShaderType::UI_EFFECT, form, ShaderPool::getCustomIdFromKey(key));
        const std::string str = ShaderPool::getShaderStr(ShaderType::UI_EFFECT, form, ShaderPool::getCustomIdFromKey(key));
        EXPECT_EQ(ShaderPool::getPropertiesFromKey(variant), form);
        EXPECT_NE(str.find(form & 2u ? "Stp" : "Sct"), std::string::npos) << str;
    }
    // the stateless effects have no pass forms
    const EffectManifest* stripes = EffectRegistry::get().find("stripes");
    ASSERT_NE(stripes, nullptr);
    EXPECT_FALSE(stripes->stateful());
    EXPECT_TRUE(EffectManifestParser::passForms(*stripes, 0u).empty());
}

// ── the runtime ──

TEST_F(EffectStateful, AllocatesItsImagesFromThePoolAndReusesThem){
    EffectStatePool& pool = EffectStatePool::get();
    ASSERT_TRUE(load(document(probes(2))));
    at(10.0);
    capture();
    // count: 1 x 1 rgba32f, ping-pong; field: box*0.25 of 160 x 80 = 40 x 20 rgba16f, ping-pong
    const size_t expected = 2 * 16 + 2 * 40 * 20 * 8;
    EXPECT_TRUE(info("p0").exists);
    EXPECT_EQ(info("p0").bytes, expected);
    EXPECT_EQ(info("p1").bytes, expected);
    EXPECT_EQ(EffectRuntime::get().stats().stateImages, 8u);
    EXPECT_EQ(EffectRuntime::get().stats().stateBytes, 2 * expected);
    EXPECT_EQ(EffectRuntime::get().simulationCount(), 2u);
    const uint64_t created = pool.createdImages();
    // the document closed: the images go back to the pool; the next document takes them again
    ASSERT_TRUE(load(document("<div></div>")));
    capture();
    EXPECT_EQ(EffectRuntime::get().simulationCount(), 0u);
    EXPECT_GE(pool.pooledImages(), 8u);
    ASSERT_TRUE(load(document(probes(2))));
    capture();
    EXPECT_EQ(pool.createdImages(), created) << "the second document's simulations made new images instead of reusing the pooled ones";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST_F(EffectStateful, FixedStepAndSubstepCap){
    ASSERT_TRUE(load(document(probes(1))));
    at(10.0);
    capture();
    EXPECT_EQ(info("p0").steps, 0) << "the first frame starts the simulation; it steps from the next";
    const double h = 1.0 / 60.0;
    at(10.0 + h);
    TestImage image = capture();
    EXPECT_EQ(info("p0").steps, 1);
    EXPECT_NEAR(counted(image, "p0").first, 1, 1);
    at(10.0 + 3.0 * h);
    image = capture();
    EXPECT_EQ(info("p0").steps, 3) << "two steps of 1/60 s in a frame of 2/60 s";
    // a long frame: at most 4 steps (the default @rate 60 max 4), the rest dropped
    at(11.0 + 3.0 * h);
    image = capture();
    EXPECT_EQ(info("p0").steps, 7);
    EXPECT_NEAR(counted(image, "p0").first, 7, 1);
    // and the clock continues from there, not catching up
    at(11.0 + 4.0 * h);
    image = capture();
    EXPECT_EQ(info("p0").steps, 8);
    EXPECT_NEAR(counted(image, "p0").first, 8, 1);
    // a clock that goes back: no steps, the simulation is anchored again
    at(5.0);
    capture();
    EXPECT_EQ(info("p0").steps, 8);
    at(5.0 + h);
    capture();
    EXPECT_EQ(info("p0").steps, 9);
}

TEST_F(EffectStateful, PausesWhenNotDrawnAndResumesWithoutCatchingUp){
    const double h = 1.0 / 60.0;
    ASSERT_TRUE(load(document(probes(2))));
    at(10.0);
    capture();
    at(10.0 + h);
    capture();
    ASSERT_EQ(info("p0").steps, 1);
    // moved out of the context: culled, paused
    scene->element("p0")->SetProperty("left", "-500px");
    at(10.0 + 2.0 * h);
    capture();
    at(12.0);
    capture();
    EXPECT_EQ(info("p0").steps, 1) << "a simulation outside the view stepped";
    EXPECT_TRUE(info("p0").paused);
    EXPECT_FALSE(info("p1").paused);
    EXPECT_GT(info("p1").steps, 1);
    // display: none: not drawn, paused
    scene->element("p1")->SetProperty("display", "none");
    const int64_t p1Steps = info("p1").steps;
    at(13.0);
    capture();
    at(14.0);
    capture();
    EXPECT_EQ(info("p1").steps, p1Steps);
    EXPECT_TRUE(info("p1").paused);
    // back: the first frame anchors the clock again (no catch-up), then one step a 1/60 s
    scene->element("p0")->SetProperty("left", "10px");
    at(20.0);
    capture();
    EXPECT_EQ(info("p0").steps, 1) << "the time away was caught up";
    EXPECT_FALSE(info("p0").paused);
    at(20.0 + h);
    TestImage image = capture();
    EXPECT_EQ(info("p0").steps, 2);
    EXPECT_NEAR(counted(image, "p0").first, 2, 1) << "the state did not survive the pause";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST_F(EffectStateful, PausesWhenInvisibleOrClipped){
    const double h = 1.0 / 60.0;
    ASSERT_TRUE(load(document(probes(1) +
                              "<div id=\"clip\" style=\"position: absolute; left: 10px; top: 130px; width: 120px; height: 90px; overflow: hidden\">"
                              "<div class=\"p\" id=\"p1\" style=\"left: 0px; top: 0px\"></div></div>")));
    at(10.0);
    capture();
    at(10.0 + h);
    capture();
    ASSERT_EQ(info("p0").steps, 1);
    ASSERT_EQ(info("p1").steps, 1);
    // visibility: hidden, and moved outside its overflow: hidden parent
    scene->element("p0")->SetProperty("visibility", "hidden");
    scene->element("p1")->SetProperty("left", "200px");
    at(10.0 + 2.0 * h);
    capture();
    at(11.0);
    capture();
    EXPECT_EQ(info("p0").steps, 1) << "a visibility: hidden simulation stepped";
    EXPECT_EQ(info("p1").steps, 1) << "a simulation clipped away by its parent stepped";
    EXPECT_TRUE(info("p0").paused);
    EXPECT_TRUE(info("p1").paused);
    scene->element("p0")->SetProperty("visibility", "visible");
    scene->element("p1")->SetProperty("left", "0px");
    at(12.0);
    capture();
    at(12.0 + h);
    capture();
    EXPECT_EQ(info("p0").steps, 2);
    EXPECT_EQ(info("p1").steps, 2);
}

TEST_F(EffectStateful, TimeScaleScalesTheSteps){
    const double h = 1.0 / 60.0;
    ASSERT_TRUE(load(document(probes(3))));
    scene->element("p1")->SetClass("fast", true);
    scene->element("p2")->SetClass("frozen", true);
    at(10.0);
    capture();
    at(10.0 + h);
    capture();
    at(10.0 + 2.0 * h);
    capture();
    EXPECT_EQ(info("p0").steps, 2);
    EXPECT_EQ(info("p1").steps, 4) << "time-scale 2 doubles the steps";
    EXPECT_EQ(info("p2").steps, 0) << "time-scale 0 freezes the simulation";
}

TEST_F(EffectStateful, HundredEffectSwitchesOnOneElement){
    const EffectManifest* stripes = EffectRegistry::get().find("stripes");
    ASSERT_NE(stripes, nullptr);
    ASSERT_TRUE(warmUpEffect(*stripes));
    EffectStatePool& pool = EffectStatePool::get();
    ASSERT_TRUE(load(document(probes(1))));
    scene->element("p0")->SetClass("plain", true);
    at(10.0);
    capture();
    const size_t liveBefore = pool.liveImages();
    uint64_t created = 0;
    for (int i = 0; i < 100; i++){
        scene->element("p0")->SetClass("plain", false);
        at(10.0 + i * 0.1);
        capture();
        ASSERT_TRUE(info("p0").exists) << "switch " << i;
        scene->element("p0")->SetClass("plain", true);
        capture();
        capture();
        EXPECT_EQ(EffectRuntime::get().simulationCount(), 0u) << "switch " << i << ": the simulation outlives its effect";
        EXPECT_EQ(pool.liveImages(), liveBefore) << "switch " << i;
        if (i == 0)
            created = pool.createdImages();
    }
    EXPECT_EQ(pool.createdImages(), created) << "the switches made images after the first";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST_F(EffectStateful, ResizeResamplesLiveAndReplaysPinned){
    const double h = 1.0 / 60.0;
    ASSERT_TRUE(load(document(probes(1))));
    at(10.0);
    capture();
    at(10.0 + 5.0 * h);
    capture();
    const int64_t before = info("p0").steps;
    ASSERT_GT(before, 0);
    // 240 px wide: the field is 60 x 20 now; a resample state keeps the simulation going
    scene->element("p0")->SetClass("wide", true);
    at(10.0 + 6.0 * h);
    TestImage image = capture();
    EXPECT_EQ(info("p0").bytes, 2u * 16u + 2u * 60u * 20u * 8u);
    EXPECT_EQ(info("p0").steps, before + 1) << "a resize restarted a resample simulation";
    EXPECT_NEAR(counted(image, "p0").first, before + 1, 1);
    // pinned: the state is defined by the time, whatever the size history
    FixedEffectClock clock(1.0);
    image = capture();
    const uint64_t replays = EffectRuntime::get().stats().replays;
    EXPECT_EQ(info("p0").steps, 60);
    EXPECT_NEAR(counted(image, "p0").first, 60, 1);
    scene->element("p0")->SetClass("wide", false);
    image = capture();
    EXPECT_GT(EffectRuntime::get().stats().replays, replays) << "a resize under the pinned clock did not replay";
    EXPECT_EQ(info("p0").steps, 60);
    EXPECT_NEAR(counted(image, "p0").first, 60, 1);
}

TEST_F(EffectStateful, PinnedClockReplaysByteIdentically){
    ASSERT_TRUE(load(document(probes(2))));
    TestImage first, again, reloaded, later;
    {
        FixedEffectClock clock(1.5);
        first = capture();
        // 1.5 s of 1/60 s steps (the replay window is 2 s)
        EXPECT_EQ(info("p0").steps, 90);
        EXPECT_NEAR(counted(first, "p0").first, 90, 1);
        const uint64_t replays = EffectRuntime::get().stats().replays;
        again = capture();
        EXPECT_EQ(EffectRuntime::get().stats().replays, replays) << "the same pinned frame replayed again";
        EXPECT_EQ(maxDifference(first, again), 0);
        // past the window: 2 s of steps before T
        clock.set(5.0);
        later = capture();
        EXPECT_EQ(info("p0").steps, 120);
        EXPECT_NEAR(info("p0").time, 5.0, 1e-9);
    }
    // a fresh document at the same time draws the same bytes
    ASSERT_TRUE(load(document(probes(2))));
    {
        FixedEffectClock clock(1.5);
        reloaded = capture();
    }
    EXPECT_EQ(maxDifference(first, reloaded), 0) << "the pinned clock's state depends on what ran before";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST_F(EffectStateful, SplatsResetsAndReseeds){
    const double h = 1.0 / 60.0;
    EffectRuntime& runtime = EffectRuntime::get();
    ASSERT_TRUE(load(document(probes(2))));
    at(10.0);
    capture();
    at(10.0 + h);
    capture();
    EffectSplatData splat;
    splat.position = Rml::Vector2f(40.f, 40.f);
    splat.strength = 3.f;
    EXPECT_EQ(runtime.splat(scene->element("p0"), splat), 1);
    EXPECT_EQ(runtime.splat(scene->element("p0"), splat, "another-effect"), 0);
    at(10.0 + 2.0 * h);
    TestImage image = capture();
    EXPECT_NEAR(counted(image, "p0").second, 3, 1) << "the splat was not taken by the next step";
    EXPECT_NEAR(counted(image, "p1").second, 0, 1);
    at(10.0 + 3.0 * h);
    image = capture();
    EXPECT_NEAR(counted(image, "p0").second, 3, 1) << "a splat is taken once";
    // at most 8 queued a step (the oldest dropped): 12 splats of 1 add 8
    splat.strength = 1.f;
    for (int i = 0; i < 12; i++)
        runtime.splat(scene->element("p0"), splat);
    at(10.0 + 4.0 * h);
    image = capture();
    EXPECT_NEAR(counted(image, "p0").second, 11, 1);
    splat.strength = 3.f;
    // a reset: the next frame starts again from @init
    EXPECT_EQ(runtime.resetSimulations(scene->element("p0")), 1);
    at(10.0 + 5.0 * h);
    image = capture();
    EXPECT_EQ(info("p0").steps, 0);
    EXPECT_NEAR(counted(image, "p0").second, 0, 1);
    EXPECT_GT(info("p1").steps, 0);
    // a @reseed parameter changed: restarted too
    at(10.0 + 6.0 * h);
    capture();
    ASSERT_EQ(info("p1").steps, 6);
    scene->element("p1")->SetClass("seeded", true);
    at(10.0 + 7.0 * h);
    image = capture();
    EXPECT_EQ(info("p1").steps, 0) << "a @reseed parameter's change did not restart";
    // pinned: a splat is logged with its time and replayed
    {
        FixedEffectClock clock(1.0);
        capture();
        EXPECT_EQ(runtime.splat(scene->element("p0"), splat), 1);
        TestImage pinned = capture();
        EXPECT_NEAR(counted(pinned, "p0").second, 3, 1);
        EXPECT_NEAR(counted(pinned, "p0").first, 60, 1);
        TestImage again = capture();
        EXPECT_EQ(maxDifference(pinned, again), 0);
    }
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST_F(EffectStateful, HundredCyclesLeaveNoImagesBehind){
    EffectStatePool& pool = EffectStatePool::get();
    EffectRuntime& runtime = EffectRuntime::get();
    ASSERT_TRUE(load(document("<div></div>")));
    at(10.0);
    capture();
    const size_t liveBefore = pool.liveImages();
    uint64_t createdAfterFirst = 0;
    for (int cycle = 0; cycle < 100; cycle++){
        ASSERT_TRUE(load(document(probes(3)))) << "cycle " << cycle;
        at(10.0 + cycle * 0.1);
        capture();
        ASSERT_EQ(runtime.simulationCount(), 3u) << "cycle " << cycle;
        ASSERT_TRUE(load(document("<div></div>"))) << "cycle " << cycle;
        capture();
        EXPECT_EQ(runtime.simulationCount(), 0u) << "cycle " << cycle << ": simulations outlive their document";
        EXPECT_EQ(pool.liveImages(), liveBefore) << "cycle " << cycle << ": state images outlive their document";
        if (cycle == 0)
            createdAfterFirst = pool.createdImages();
    }
    EXPECT_EQ(pool.createdImages(), createdAfterFirst) << "the pool made images after the first cycle";
    EXPECT_LE(pool.pooledBytes(), EffectStatePool::kMaxPooledBytes);
    pool.trim(0);
    EXPECT_EQ(pool.pooledImages(), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST_F(EffectStateful, BudgetsAndHardCap){
    EffectRuntime& runtime = EffectRuntime::get();
    EffectStatefulBudget budget;
    budget.perDocument = 2;
    runtime.setStatefulBudget(budget);
    ASSERT_TRUE(load(document(probes(3))));
    at(10.0);
    capture();
    // over the soft budget: a warning, every simulation runs
    EXPECT_EQ(countDiagnostics("HTMLUI-FX-0018"), 1u);
    EXPECT_TRUE(info("p2").exists);
    capture();
    EXPECT_EQ(countDiagnostics("HTMLUI-FX-0018"), 1u) << "the budget warning repeats every frame";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
    // the hard cap: the third is not simulated (an error)
    runtime.clearStatefulDiagnostics();
    budget = EffectStatefulBudget();
    budget.hardCap = 2;
    runtime.setStatefulBudget(budget);
    ASSERT_TRUE(load(document(probes(3))));
    capture();
    EXPECT_EQ(runtime.simulationCount(), 2u);
    int simulated = 0;
    for (const char* id : {"p0", "p1", "p2"})
        simulated += info(id).exists ? 1 : 0;
    EXPECT_EQ(simulated, 2);
    EXPECT_EQ(countDiagnostics("HTMLUI-FX-0018"), 1u);
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_ERROR, "HTMLUI-FX-0018"), 1u);
}

TEST_F(EffectStateful, UnsupportedFormatIsReported){
    EffectStatePool::setUnsupportedFormatsForTesting(1u << (unsigned)EffectStateFormat::RGBA32F);
    ASSERT_TRUE(load(document(probes(1))));
    at(10.0);
    TestImage image = capture();
    EXPECT_FALSE(info("p0").exists);
    EXPECT_EQ(countDiagnostics("HTMLUI-FX-0017"), 1u);
    EXPECT_NEAR(counted(image, "p0").first, 0, 1) << "an effect whose states the device cannot hold drew";
    capture();
    EXPECT_EQ(countDiagnostics("HTMLUI-FX-0017"), 1u) << "reported again every frame";
}
