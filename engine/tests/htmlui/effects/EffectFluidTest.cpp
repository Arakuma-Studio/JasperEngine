// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The `fluid` UI effect (Pavel Dobryakov's WebGL-Fluid-Simulation on the shared kernels
// effects/lib/fam_fluid_solver.glsl and fam_fluid_look.glsl) and the effect-system features it
// added: @frame passes (once a drawn frame, after the steps), `aspect <short>` state sizes,
// @preset, the `port` @source, and the agents' scripted input (EffectRuntime::setPointerPath,
// splats logged at a time). Rendering tests run on the backend of the run (glcore or Vulkan).

#include "EffectTestCommon.h"

#include "HtmlUIClock.h"
#include "htmlui/effects/EffectInstancers.h"
#include "htmlui/effects/EffectSimulator.h"
#include "htmlui/effects/EffectSyntax.h"
#include "htmlui/effects/EffectValue.h"

#include <set>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    std::string manifestOf(const std::string& block, bool stateful = true){
        return "#version 450\n"
               "// @effect     fx-test\n"
               "// @kind       paint\n"
               "// @category   interactive\n"
               "// @summary    A manifest of the fluid tests\n"
               "// @source     original\n"
               "// @cost       " + std::string(stateful ? "stateful" : "low") + "\n" + block +
               "\n#include \"effects/lib/jfx_common.glsl\"\n"
               "#include \"effects/lib/main_paint.glsl\"\n" +
               (stateful ? "#include \"effects/lib/main_step.glsl\"\n" : "");
    }

    std::string diagnosticsOf(const std::string& source, EffectManifest* out = nullptr){
        EffectManifest m;
        std::vector<EffectDiagnostic> diags;
        if (EffectManifestParser::parse(source, "effects/fx-test.frag", true, m, diags))
            EffectManifestParser::validate(m, source, diags);
        if (out)
            *out = m;
        std::string all;
        for (const EffectDiagnostic& d : diags)
            all += std::to_string(d.line) + ": " + d.message + "\n";
        return all;
    }

    const EffectManifest& fluid(){
        const EffectManifest* m = EffectRegistry::get().find("fluid");
        EXPECT_NE(m, nullptr);
        return *m;
    }

    float valueOf(const EffectManifest& m, const std::vector<EffectValue>& values, const std::string& name){
        const int index = m.paramIndex(name);
        EXPECT_GE(index, 0) << name;
        return index >= 0 ? values[(size_t)index].f : -1.f;
    }

    std::vector<EffectValue> valuesWith(const EffectManifest& m, const std::vector<std::pair<std::string, std::string>>& given){
        std::vector<EffectValue> values;
        std::set<std::string> written;
        for (const auto& g : given)
            written.insert(g.first);
        for (const EffectParamDef& def : m.params){
            std::string text = def.defaultText;
            for (const auto& g : given)
                if (g.first == def.name)
                    text = g.second;
            EffectValue value;
            std::string reason;
            EXPECT_TRUE(EffectValueParser::parse(def, text, value, reason)) << def.name << ": " << reason;
            values.push_back(value);
        }
        applyEffectPreset(m, values, written);
        return values;
    }

    std::string document(const std::string& arguments){
        return "<!DOCTYPE html><html><head><style>\n"
               "html, body { margin: 0; width: 360px; height: 240px; overflow: hidden; background: #000; }\n"
               "#f { position: absolute; left: 20px; top: 20px; width: 320px; height: 200px; background: #000;"
               " background-image: effect(fluid" + (arguments.empty() ? std::string() : ", " + arguments) + "); }\n"
               "</style></head><body><div id=\"f\"></div></body></html>";
    }

    struct EffectFluid : ::testing::Test {
        std::unique_ptr<EffectScene> scene;

        void SetUp() override{
            TestSystem::get().clearLogs();
            scene = std::make_unique<EffectScene>(360, 240, 1);
            if (!scene->valid())
                GTEST_SKIP() << "no GPU";
            ASSERT_TRUE(warmUpEffect(fluid())) << "fluid.frag does not compile in every form";
        }

        void TearDown() override{
            EffectClock::setFixedTime(std::nullopt);
            EffectRuntime::get().clearTestInput();
            scene.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
        }

        bool load(const std::string& arguments = std::string()){
            std::string error;
            const bool ok = scene->loadHtml(document(arguments), &error);
            EXPECT_TRUE(ok) << error;
            return ok;
        }

        TestImage at(double t){
            EffectClock::setFixedTime(t);
            TestImage image;
            EXPECT_TRUE(scene->capture(image));
            return image;
        }

        TestImage pinned(const std::string& arguments, double t){
            if (!load(arguments))
                return TestImage();
            return at(t);
        }

        EffectSimulationInfo info(){
            return EffectRuntime::get().simulationInfo(scene->element("f"), "fluid", 0);
        }

        // the mean of R + G + B and the brightest pixel's alpha-free maximum, over the area
        std::pair<double, int> brightness(const TestImage& image){
            double sum = 0.0;
            int peak = 0;
            size_t count = 0;
            for (int y = 20; y < 220; y++){
                for (int x = 20; x < 340; x++){
                    const size_t at = ((size_t)y * (size_t)image.width + (size_t)x) * 4;
                    const int rgb = image.rgba[at] + image.rgba[at + 1] + image.rgba[at + 2];
                    sum += rgb;
                    peak = std::max(peak, std::max<int>(image.rgba[at], std::max(image.rgba[at + 1], image.rgba[at + 2])));
                    count++;
                }
            }
            return {sum / (double)std::max<size_t>(count, 1), peak};
        }
    };

}

