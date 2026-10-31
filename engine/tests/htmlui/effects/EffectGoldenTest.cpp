// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The golden driver of the effects (phase-5 spec 5.2, 5.3): every effect EffectRegistry lists
// (auto-enumerated: a new effect is tested by adding its .frag) is rendered in its fixture with
// the effect clock fixed, compared with golden/effects/<name>.t1.png (t = 1.5 s) and, when it
// is animated, <name>.t2.png (t = 4 s), and checked against the invariants I1-I7; a stateful
// effect (phase 6) also against <name>.t0.png (t = 0.25 s: its early state) and I8-I9. The
// goldens are llvmpipe renders; on a hardware GPU the effects listed in kHardwareTolerances are
// compared with the looser tolerances it needs (see there), every other effect, and every effect on
// llvmpipe and lavapipe, with the default one.

#include "EffectTestCommon.h"

#include "harness/GpuEnvironment.h"
#include "harness/ImageCompare.h"

#include "htmlui/HtmlUIClock.h"

#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    constexpr double kT1 = 1.5;
    constexpr double kT2 = 4.0;
    constexpr double kT0 = 0.25;   // stateful effects: the simulation's early state (within the harness's 0.5 s pointer arc)

    // paint effects that also get a gradient-text golden (<name>.text): the filter form through
    // -jfx-text-fill (the wave-2 packages add theirs: aurora, conic-spin, gradient-pan, plasma,
    // holographic, sheen)
    const std::set<std::string> kTextEffects = {"aurora", "conic-spin", "gradient-pan", "plasma", "stripes", "holographic", "sheen"};

    // ---- the hardware-GPU tolerances ------------------------------------------------------------
    //
    // The goldens are glcore renders on llvmpipe (Mesa's software rasterizer). On a hardware GPU
    // (measured on an NVIDIA GTX 1080, driver 580, Vulkan, and checked on an RTX 3060 Ti, driver 595; the
    // same tests failed on a GTX 1650)
    // these effects draw a little or a lot differently, and the owner chose looser tolerances
    // over per-GPU goldens (2026-10-27):
    //   - hash noise built on sin(): fract(sin(x) * 43758.5) and fam_hash_phase() amplify the last
    //     bits of sin(), which a GPU's sin() (a fast approximation) and llvmpipe's do not share;
    //     cells, curtains, columns and spawn points land elsewhere (voronoi, aurora,
    //     falling-lines, magnetic-filings, particle-flow, particles, data-mosh's blocks);
    //   - simulations step the same equations in half-float targets whose filtering and
    //     rounding differ by a bit, and the chaotic ones (boids, fluid) diverge from their early
    //     state on: their early state (.t0) still matches but for a few pixels;
    //   - smooth gradients and noise (mesh-gradient, grain) differ by a few levels.
    // Each entry is the measured distance (in its comment) plus a margin of about 25-30 %, with
    // the finest comparison that passes on the GPU:
    //   pixels(): per pixel (channel threshold 24, or the threshold given) with a larger share of
    //             bad pixels or a larger mean difference: effects that match but for scattered
    //             pixels;
    //   blocks(): the mean colours of 16x16 or 32x32 blocks (every block within the threshold)
    //             and the colour histograms: effects whose particles or flow lie elsewhere pixel
    //             by pixel but cover the same regions with the same colours;
    //   means():  the mean colour, the colour histograms and the edge histogram only (see
    //             CompareOptions): hash-noise patterns laid out differently on a GPU, whose
    //             colours and scale are still the golden's.
    // They are still regression tests: each was checked on the GTX 1080 to fail with a shader
    // mutation of its effect (one parameter's mapping off by 25-50 %: a larger voronoi scale,
    // boids or particles 30 % larger, faster fluid dissipation, ...), and the invariants I1-I9
    // below (exact or within 1) are unchanged.
    struct HardwareTolerance {
        CompareOptions early;   // .t0, stateful effects
        CompareOptions late;    // .t1 and .t2
        CompareOptions text;    // .text, the gradient-text form
    };

    CompareOptions pixels(double badPercent, double meanDiff = 1.0, int threshold = 24){
        CompareOptions options;
        options.pixelThreshold = threshold;
        options.maxBadFraction = badPercent / 100.0;
        options.maxMeanDiff = meanDiff;
        return options;
    }

    CompareOptions blocks(int size, double meanDiff, double histogramPercent, int threshold = 24){
        CompareOptions options;
        options.blockSize = size;
        options.pixelThreshold = threshold;
        options.maxBadFraction = 0.0;
        options.maxMeanDiff = meanDiff;
        options.maxHistogramDistance = histogramPercent;
        return options;
    }

    CompareOptions means(int threshold, double histogramPercent, double edgeHistogramPercent){
        CompareOptions options = blocks(1 << 16, threshold, histogramPercent, threshold);
        options.maxGradientHistogramDistance = edgeHistogramPercent;
        return options;
    }

    // the measured values: bad pixel shares at the channel threshold, mean differences, the
    // largest block difference, colour (and edge) histogram distances, of .t0 / .t1 / .t2
    const std::map<std::string, HardwareTolerance>& hardwareTolerances(){
        static const std::map<std::string, HardwareTolerance> tolerances = {
            // t1 / t2: mean colours 4 / 5 apart, histograms 6.0 / 11.3 %, edges 0.44 / 0.50 %;
            // text 1.79 % bad, mean 0.93
            {"aurora", {{}, means(12, 14.0, 0.65), pixels(2.25, 1.2)}},  // RTX 3060 Ti: mean colours 9 / 7 apart, histograms 13.4 / 10.0 %, edges 0.33 / 0.24 %
            // t0 1.04 % bad, mean 1.20; t1 / t2 in 16 px blocks: largest 23 / 18, mean 1.77 / 2.20,
            // histograms 0.36 / 0.47 %
            {"boids", {pixels(1.3, 1.5), blocks(16, 2.8, 0.6, 30), {}}},
            // t0 0.49 % bad; t1 / t2 4.20 / 4.50 % bad, mean 4.59 / 4.87
            {"data-mosh", {pixels(0.6), pixels(5.6, 6.1), {}}},
            // 32 px blocks: largest 13 / 17, mean 3.88 / 4.04, histograms 0.05 / 0.05 %
            {"falling-lines", {{}, blocks(32, 5.0, 0.1), {}}},
            // t0 within the default; t1 / t2 in 32 px blocks: largest 18 / 10, mean 2.81 / 2.10,
            // histograms 1.56 / 1.19 %
            {"fluid", {{}, blocks(32, 3.6, 2.0), {}}},
            // t2 0.32 % bad (largest 30)
            {"grain", {{}, pixels(0.4), {}}},
            // 16 px blocks: largest 11 / 12 / 12, mean 1.99 / 2.07 / 2.11, histograms 0.34 / 0.36 / 0.17 %
            {"magnetic-filings", {blocks(16, 2.7, 0.5), blocks(16, 2.7, 0.5), {}}},
            // largest 26 / 30 (threshold 34), mean 2.50 / 2.76
            {"mesh-gradient", {{}, pixels(0.05, 3.5, 34), {}}},
            // 0.14 / 0.21 / 0.19 % bad
            {"particle-field", {pixels(0.27), pixels(0.27), {}}},
            // 32 px blocks: largest 18 / 19 / 18, mean 3.35 / 3.33 / 3.39, histograms 0.48 / 0.51 / 0.57 %
            {"particle-flow", {blocks(32, 4.4, 0.75), blocks(32, 4.4, 0.75), {}}},
            // 32 px blocks: largest 12 / 7 / 8, mean 1.74 / 0.61 / 0.59, histograms 0.49 / 0.47 / 0.41 %
            {"particles", {blocks(32, 2.3, 0.65), blocks(32, 0.8, 0.65), {}}},
            // 0.18 / 0.37 / 0.39 % bad
            {"pixel-sort", {pixels(0.5), pixels(0.5), {}}},
            // t0 / t1 0.051 / 0.071 % bad
            {"shatter", {pixels(0.13), pixels(0.13), {}}},  // RTX 3060 Ti: t1 0.0995 % bad
            // t0 within the default; t1 0.13 % bad
            {"smoke-fill", {{}, pixels(0.16), {}}},
            // t1 / t2: mean colours 2 / 3 apart, histograms 2.27 / 3.41 %, edges 0.67 / 1.24 %
            {"voronoi", {{}, means(4, 4.3, 2.4), {}}},  // RTX 3060 Ti: mean colours 2 / 2 apart, histograms 1.90 / 1.03 %, edges 1.84 / 1.77 %
        };
        return tolerances;
    }

    // the goldens' renderer (llvmpipe, lavapipe) keeps the default comparison for every effect
    const HardwareTolerance& toleranceOf(const std::string& name){
        static const HardwareTolerance strict;
        if (GpuEnvironment::isReferenceRenderer())
            return strict;
        const auto& tolerances = hardwareTolerances();
        const auto found = tolerances.find(name);
        return found == tolerances.end() ? strict : found->second;
    }

    bool updatingGoldens(){
        const char* value = std::getenv("JASPER_HTMLUI_UPDATE_GOLDEN");
        return value && std::string(value) == "1";
    }

    std::vector<std::string> builtinNames(){
        std::vector<std::string> names;
        for (const EffectManifest* m : EffectRegistry::get().builtins())
            names.push_back(m->name);
        return names;
    }

    std::string goldenFile(const std::string& name){
        return sourcePath("golden/" + name + ".png");
    }

    // renders one declaration of a fixture at t, with the test pointer for pointer effects
    bool render(EffectScene& scene, const EffectManifest& m, const std::string& fixture, const std::string& declaration, double t,
                TestImage& image, std::string* error = nullptr){
        if (!scene.load(fixture, declaration, error))
            return false;
        EffectClock::setFixedTime(t);
        if (m.pointer != EffectPointer::None)
            setTestPointer(scene, m);
        return scene.capture(image);
    }

    struct EffectGoldenTest : ::testing::TestWithParam<std::string> {
        void SetUp() override{
            TestSystem::get().clearLogs();
        }
        void TearDown() override{
            EffectClock::setFixedTime(std::nullopt);
            EffectRuntime::get().clearTestInput();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u) << logText(D_LOG_WARN);
        }
    };

}

