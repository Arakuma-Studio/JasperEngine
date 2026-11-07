// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLEEFFECTCHECKS_H
#define PARTICLEEFFECTCHECKS_H

#include "particles/ParticleQaSettings.h"
#include "util/QaChecks.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    // What one deterministic run of an effect measured (ParticleEffectPreview fills it), and the
    // checks of docs/specs/S91-effect-authoring.md R27-R29 made from it. The checks are pure: the
    // same measurements and thresholds give the same rows (unit tests feed made-up ones).
    struct ParticleMeasurements {
        double seconds = 0.0;
        double tickSeconds = 0.0;      // the shortest tick of the run's emitters
        uint64_t ticks = 0;
        bool looping = false;          // a root system loops
        bool runCut = false;           // seconds or times set the run's length (not one play)
        bool additive = false;         // an emitter of the tree draws additively
        uint32_t emitters = 0;         // the systems and their sub-emitter children
        bool noise = false;            // an emitter runs the Noise module (the bench's dearer row)
        double ticksPerSecond = 30.0;  // the fastest emitter's

        // count
        uint32_t peak = 0;
        double peakAt = 0.0;
        std::vector<std::pair<double, uint32_t>> curve;
        uint64_t emitted = 0;
        bool ended = false;            // no particle and no emission left before the end
        double endedAt = 0.0;
        bool stillAlive = false;       // one-shot roots still had particles or emission at the end

        // capacity: births Max Particles refused (a run with it lifted counts them)
        uint64_t neverBorn = 0;
        double capacityAt = -1.0;
        std::string capacityEmitter;
        uint32_t capacityMax = 0;
        uint32_t capacityNeeded = 0;   // that emitter's peak live count with Max Particles lifted

        // bounds (world, the union of the run)
        bool hasBounds = false;
        float boundsMin[3] = {0, 0, 0};
        float boundsMax[3] = {0, 0, 0};
        float largestSize = 0.0f;
        double largestAt = 0.0;

        // overdraw: the analytic layers of the particles' quads over a 64 x 64 grid of the frame
        double avgLayers = 0.0;
        double peakLayers = 0.0;
        double overdrawAt = 0.0;
        double fillMpix = 0.0;         // at 1080p with the effect 25% of the screen's height
        std::string overdrawEmitter;
        // the GPU heat row's layers on the same grid (overdraw_map)
        bool heat = false;
        double heatAvgLayers = 0.0;
        double heatPeakLayers = 0.0;

        // visibility: the cells' pixels when there are cells, and always the analytic cover of every
        // sampled tick (the quads weighted by their alpha), so cells that miss the live part of a run
        // say nothing about whether it is visible
        bool visibilityFromImages = false;
        std::vector<std::pair<double, double>> coverage;   // (t, % of the view covered): cells, else analytic
        double maxCoverage = 0.0;                          // the cells' best (analytic without cells)
        double analyticMaxCoverage = 0.0;
        // per cell: (% covered in its pixels, % the analytic cover gives at its tick)
        std::vector<std::pair<double, double>> cellCoverage;
        double clippedPercent = 0.0;   // of the covered pixels, clipped to white (cells only)
        double clippedAt = -1.0;
        double meanAlpha = 0.0;

        // temporal
        double coverageJump = 0.0;     // the largest change of covered % between two ticks
        double coverageJumpAt = 0.0;
        double loopSeam = -1.0;        // covered % change across the first cycle wrap (looping)

        // pop: the share of live particles dying in one tick while still visible
        double popPercent = 0.0;
        double popAt = -1.0;
        std::string popEmitter;
    };

    struct ParticleCheckRequest {
        std::string platform = "mobile";   // mobile, web, desktop
        ParticleQaSettings qa;
        bool hasImages = false;
    };

    class ParticleEffectChecks {
    public:
        static const std::vector<std::string>& checkIds();

        // cost{method, platform, sim_ms, prep_ms, ...}: an estimate from docs/perf/particles.md's
        // bench rows, never a measurement on the machine running it
        static qa::Json cost(const ParticleMeasurements& m, const std::string& platform);
        // the rows, in checkIds() order (rounds not counted yet)
        static qa::Json rows(const ParticleMeasurements& m, const ParticleCheckRequest& request);
        // metrics{count, bounds, overdraw, visibility, temporal, cost, scene_context}
        static qa::Json metrics(const ParticleMeasurements& m, const std::string& platform);
        // thresholds{values, changed}
        static qa::Json thresholds(const ParticleQaSettings& qa);

        static constexpr const char* CLOSING =
            "Report these numbers; cost is an estimate, not measured on a device, and never says an effect runs on a phone.";
    };

}

#endif //PARTICLEEFFECTCHECKS_H