// ── the manifest and the grammar it added ──

TEST(EffectFluidManifest, DefaultsMatchTheFluidParameters){
    const EffectManifest& m = fluid();
    // Pavel's defaults
    std::vector<EffectValue> values = valuesWith(m, {});
    EXPECT_FLOAT_EQ(valueOf(m, values, "density-dissipation"), 1.f);
    EXPECT_FLOAT_EQ(valueOf(m, values, "velocity-dissipation"), 0.2f);
    EXPECT_FLOAT_EQ(valueOf(m, values, "pressure"), 0.8f);
    EXPECT_FLOAT_EQ(valueOf(m, values, "pressure-iterations"), 20.f);
    EXPECT_FLOAT_EQ(valueOf(m, values, "curl"), 30.f);
    EXPECT_FLOAT_EQ(valueOf(m, values, "splat-radius"), 0.25f);
    EXPECT_FLOAT_EQ(valueOf(m, values, "splat-force"), 6000.f);
    EXPECT_FLOAT_EQ(valueOf(m, values, "bloom-intensity"), 0.8f);
    EXPECT_FLOAT_EQ(valueOf(m, values, "sunrays-weight"), 1.f);
}

// a preset sets the parameters left at their defaults; a parameter written otherwise keeps its value
TEST(EffectFluidManifest, PresetsSetTheParametersLeftAtTheirDefaults){
    const EffectManifest& m = fluid();
    std::vector<EffectValue> ink = valuesWith(m, {{"preset", "ink"}});
    EXPECT_FLOAT_EQ(valueOf(m, ink, "curl"), 8.f);
    EXPECT_FLOAT_EQ(valueOf(m, ink, "pressure-iterations"), 30.f);
    EXPECT_FLOAT_EQ(valueOf(m, ink, "density-dissipation"), 0.15f);
    EXPECT_FLOAT_EQ(valueOf(m, ink, "bloom"), 0.f);
    EXPECT_FLOAT_EQ(valueOf(m, ink, "pressure"), 0.8f) << "a parameter the preset does not list keeps its default";
    std::vector<EffectValue> mixed = valuesWith(m, {{"preset", "ink"}, {"curl", "20"}});
    EXPECT_FLOAT_EQ(valueOf(m, mixed, "curl"), 20.f) << "a written parameter wins over the preset";
    EXPECT_FLOAT_EQ(valueOf(m, mixed, "pressure-iterations"), 30.f);
    std::vector<EffectValue> calm = valuesWith(m, {{"preset", "calm-water"}});
    EXPECT_FLOAT_EQ(valueOf(m, calm, "bloom-iterations"), 6.f);
    EXPECT_FLOAT_EQ(valueOf(m, calm, "sunrays"), 0.f);
    std::vector<EffectValue> wild = valuesWith(m, {{"preset", "psychedelic"}});
    EXPECT_FLOAT_EQ(valueOf(m, wild, "curl"), 50.f);
    EXPECT_FLOAT_EQ(valueOf(m, wild, "bloom-threshold"), 0.3f);
    // a written parameter wins even at its default (as the native component's explicit values)
    std::vector<EffectValue> explicitOn = valuesWith(m, {{"preset", "ink"}, {"bloom", "on"}, {"curl", "30"}});
    EXPECT_FLOAT_EQ(valueOf(m, explicitOn, "bloom"), 1.f) << "bloom on written: kept";
    EXPECT_FLOAT_EQ(valueOf(m, explicitOn, "curl"), 30.f) << "curl 30 written: kept";
    EXPECT_FLOAT_EQ(valueOf(m, explicitOn, "density-dissipation"), 0.15f);
    // the RCSS path and the raw call: the same values
    for (const std::string& call : {std::string("fluid, preset ink, bloom on")}){
        EffectSnapshotResult r = snapshotFromCall(call, false);
        ASSERT_TRUE(r.snapshot) << call;
        EXPECT_FLOAT_EQ(valueOf(m, r.snapshot->values, "bloom"), 1.f);
        EXPECT_FLOAT_EQ(valueOf(m, r.snapshot->values, "curl"), 8.f);
    }
    auto rcssOf = [&](const std::string& arguments){
        EffectCall parsed;
        std::vector<EffectDiagnostic> diags;
        EXPECT_TRUE(EffectSyntax::parseArguments(arguments, parsed, diags)) << arguments;
        return EffectSyntax::toRcss(parsed, m, false, std::string_view(), diags);
    };
    // the RCSS slots carry the preset's values for what the call leaves out, and the written ones
    EXPECT_NE(rcssOf("fluid, preset ink"), rcssOf("fluid")) << "the preset's values are not in the slots";
    EXPECT_NE(rcssOf("fluid, preset ink, bloom on"), rcssOf("fluid, preset ink")) << "bloom on written at its default lost to the preset";
    EXPECT_EQ(rcssOf("fluid, preset ink, curl 8"), rcssOf("fluid, preset ink"));
    std::vector<EffectValue> plain = valuesWith(m, {{"preset", "default"}, {"curl", "12"}});
    EXPECT_FLOAT_EQ(valueOf(m, plain, "curl"), 12.f);
    EXPECT_FLOAT_EQ(valueOf(m, plain, "pressure-iterations"), 20.f);
}

