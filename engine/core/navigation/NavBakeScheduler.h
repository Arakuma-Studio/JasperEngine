// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Tile jobs that own their input snapshot follow OpenEngine's NavigationMeshBake.cpp
// (Engine/Modules/PathfindingECS/Source/NavigationMeshBake.cpp), Copyright (c) 2026 Gamecrafters
// Guild Limited and contributors, MIT License; here results are kept by tile index (determinism),
// cancellable and time-sliced on single-threaded web. See NOTICE.md.

#ifndef NAV_BAKE_SCHEDULER_H
#define NAV_BAKE_SCHEDULER_H

// The tiles of one bake (spec R4, R23, D6): one S06 job per tile (the Background lane and the
// Heavy gate in the editor, the Normal lane at runtime), or, without worker threads (web), the
// tiles' stages run on the main thread within a per-frame budget. Results land by tile index, so
// the order jobs finish in never changes what is assembled; progress and cancel are lock-free.

#include "Export.h"
#include "jobs/JobTypes.h"
#include "jobs/Task.h"
#include "navigation/NavMeshBuilder.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine::jobs {
    class JobSystem;
}

namespace jasperengine::nav {

    struct NavBakeOptions {
        jobs::Lane lane = jobs::Lane::Normal;
        // Jobs: one job per tile; TimeSliced: stages on the caller's thread, run by pump(); Auto
        // (the default): time-sliced exactly when the job system has no workers
        enum class Mode : uint8_t { Auto, Jobs, TimeSliced };
        Mode mode = Mode::Auto;
        // null: the process job system
        jobs::JobSystem* system = nullptr;
    };

    class JASPERENGINE_API NavBake {
    public:
        // Builds `tiles` (tile indices of the input) of `input`; jobs start at once
        NavBake(std::shared_ptr<const NavBakeInput> input, std::vector<uint32_t> tiles, const NavBakeOptions& options = {});
        // Cancels what has not run; running jobs finish on their own copy of the state
        ~NavBake();
        NavBake(const NavBake&) = delete;
        NavBake& operator=(const NavBake&) = delete;

        // Time-sliced bakes: runs stages until budgetMs is spent (at least one stage). A no-op
        // for job bakes.
        void pump(double budgetMs);
        bool isTimeSliced() const { return timeSliced; }

        bool isDone() const;
        // Waits for the tile jobs (helping run them); a time-sliced bake must be pumped instead
        void wait();
        int getTilesDone() const;
        int getTilesTotal() const;
        float getProgress() const;

        // Stops the bake: no tile starts any more, isDone() becomes true once running tiles end
        void cancel();
        bool isCancelled() const;

        // Once done (and not cancelled): every requested tile's result, ascending tile index
        std::vector<NavTileBuildResult>& getResults();
        const std::shared_ptr<const NavBakeInput>& getInput() const { return input; }
        double getElapsedMs() const;

    private:
        struct Shared;
        std::shared_ptr<const NavBakeInput> input;
        std::shared_ptr<Shared> shared;
        bool timeSliced = false;
        // jobs: one task per tile
        std::vector<jobs::TaskRef> tasks;
        // time-sliced: the tiles still to build, the job in progress
        std::vector<uint32_t> queue;
        size_t next = 0;
        std::unique_ptr<NavMeshBuilder::TileJob> active;
    };

}

#endif // NAV_BAKE_SCHEDULER_H
