// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleEffectChecks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine::editor;

namespace {

    using Json = qa::Json;

    double round3(double value){
        return std::round(value * 1000.0) / 1000.0;
    }

    double round2(double value){
        return std::round(value * 100.0) / 100.0;
    }

    std::string fixed(double value, int digits){
        char text[48];
        std::snprintf(text, sizeof(text), "%.*f", digits, value);
        return text;
    }

    // Costs per particle and per emitter from docs/perf/particles.md: server E's single-thread rows
    // (2026-11-01) for desktop, its simd128 web rows, and for mobile, which has no measured row,
    // twice desktop's (the ratio of S20 R21's per-particle budgets, mobile 0.8 ms / 10 k against
    // desktop 4.0 ms / 100 k on one thread)
    struct BenchRow {
        const char* platform;
        double noiseUs;      // a tick of the reference stack, per particle
        double plainUs;      // the same without Noise
        double prepUs;       // render prep, per particle
        double emitterSimUs; // per emitter (many_small_1000x100)
        double emitterPrepUs;
        const char* basis;
    };

    const BenchRow& benchRow(const std::string& platform){
        static const BenchRow rows[] = {
            {"desktop", 0.0350, 0.0081, 0.0135, 1.6, 2.9, "docs/perf/particles.md, server E, one thread (2026-11-01)"},
            {"web", 0.0753, 0.0143, 0.0290, 3.2, 5.8, "docs/perf/particles.md, web simd128 rows; per-emitter costs assumed twice desktop's"},
            {"mobile", 0.0700, 0.0162, 0.0270, 3.2, 5.8, "assumed: twice desktop's one-thread rows (S20 R21's budget ratio); no mobile row is measured"},
        };
        for (const BenchRow& row : rows){
            if (platform == row.platform) return row;
        }
        return rows[2];
    }

    float overdrawThreshold(const ParticleQaSettings& qa, const std::string& platform){
        if (platform == "desktop") return qa.overdrawDesktop;
        if (platform == "web") return qa.overdrawWeb;
        return qa.overdrawMobile;
    }

    float cpuThreshold(const ParticleQaSettings& qa, const std::string& platform){
        if (platform == "desktop") return qa.cpuMsDesktop;
        if (platform == "web") return qa.cpuMsWeb;
        return qa.cpuMsMobile;
    }

    Json capacityHit(const ParticleMeasurements& m){
        Json row = qa::row("capacity_hit", "content");
        row["method"] = "births of a second run with Max Particles lifted, minus this run's (same seed, same ticks)";
        row["threshold"] = 0;
        row["threshold_source"] = "the effect's Max Particles";
        row["measured"] = {{"never_born", m.neverBorn}, {"peak", m.peak}};
        if (m.neverBorn == 0){
            row["status"] = "PASS";
            return row;
        }
        row["status"] = "FAIL";
        row["measured"]["max_particles"] = m.capacityMax;
        row["limited_by"] = m.capacityEmitter + ": Max Particles " + std::to_string(m.capacityMax) + " refused " + std::to_string(m.neverBorn) +
                            " births; emission was silently limited.";
        row["measured"]["needed"] = m.capacityNeeded;
        row["fix_hint"] = "Raise " + m.capacityEmitter + "'s Max Particles to " + std::to_string(std::max(m.capacityNeeded, m.capacityMax + 1)) +
                          " (its peak with nothing refused), or emit fewer (lower the burst or the rate).";
        if (m.capacityAt >= 0.0) qa::addAt(row, m.capacityAt);
        return row;
    }

    Json overdrawBudget(const ParticleMeasurements& m, const ParticleCheckRequest& request){
        Json row = qa::row("overdraw_budget", "platform");
        const double warn = overdrawThreshold(request.qa, request.platform);
        row["method"] = "analytic: the particles' quads summed over a 64 x 64 grid of the frame, the peak cell's layers";
        row["threshold"] = {{"warn", warn}, {"fail", warn * 2.0}};
        row["threshold_source"] = "particles.qa.overdraw_" + request.platform;
        row["measured"] = {{"peak_layers", round2(m.peakLayers)}, {"avg_layers", round2(m.avgLayers)}};
        if (m.heat) row["measured"]["heat_peak_layers"] = round2(m.heatPeakLayers);
        row["status"] = m.peakLayers > warn * 2.0 ? "FAIL" : m.peakLayers > warn ? "WARN" : "PASS";
        if (row["status"] != "PASS"){
            row["limited_by"] = m.overdrawEmitter + " stacks " + fixed(m.peakLayers, 1) + " layers at its densest (" + request.platform + " warns over " +
                                fixed(warn, 0) + ").";
            row["fix_hint"] = "Fewer or smaller particles in " + m.overdrawEmitter + " (Max Particles, the emission, Start Size, Size over Lifetime).";
            qa::addAt(row, m.overdrawAt);
        }
        return row;
    }