TEST(EffectFluidManifest, FramePassesAspectSizesAndPresetsParse){
    EffectManifest m;
    const std::string ok = manifestOf("// @state      vel rg16f aspect 128 max 512\n"
                                      "// @state      look rgba16f aspect 64\n"
                                      "// @step       move vel\n"
                                      "// @frame      glow look x2 if shading\n"
                                      "// @param      preset enum calm calm|wild -- A look\n"
                                      "// @param      curl number 30 0 50 -- Swirl\n"
                                      "// @param      shading boolean on -- Light\n"
                                      "// @preset     wild curl 50, shading off\n");
    EXPECT_EQ(diagnosticsOf(ok, &m), "");
    ASSERT_EQ(m.states.size(), 2u);
    EXPECT_EQ(m.states[0].aspectShort, 128);
    EXPECT_EQ(m.states[0].boxMax, 512);
    EXPECT_FALSE(m.states[0].fixedSize);
    EXPECT_EQ(m.states[1].aspectShort, 64);
    EXPECT_EQ(m.states[1].boxMax, 1024);
    ASSERT_EQ(m.passes.size(), 2u);
    EXPECT_FALSE(m.passes[0].frame);
    EXPECT_TRUE(m.passes[1].frame);
    EXPECT_EQ(m.passes[1].repeat, 2);
    EXPECT_EQ(m.passes[1].ifParam, "shading");
    ASSERT_EQ(m.presets.size(), 1u);
    EXPECT_EQ(m.presets[0].option, "wild");
    EXPECT_EQ(m.presets[0].values, (std::vector<std::pair<std::string, std::string>>{{"curl", "50"}, {"shading", "off"}}));
    // the signature tells @frame and aspect apart from @step and box
    EffectManifest other;
    diagnosticsOf(manifestOf("// @state      vel rg16f aspect 128 max 512\n"
                             "// @state      look rgba16f aspect 64\n"
                             "// @step       move vel\n"
                             "// @step       glow look x2\n"), &other);
    EXPECT_NE(m.stateSignature(), other.stateSignature());

    // the mistakes
    EXPECT_NE(diagnosticsOf(manifestOf("// @state      vel rg16f aspect 0\n// @step       move vel\n")).find("aspect size"), std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf("// @state      vel rg16f aspect\n// @step       move vel\n")).find("aspect size"), std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf("// @state      vel rg16f aspect 64\n// @frame      glow vel\n")).find("at least one @step"),
              std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf("// @frame      glow vel\n", false)).find("@state"), std::string::npos);
    const std::string states = "// @state      vel rg16f aspect 64\n// @step       move vel\n";
    EXPECT_NE(diagnosticsOf(manifestOf(states + "// @param      curl number 30 0 50 -- Swirl\n// @preset     wild curl 50\n"))
                  .find("enum parameter named 'preset'"), std::string::npos);
    const std::string presetParam = states + "// @param      preset enum calm calm|wild -- A look\n// @param      curl number 30 0 50 -- Swirl\n";
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @preset     storm curl 50\n")).find("no option 'storm'"), std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @preset     wild swirl 50\n")).find("not a parameter"), std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @preset     wild curl 80\n")).find("outside the range"), std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @preset     wild curl lots\n")).find("is not"), std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @preset     wild preset calm\n")).find("cannot set 'preset'"), std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @preset     wild curl 40\n// @preset     wild curl 45\n")).find("declared twice"),
              std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @preset     wild\n")).find("sets no parameter"), std::string::npos);
    // if <param>: a boolean or number parameter, not on @init
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @frame      glow vel if nothing\n")).find("not a parameter"), std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @frame      glow vel if preset\n")).find("not a boolean or a number"),
              std::string::npos);
    EXPECT_NE(diagnosticsOf(manifestOf(presetParam + "// @frame      glow vel if\n")).find("if names"), std::string::npos);
    // an @frame pass writes a state of its own (draw-only), not one a step writes
    EXPECT_NE(diagnosticsOf(manifestOf("// @state      vel rg16f aspect 64\n// @step       move vel\n// @frame      glow vel\n"))
                  .find("@frame pass 'glow' writes 'vel'"), std::string::npos);
    // @source: shaders.com, original or a port with its author and licence
    std::string source = manifestOf(states);
    source.replace(source.find("// @source     original"), 22, "// @source     somewhere else");
    EXPECT_NE(diagnosticsOf(source).find("@source starts with"), std::string::npos);
}

// ── the simulation ──

// the grids: velocity, pressure and aux keep the area's aspect with a 128 short side, the dye the
// area's px, bloom 256 and sunrays 196 on the short side (Pavel's getResolution), pairs twice
TEST_F(EffectFluid, GridSizesFollowTheAreasAspect){
    ASSERT_TRUE(load());
    at(0.05);
    const EffectSimulationInfo sim = info();
    ASSERT_TRUE(sim.exists);
    // 320 x 200: aspect 1.6
    const size_t vel = 205u * 128u * 4u * 2u, scalar = 205u * 128u * 2u * 2u;
    const size_t dye = 320u * 200u * 8u * 2u, ptr = 8u * 2u;
    const size_t bloom = 410u * 256u * 8u * 2u, sunrays = 314u * 196u * 2u * 2u;
    EXPECT_EQ(sim.bytes, vel + 2u * scalar + dye + ptr + 2u * bloom + sunrays);
    EXPECT_EQ(sim.images, 16);
    EXPECT_LT(sim.bytes, 8u * 1024u * 1024u) << "one instance's state memory";
}

// Pavel's start: a burst of bright splats with bloom; the fluid moves on
TEST_F(EffectFluid, DrawsTheStartBurstAndEvolves){
    ASSERT_TRUE(load());
    const TestImage early = at(0.1);
    const TestImage later = at(0.5);
    const auto a = brightness(early);
    EXPECT_GT(a.first, 20.0) << "the start burst is not drawn";
    EXPECT_GT(a.second, 200) << "no bright dye (the burst is x10 of a pointer colour)";
    EXPECT_GT(maxDifference(early, later), 40) << "the fluid did not move";
    EXPECT_EQ(info().steps, 30);
    const TestImage noBurst = pinned("splats-on-start off, bloom off", 0.1);
    EXPECT_LT(brightness(noBurst).second, 2) << "without the burst and input the fluid is empty";
    // Pavel's bloom dither over black: a faint grain of at most (1/255 gamma-encoded) ~13 levels
    const TestImage grain = pinned("splats-on-start off", 0.1);
    EXPECT_LE(brightness(grain).second, 14);
}

