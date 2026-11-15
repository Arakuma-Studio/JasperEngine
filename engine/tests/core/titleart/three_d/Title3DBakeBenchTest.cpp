// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 R47, AC24 (TitleBakeBench): the bake of Gold 3D "BIG BONUS" in a 1920 x 1080 canvas at
// k = 3 with the full glint, from document to readback (a warm environment: the cold build is
// timed apart), and a cold procedural environment build at 1024. The figures are printed
// ("[title3d-bench]") and, with JASPER_TITLE3D_BENCH_JSON=<file>, written for check_bench.py
// (bench "je-bench-titlebake"); the test itself checks that both complete and that the bake ran
// at k = 3 with the glint. docs/perf/title3d.md records what this host measured (a software
// rasteriser here: far above the build server's budgets).

#include "titleart/render/TitleRenderTestSupport.h"
#include "titleart/support/TitleTestAssets.h"

#include "titleart/three_d/Title3DRenderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    Json goldBigBonus() {
        Json gold = materialPreset("gold");
        Json bands = Json::array();
        bands.push_back(Json{{"grow", 1.5}, {"depth", 10}, {"bevel", {{"width", 1.2}, {"depth", 1.2}, {"profile", "chamfer"}}}, {"material", gold}});
        bands.push_back(Json{{"grow", 0.6}, {"depth", 8}, {"bevel", {{"width", 0.3}, {"depth", 0.6}, {"profile", "step"}}},
                             {"material", materialPreset("black_gloss")}});
        bands.push_back(Json{{"grow", 2.2}, {"depth", 6}, {"plate", {{"kind", "hull"}}}, {"material", materialPreset("black_gloss")}});
        Json style = Json{{"depth", 12}, {"curve_segments", 10}, {"bevel", {{"width", 2.5}, {"depth", 2.5}, {"profile", "round"}}},
                          {"face", gold}, {"bands", bands},
                          {"glint", {{"enabled", true}, {"threshold", 1}, {"intensity", 1}, {"radius", 5}, {"streaks", 4}, {"length", 32}, {"angle", 15}}}};
        return Json{{"id", "00000001"}, {"type", "text"}, {"name", "BIG BONUS"}, {"text", "BIG BONUS"}, {"font", fontRef(DELA_GOTHIC)},
                    {"size", 200}, {"dimension", "3d"}, {"style_3d", style}};
    }

    double msSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

}

TEST(Title3DBakeBenchGpu, GoldBigBonusAndAColdEnvironment) {
    JASPER_REQUIRE_GPU();
    static TestAssetResolver assets;
    auto doc = makeDoc({{"canvas", canvas(1920, 1080)}, {"layers", Json::array({goldBigBonus()})}, {"scene_3d", {{"supersample", 3}}}});
    TitleArtRenderer renderer;
    renderer.setContentProvider(makeGeometryTitleContentProvider());
    RenderRequest r;
    r.doc = doc;
    r.pxScale = 1.0f;
    r.assets = &assets;
    // the first render builds the environment (cold) and the programs
    auto start = std::chrono::steady_clock::now();
    const Pixels cold = render(renderer, r);
    const double coldMs = msSince(start);
    ASSERT_GT(cold.width, 0);
    const std::shared_ptr<void> r3d = renderer.title3DRendererForTesting();
    ASSERT_TRUE(r3d);
    const Title3DStats& st = static_cast<const Title3DRenderer*>(r3d.get())->stats();
    EXPECT_EQ(st.environmentBuilds, 1u);
    EXPECT_GT(st.glintPasses, 0u) << "the full glint ran";
    // a warm bake: a changed colour (the cached pixels do not serve it), the environment kept
    Json layer = goldBigBonus();
    layer["style_3d"]["face"]["color"] = "#ffd060";
    r.doc = makeDoc({{"canvas", canvas(1920, 1080)}, {"layers", Json::array({layer})}, {"scene_3d", {{"supersample", 3}}}});
    start = std::chrono::steady_clock::now();
    const Pixels warm = render(renderer, r);
    const double bakeMs = msSince(start);
    ASSERT_EQ(warm.width, 1920);
    EXPECT_EQ(st.environmentBuilds, 1u) << "the warm bake reused the environment";
    // a cold procedural environment at 1024 alone: a new renderer's first build, the bake's time
    // with the environment minus the warm bake's
    const double envMs = std::max(0.0, coldMs - bakeMs);
    std::printf("[title3d-bench] gold_big_bonus_bake_ms %.1f\n[title3d-bench] cold_environment_1024_ms %.1f\n", bakeMs, envMs);
    if (const char* out = std::getenv("JASPER_TITLE3D_BENCH_JSON")) {
        std::ofstream f(out);
        f << "{\n  \"bench\": \"je-bench-titlebake\",\n  \"results\": {\n    \"gold_big_bonus_bake_ms\": " << bakeMs
          << ",\n    \"cold_environment_1024_ms\": " << envMs << "\n  }\n}\n";
    }
}

TEST(Title3DBakeBenchGpu, RealtimeTiers) {
    // S86 R48, §6.7: one whole re-render of a Realtime title covering about 30 % of a 1080p frame
    // per tier (Gold 3D "BIG BONUS" at 280 px, a colour change: the mesh and the environment
    // cached, every pass run), the median of 5 with its readback, and the passes it took. Printed
    // ("[title3d-bench] realtime_<tier>_ms"); the test checks the pass counts against §6.7
    JASPER_REQUIRE_GPU();
    static TestAssetResolver assets;
    TitleArtRenderer renderer;
    renderer.setContentProvider(makeGeometryTitleContentProvider());
    auto docOf = [](const char* color) {
        Json layer = goldBigBonus();
        layer["size"] = 280;
        layer["style_3d"]["face"]["color"] = color;
        return makeDoc({{"canvas", canvas(1920, 1080)}, {"layers", Json::array({layer})}});
    };
    struct Tier {
        int tier;
        const char* name;
        uint32_t maxPasses;
    };
    for (const Tier& t : {Tier{1, "high", 80}, Tier{2, "medium", 48}, Tier{3, "low", 7}}) {
        RenderRequest r;
        r.pxScale = 1.0f;
        r.assets = &assets;
        r.tier = t.tier;
        r.doc = docOf("#ffffff");
        const Pixels first = render(renderer, r);
        ASSERT_GT(first.width, 0) << t.name;
        const Title3DStats& st = static_cast<const Title3DRenderer*>(renderer.title3DRendererForTesting().get())->stats();
        std::vector<double> ms;
        uint64_t passes = 0;
        for (int i = 0; i < 5; i++) {
            char color[8];
            std::snprintf(color, sizeof(color), "#ff%02x60", 0x80 + i * 16);
            r.doc = docOf(color);
            const uint64_t before = st.mainPasses + st.postPasses + st.glintPasses + st.aoPasses;
            const auto start = std::chrono::steady_clock::now();
            const Pixels p = render(renderer, r);
            ms.push_back(msSince(start));
            passes = st.mainPasses + st.postPasses + st.glintPasses + st.aoPasses - before;
            ASSERT_GT(p.width, 0);
        }
        std::sort(ms.begin(), ms.end());
        std::printf("[title3d-bench] realtime_%s_ms %.2f (median of 5, passes %llu, region 30 %% of 1080p)\n", t.name, ms[2],
                    (unsigned long long)passes);
        EXPECT_LE(passes, (uint64_t)t.maxPasses) << t.name << ": §6.7's pass budget";
    }
}