    Json invisible(const ParticleMeasurements& m, const ParticleCheckRequest& request){
        Json row = qa::row("invisible", "content");
        const double t = request.qa.invisiblePercent;
        row["method"] = "analytic: grid cells the particles cover, weighted by their alpha, at every sampled tick; with cells, also their "
                        "pixels where the analytic cover says something shows";
        row["threshold"] = t;
        row["threshold_source"] = "particles.qa.invisible_percent";
        row["measured"] = {{"analytic", round3(m.analyticMaxCoverage)}};
        // the cells that should show something, and the best of what they show
        int shouldShow = 0;
        double shown = 0.0;
        for (const auto& [pixels, analytic] : m.cellCoverage){
            if (analytic < t) continue;
            shouldShow++;
            shown = std::max(shown, pixels);
        }
        if (request.hasImages) row["measured"]["cells"] = round3(m.maxCoverage);
        if (m.analyticMaxCoverage >= t && (!request.hasImages || shouldShow == 0 || shown >= t)){
            row["status"] = "PASS";
            return row;
        }
        row["status"] = "FAIL";
        if (m.analyticMaxCoverage >= t){
            row["limited_by"] = "The particles cover the view but the cells show nothing: the texture or the colour draws no pixel.";
            row["fix_hint"] = "Check the Renderer's texture (its alpha) and the Start Color.";
        }else if (m.peak == 0){
            row["limited_by"] = "No particle was alive during the run.";
            row["fix_hint"] = "Enable emission: a rate (rateOverTime) or a burst, and a Start Lifetime above 0.";
        }else if (m.meanAlpha < 0.02){
            row["limited_by"] = "The particles are transparent (mean alpha " + fixed(m.meanAlpha, 3) + ").";
            row["fix_hint"] = "Raise the alpha of Start Color (or of Color over Lifetime).";
        }else{
            row["limited_by"] = std::to_string(m.peak) + " particles lived but covered under " + fixed(request.qa.invisiblePercent, 2) + "% of the view.";
            row["fix_hint"] = "Raise Start Size, or check the texture and the Renderer.";
        }
        return row;
    }

    Json blownOut(const ParticleMeasurements& m, const ParticleCheckRequest& request){
        Json row = qa::row("blown_out", "content");
        row["threshold"] = request.qa.blownOutPercent;
        row["threshold_source"] = "particles.qa.blown_out_percent";
        if (!request.hasImages){
            row["method"] = "needs cells: frames 0 renders none";
            return row;
        }
        if (!m.additive){
            row["method"] = "no emitter draws additively: white is the colour drawn, not layers saturating";
            row["status"] = "PASS";
            return row;
        }
        row["method"] = "covered pixels of the cells on black whose red, green and blue are all 250 or more (additive effects)";
        row["measured"] = round2(m.clippedPercent);
        row["status"] = m.clippedPercent > request.qa.blownOutPercent ? "WARN" : "PASS";
        if (row["status"] == "WARN"){
            row["limited_by"] = fixed(m.clippedPercent, 0) + "% of the covered pixels are clipped to white: additive layers saturate.";
            row["fix_hint"] = "Darken the additive colours (Start Color, Color over Lifetime), or use fewer overlapping particles.";
            if (m.clippedAt >= 0.0) qa::addAt(row, m.clippedAt);
        }
        return row;
    }