// a pinned time draws the same bytes twice, after other times, and in a fresh document; @frame
// passes do not run again for an unchanged pinned frame
TEST_F(EffectFluid, PinnedClockIsDeterministic){
    ASSERT_TRUE(load());
    const TestImage first = at(0.75);
    const uint64_t replays = EffectRuntime::get().stats().replays;
    const uint64_t passes = EffectRuntime::get().stats().stepPasses;
    const TestImage again = at(0.75);
    EXPECT_EQ(EffectRuntime::get().stats().replays, replays);
    EXPECT_EQ(EffectRuntime::get().stats().stepPasses, passes) << "an unchanged pinned frame ran passes";
    EXPECT_EQ(maxDifference(first, again), 0);
    at(0.25);
    const TestImage back = at(0.75);
    EXPECT_EQ(maxDifference(first, back), 0) << "t replayed after another time differs";
    const TestImage reloaded = pinned("", 0.75);
    EXPECT_EQ(maxDifference(first, reloaded), 0) << "the pinned state depends on what ran before";
    const TestImage seeded = pinned("seed 7", 0.75);
    EXPECT_GT(maxDifference(first, seeded), 40) << "another seed draws another burst";
}

// a splat (effectSplat's path) changes the image, at the step its time falls in
TEST_F(EffectFluid, SplatsChangeTheImage){
    ASSERT_TRUE(load("splats-on-start off, bloom off"));
    const TestImage empty = at(0.5);
    EXPECT_LT(brightness(empty).second, 2);
    EffectSplatData splat;
    splat.position = Rml::Vector2f(160.f, 100.f);
    splat.radius = 30.f;
    splat.velocity = Rml::Vector2f(400.f, 0.f);
    splat.color[0] = 1.f;
    splat.color[1] = 0.2f;
    splat.color[2] = 0.05f;
    splat.strength = 1.5f;
    ASSERT_EQ(EffectRuntime::get().splat(scene->element("f"), splat, "fluid"), 1);
    const TestImage hit = at(0.5);
    EXPECT_GT(brightness(hit).second, 150) << "the splat was not painted";
    // logged at 0.25 s: a quarter second of its evolution at 0.5 s
    ASSERT_EQ(EffectRuntime::get().clearSplats(scene->element("f"), "fluid"), 1);
    EXPECT_EQ(info().splats, 0);
    ASSERT_EQ(EffectRuntime::get().splat(scene->element("f"), splat, "fluid", 0.25), 1);
    const TestImage earlier = at(0.5);
    EXPECT_GT(maxDifference(hit, earlier), 20) << "the splat's time was ignored";
    EXPECT_GT(brightness(earlier).second, 50);
}