TEST_P(EffectGoldenTest, GoldensAndInvariants){
    const EffectManifest* m = EffectRegistry::get().find(GetParam());
    ASSERT_NE(m, nullptr);
    EffectScene scene;
    if (!scene.valid()) {
        GTEST_SKIP() << "no GPU";
    }
    ASSERT_TRUE(warmUpEffect(*m)) << m->name << ": its variants do not compile";
    const std::string fixture = fixtureOf(*m);
    // transitions are captured part-way (5.2: progress 40%)
    const bool transition = m->category == "transition";
    const std::string declaration = transition ? declarationOf(*m, "progress 40%") : declarationOf(*m);
    const HardwareTolerance& tolerance = toleranceOf(m->name);
    std::string error;

    // t1, twice (I2: two renders of the same frame are identical), then t2
    TestImage t1, again, t2;
    ASSERT_TRUE(render(scene, *m, fixture, declaration, kT1, t1, &error)) << error;
    ASSERT_TRUE(render(scene, *m, fixture, declaration, kT1, again, &error)) << error;
    EXPECT_EQ(maxDifference(t1, again), 0) << m->name << ": two renders at the same fixed time differ (I2)";
    ASSERT_TRUE(render(scene, *m, fixture, declaration, kT2, t2, &error)) << error;
    const CompareResult golden = compareWithGolden("effects/" + m->name + ".t1", t1.rgba, t1.width, t1.height, tolerance.late);
    EXPECT_TRUE(golden.passed) << golden.message;

    // I3: a .t2 golden exists exactly when the effect is animated
    const bool animated = maxDifference(t1, t2) > 1;
    const std::string t2Name = "effects/" + m->name + ".t2";
    if (updatingGoldens()){
        if (animated)
            compareWithGolden(t2Name, t2.rgba, t2.width, t2.height, tolerance.late);
        else
            std::remove(goldenFile(t2Name).c_str());
    }else if (fileExists(goldenFile(t2Name))){
        EXPECT_TRUE(animated) << m->name << ": has a .t2 golden but renders the same at t1 and t2 (I3)";
        const CompareResult second = compareWithGolden(t2Name, t2.rgba, t2.width, t2.height, tolerance.late);
        EXPECT_TRUE(second.passed) << second.message;
    }else{
        EXPECT_FALSE(animated) << m->name << ": renders differently at t1 and t2 but has no .t2 golden (I3)";
    }

    if (m->stateful()){
        // I8: a stateful effect's early state (t0) has a golden and differs from t1: it evolves
        TestImage t0;
        ASSERT_TRUE(render(scene, *m, fixture, declaration, kT0, t0, &error)) << error;
        const CompareResult early = compareWithGolden("effects/" + m->name + ".t0", t0.rgba, t0.width, t0.height, tolerance.early);
        EXPECT_TRUE(early.passed) << early.message;
        EXPECT_GT(maxDifference(t0, t1), 1) << m->name << ": its simulation draws the same at t0 and t1 (I8)";
        // I9: the pinned clock's state depends on the time only: t1 reached from t2 in the same
        // document (a replay) is t1's render byte for byte
        TestImage back;
        ASSERT_TRUE(render(scene, *m, fixture, declaration, kT2, back, &error)) << error;
        EffectClock::setFixedTime(kT1);
        ASSERT_TRUE(scene.capture(back));
        EXPECT_EQ(maxDifference(t1, back), 0) << m->name << ": t1 replayed after t2 differs from t1 (I9)";
    }

    // I1: opacity 0 is the render without the effect
    TestImage transparent, without;
    ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "opacity 0"), kT1, transparent, &error)) << error;
    ASSERT_TRUE(render(scene, *m, fixture, noEffectDeclaration(*m), kT1, without, &error)) << error;
    EXPECT_LE(maxDifference(transparent, without), 1) << m->name << ": opacity 0 differs from no effect (I1)";

    // I4 (transitions): progress 0% is no effect, 100% removes the area, invert swaps them
    if (transition){
        TestImage start, end, invertedStart, invertedEnd, hidden;
        ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "progress 0%"), kT1, start, &error)) << error;
        ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "progress 100%"), kT1, end, &error)) << error;
        ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "progress 0%, invert on"), kT1, invertedStart, &error)) << error;
        ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "progress 100%, invert on"), kT1, invertedEnd, &error)) << error;
        EXPECT_LE(maxDifference(start, without), 1) << m->name << ": progress 0% is not the content (I4)";
        EXPECT_LE(maxDifference(invertedEnd, without), 1) << m->name << ": inverted progress 100% is not the content (I4)";
        EXPECT_LE(maxDifference(end, invertedStart), 1) << m->name << ": invert does not swap the ends (I4)";
        // progress 100% leaves the area's alpha at most 1/255: the render is the one without the element
        ASSERT_TRUE(render(scene, *m, fixture, "opacity: 0", kT1, hidden, &error)) << error;
        EXPECT_LE(maxDifference(end, hidden), 1) << m->name << ": progress 100% does not remove the area (I4)";
    }

    // the gradient-text golden of the listed paint effects
    if (kTextEffects.count(m->name)){
        TestImage text;
        ASSERT_TRUE(render(scene, *m, "text", "background-image: effect(" + m->name + ")", kT1, text, &error)) << error;
        const CompareResult textGolden = compareWithGolden("effects/" + m->name + ".text", text.rgba, text.width, text.height, tolerance.text);
        EXPECT_TRUE(textGolden.passed) << textGolden.message;
    }

    // I6: a paint effect in filter form over nothing leaves nothing
    if (m->kind == EffectKind::Paint){
        TestImage empty;
        ASSERT_TRUE(render(scene, *m, "empty", "filter: effect(" + m->name + ")", kT1, empty, &error)) << error;
        int alpha = 0;
        for (size_t i = 3; i < empty.rgba.size(); i += 4)
            alpha = std::max(alpha, (int)empty.rgba[i]);
        EXPECT_EQ(alpha, 0) << m->name << ": its filter form paints over transparent content (I6)";
    }

    // I5: 0x0, 1x1, 1x400 and 400x1 elements render without a renderer error (the log is checked in TearDown)
    {
        EffectScene sizes(420, 420);
        TestImage image;
        ASSERT_TRUE(render(sizes, *m, "sizes", declaration, kT1, image, &error)) << error;
        if (m->kind == EffectKind::Paint){
            // and in filter form
            ASSERT_TRUE(render(sizes, *m, "sizes", "filter: effect(" + m->name + ")", kT1, image, &error)) << error;
        }
    }

    // I7: a manifest, a golden, a catalogue entry, and a LICENSE entry for a shaders.com port
    EXPECT_FALSE(m->summary.empty());
    EXPECT_TRUE(fileExists(goldenFile("effects/" + m->name + ".t1")));
    const std::string catalogue = readText(sourcePath("../../../docs/mcp/guide/html-ui-effects.md"));
    EXPECT_NE(catalogue.find("### `" + m->name + "`"), std::string::npos) << m->name << " has no entry in html-ui-effects.md (I7)";
    EXPECT_TRUE(fileExists(sourcePath("../../../docs/mcp/guide/img/effects/" + m->name + ".png"))) << m->name << " has no thumbnail (I7)";
    if (m->source.rfind("shaders.com", 0) == 0){
        const std::string license = readText(sourcePath("../../shaderlib/effects/LICENSE.shaders-com.txt"));
        EXPECT_NE(license.find("  " + m->name + " "), std::string::npos) << m->name << " is not listed in LICENSE.shaders-com.txt (I7)";
    }
}

// a hardware tolerance names a built-in effect (a renamed or removed effect takes its entry along)
TEST(EffectGoldenTolerances, NameBuiltinEffects){
    for (const auto& entry : hardwareTolerances())
        EXPECT_NE(EffectRegistry::get().find(entry.first), nullptr) << entry.first;
}

INSTANTIATE_TEST_SUITE_P(EveryEffect, EffectGoldenTest, ::testing::ValuesIn(builtinNames()),
                         [](const ::testing::TestParamInfo<std::string>& info){
                             std::string name = info.param;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });
