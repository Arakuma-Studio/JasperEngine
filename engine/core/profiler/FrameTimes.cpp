// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FrameTimes.h"

#include <algorithm>
#include <chrono>

using namespace jasperengine;

namespace {

    struct State {
        FrameTimeSample ring[FrameTimes::CAPACITY];
        size_t head = 0;            // the next write
        size_t count = 0;
        uint64_t frame = 0;
        bool started = false;
        bool external = false;
        std::chrono::steady_clock::time_point frameStart;
        double cpuMs = -1.0;        // endFrameWork of the current frame (< 0: not called)
        uint32_t drawCalls = 0;
        // a GPU time that arrives for the frame still being drawn waits for its sample
        uint64_t pendingGpuFrame = 0;
        float pendingGpuMs = -1.0f;
        uint32_t pendingGpuFlags = 0;
    };

    State& state() {
        static State s;
        return s;
    }

    double msSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

}

void FrameTimes::beginFrame() {
    State& s = state();
    const auto now = std::chrono::steady_clock::now();
    if (s.started) {
        FrameTimeSample sample;
        sample.frame = s.frame;
        sample.frameMs = (float)std::chrono::duration<double, std::milli>(now - s.frameStart).count();
        sample.cpuMs = s.cpuMs >= 0.0 ? (float)s.cpuMs : sample.frameMs;
        sample.fps = sample.frameMs > 0.0f ? 1000.0f / sample.frameMs : 0.0f;
        sample.drawCalls = s.drawCalls;
        if (s.pendingGpuFrame == s.frame && s.pendingGpuMs >= 0.0f) {
            sample.gpuMs = s.pendingGpuMs;
            sample.flags |= s.pendingGpuFlags;
        }
        s.ring[s.head] = sample;
        s.head = (s.head + 1) % CAPACITY;
        s.count = std::min(s.count + 1, CAPACITY);
    }
    s.started = true;
    s.frame++;
    s.frameStart = now;
    s.cpuMs = -1.0;
    s.drawCalls = 0;
}

void FrameTimes::endFrameWork() {
    State& s = state();
    if (s.started)
        s.cpuMs = msSince(s.frameStart);
}

void FrameTimes::setExternalFrames(bool external) {
    state().external = external;
}

bool FrameTimes::hasExternalFrames() {
    return state().external;
}

uint64_t FrameTimes::currentFrame() {
    return state().frame;
}

void FrameTimes::setDrawCalls(uint32_t drawCalls) {
    state().drawCalls = drawCalls;
}

void FrameTimes::setGpuMs(uint64_t frame, double gpuMs, bool sumOfPasses) {
    State& s = state();
    const uint32_t flags = sumOfPasses ? FLAG_GPU_SUM : 0u;
    if (frame == s.frame) {
        s.pendingGpuFrame = frame;
        s.pendingGpuMs = (float)gpuMs;
        s.pendingGpuFlags = flags;
        return;
    }
    // completed frames, newest first: the answer is a few frames old
    for (size_t i = 0; i < s.count; i++) {
        FrameTimeSample& sample = s.ring[(s.head + CAPACITY - 1 - i) % CAPACITY];
        if (sample.frame == frame) {
            sample.gpuMs = (float)gpuMs;
            sample.flags = (sample.flags & ~FLAG_GPU_SUM) | flags;
            return;
        }
        if (sample.frame < frame)
            return;
    }
}

size_t FrameTimes::size() {
    return state().count;
}

FrameTimeSample FrameTimes::at(size_t index) {
    const State& s = state();
    if (index >= s.count)
        return FrameTimeSample();
    return s.ring[(s.head + CAPACITY - s.count + index) % CAPACITY];
}

std::vector<FrameTimeSample> FrameTimes::snapshot(size_t newest) {
    const State& s = state();
    const size_t n = std::min(newest, s.count);
    std::vector<FrameTimeSample> out;
    out.reserve(n);
    for (size_t i = s.count - n; i < s.count; i++)
        out.push_back(at(i));
    return out;
}

FrameTimeSample FrameTimes::latest() {
    const State& s = state();
    return s.count ? at(s.count - 1) : FrameTimeSample();
}

double FrameTimes::latestGpuMs(size_t window) {
    const State& s = state();
    const size_t n = std::min(window, s.count);
    for (size_t i = 0; i < n; i++) {
        const FrameTimeSample& sample = s.ring[(s.head + CAPACITY - 1 - i) % CAPACITY];
        if (sample.gpuMs >= 0.0f)
            return sample.gpuMs;
    }
    return -1.0;
}

void FrameTimes::clear() {
    State& s = state();
    s.head = 0;
    s.count = 0;
}