// a scripted pointer path is a drag: it paints, identically each time, and another path paints elsewhere
TEST_F(EffectFluid, PointerPathsReplayExactly){
    ASSERT_TRUE(load("splats-on-start off, bloom off"));
    at(0.5);
    std::vector<Rml::Vector3f> path;
    for (int k = 0; k <= 30; k++)
        path.emplace_back(40.f + 8.f * (float)k, 100.f + 30.f * std::sin((float)k * 0.3f), 0.1f + 0.01f * (float)k);
    ASSERT_EQ(EffectRuntime::get().setPointerPath(scene->element("f"), path, "fluid"), 1);
    EXPECT_EQ(info().pathSamples, 31);
    const TestImage painted = at(0.5);
    EXPECT_GT(brightness(painted).second, 30) << "the drag did not paint";
    const TestImage again = at(0.5);
    EXPECT_EQ(maxDifference(painted, again), 0);
    // the same path set again: the same bytes
    ASSERT_EQ(EffectRuntime::get().setPointerPath(scene->element("f"), path, "fluid"), 1);
    EXPECT_EQ(maxDifference(painted, at(0.5)), 0);
    std::vector<Rml::Vector3f> other = path;
    for (Rml::Vector3f& p : other)
        p.y += 40.f;
    ASSERT_EQ(EffectRuntime::get().setPointerPath(scene->element("f"), other, "fluid"), 1);
    EXPECT_GT(maxDifference(painted, at(0.5)), 20) << "another path drew the same";
    ASSERT_EQ(EffectRuntime::get().setPointerPath(scene->element("f"), {}, "fluid"), 1);
    EXPECT_LT(brightness(at(0.5)).second, 2) << "a cleared path still paints";
}

// preset mobile is shading, bloom and sunrays off; ink draws differently from the default
TEST_F(EffectFluid, PresetsDraw){
    const TestImage plain = pinned("", 0.4);
    const TestImage mobile = pinned("preset mobile", 0.4);
    const TestImage explicitOff = pinned("shading off, bloom off, sunrays off", 0.4);
    EXPECT_EQ(maxDifference(mobile, explicitOff), 0) << "preset mobile is not its three values";
    EXPECT_GT(maxDifference(plain, mobile), 20);
    const TestImage ink = pinned("preset ink", 0.4);
    EXPECT_GT(maxDifference(plain, ink), 20);
    // bloom and sunrays off: their @frame passes do not run (if bloom, if sunrays)
    {
        ASSERT_TRUE(load("bloom off, sunrays off"));
        at(0.4);
        const uint64_t before = EffectRuntime::get().stats().stepPasses;
        EffectRuntime::get().resetSimulations(scene->element("f"));
        at(0.45);
        const uint64_t passes = EffectRuntime::get().stats().stepPasses - before;
        ASSERT_TRUE(load());
        at(0.4);
        const uint64_t before2 = EffectRuntime::get().stats().stepPasses;
        EffectRuntime::get().resetSimulations(scene->element("f"));
        at(0.45);
        const uint64_t all = EffectRuntime::get().stats().stepPasses - before2;
        // 8 bloom-down, 8 bloom-up, prefilter, final, sunrays, 2 blurs
        EXPECT_EQ(all - passes, 21u) << all << " passes with the look, " << passes << " without";
    }
    // the look's switches each change the image
    EXPECT_GT(maxDifference(plain, pinned("bloom off", 0.4)), 10);
    EXPECT_GT(maxDifference(plain, pinned("sunrays off", 0.4)), 10);
    EXPECT_GT(maxDifference(plain, pinned("shading off", 0.4)), 2);
}


// ── the review's findings ──

