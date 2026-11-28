// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one scheduler of FBX imports (spec S98 D10): an .fbx converted into the .glb the engine loads
// in its place. Any thread queues a file; one Background job (Heavy, S06) converts the queue a file
// at a time, and a thread that may wait (Play startup, an export, a reload job) converts a file or
// the whole queue itself. The main thread only queues and asks, so no tool call waits for a
// conversion (REQ-32) and a model whose import is queued loads once it lands (REQ-33).

#pragma once

#include "jobs/Task.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace jasperengine::editor {

    class FbxImportQueue {
    public:
        // What a file needs, decided on the converting thread (it hashes the FBX)
        enum class Need { None, Create, Update };

        struct Hooks {
            // Need::None when its .glb is current
            std::function<Need(const std::filesystem::path& fbx)> need;
            // converts it; true when its .glb is current afterwards
            std::function<bool(const std::filesystem::path& fbx, bool force)> convert;
            // after each conversion and when the queue empties, outside the lock (wakes the main thread)
            std::function<void()> changed;
        };

        // A batch runs from the first file queued to the queue's end; its counts stay until the next
        struct Progress {
            uint64_t batch = 0;
            size_t done = 0;    // conversions of the batch that ended
            size_t total = 0;   // conversions of the batch (files already current are not counted)
            size_t failed = 0;
            bool active = false; // a file is queued or converting
        };

        explicit FbxImportQueue(Hooks hooks);
        ~FbxImportQueue();
        FbxImportQueue(const FbxImportQueue&) = delete;
        FbxImportQueue& operator=(const FbxImportQueue&) = delete;

        // Any thread. A file whose last conversion failed is skipped until it changes, unless forced.
        void request(const std::filesystem::path& fbx, bool force = false);
        // Queued or converting
        bool isPending(const std::filesystem::path& fbx) const;
        // Its last conversion failed and the FBX has not changed since
        bool hasFailed(const std::filesystem::path& fbx) const;
        // Submits the job when a file is queued, none runs and nothing was queued since the last
        // call. The main thread calls it once a frame, so the requests of a scene's load, which
        // can take a few frames, make one batch.
        void start();
        // Converts the file on this thread, once a conversion of it running elsewhere ends; false
        // when it failed. Never on the main thread.
        bool convertNow(const std::filesystem::path& fbx, bool force = false);
        // Converts what is queued on this thread, then waits for the conversions running
        // elsewhere. Never on the main thread.
        void drain();
        Progress progress() const;
        // Waits for the job (the owner's teardown)
        void wait();

        // Test hook (JASPER_TEST_FBX_IMPORT_DELAY_MS): slept before each conversion
        static void setDelayForTest(int milliseconds);

    private:
        struct Stamp {
            uintmax_t size = 0;
            std::filesystem::file_time_type time;
        };
        using Item = std::pair<std::filesystem::path, bool>; // the file, forced

        static bool stampOf(const std::filesystem::path& fbx, Stamp& stamp);
        // the files are in `running`; true when none failed
        bool process(const std::vector<Item>& items);
        void finish(const std::filesystem::path& fbx, bool converted, bool ok);
        void enter(); // a file joins the queue or the conversions (lock held)
        std::vector<Item> takeQueued(); // lock held
        void runJob();

        Hooks hooks;
        mutable std::mutex mutex;
        std::condition_variable ended; // a conversion ended
        std::map<std::filesystem::path, bool> queued;
        std::set<std::filesystem::path> running;
        std::map<std::filesystem::path, Stamp> failures;
        Progress counts;
        bool announced = false;
        uint64_t requests = 0;      // files queued so far
        uint64_t requestsSeen = 0;  // ... at the last start()
        bool jobRunning = false;
        jobs::Task<void> job;
    };

}