    Json pop(const ParticleMeasurements& m, const ParticleCheckRequest& request){
        Json row = qa::row("pop", "content");
        row["method"] = "per tick and emitter of 20 particles or more: the share dying that tick with alpha 0.25 or more";
        row["threshold"] = request.qa.popPercent;
        row["threshold_source"] = "particles.qa.pop_percent";
        row["measured"] = round2(m.popPercent);
        row["status"] = m.popPercent > request.qa.popPercent ? "WARN" : "PASS";
        if (row["status"] == "WARN"){
            row["limited_by"] = fixed(m.popPercent, 0) + "% of " + m.popEmitter + "'s particles vanish in one tick while visible.";
            row["fix_hint"] = "Fade them out: Color over Lifetime alpha to 0 (or Size over Lifetime to 0) at the end of their life.";
            if (m.popAt >= 0.0) qa::addAt(row, m.popAt);
        }
        return row;
    }

    Json neverEnds(const ParticleMeasurements& m){
        Json row = qa::row("never_ends", "content");
        row["threshold"] = round3(m.seconds);
        row["threshold_source"] = "the run's seconds";
        if (m.looping){
            row["method"] = "skipped: a root system loops by design";
            return row;
        }
        if (m.runCut){
            row["method"] = "skipped: seconds or times set the run's length, not one play";
            return row;
        }
        row["method"] = "a one-shot run until no particle and no emission is left";
        row["measured"] = m.ended ? Json(round3(m.endedAt)) : Json(nullptr);
        row["status"] = m.stillAlive ? "WARN" : "PASS";
        if (m.stillAlive){
            row["limited_by"] = "The one-shot still has particles or emission after " + fixed(m.seconds, 2) + " s (a looping sub-emitter or a very long lifetime).";
            row["fix_hint"] = "Turn Looping off in its sub-emitters' effects, or shorten Start Lifetime.";
            qa::addAt(row, m.seconds);
        }
        return row;
    }

    Json platformBudget(const ParticleMeasurements& m, const ParticleCheckRequest& request, const Json& cost){
        Json row = qa::row("platform_budget", "platform");
        const double warn = cpuThreshold(request.qa, request.platform);
        const double ms = cost.value("sim_ms", 0.0) + cost.value("prep_ms", 0.0);
        row["method"] = "estimate from the bench table at the peak count, not measured on a device";
        row["threshold"] = {{"warn", warn}, {"fail", warn * 2.0}};
        row["threshold_source"] = "particles.qa.cpu_ms_" + request.platform;
        row["measured"] = {{"cpu_ms", round3(ms)}, {"estimate", true}};
        row["status"] = ms > warn * 2.0 ? "FAIL" : ms > warn ? "WARN" : "PASS";
        if (row["status"] != "PASS"){
            row["limited_by"] = std::to_string(m.peak) + " particles in " + std::to_string(m.emitters) + " emitter(s) are estimated at " + fixed(ms, 2) +
                                " ms a frame on " + request.platform + " (estimate, not measured on a device).";
            row["fix_hint"] = m.noise ? "Fewer particles, or the Noise module off (it is two thirds of a tick)." : "Fewer particles or emitters.";
            qa::addAt(row, m.peakAt);
        }
        return row;
    }

}

const std::vector<std::string>& ParticleEffectChecks::checkIds(){
    static const std::vector<std::string> ids = {"capacity_hit", "overdraw_budget", "invisible", "blown_out", "pop", "never_ends", "platform_budget"};
    return ids;
}

qa::Json ParticleEffectChecks::cost(const ParticleMeasurements& m, const std::string& platform){
    const BenchRow& row = benchRow(platform);
    // a frame at 60 fps runs ticksPerSecond / 60 ticks
    const double ticksPerFrame = std::max(m.ticksPerSecond, 1.0) / 60.0;
    const double particleUs = m.noise ? row.noiseUs : row.plainUs;
    const double simMs = ((double)m.peak * particleUs + (double)m.emitters * row.emitterSimUs) * ticksPerFrame / 1000.0;
    const double prepMs = ((double)m.peak * row.prepUs + (double)m.emitters * row.emitterPrepUs) / 1000.0;
    return {{"method", "estimate_from_bench_table"}, {"note", "estimate, not measured on a device"}, {"platform", row.platform},
            {"basis", row.basis}, {"sim_ms", round3(simMs)}, {"prep_ms", round3(prepMs)}, {"peak", m.peak}, {"emitters", m.emitters},
            {"noise", m.noise}, {"ticks_per_frame", round2(ticksPerFrame)}};
}