// a splat logged after the pinned time is future input: not drawn until the time reaches it
TEST_F(EffectFluid, FutureSplatsWaitForTheirTime){
    ASSERT_TRUE(load("splats-on-start off, bloom off"));
    at(0.5);
    EffectSplatData splat;
    splat.position = Rml::Vector2f(160.f, 100.f);
    splat.radius = 30.f;
    splat.strength = 1.5f;
    ASSERT_EQ(EffectRuntime::get().splat(scene->element("f"), splat, "fluid", 0.8), 1);
    EXPECT_LT(brightness(at(0.5)).second, 2) << "a splat at 0.8 s drawn at 0.5 s";
    EXPECT_GT(brightness(at(0.85)).second, 100) << "the splat at 0.8 s is missing at 0.85 s";
}

// scripted input older than @replay (2 s) still counts: the replay starts at the earliest input
TEST_F(EffectFluid, OldScriptedInputIsReplayed){
    ASSERT_TRUE(load("splats-on-start off, bloom off, density-dissipation 0"));
    at(1.0);
    EffectSplatData splat;
    splat.position = Rml::Vector2f(160.f, 100.f);
    splat.radius = 30.f;
    splat.strength = 1.5f;
    ASSERT_EQ(EffectRuntime::get().splat(scene->element("f"), splat, "fluid", 0.5), 1);
    const TestImage late = at(3.5);
    EXPECT_GT(brightness(late).second, 60) << "a splat 3 s before the pinned time was ignored";
    EXPECT_GE(info().steps, 180) << "the replay did not reach back to the splat";
}

// a parameter change redraws the look even without a step (a frozen simulation), and bloom
// switched on computes its images
TEST_F(EffectFluid, ParameterChangesRedoTheLookWhileFrozen){
    const std::string html = "<!DOCTYPE html><html><head><style>\n"
                             "html, body { margin: 0; width: 360px; height: 240px; overflow: hidden; background: #000; }\n"
                             "#f { position: absolute; left: 20px; top: 20px; width: 320px; height: 200px; background: #000;"
                             " background-image: effect(fluid, time-scale 0, bloom off, sunrays-weight 0.3); }\n"
                             "#f.lit { background-image: effect(fluid, time-scale 0, bloom on, sunrays-weight 0.3); }\n"
                             "#f.rays { background-image: effect(fluid, time-scale 0, bloom on, sunrays-weight 1); }\n"
                             "</style></head><body><div id=\"f\"></div></body></html>";
    std::string error;
    ASSERT_TRUE(scene->loadHtml(html, &error)) << error;
    double t = 10.0;
    auto live = [&](){
        HtmlUIClock::setFixedTime(t += 1.0 / 60.0);
        TestImage image;
        EXPECT_TRUE(scene->capture(image));
        return image;
    };
    live();
    const TestImage off = live();
    ASSERT_EQ(info().steps, 0) << "time-scale 0 freezes it";
    scene->element("f")->SetClass("lit", true);
    live();
    const TestImage lit = live();
    EXPECT_EQ(info().steps, 0);
    EXPECT_GT(maxDifference(off, lit), 10) << "bloom switched on while frozen draws nothing new";
    scene->element("f")->SetClass("rays", true);
    live();
    const TestImage rays = live();
    EXPECT_GT(maxDifference(lit, rays), 5) << "sunrays-weight changed while frozen: the look did not follow";
    // the same as a fresh frozen simulation with those parameters
    const std::string fresh = "<!DOCTYPE html><html><head><style>\n"
                              "html, body { margin: 0; width: 360px; height: 240px; overflow: hidden; background: #000; }\n"
                              "#f { position: absolute; left: 20px; top: 20px; width: 320px; height: 200px; background: #000;"
                              " background-image: effect(fluid, time-scale 0, bloom on, sunrays-weight 1); }\n"
                              "</style></head><body><div id=\"f\"></div></body></html>";
    ASSERT_TRUE(scene->loadHtml(fresh, &error)) << error;
    live();
    EXPECT_LE(maxDifference(rays, live()), 1);
}

