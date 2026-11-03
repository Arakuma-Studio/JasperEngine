// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from OpenEngine's VCSStatusPoller (Engine/Modules/VCSIntegration/Include/VCSIntegration/
// VCSStatusPoller.h, Source/VCSStatusPoller.cpp, MIT, Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors; NOTICE.md), logging through the engine's logger.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace jasperengine::editor::vcs {

    // The background status poll (docs/specs/S29-version-control.md R5): one poll as soon as it
    // starts, then one per interval, and one right after each wake(). A burst of wakes (saves
    // during a slider drag) collapses into at most two polls: the immediate one, then one more
    // after a short cooldown. The poll function runs on the poll thread only; an exception it
    // throws is logged and the next poll runs as scheduled.
    class VcsStatusPoller {
    public:
        using PollFunction = std::function<void()>;

        static constexpr int kDefaultIntervalSeconds = 10;
        static constexpr std::chrono::milliseconds kWakeCooldown{500};

        VcsStatusPoller() = default;
        ~VcsStatusPoller();
        VcsStatusPoller(const VcsStatusPoller&) = delete;
        VcsStatusPoller& operator=(const VcsStatusPoller&) = delete;

        // Starts the poll thread, stopping a running one first. threadName names the thread
        // ("je-vcs") and prefixes the poll-failure logs.
        void start(std::string threadName, PollFunction poll);
        // Stops and joins the poll thread (a poll in progress finishes first). Safe when stopped.
        void stop();
        // A poll now instead of at the next interval. Any thread; ignored while stopped.
        void wake();
        // True from stop() until the next start(): a poll in progress reads it to give up early
        bool isStopping() const { return stopping.load(); }
        bool isRunning() const { return thread.joinable(); }

        // Seconds between polls; a non-positive value restores the default. From the next wait.
        void setIntervalSeconds(int seconds);
        int getIntervalSeconds() const { return intervalSeconds.load(); }
        // Tests: the interval in milliseconds (below a second)
        void setIntervalMilliseconds(int milliseconds);

    private:
        void run();
        void pollOnce();
        // Blocks until the interval elapses, wake() is called or stop() begins; true when a
        // wake ended the wait
        bool waitForNextPoll();
        void waitForCooldown();

        std::string threadName;
        PollFunction poll;
        std::atomic<bool> stopping{false};
        std::atomic<int> intervalSeconds{kDefaultIntervalSeconds};
        std::atomic<int> intervalMilliseconds{0};
        std::mutex mutex;
        std::condition_variable wakeCondition;
        bool wakeRequested = false;   // guarded by mutex
        std::thread thread;
    };

}
