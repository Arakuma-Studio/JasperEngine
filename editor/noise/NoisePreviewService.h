// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Noise Graph window's live previews (T2 N10, R56): one Background job per graph revision. The window
// polls Task::isReady, because MainThreadQueue stops while the game is paused. A newer revision cancels the
// older through a CancelToken, and what an older revision still produces never reaches the caller. Nothing
// here touches the GPU: thumbnails come back as pixels, the window uploads them.

#include "graph/Graph.h"
#include "jobs/JobSystem.h"
#include "noise/NoiseTypes.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    struct NoisePreviewRequest {
        graph::Graph graph;
        uint64_t revision = 0;
        uint32_t seed = 1;
        int thumbnailSize = 64;
        int imageSize = 128;            // the Preview image, of the height output or the graph's first
        float terrainSize = 1000.0f;    // metres: what world space's preview spans
        float maxHeight = 100.0f;
        std::vector<std::pair<std::string, float>> variables;
    };

    // One node's first output, sampled at thumbnailSize
    struct NoiseNodePreview {
        std::string nodeId;
        std::string outputName;         // "node:n3:out", "height"
        std::vector<uint8_t> rgba;      // thumbnailSize squared; a coord output has x in red and z in green
        bool twoChannels = false;
        noise::Interval observed;
        noise::Interval analytic;
        double ms = 0.0;
    };

    struct NoisePreviewImage {
        int size = 0;
        std::vector<uint8_t> rgba;
        std::string outputName;
        float min = 0.0f, max = 0.0f, mean = 0.0f;
    };

    struct NoisePreviewResult {
        uint64_t revision = 0;
        bool ok = false;                // false: the graph has errors, or the run was cancelled
        int thumbnailSize = 0;
        std::vector<NoiseNodePreview> nodes;
        NoisePreviewImage image;
    };

    class NoisePreviewService {
    public:
        NoisePreviewService() = default;
        ~NoisePreviewService();
        NoisePreviewService(const NoisePreviewService&) = delete;
        NoisePreviewService& operator=(const NoisePreviewService&) = delete;

        // Starts the request's job; the one running, if any, is cancelled
        void request(NoisePreviewRequest request);
        // Nothing runs after this returns but what a cancelled job still has to unwind
        void cancel();
        // The newest request's result, once, when it is done. False while it runs, and for a cancelled
        // or failed job.
        bool poll(NoisePreviewResult& out);

        bool busy() const { return task.isValid() && !task.isReady(); }
        uint64_t requestedRevision() const { return currentRevision; }
        uint64_t jobsSubmitted() const { return submitted; }
        // Blocks until the newest request is done (a test's frame loop; the window only polls)
        void waitForCurrent() const { task.wait(); }

        // The job's body: compiles with CompileOptions::previewNodes and evaluates every output once. False
        // when cancelled or when the graph does not compile.
        static bool evaluate(const NoisePreviewRequest& request, jobs::CancelToken* cancel, NoisePreviewResult& out);

    private:
        jobs::Task<NoisePreviewResult> task;
        jobs::CancelToken currentCancel;
        uint64_t currentRevision = 0;
        uint64_t submitted = 0;
    };

}