qa::Json ParticleEffectChecks::rows(const ParticleMeasurements& m, const ParticleCheckRequest& request){
    const Json estimate = cost(m, request.platform);
    Json out = Json::array();
    out.push_back(capacityHit(m));
    out.push_back(overdrawBudget(m, request));
    out.push_back(invisible(m, request));
    out.push_back(blownOut(m, request));
    out.push_back(pop(m, request));
    out.push_back(neverEnds(m));
    out.push_back(platformBudget(m, request, estimate));
    return out;
}

qa::Json ParticleEffectChecks::metrics(const ParticleMeasurements& m, const std::string& platform){
    Json curve = Json::array();
    for (const auto& [t, live] : m.curve) curve.push_back({round3(t), live});
    Json count = {{"peak", m.peak}, {"peak_at", round3(m.peakAt)}, {"curve", curve}, {"emitted", m.emitted}};
    if (m.ended) count["ended_at"] = round3(m.endedAt);
    Json bounds = Json(nullptr);
    if (m.hasBounds){
        bounds = {{"min", {round3(m.boundsMin[0]), round3(m.boundsMin[1]), round3(m.boundsMin[2])}},
                  {"max", {round3(m.boundsMax[0]), round3(m.boundsMax[1]), round3(m.boundsMax[2])}},
                  {"size", {round3(m.boundsMax[0] - m.boundsMin[0]), round3(m.boundsMax[1] - m.boundsMin[1]), round3(m.boundsMax[2] - m.boundsMin[2])}},
                  {"largest_size", round3(m.largestSize)}, {"largest_at", round3(m.largestAt)}};
    }
    Json overdraw = {{"method", "cpu_analytic"}, {"avg_layers", round2(m.avgLayers)}, {"peak_layers", round2(m.peakLayers)},
                     {"peak_at", round3(m.overdrawAt)}, {"fill_mpix", round3(m.fillMpix)}};
    if (m.heat){
        overdraw["heat"] = {{"method", "gpu_additive_heat_row"}, {"avg_layers", round2(m.heatAvgLayers)}, {"peak_layers", round2(m.heatPeakLayers)}};
    }
    // at most 32 samples of the coverage
    Json coverage = Json::array();
    const size_t samples = std::min<size_t>(32, m.coverage.size());
    for (size_t s = 0; s < samples; s++){
        const size_t k = samples > 1 ? (size_t)std::llround((double)s * (double)(m.coverage.size() - 1) / (double)(samples - 1)) : 0;
        coverage.push_back({round3(m.coverage[k].first), round3(m.coverage[k].second)});
    }
    Json visibility = {{"method", m.visibilityFromImages ? "cells" : "analytic"}, {"coverage", coverage}, {"max_coverage", round3(m.maxCoverage)},
                       {"analytic_max_coverage", round3(m.analyticMaxCoverage)}, {"mean_alpha", round3(m.meanAlpha)}};
    if (m.visibilityFromImages) visibility["clipped_percent"] = round2(m.clippedPercent);
    Json temporal = {{"coverage_jump", round3(m.coverageJump)}, {"coverage_jump_at", round3(m.coverageJumpAt)},
                     {"pop_percent", round2(m.popPercent)}};
    if (m.looping && m.loopSeam >= 0.0) temporal["loop_seam"] = round3(m.loopSeam);
    // the private scene simulates the effect alone
    Json sceneContext = {{"collision_world", "skipped"}, {"force_fields", "skipped"}, {"world_budget", "not_applied"}, {"triggers", "none_sent"}};
    return {{"count", count}, {"bounds", bounds}, {"overdraw", overdraw}, {"visibility", visibility}, {"temporal", temporal},
            {"cost", cost(m, platform)}, {"scene_context", sceneContext}};
}

qa::Json ParticleEffectChecks::thresholds(const ParticleQaSettings& settings){
    const ParticleQaSettings defaults;
    qa::ThresholdAudit audit;
    for (const ParticleQaField& field : particleQaFields()){
        if (field.real) audit.add(field.key, settings.*field.real, defaults.*field.real);
        else audit.add(field.key, settings.*field.whole, defaults.*field.whole);
    }
    return audit.json();
}
