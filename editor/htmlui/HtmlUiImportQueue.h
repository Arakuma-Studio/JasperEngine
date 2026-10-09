// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "RemoteImporter.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::htmlui {

    // One remote URL to import on the worker
    struct ImportJob {
        ImportTarget target;
        std::string url;
        std::string kind;                    // RemoteReference::kind hint
        ImportLocation location;
        std::vector<std::string> documents;  // asset-relative documents that reference it (reloaded after the batch)
    };

    struct ImportStatus {
        enum class State { Queued, Importing, Imported, Failed };
        State state = State::Queued;
        std::string error;                   // Failed
    };

    // The auto-import queue (phase-2 §3.13.4): one background worker imports one URL at a
    // time; a URL is queued once (de-duplicated by its normalized form while queued, running or
    // already imported in this session); a URL that failed is not retried automatically in the
    // same session (a manual request, the Inspector's Import, retries it). When the queue runs
    // empty the finished jobs form a batch the main thread takes to refresh the assets and
    // reload the documents. Free of ImGui and editor state (tested on its own).
    class ImportQueue {
    public:
        // runs on the worker thread; the default runs a RemoteImporter for the job's target
        using Runner = std::function<ImportedResource(const ImportJob& job)>;

        struct Batch {
            std::vector<ImportedResource> results;
            std::vector<std::string> documents;   // unique, asset-relative
        };

        explicit ImportQueue(Runner runner = {});
        ~ImportQueue();
        ImportQueue(const ImportQueue&) = delete;
        ImportQueue& operator=(const ImportQueue&) = delete;

        // false when the URL is already queued or running; automatic requests also skip URLs
        // imported or failed in this session. A request for a queued URL adds its documents.
        bool enqueue(ImportJob job, bool manual);
        std::optional<ImportStatus> status(const std::string& url) const;
        bool busy() const;                    // a job queued or running
        size_t pending() const;               // queued + running

        // the finished batch, once the queue went idle (main thread)
        bool takeFinishedBatch(Batch& batch);
        // blocks until the queue is idle (tests, the CLI)
        void waitIdle();
        // forgets the session's imported/failed URLs (a project change)
        void clearSession();
        // drops the queued jobs, waits for the running one, stops the worker; later enqueues fail
        void stop();

        static ImportedResource runImport(const ImportJob& job);

    private:
        void work();

        Runner runner;
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable idle;
        std::deque<ImportJob> queue;
        std::map<std::string, ImportStatus> states;   // by normalized URL
        std::string running;
        Batch finished;
        bool batchReady = false;
        bool stopping = false;
        std::thread worker;
    };

}
