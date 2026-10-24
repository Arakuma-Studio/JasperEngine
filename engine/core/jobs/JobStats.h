// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOBS_JOBSTATS_H
#define JOBS_JOBSTATS_H

#include "Export.h"
#include "jobs/JobTypes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine { namespace jobs {

    struct LaneStats {
        uint32_t queued = 0;
        uint32_t running = 0;
    };

    struct GateStats {
        int max = 0;
        int taken = 0;
        int heavyMax = 0;
        int heavyTaken = 0;
        int reserve = 0;
        // the most slots taken at once (all, Heavy) over the stats window
        int peakTaken = 0;
        int peakHeavyTaken = 0;
    };

    struct ChannelStats {
        const char* name = "";
        uint32_t queued = 0;
        uint32_t running = 0;
        uint32_t cap = 0;
    };

    struct WorkerStats {
        // Share of the window spent running jobs (0-100).
        float busyPct = 0.0f;
        // Jobs this worker ran over the window.
        uint64_t jobs = 0;
    };

    struct TotalStats {
        uint64_t executed = 0;
        uint64_t stolen = 0;
        uint64_t cancelled = 0;
        uint64_t failed = 0;
        // Envelopes too large for a slab, which took a heap allocation.
        uint64_t heapEnvelopes = 0;
    };

    // A snapshot of the scheduler (the get_job_system_stats payload).
    struct Stats {
        int workers = 0;
        bool singleThread = false;
        LaneStats lanes[kLaneCount];
        GateStats gate;
        std::vector<ChannelStats> channels;
        std::vector<WorkerStats> perWorker;
        TotalStats totals;
        uint32_t blockingThreads = 0;
        // Jobs tagged Tag::Asset that are queued or running.
        uint32_t assetJobs = 0;
        // Frames the per-worker figures cover (0 before the first sampleFrame: since start).
        int windowFrames = 0;
    };

    // What a profiler wants to hear from the scheduler (S15). With no hooks installed every
    // hook site costs one relaxed load and a branch. Names are static strings.
    enum class WaitKind : uint8_t {
        Task,
        Counter,
        ParallelFor,
        Strand,
        Channel,
    };

    struct ProfilerHooks {
        // On a thread the scheduler owns, before its first job once the hooks are installed.
        void (*onThreadStart)(const char* threadName) = nullptr;
        void (*onJobBegin)(const char* jobName, Lane lane, int workerId) = nullptr;
        void (*onJobEnd)() = nullptr;
        void (*onWaitBegin)(WaitKind kind) = nullptr;
        void (*onWaitEnd)() = nullptr;
        void (*onIdleBegin)() = nullptr;
        void (*onIdleEnd)() = nullptr;
    };

    // Installs `hooks` (copied) for every scheduler; all-null hooks remove them.
    JASPERENGINE_API void setProfilerHooks(const ProfilerHooks& hooks);

    enum class ThreadKind : uint8_t {
        Main,
        Worker,
        Blocking,
        IO,
        Other,
    };

    // What one thread of the job system is doing (S28's hang report).
    struct ThreadOccupancy {
        const char* name = "";
        ThreadKind kind = ThreadKind::Other;
        // The job running on it (SubmitOptions::name), null when idle.
        const char* runningJob = nullptr;
        // Since when that job has run, as first observed by snapshotOccupancy (steady clock,
        // ns): accurate to the caller's sampling period, at no cost to the jobs.
        uint64_t sinceNs = 0;
    };

    // Copies up to `capacity` threads into `out` and returns how many; lock-free and
    // allocation-free, for one sampling thread (the watchdog).
    JASPERENGINE_API size_t snapshotOccupancy(ThreadOccupancy* out, size_t capacity);

}}

#endif // JOBS_JOBSTATS_H
