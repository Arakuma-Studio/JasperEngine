// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Job estimates and reports, implemented in McpJobs.cpp (docs/specs/S99-agent-feedback-area-lights.md R13-R16). A long job
// (record_game, build_project, bake_impostors, export_title) gets an estimate when it is submitted:
// a cost model per kind, corrected by the median measured/model ratio of the kind's last 16
// completions in the project's .jasperengine/user/job-history.json (clamped to [0.25, 4]). On
// completion its result gets a report: the time measured against the estimate, and the downgrades
// (fallbacks) the job took. The history is per user: a corrupt or missing file starts empty and
// never fails a job.

#include "McpJson.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

    // A downgrade a job took: an encoder, a build method, a texture format, a clamped size
    struct JobFallback {
        std::string step;
        std::string from;
        std::string to;
        std::string reason;

        bool operator==(const JobFallback& other) const = default;
    };

    struct JobEstimate {
        double seconds = 0.0;
        uint64_t bytes = 0;             // 0: no estimate of the output's size
        std::string basis = "model";    // "model" | "history"
        std::string confidence = "low"; // "low" | "medium" | "high"
        double modelSeconds = 0.0;      // the model's own seconds (what the history's ratios divide)
        size_t samples = 0;             // the completions the history correction used

        // {seconds, bytes?, basis, confidence}
        Json toJson() const;
    };

    // The cost models (seconds of a typical machine; the history corrects them per project)
    namespace jobmodel {
        // frames at width x height: one game frame per editor frame, read back and encoded (PNG on
        // workers, H.264 / ProRes through FFmpeg, then the decode check)
        double recordSeconds(int64_t frames, int width, int height, bool movie);
        // a build: the files and bytes it cooks and copies, plus the compile when it compiles
        enum class BuildKind { Prebuilt, CompiledDesktop, CompiledWeb, Source };
        double buildSeconds(BuildKind kind, uint64_t files, uint64_t bytes);
        // impostors x views x 2 passes, one view a frame, plus the two atlases' PNGs
        double impostorSeconds(int impostors, int views, int resolution);
        // layers x canvas pixels per output
        double titleExportSeconds(size_t layers, int canvasWidth, int canvasHeight, size_t outputs);
    }

    // Seconds left of a job estimated at `estimated` after `elapsed`: from the estimate early, from
    // the progress (when known, in [0,1]) late; never negative
    double jobRemainingSeconds(double estimated, double elapsed, double progress);
    // "12 s", "3 min", "1 h 05 min"
    std::string formatJobDuration(double seconds);

    class JobHistory {
    public:
        static constexpr size_t kKeep = 16;
        static constexpr double kMinRatio = 0.25;
        static constexpr double kMaxRatio = 4.0;

        struct Sample {
            double seconds = 0.0;       // measured
            double model = 0.0;         // the model's seconds for the same work
        };

        // the file's text: anything unreadable (not JSON, wrong shapes, bad numbers) is dropped,
        // an unreadable file reads as empty
        static JobHistory parse(const std::string& text);
        std::string serialize() const;

        // keeps the kind's last kKeep; a non-positive or non-finite measure is ignored
        void add(const std::string& kind, double seconds, double model);
        const std::vector<Sample>& samples(const std::string& kind) const;
        // the median measured/model ratio clamped to [kMinRatio, kMaxRatio]; false when none
        bool ratio(const std::string& kind, double& out) const;
        // the model's estimate corrected by the history
        JobEstimate estimate(const std::string& kind, double modelSeconds, uint64_t bytes) const;

        // <project>/.jasperengine/user/job-history.json
        static std::filesystem::path fileFor(const std::filesystem::path& projectPath);
        // missing or corrupt: empty
        static JobHistory load(const std::filesystem::path& file);
        // load, add, write through AtomicFile (the editor's own file: util/SafeWrite.h); thread-safe.
        // False when the write failed (the job is not affected)
        static bool record(const std::filesystem::path& file, const std::string& kind, double seconds, double model);

    private:
        std::map<std::string, std::vector<Sample>> kinds;
    };

}
