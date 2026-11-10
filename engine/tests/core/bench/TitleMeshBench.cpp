// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The 3D title geometry budgets (docs/specs/S86-title-3d.md R46, AC24):
//   je-bench-titlemesh [--json <file>]
// "BIG BONUS" in Dela Gothic One at 200 px with the Gold 3D structure (letter with a chamfer
// ring, a step ring and a plate) built cold on one thread and with the job system's workers,
// its triangle count and vertex ratio; 激熱 (Dela Gothic One: the spec's Yuji Syuku is not among
// the test fonts); an inflate of three kanji at detail 4; retyping the last letter (only the
// changed islands rebuild); a material edit (no geometry). Medians of repeated cold runs (a new
// mesher each run). check_bench.py gates them with tools/perf/bench-thresholds.json (the budgets
// of the reference build server); docs/perf/title3d.md records what this host measured.

#include "BenchUtil.h"
#include "JobsTestSystem.h"

#include "Engine.h"

#include "TestPaths.h"
#include "titleart/render/TitleContent.h"
#include "titleart/support/TitleTestAssets.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/mesher/TitleMesher.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    // the Gold 3D structure: a gold letter with a round bevel, a chamfered gold ring, a dark step
    // ring and a black plate (Pachislop's Gold 3D look, simplified to its solids)
    Json goldStyle(const char* mode = "extrude") {
        Json gold = materialPreset("gold");
        Json bands = Json::array();
        bands.push_back(Json{{"grow", 1.5}, {"depth", 10}, {"bevel", {{"width", 1.2}, {"depth", 1.2}, {"profile", "chamfer"}}}, {"material", gold}});
        bands.push_back(Json{{"grow", 0.6}, {"depth", 8}, {"bevel", {{"width", 0.3}, {"depth", 0.6}, {"profile", "step"}}},
                             {"material", materialPreset("black_gloss")}});
        bands.push_back(Json{{"grow", 2.2}, {"depth", 6}, {"plate", {{"kind", "hull"}}}, {"material", materialPreset("black_gloss")}});
        return Json{{"mode", mode}, {"depth", 12}, {"curve_segments", 10}, {"bevel", {{"width", 2.5}, {"depth", 2.5}, {"profile", "round"}}},
                    {"face", gold}, {"bands", bands}};
    }

    three_d::TitleMeshRequest requestFor(const std::string& text, Json style, TestAssetResolver& assets) {
        Json layer = Json{{"id", "00000001"}, {"type", "text"}, {"name", "T"}, {"text", text}, {"font", fontRef(DELA_GOTHIC)}, {"size", 200},
                          {"dimension", "3d"}, {"style_3d", std::move(style)}};
        const TitleArtDoc doc = documentWith({layer});
        static auto content = makeGeometryTitleContentProvider();
        three_d::TitleMeshRequest r;
        r.source = three_d::collectTitleSource(doc.layers()[0], doc, *content, &assets, nullptr);
        r.style = decodeStyle3D(doc.layers()[0]);
        r.blocks = three_d::collectCharBlocks(doc.layers()[0], r.style.bands.size());
        r.tier = TitleTier::Bake;
        r.outputScale = 1.0;
        return r;
    }

    template <typename Fn>
    double medianMs(int runs, Fn&& fn) {
        std::vector<double> samples;
        for (int i = 0; i < runs; i++) {
            const auto start = bench::Clock::now();
            fn();
            samples.push_back(bench::secondsSince(start) * 1.0e3);
        }
        return bench::percentile(samples, 0.5);
    }

}

int main(int argc, char** argv) {
    const char* json = nullptr;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) json = argv[++i];
    }
    jasperengine::test::TestPaths paths;
    paths.sourceDir = JE_TITLE_BENCH_SOURCE_DIR;
    jasperengine::test::setTestPaths(paths);
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    TestAssetResolver assets;
    bench::Results results("je-bench-titlemesh");

    const three_d::TitleMeshRequest big = requestFor("BIG BONUS", goldStyle(), assets);
    std::shared_ptr<const TitleMesh> mesh;
    const double bigMs = medianMs(5, [&] {
        three_d::TitleMesher mesher;
        mesh = mesher.build(big);
    });
    results.add("big_bonus_gold_ms", bigMs, "ms");
    results.add("big_bonus_triangles", mesh->stats.triangles, "");
    results.add("big_bonus_vertex_ratio", (double)mesh->stats.vertices / std::max<uint32_t>(1, mesh->stats.triangles), "");

    const three_d::TitleMeshRequest kanji = requestFor("激熱", goldStyle(), assets);
    results.add("kanji_gold_ms", medianMs(3, [&] {
                    three_d::TitleMesher mesher;
                    mesher.build(kanji);
                }),
                "ms");

    Json inflate = goldStyle("inflate");
    inflate["inflate"] = Json{{"height", 12}, {"detail", 4}};
    const three_d::TitleMeshRequest puffed = requestFor("激熱王", inflate, assets);
    results.add("inflate_3_kanji_detail4_ms", medianMs(3, [&] {
                    three_d::TitleMesher mesher;
                    mesher.build(puffed);
                }),
                "ms");

    // retyping the last letter: the islands of the other letters come from the shape cache
    const three_d::TitleMeshRequest retyped = requestFor("BIG BONUZ", goldStyle(), assets);
    std::vector<double> retype;
    for (int i = 0; i < 5; i++) {
        three_d::TitleMesher mesher;
        mesher.build(big);
        const auto start = bench::Clock::now();
        mesher.build(retyped);
        retype.push_back(bench::secondsSince(start) * 1.0e3);
    }
    results.add("retype_last_letter_ms", bench::percentile(retype, 0.5), "ms");

    // a material edit: the geometry key is the same, the mesher answers from its cache
    Json recoloured = goldStyle();
    recoloured["face"]["color"] = "#ff0000";
    const three_d::TitleMeshRequest material = requestFor("BIG BONUS", recoloured, assets);
    three_d::TitleMesher warm;
    warm.build(big);
    const uint64_t before = warm.meshBuilds();
    const double materialMs = medianMs(5, [&] { warm.build(material); });
    results.add("material_edit_mesh_builds", (double)(warm.meshBuilds() - before), "");
    results.add("material_edit_ms", materialMs, "ms");

    if (json && !results.write(json)) {
        std::fprintf(stderr, "cannot write %s\n", json);
        return 1;
    }
    return 0;
}
