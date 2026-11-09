// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_STROKE_H
#define TERRAIN_STROKE_H

// T1 R33-R35: samples of a drag become stamps, so a stroke's result does not depend on the event
// rate. Stamps fall at arc-length multiples of spacing along the sampled path; only holding still
// adds stamps over time.

#include "Export.h"

#include <span>
#include <vector>

namespace jasperengine::terraincore {

    struct StrokeSample {
        float time = 0.0f;      // seconds; only differences matter
        float x = 0.0f, z = 0.0f;
        float pressure = 1.0f;
    };

    struct StrokeStamp {
        float x = 0.0f, z = 0.0f;
        float pressure = 1.0f;
    };

    class JASPERENGINE_API StrokeResampler {
    public:
        static constexpr float SECONDS_PER_STAMP = 1.0f / 60.0f;
        // R35: past this share of the spacing per 1/60 s, a pointer is moving and only distance
        // deposits; R35's cap is today's hitch clamp
        static constexpr float HOLD_SPEED_SHARE = 1.0f / 8.0f;
        static constexpr int MAX_HELD_STAMPS_PER_SAMPLE = 3;
        // R37: how long into a stroke a pen's zero still means a click. A tap reaches the
        // input layer before the pen's sensor reads any pressure (Terrain3D found the same,
        // terrain_3d_editor.cpp:118-133), and a click must land as one full stamp.
        static constexpr float PEN_CLICK_SECONDS = 0.1f;

        // R37: what a sample presses with. A pen reads 0..1; anything else (the input layer's -1,
        // NaN) is a mouse, which presses fully, and so is a pen's zero within PEN_CLICK_SECONDS of
        // the stroke's start.
        static float samplePressure(float reading, float secondsIntoStroke);

        // R33's spacing for a radius and a map's texel size, both terrain-local
        static float defaultSpacing(float radius, float texelSize);

        explicit StrokeResampler(float spacing);

        // Stamps this sample adds, in order. The first sample is one stamp: a click.
        void add(const StrokeSample& sample, std::vector<StrokeStamp>& out);

    private:
        double spacing;
        bool started = false;
        StrokeSample last;
        double arcPastLastStamp = 0.0;
        double heldStamps = 0.0;
    };

}

#endif // TERRAIN_STROKE_H
