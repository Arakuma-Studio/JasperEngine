// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The one path of terrain generation (T2 R28): validate, snapshot what the graph reads, evaluate on a
// job, apply on the main thread through T1's terrain::Batch as one undo step. The Generate section,
// the Create Terrain dialog and generate_terrain all call this. Nothing here writes a map file: T1's
// store and command own persistence and refresh.

#include "ecs/Entity.h"
#include "graph/Graph.h"
#include "jobs/JobSystem.h"
#include "noise/NoiseGraphRuntime.h"
#include "terrain/TerrainMapStore.h"
#include "terrain/TerrainOps.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Which outputs a run writes (R60)
    struct GenerationApplyTo {
        bool height = true;
        bool layers = true;
        bool density = true;
    };

    struct GenerationRegion {
        float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;   // terrain-local metres
    };

    struct GenerationRequest {
        graph::Graph graph;
        std::string displayName;            // "Mountains": the undo name's
        uint32_t seed = 1;
        std::vector<std::pair<std::string, float>> variables;
        std::optional<GenerationRegion> region;
        float feather = 0.0f;               // metres
        int resolution = 0;                 // 0: the terrain's
        GenerationApplyTo applyTo;
        bool dryRun = false;
        // A preview a person or an agent waits on (the Generate section's Preview): a dry run on Lane::Normal,
        // ahead of Background's Heavy compiles and cooks, at most kInteractiveResolution per side (Deviations)
        bool interactive = false;
        // The terrain as the store cannot tell it; start() fills these from the component
        float terrainSize = 1000.0f;        // metres
        int layerCount = -1;                // surface layers; -1: no check
    };

    // Per-output stats of a generation (R63)
    struct GenerationOutputStats {
        std::string name;
        uint64_t hash = 0;
        float coverage = 0.0f;              // layers, density: the mean weight
    };

    struct GenerationStats {
        float heightMin = 0.0f, heightMax = 0.0f, heightMean = 0.0f;
        float p5 = 0.0f, p50 = 0.0f, p95 = 0.0f;
        std::vector<int64_t> slopeHistogram;    // 18 bins of 5 degrees
        int64_t clippedLow = 0, clippedHigh = 0;
    };

    struct GenerationIssue {
        std::string code;
        std::string message;
        std::string nodeId;
        bool warning = false;
    };

    struct GenerationResult {
        bool ok = false;
        bool cancelled = false;
        std::string error;                  // the first error
        std::string errorCode;
        std::vector<GenerationIssue> issues;
        std::vector<GenerationIssue> warnings;
        std::vector<GenerationOutputStats> outputs;
        GenerationStats stats;
        std::vector<terrain::MapChangeReport> changed;
        std::string undoName;
        // the plan
        size_t ops = 0;
        std::vector<std::string> deadNodes, foldedNodes;
        size_t peakBytes = 0;
        // timings
        double evaluateMs = 0.0, erosionMs = 0.0, applyMs = 0.0;
        uint64_t dropletSteps = 0;
        bool dryRun = false;
        // Normalized heights of what ran, width * depth, for images
        std::vector<float> heights;
        int width = 0, depth = 0;
        terrain::TexelRect rect;
    };

    // The largest side an interactive preview evaluates, whatever the terrain's resolution
    inline constexpr int kInteractiveResolution = 513;

    // Pure generation over any store (AC20 uses a fake one): no project, no scene, no job. The editor's
    // start() below is this plus a job and a command.
    class TerrainGeneration {
    public:
        struct Prepared;

        // R32: everything that can be refused is refused here, before any evaluation. Snapshots what
        // the graph reads. Main thread. Null with result.ok false on a refusal.
        static std::shared_ptr<Prepared> prepare(const terrain::TerrainMapStore& store, const GenerationRequest& request,
                                                 GenerationResult& result);
        // Runs anywhere, with no store: a job's body. False when cancelled.
        static bool evaluate(Prepared& prepared, jobs::CancelToken* cancel, bool parallel, GenerationResult& result);
        // Whether the terrain still is what prepare snapshotted (R33)
        static bool stillMatches(const Prepared& prepared, const terrain::TerrainMapStore& store);
        // Writes through one Batch (R29) and fills changed. Main thread. Skips writing on a dry run.
        static bool apply(Prepared& prepared, terrain::TerrainMapStore& store, GenerationResult& result);
        // The layers and foliage layers the graph writes, for what must exist before apply
        static std::vector<int> layersOf(const Prepared& prepared);
        static std::vector<int> densitiesOf(const Prepared& prepared);
        // The resolution prepare settled on
        static terrain::TexelRect heightRectOf(const Prepared& prepared);
        static bool replacesHeightMap(const Prepared& prepared);
        // R29's "Generate Terrain (Mountains, seed 7)"
        static std::string undoNameOf(const GenerationRequest& request);
        // The name for a graph's undo step and reports: its preset's name, else the file's stem
        static std::string displayNameOf(const graph::Graph& graph, const std::string& path);

        // A running generation on an entity's terrain, as a job
        class Run {
        public:
            virtual ~Run() = default;
            virtual void cancel() = 0;
            virtual bool finished() const = 0;
            virtual float progress() const = 0;
        };
        using Done = std::function<void(const GenerationResult&)>;

        // R28: on Lane::Background, GateClass::Heavy; done runs on the main thread, always, once.
        // Null with done called at once when the request is refused. Refused in Play (R33).
        static std::shared_ptr<Run> start(Project* project, uint32_t sceneId, Entity entity, GenerationRequest request, Done done);
    };

}
