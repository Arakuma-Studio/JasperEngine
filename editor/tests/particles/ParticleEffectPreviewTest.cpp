// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S91 R26-R29: the deterministic run behind preview_particle_effect, metrics only (no graphics
// view): the same request gives the same report, capacity_hit fails on a burst over Max
// Particles, invisible on an empty effect, pop on particles dying opaque, never_ends on a
// one-shot that outlives the run; the analytic overdraw of stacked quads; the checks of made-up
// measurements against the project's thresholds.

#include "particles/ParticleEffectChecks.h"
#include "particles/ParticleEffectPreview.h"

#include "particles/ParticleEffectCodec.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::particles;

namespace {

    ParticleEffectDocument effectOf(const std::string& json){
        ParticleEffectDocument document;
        std::vector<Diagnostic> diagnostics;
        EXPECT_TRUE(ParticleEffectCodec::read(json, document, diagnostics)) << ParticleEffectCodec::format(diagnostics);
        return document;
    }

    ParticlePreviewRequest requestOf(const std::string& json){
        ParticlePreviewRequest request;
        ParticlePreviewSystem system;
        system.name = "test";
        system.document = effectOf(json);
        request.systems.push_back(system);
        request.frames = 0;
        return request;
    }

    qa::Json reportOf(const ParticlePreviewRequest& request){
        qa::Json report;
        std::string error;
        EXPECT_TRUE(ParticleEffectPreview::runCpu(request, report, error)) << error;
        return report;
    }

    const qa::Json& checkOf(const qa::Json& report, const std::string& id){
        static const qa::Json none;
        for (const qa::Json& check : report["checks"]){
            if (check["id"] == id) return check;
        }
        ADD_FAILURE() << "no check " << id;
        return none;
    }

    // a one-shot burst of `count` into Max Particles `max`, still, lifetime 1 s
    std::string burst(int count, int max, const std::string& extra = std::string()){
        return R"({"version": 1, "duration": 1, "looping": false, "seed": 5, "maxParticles": )" + std::to_string(max) + R"(, "modules": [
            {"type": "main", "startLifetime": 1, "startSpeed": 2, "startSize": 0.2},
            {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": )" + std::to_string(count) + "}]}" + extra + "]}";
    }

}

TEST(ParticleEffectPreview, TheSameRequestGivesTheSameReport){
    const ParticlePreviewRequest request = requestOf(burst(300, 1000));
    const qa::Json first = reportOf(request);
    const qa::Json second = reportOf(request);
    EXPECT_EQ(first.dump(), second.dump());
    EXPECT_EQ(first["seeds"][0]["seed"], 5) << "the effect's own seed";

    ParticlePreviewRequest reseeded = request;
    reseeded.seed = 9;
    const qa::Json other = reportOf(reseeded);
    EXPECT_NE(other["state_hash"], first["state_hash"]) << "another seed is another run";
    EXPECT_EQ(other["seeds"][0]["seed"], 9);
}

TEST(ParticleEffectPreview, CapacityHitFailsOnABurstOverMaxParticles){
    const qa::Json over = reportOf(requestOf(burst(600, 400)));
    const qa::Json& hit = checkOf(over, "capacity_hit");
    EXPECT_EQ(hit["status"], "FAIL") << hit.dump();
    EXPECT_EQ(hit["measured"]["never_born"], 200);
    EXPECT_EQ(hit["measured"]["max_particles"], 400);
    EXPECT_EQ(over["metrics"]["count"]["peak"], 400);
    ASSERT_EQ(over["fixes"].size(), 1u);
    EXPECT_EQ(over["fixes"][0]["patch"]["maxParticles"], 600);

    const qa::Json under = reportOf(requestOf(burst(300, 400)));
    EXPECT_EQ(checkOf(under, "capacity_hit")["status"], "PASS");
    // exactly at the limit refuses nothing
    EXPECT_EQ(checkOf(reportOf(requestOf(burst(400, 400))), "capacity_hit")["status"], "PASS");
}

TEST(ParticleEffectPreview, InvisibleFailsOnAnEmptyEffect){
    const qa::Json empty = reportOf(requestOf(R"({"version": 1, "duration": 1, "looping": false, "modules": [
        {"type": "main", "startLifetime": 1}, {"type": "emission", "rateOverTime": 0}]})"));
    const qa::Json& invisible = checkOf(empty, "invisible");
    EXPECT_EQ(invisible["status"], "FAIL");
    EXPECT_EQ(invisible["limited_by"], "No particle was alive during the run.");
    EXPECT_EQ(empty["verdict"], "FAIL");
    EXPECT_EQ(checkOf(reportOf(requestOf(burst(50, 100))), "invisible")["status"], "PASS");
}

TEST(ParticleEffectPreview, PopWarnsWhenParticlesDieOpaque){
    // every particle of the burst dies in the same tick at full alpha
    EXPECT_EQ(checkOf(reportOf(requestOf(burst(100, 1000))), "pop")["status"], "WARN");
    // the same, faded out over their life
    const std::string fade = R"(, {"type": "colorOverLifetime", "color": {"mode": "gradient", "gradient": {"colorKeys": [[0, 1, 1, 1], [1, 1, 1, 1]],
        "alphaKeys": [[0, 1], [1, 0]]}}})";
    const qa::Json faded = reportOf(requestOf(burst(100, 1000, fade)));
    EXPECT_EQ(checkOf(faded, "pop")["status"], "PASS") << checkOf(faded, "pop").dump();
}

