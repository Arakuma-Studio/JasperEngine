// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleCaptureChecks.h"

#include "particles/ParticlePixel.h"
#include "pixelart/PixelSnap.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;

const std::vector<std::string>& editor::pixelart::particleCheckIds(){
    static const std::vector<std::string> ids = {"off_grid", "off_palette", "partial_alpha", "spill", "one_shot_end", "loop_seam",
                                                 "determinism", "over_budget"};
    return ids;
}

lint::LintReport editor::pixelart::particleCaptureChecks(const ParticleCapture& capture, uint32_t budget){
    lint::LintReport report;
    // a pixel effect: one of its emitters (the effect's, or a sub-emitter's) draws on the grid
    bool pixelEffect = false;
    for (const CapturedFrame& frame : capture.frames()){
        for (const CapturedParticle& p : frame.particles) pixelEffect = pixelEffect || p.pixel;
    }

    // off_grid: each particle's pixel box where the vertex stage puts it, against the grid
    lint::LintResult grid;
    grid.id = "off_grid";
    grid.thresholdSource = "default";
    if (!pixelEffect){
        grid.status = lint::LintStatus::Skip;
        grid.message = "not a pixel effect (no Pixel Art module, Render Mode not Pixel)";
    }else{
        // measured on what the GPU drew: base pixels whose screen pixels are not one colour
        // (ParticleCapture draws every base pixel kProbeScale times each way)
        uint32_t uneven = 0, drawn = 0;
        for (size_t f = 0; f < capture.frames().size(); f++){
            const CapturedFrame& frame = capture.frames()[f];
            uneven += frame.offGrid;
            for (const auto& at : frame.offGridAt) grid.addSpot({(int)f + 1, at.first, at.second});
            for (int y = 0; y < frame.image.height(); y++)
                for (int x = 0; x < frame.image.width(); x++)
                    drawn += frame.image.at(x, y)[3] > 0 ? 1 : 0;
        }
        grid.count = (int)uneven;
        grid.measured = uneven;
        grid.threshold = 0;
        grid.status = uneven > 0 ? lint::LintStatus::Fail : lint::LintStatus::Pass;
        if (uneven > 0){
            grid.message = std::to_string(uneven) + " of " + std::to_string(drawn + uneven) + " drawn base pixels sit between grid lines";
            grid.fix = "turn on the Pixel Art module's Snap to Pixel Grid";
        }
    }
    report.add(grid);

    // S48's lints: one implementation, theirs (S48 R28, D13)
    for (const char* id : {"off_palette", "partial_alpha", "spill", "one_shot_end", "loop_seam", "determinism"}){
        lint::LintResult skip;
        skip.id = id;
        skip.status = lint::LintStatus::Skip;
        skip.thresholdSource = "default";
        skip.message = "needs S48 (Pixel FX): its EffectLints compute " + std::string(id);
        report.add(skip);
    }

    // over_budget: the peak against the effect's share of the world budget
    lint::LintResult over;
    over.id = "over_budget";
    over.thresholdSource = "particles.budget / 16";
    uint32_t peak = 0;
    int peakFrame = 0;
    for (size_t f = 0; f < capture.frames().size(); f++){
        if (capture.frames()[f].count > peak){
            peak = capture.frames()[f].count;
            peakFrame = (int)f + 1;
        }
    }
    const double share = (double)std::max(budget, 16u) / 16.0;
    over.measured = peak;
    over.threshold = share;
    over.count = peak > share ? 1 : 0;
    if (over.count) over.at.push_back({peakFrame, 0, 0});
    over.status = peak > budget ? lint::LintStatus::Fail : (peak > share ? lint::LintStatus::Warn : lint::LintStatus::Pass);
    if (over.status != lint::LintStatus::Pass){
        over.message = std::to_string(peak) + " particles at frame " + std::to_string(peakFrame);
        over.fix = "lower the rate, the bursts or the lifetime";
    }
    report.add(over);
    return report;
}