// extreme splats are clamped (the half-float velocity stays finite), non-finite ones refused
TEST_F(EffectFluid, ExtremeSplatsStayFinite){
    ASSERT_TRUE(load("splats-on-start off, bloom off"));
    at(0.5);
    EffectSplatData splat;
    splat.position = Rml::Vector2f(160.f, 100.f);
    splat.radius = 30.f;
    splat.strength = 1.5f;
    splat.velocity = Rml::Vector2f(1.0e9f, -1.0e9f);
    ASSERT_EQ(EffectRuntime::get().splat(scene->element("f"), splat, "fluid", 0.3), 1);
    EffectSplatData broken = splat;
    broken.velocity.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(EffectRuntime::get().splat(scene->element("f"), broken, "fluid", 0.3), 0) << "a NaN velocity was taken";
    broken = splat;
    broken.strength = std::numeric_limits<float>::infinity();
    EXPECT_EQ(EffectRuntime::get().splat(scene->element("f"), broken, "fluid", 0.3), 0) << "an infinite strength was taken";
    // a second, ordinary splat later: still drawn (the fluid is not poisoned with NaN)
    EffectSplatData later = splat;
    later.velocity = Rml::Vector2f(0.f, 0.f);
    later.position = Rml::Vector2f(80.f, 60.f);
    ASSERT_EQ(EffectRuntime::get().splat(scene->element("f"), later, "fluid", 0.45), 1);
    const TestImage image = at(0.5);
    const Rml::Rectanglef box = scene->box("f");
    int peak = 0;
    for (int y = (int)box.Top() + 50; y < (int)box.Top() + 70; y++)
        for (int x = (int)box.Left() + 70; x < (int)box.Left() + 90; x++){
            const size_t i = ((size_t)y * (size_t)image.width + (size_t)x) * 4;
            peak = std::max(peak, (int)std::max(image.rgba[i], std::max(image.rgba[i + 1], image.rgba[i + 2])));
        }
    EXPECT_GT(peak, 100) << "after an extreme splat the fluid draws nothing more (NaN)";
}

// half-float devices: without 32-bit float targets the effect still runs
TEST_F(EffectFluid, RunsWithoutThirtyTwoBitFloatTargets){
    EffectStatePool::setUnsupportedFormatsForTesting((1u << (unsigned)EffectStateFormat::RGBA32F) | (1u << (unsigned)EffectStateFormat::R32F));
    EffectRuntime::get().clearStatefulDiagnostics();
    const TestImage image = pinned("", 0.25);
    EffectStatePool::setUnsupportedFormatsForTesting(0);
    EXPECT_TRUE(info().exists);
    EXPECT_GT(brightness(image).second, 100);
    for (const auto& d : EffectRuntime::get().statefulDiagnostics())
        EXPECT_NE(d.first, "HTMLUI-FX-0017") << d.second;
}

// with a scripted path the live pointer is no input: moving it over a pinned view replays nothing
TEST_F(EffectFluid, ScriptedPathIgnoresTheLivePointer){
    ASSERT_TRUE(load("splats-on-start off, bloom off"));
    at(0.5);
    std::vector<Rml::Vector3f> path;
    for (int k = 0; k <= 10; k++)
        path.emplace_back(40.f + 20.f * (float)k, 100.f, 0.1f + 0.02f * (float)k);
    ASSERT_EQ(EffectRuntime::get().setPointerPath(scene->element("f"), path, "fluid"), 1);
    const TestImage first = at(0.5);
    const uint64_t replays = EffectRuntime::get().stats().replays;
    EffectRuntime::get().setTestPointer(scene->context(), Rml::Vector2f(100.f, 80.f), true);
    const TestImage moved = at(0.5);
    EffectRuntime::get().setTestPointer(scene->context(), Rml::Vector2f(200.f, 150.f), false);
    const TestImage again = at(0.5);
    EXPECT_EQ(EffectRuntime::get().stats().replays, replays) << "the live pointer replayed a scripted simulation";
    EXPECT_EQ(maxDifference(first, moved), 0);
    EXPECT_EQ(maxDifference(first, again), 0);
}
