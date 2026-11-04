// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FrameLimiter.h"

#include <chrono>
#include <cmath>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

using namespace jasperengine;

namespace {

    // On Windows sleep_for wakes on the 15.6 ms system tick, too coarse to pace frames
    void sleepSeconds(double seconds){
        if (seconds <= 0.0) return;
#if defined(_WIN32)
        static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(seconds * 1e7); // relative, in 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)){
            WaitForSingleObject(timer, INFINITE);
            return;
        }
#endif
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    }

}

double FrameLimiter::now(){
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

void FrameLimiter::reset(){
    started = false;
    deadline = 0.0;
}

void FrameLimiter::wait(double fps){
    if (fps <= 0.0){
        reset();
        return;
    }
    const double period = 1.0 / fps;
    double current = now();
    if (!started){
        started = true;
        deadline = current + period;
        return;
    }
    if (current - deadline > period){
        // far behind: no burst of frames to catch up
        deadline = current + period;
        return;
    }
    if (deadline > current + 0.001){
        sleepSeconds(deadline - current - 0.001);
    }
    while ((current = now()) < deadline){
        std::this_thread::yield();
    }
    deadline += period;
}

bool FrameLimiter::isDue(double fps, double tolerance){
    return isDueAt(now(), fps, tolerance);
}

bool FrameLimiter::isDueAt(double current, double fps, double tolerance){
    if (fps <= 0.0){
        reset();
        return true;
    }
    const double period = 1.0 / fps;
    if (!started){
        started = true;
        deadline = current + period;
        return true;
    }
    if (current + tolerance < deadline) return false;
    deadline += period;
    if (current - deadline > period){
        deadline = current + period;
    }
    return true;
}

double FrameLimiter::secondsUntilDue() const{
    return secondsUntilDueAt(now());
}

double FrameLimiter::secondsUntilDueAt(double current) const{
    if (!started || deadline <= current) return 0.0;
    return deadline - current;
}