TEST(ParticleEffectPreview, NeverEndsWarnsOnAOneShotThatOutlivesTheRun){
    const qa::Json ended = reportOf(requestOf(burst(10, 100)));
    EXPECT_EQ(checkOf(ended, "never_ends")["status"], "PASS");
    EXPECT_NEAR(ended["metrics"]["count"]["ended_at"].get<double>(), 1.0, 0.1);

    const std::string forever = R"({"version": 1, "duration": 1, "looping": false, "modules": [
        {"type": "main", "startLifetime": 100, "startSpeed": 0}, {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 5}]}]})";
    const qa::Json lasting = reportOf(requestOf(forever));
    EXPECT_EQ(checkOf(lasting, "never_ends")["status"], "WARN");

    const qa::Json looping = reportOf(requestOf(R"({"version": 1, "duration": 1, "looping": true, "modules": [{"type": "main"}]})"));
    EXPECT_EQ(checkOf(looping, "never_ends")["status"], "SKIP");
}

TEST(ParticleEffectPreview, StackedQuadsCountAsLayers){
    // 30 still particles at the same point: the grid cells inside their quad hold 30 layers
    const std::string stacked = R"({"version": 1, "duration": 1, "looping": false, "seed": 3, "modules": [
        {"type": "main", "startLifetime": 2, "startSpeed": 0, "startSize": 1},
        {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 30}]}, {"type": "shape", "enabled": false}]})";
    const qa::Json report = reportOf(requestOf(stacked));
    EXPECT_NEAR(report["metrics"]["overdraw"]["peak_layers"].get<double>(), 30.0, 0.01);
    const qa::Json& budget = checkOf(report, "overdraw_budget");
    EXPECT_EQ(budget["status"], "FAIL") << "30 layers over mobile's 2 x 8";
    EXPECT_EQ(budget["threshold_source"], "particles.qa.overdraw_mobile");

    ParticlePreviewRequest desktop = requestOf(stacked);
    desktop.platform = "desktop";
    EXPECT_EQ(checkOf(reportOf(desktop), "overdraw_budget")["status"], "WARN") << "over desktop's 20, under 40";
}

TEST(ParticleEffectChecks, CostIsAnEstimateFromTheBenchTable){
    ParticleMeasurements m;
    m.peak = 10000;
    m.emitters = 1;
    m.noise = true;
    m.ticksPerSecond = 60.0;
    const qa::Json cost = ParticleEffectChecks::cost(m, "desktop");
    EXPECT_EQ(cost["method"], "estimate_from_bench_table");
    EXPECT_EQ(cost["note"], "estimate, not measured on a device");
    EXPECT_NEAR(cost["sim_ms"].get<double>(), (10000 * 0.035 + 1.6) / 1000.0, 1e-3);
    ParticleCheckRequest request;
    request.platform = "mobile";
    const qa::Json rows = ParticleEffectChecks::rows(m, request);
    const qa::Json& budget = rows[6];
    ASSERT_EQ(budget["id"], "platform_budget");
    EXPECT_NE(budget["method"].get<std::string>().find("not measured on a device"), std::string::npos);
    EXPECT_EQ(budget["status"], "FAIL") << "10 k particles with Noise on mobile: about 0.7 ms simulated + 0.27 ms prep, over 2 x 0.25";
    EXPECT_EQ(rows[3]["status"], "SKIP") << "blown_out needs cells";
}

TEST(ParticleEffectChecks, ChangedThresholdsAreReported){
    ParticleQaSettings qa;
    qa.popPercent = 80.0f;
    const qa::Json thresholds = ParticleEffectChecks::thresholds(qa);
    ASSERT_EQ(thresholds["changed"].size(), 1u);
    EXPECT_EQ(thresholds["changed"][0]["key"], "particles.qa.pop_percent");

    ParticleMeasurements m;
    m.peak = 100;
    m.popPercent = 50.0;
    ParticleCheckRequest request;
    EXPECT_EQ(ParticleEffectChecks::rows(m, request)[4]["status"], "WARN");
    request.qa = qa;
    EXPECT_EQ(ParticleEffectChecks::rows(m, request)[4]["status"], "PASS");
}

TEST(ParticleEffectChecks, InvisibleFollowsTheWholeRunNotTheCells){
    ParticleMeasurements m;
    m.peak = 100;
    m.meanAlpha = 1.0;
    m.analyticMaxCoverage = 12.0;
    // two cells that missed the live part of the run (the start and the end): blank, and blank by the analytic cover too
    m.cellCoverage = {{0.0, 0.0}, {0.0, 0.0}};
    ParticleCheckRequest request;
    request.hasImages = true;
    EXPECT_EQ(ParticleEffectChecks::rows(m, request)[2]["status"], "PASS");
    // a cell where the quads cover the view but no pixel shows: the texture or colour draws nothing
    m.cellCoverage = {{0.0, 9.0}};
    const qa::Json row = ParticleEffectChecks::rows(m, request)[2];
    EXPECT_EQ(row["status"], "FAIL");
    EXPECT_NE(row["limited_by"].get<std::string>().find("cells show nothing"), std::string::npos);
    // no cell at all (frames 0, with or without overdraw_map): the analytic cover decides
    m.cellCoverage.clear();
    request.hasImages = false;
    EXPECT_EQ(ParticleEffectChecks::rows(m, request)[2]["status"], "PASS");
}

TEST(ParticleEffectPreview, CutRunsSkipNeverEndsAndVariableStepsCostAFrameTick){
    ParticlePreviewRequest cut = requestOf(burst(10, 100));
    cut.seconds = 0.5;
    EXPECT_EQ(checkOf(reportOf(cut), "never_ends")["status"], "SKIP");
    const qa::Json variable = reportOf(requestOf(R"({"version": 1, "duration": 1, "looping": true, "tickRate": 0, "modules": [{"type": "main"}]})"));
    EXPECT_DOUBLE_EQ(variable["metrics"]["cost"]["ticks_per_frame"].get<double>(), 1.0);
}
