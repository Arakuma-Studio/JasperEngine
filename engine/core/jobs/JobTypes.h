// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOBTYPES_H
#define JOBTYPES_H

#include "Export.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine { namespace jobs {

    class CancelToken;

    // The three priorities of the scheduler. A worker dequeues High global work first, then
    // its own local deque, Normal global work, steals from other workers, and Background
    // last (admitted by the BackgroundGate).
    enum class Lane : uint8_t {
        High,       // work the frame waits on
        Normal,     // the default
        Background, // asset decode, LOD, cooks, editor tasks: never in the frame's way
    };

    constexpr size_t kLaneCount = 3;

    // The BackgroundGate class of a Background job. Heavy jobs (texture cooks, mesh
    // simplification, shader compiles, nav bakes) take at most maxBackground - 2 slots, so
    // the short decodes the frame waits on always find one.
    enum class GateClass : uint8_t {
        Default,
        Heavy,
    };

    // What a dependent does when a dependency ends Failed or Cancelled.
    enum class DepPolicy : uint8_t {
        RequireSuccess, // the dependent is cancelled, and so are its own dependents
        Always,         // the dependent runs once every dependency is terminal
    };

    enum class TaskStatus : uint8_t {
        Pending,
        Running,
        Completed,
        Failed,     // the job threw: get() rethrows the exception
        Cancelled,  // cancel() before it ran, a cancelled dependency, or shutdown
    };

    inline bool isTerminal(TaskStatus status){
        return status >= TaskStatus::Completed;
    }

    // Bits of SubmitOptions::tags.
    namespace Tag {
        // Asset loading work: counted by Engine::getQueuedResourceCount while queued or running.
        constexpr uint32_t Asset = 1u << 0;
    }

    struct SubmitOptions {
        Lane lane = Lane::Normal;
        GateClass gate = GateClass::Default;
        // Static string: profilers and the watchdog keep the pointer.
        const char* name = "job";
        uint32_t tags = 0;
        // Shared cancellation of a group of jobs; the job sees it through jobs::cancelRequested().
        CancelToken* cancel = nullptr;
        DepPolicy deps = DepPolicy::RequireSuccess;
    };

    // How a JobSystem is built. -1 everywhere selects the platform policy.
    struct Config {
        // Worker threads: -1 = thread::resolveWorkerCount (JE_JOB_WORKERS, else the platform
        // policy); 0 = single-thread mode (every job runs from pump() or a wait).
        int workers = -1;
        // Workers kept free of Background work: -1 = 2 on desktop, 1 on mobile and web.
        int reserveForFrame = -1;
        // The most threads the blocking channels may create: -1 = 8 on desktop, 1 on threaded
        // web, 0 in single-thread mode (channel jobs then run from pump()).
        int blockingBudget = -1;
        // Per-thread occupancy and per-worker job counts (Stats, snapshotOccupancy). Off only
        // to measure what they cost (je-bench-memory).
        bool collectStats = true;
        // A wait that would close a cycle (a task waiting on itself through other tasks)
        // aborts with a report instead of hanging. On by default in builds with assertions.
#ifdef NDEBUG
        bool detectWaitCycles = false;
#else
        bool detectWaitCycles = true;
#endif
    };

    JASPERENGINE_API const char* laneName(Lane lane);

}}

#endif // JOBTYPES_H
