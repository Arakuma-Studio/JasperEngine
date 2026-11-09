// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 R38 on S91's checks: the preview's off_grid row (base pixels the pixel view drew between grid
// lines) and S48's pixel-art lints (SKIP until S48 lands), from made-up measurements; an Analytic
// effect's live particles count in a metrics-only run (its twin's, S49 R19).

#include "particles/ParticleEffectChecks.h"
#include "particles/ParticleEffectPreview.h"

#include "particles/ParticleEffectCodec.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::particles;

namespace {

    const qa::Json& rowOf(const qa::Json& rows, const std::string& id){
        static const qa::Json none;
        for (const qa::Json& row : rows){
            if (row["id"] == id) return row;
        }
        ADD_FAILURE() << "no check " << id;
        return none;
    }

}

TEST(ParticlePixelChecks, OffGridIsMeasuredOnThePixelView){
    ParticleMeasurements m;
    ParticleCheckRequest request;
    // not a pixel effect: nothing to measure
    EXPECT_EQ(rowOf(ParticleEffectChecks::rows(m, request), "off_grid")["status"], "SKIP");
    // a pixel effect previewed in perspective: SKIP, naming the pixel view
    m.pixelEffect = true;
    const qa::Json perspective = rowOf(ParticleEffectChecks::rows(m, request), "off_grid");
    EXPECT_EQ(perspective["status"], "SKIP");
    EXPECT_NE(perspective["limited_by"].get<std::string>().find("pixel"), std::string::npos);
    // drawn on the grid
    m.pixelGrid = true;
    m.pixelsDrawn = 400;
    EXPECT_EQ(rowOf(ParticleEffectChecks::rows(m, request), "off_grid")["status"], "PASS");
    // base pixels between grid lines
    m.offGrid = 37;
    m.offGridAt = {{0.25, {3, 4}}};
    const qa::Json failed = rowOf(ParticleEffectChecks::rows(m, request), "off_grid");
    EXPECT_EQ(failed["status"], "FAIL");
    EXPECT_EQ(failed["measured"]["off_grid"], 37);
    EXPECT_NE(failed["fix_hint"].get<std::string>().find("Snap to Pixel Grid"), std::string::npos);
    EXPECT_EQ(failed["at"].size(), 1u);
}

TEST(ParticlePixelChecks, S48LintsSkipUntilS48Lands){
    ParticleMeasurements m;
    m.pixelEffect = true;
    const qa::Json rows = ParticleEffectChecks::rows(m, ParticleCheckRequest());
    for (const char* id : {"off_palette", "partial_alpha", "spill", "one_shot_end", "loop_seam", "determinism"}){
        const qa::Json& row = rowOf(rows, id);
        EXPECT_EQ(row["status"], "SKIP") << id;
        EXPECT_NE(row["limited_by"].get<std::string>().find("needs S48"), std::string::npos) << id;
    }
    EXPECT_EQ(rows.size(), ParticleEffectChecks::checkIds().size());
}

TEST(ParticlePixelChecks, AnAnalyticEffectsParticlesCount){
    ParticleEffectDocument d;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::read(R"({"version": 1, "duration": 4, "looping": true, "prewarm": true, "simulationMode": "analytic", "seed": 3,
        "modules": [{"type": "main", "startLifetime": 1, "startSpeed": 2, "startSize": 1}, {"type": "emission", "rateOverTime": 20},
                    {"type": "pixelArt"}, {"type": "renderer", "renderMode": "pixel"}]})", d, diagnostics)) << ParticleEffectCodec::format(diagnostics);
    ParticlePreviewRequest request;
    ParticlePreviewSystem system;
    system.name = "loop";
    system.document = d;
    request.systems.push_back(system);
    request.frames = 0;
    qa::Json report;
    std::string error;
    ASSERT_TRUE(ParticleEffectPreview::runCpu(request, report, error)) << error;
    EXPECT_GT(report["metrics"]["count"]["peak"].get<uint32_t>(), 10u) << "the twin's live particles";
    EXPECT_EQ(rowOf(report["checks"], "off_grid")["status"], "SKIP") << "measured on the pixel view's cells only";
}
