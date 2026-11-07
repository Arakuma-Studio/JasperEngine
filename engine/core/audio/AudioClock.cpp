// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioClock.h"

#include <cmath>

using namespace jasperengine;

void AudioClock::reset(unsigned int rate) {
    sampleRate = rate;
    accumulated = 0.0;
    frames = 0;
}

uint32_t AudioClock::advance(double seconds) {
    if (!(seconds > 0.0)) return 0;
    accumulated += seconds;
    const uint64_t due = (uint64_t)std::llround(accumulated * sampleRate);
    const uint32_t count = due > frames ? (uint32_t)(due - frames) : 0;
    frames += count;
    return count;
}

uint32_t AudioClock::peek(double seconds) const {
    if (!(seconds > 0.0)) return 0;
    const uint64_t due = (uint64_t)std::llround((accumulated + seconds) * sampleRate);
    return due > frames ? (uint32_t)(due - frames) : 0;
}

uint64_t AudioClock::cursor() const {
    return frames;
}

double AudioClock::seconds() const {
    return sampleRate > 0 ? (double)frames / sampleRate : 0.0;
}
