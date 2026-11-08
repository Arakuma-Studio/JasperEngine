// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_WORK_QUEUE_H
#define VOXEL_WORK_QUEUE_H

// R15: all meshing and shape building goes through this queue, which feeds jobs::submit on the Background
// lane. Lanes are fixed, so the order (edit work, then near, then far, re-sorted when the focus moves) is kept
// here. Results are polled, never posted, so they arrive whenever the system updates and nothing
// makes a thread of its own (PTHREAD_POOL_SIZE_STRICT).

#include "Export.h"
#include "jobs/JobSystem.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace jasperengine::voxel {

    enum class WorkPriority : uint8_t { Edit = 0, Near = 1, Far = 2 };

    class JASPERENGINE_API WorkQueue {
    public:
        struct Item {
            uint64_t id = 0;
            WorkPriority priority = WorkPriority::Far;
            double distance = 0.0;                // to the focus, voxels; lower runs first
            std::function<void()> work;           // runs on a job; keeps its own result
        };

        // null: the process instance. No job system at all (tests) runs work inline at pump().
        explicit WorkQueue(jobs::JobSystem* system = nullptr);

        // Runs everything that was in flight to the end, then drops what is queued
        ~WorkQueue();

        void push(uint64_t id, WorkPriority priority, double distance, std::function<void()> work);
        // Called on the worker as each job finishes, any queue's: its result waits for the next pump,
        // and a host whose loop sleeps while idle (the editor) wakes it here. Null: nothing.
        static void setFinishedNotifier(std::function<void()> notify);
        // True when it was still queued, or in flight and not started
        bool cancel(uint64_t id);

        // cancel(), and when the job already runs, waits for it: its closure's data may then go
        void cancelAndWait(uint64_t id);

        // Re-sorts when the focus moves: gives each queued item a new priority and distance
        using Rank = std::function<void(uint64_t id, WorkPriority& priority, double& distance)>;
        void reprioritize(const Rank& rank);

        // Once per frame, main thread: polls finished jobs and feeds new ones up to the limit
        void pump();

        // What finished since the last call. A cancelled or failed item is in `dropped`.
        void takeFinished(std::vector<uint64_t>& finished, std::vector<uint64_t>& dropped);

        size_t queued() const { return items.size(); }
        size_t inFlight() const { return running.size(); }
        double averageJobMs() const { return averageMs; }
        // The largest number of jobs in flight so far, for AC13
        size_t peakInFlight() const { return peak; }

        // Waits for everything queued and in flight, running it where it can (R20's blocking start)
        void finishAll();

        void setPumpBudgetMs(double ms) { pumpBudgetMs = ms; }

    private:
        struct Running {
            uint64_t id = 0;
            jobs::Task<bool> task;
        };

        int slotsToFill() const;
        size_t nextIndex() const;
        void pollRunning();
        void submitNext();

        jobs::JobSystem* requested;
        std::vector<Item> items;
        std::vector<Running> running;
        std::vector<uint64_t> finishedIds;
        std::vector<uint64_t> droppedIds;
        std::shared_ptr<std::atomic<float>> sharedAverage;
        double averageMs = 1.0;
        double pumpBudgetMs = 4.0;
        size_t peak = 0;
    };

}

#endif
