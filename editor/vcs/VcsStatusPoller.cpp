// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from OpenEngine's VCSStatusPoller.cpp (MIT, Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors; NOTICE.md).

#include "vcs/VcsStatusPoller.h"

#include "Out.h"
#include "thread/ThreadUtil.h"

#include <exception>
#include <utility>

namespace jasperengine::editor::vcs {

VcsStatusPoller::~VcsStatusPoller() {
    stop();
}

void VcsStatusPoller::start(std::string name, PollFunction function) {
    stop();
    threadName = std::move(name);
    poll = std::move(function);
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = false;
        wakeRequested = false;
    }
    thread = std::thread(&VcsStatusPoller::run, this);
}

void VcsStatusPoller::stop() {
    {
        // set under the lock so the poll thread cannot miss the notify between testing its
        // wait predicate and blocking
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    wakeCondition.notify_all();
    if (thread.joinable()) thread.join();
}

void VcsStatusPoller::wake() {
    {
        // tested under the lock so a stop() racing this call cannot leave the request set for
        // the next start() to act on
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) return;
        wakeRequested = true;
    }
    wakeCondition.notify_one();
}

void VcsStatusPoller::setIntervalSeconds(int seconds) {
    intervalSeconds = seconds > 0 ? seconds : kDefaultIntervalSeconds;
    intervalMilliseconds = 0;
}

void VcsStatusPoller::setIntervalMilliseconds(int milliseconds) {
    intervalMilliseconds = milliseconds > 0 ? milliseconds : 0;
}

void VcsStatusPoller::run() {
    ::jasperengine::thread::setCurrentName(threadName.c_str());
    if (!stopping) pollOnce();
    while (!stopping) {
        const bool woken = waitForNextPoll();
        if (stopping) break;
        pollOnce();
        if (woken) waitForCooldown();
    }
}

void VcsStatusPoller::pollOnce() {
    try {
        poll();
    } catch (const std::exception& e) {
        Out::warning("%s: status poll failed: %s", threadName.c_str(), e.what());
    } catch (...) {
        Out::warning("%s: status poll failed with an unknown error", threadName.c_str());
    }
}

bool VcsStatusPoller::waitForNextPoll() {
    std::unique_lock<std::mutex> lock(mutex);
    const int ms = intervalMilliseconds.load();
    const std::chrono::milliseconds interval = ms > 0 ? std::chrono::milliseconds(ms) : std::chrono::seconds(intervalSeconds.load());
    wakeCondition.wait_for(lock, interval, [this] { return stopping.load() || wakeRequested; });
    const bool woken = wakeRequested;
    wakeRequested = false;
    return woken;
}

void VcsStatusPoller::waitForCooldown() {
    // leaves wakeRequested alone: a wake during the cooldown makes the next waitForNextPoll
    // return at once
    std::unique_lock<std::mutex> lock(mutex);
    wakeCondition.wait_for(lock, kWakeCooldown, [this] { return stopping.load(); });
}

}
