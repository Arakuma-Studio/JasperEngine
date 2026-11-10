// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainStroke.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::terraincore;

float StrokeResampler::defaultSpacing(float radius, float texelSize) {
    return std::max(0.25f * radius, 0.5f * texelSize);
}

float StrokeResampler::samplePressure(float reading, float secondsIntoStroke) {
    if (!(reading >= 0.0f && reading <= 1.0f)) return 1.0f;
    if (reading == 0.0f && secondsIntoStroke <= PEN_CLICK_SECONDS) return 1.0f;
    return reading;
}

StrokeResampler::StrokeResampler(float spacing): spacing(spacing > 0.0f ? spacing : 1.0f) {}

void StrokeResampler::add(const StrokeSample& sample, std::vector<StrokeStamp>& out) {
    if (!started) {
        started = true;
        last = sample;
        out.push_back(StrokeStamp{sample.x, sample.z, sample.pressure});
        return;
    }

    const double dx = static_cast<double>(sample.x) - last.x;
    const double dz = static_cast<double>(sample.z) - last.z;
    const double length = std::sqrt(dx * dx + dz * dz);
    const double seconds = static_cast<double>(sample.time) - last.time;

    // Distance: a stamp at each multiple of the spacing along this segment
    double along = spacing - arcPastLastStamp;
    while (along <= length) {
        const double t = length > 0.0 ? along / length : 0.0;
        out.push_back(StrokeStamp{static_cast<float>(last.x + dx * t), static_cast<float>(last.z + dz * t),
                                  static_cast<float>(last.pressure + (static_cast<double>(sample.pressure) - last.pressure) * t)});
        along += spacing;
    }
    arcPastLastStamp = length - (along - spacing);

    // Time: only while the pointer is all but still
    if (seconds > 0.0) {
        const double framesElapsed = seconds / SECONDS_PER_STAMP;
        const bool holding = length / framesElapsed < spacing * HOLD_SPEED_SHARE;
        if (holding) {
            heldStamps += framesElapsed;
            const int due = static_cast<int>(std::floor(heldStamps));
            const int stamps = std::min(due, MAX_HELD_STAMPS_PER_SAMPLE);
            heldStamps -= due;
            for (int i = 0; i < stamps; i++) {
                out.push_back(StrokeStamp{sample.x, sample.z, sample.pressure});
            }
        }
    }

    last = sample;
}
