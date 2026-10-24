// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_JOBSTESTUTIL_H
#define TESTS_JOBSTESTUTIL_H

#include "jobs/JobSystem.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

// Sanitizers slow the scheduler 5-50x: tests scale their iteration counts and time budgets.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    #define JE_TEST_SANITIZED 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
        #define JE_TEST_SANITIZED 1
    #endif
#endif
#ifndef JE_TEST_SANITIZED
    #define JE_TEST_SANITIZED 0
#endif

namespace testsupport {

    // `normal` iterations, or `sanitized` under a sanitizer.
    inline int scaled(int normal, int sanitized){
        return JE_TEST_SANITIZED ? sanitized : normal;
    }

    inline double timeScale(){
        return JE_TEST_SANITIZED ? 10.0 : 1.0;
    }

    // Runs `body`; a deadlock (still running after `seconds`) ends the process with a report,
    // since a hung test cannot be recovered.
    inline void runWithWatchdog(double seconds, const char* what, const std::function<void()>& body){
        jasperengine::jobs::Event done;
        std::thread watchdog([&done, seconds, what](){
            if (!done.waitFor(seconds * 1000.0)){
                std::fprintf(stderr, "WATCHDOG: '%s' did not finish within %.1f s (deadlock)\n", what, seconds);
                std::fflush(stderr);
                std::_Exit(3);
            }
        });
        body();
        done.set();
        watchdog.join();
    }

    inline void spinFor(double microseconds){
        const auto end = std::chrono::steady_clock::now() + std::chrono::nanoseconds((int64_t)(microseconds * 1000.0));
        while (std::chrono::steady_clock::now() < end){
        }
    }

    inline double elapsedMs(std::chrono::steady_clock::time_point since){
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
    }

    // Spins until `predicate` holds or `timeoutMs` passes; whether it held.
    inline bool spinUntil(const std::function<bool()>& predicate, double timeoutMs){
        const auto start = std::chrono::steady_clock::now();
        while (!predicate()){
            if (elapsedMs(start) > timeoutMs)
                return false;
            std::this_thread::yield();
        }
        return true;
    }

    // The worker-count parameter of the scheduler suites.
    inline std::string workersName(const ::testing::TestParamInfo<int>& info){
        return "workers" + std::to_string(info.param);
    }

    inline jasperengine::jobs::Config configFor(int workers){
        jasperengine::jobs::Config config;
        config.workers = workers;
        config.reserveForFrame = 0;
        config.detectWaitCycles = true;
        return config;
    }

    // Occupies the only worker of a 1-worker system until release(): what is submitted
    // meanwhile stays queued.
    class WorkerBlocker {
    public:
        explicit WorkerBlocker(jasperengine::jobs::JobSystem& system){
            task = system.submit([this](){
                started.set();
                release.wait();
            }, {jasperengine::jobs::Lane::High, jasperengine::jobs::GateClass::Default, "blocker"});
            if (system.getWorkerCount() > 0)
                started.wait();
        }
        ~WorkerBlocker(){ unblock(); }
        void unblock(){
            release.set();
            task.wait();
        }

    private:
        jasperengine::jobs::Event started;
        jasperengine::jobs::Event release;
        jasperengine::jobs::Task<void> task;
    };

}

#endif // TESTS_JOBSTESTUTIL_H
