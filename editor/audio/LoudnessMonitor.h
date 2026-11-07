// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOUDNESSMONITOR_H
#define LOUDNESSMONITOR_H

#include "audio/AudioMeter.h"

#include <memory>
#include <optional>
#include <vector>

namespace jasperengine::editor {

    // The Master group's live loudness in Play (S38 R12, R13): drains AudioMixer's tap into an
    // AudioMeter. The tap is on only while something looks: a visible panel (markVisible, each
    // frame) or an agent (requestByAgent: from its first read until Stop).
    class LoudnessMonitor {
    public:
        struct Reading {
            std::optional<double> momentary;
            std::optional<double> shortTerm;
            std::optional<double> integrated;
            double truePeakDb = AudioMeter::SILENCE_DB;
            // of the limiters on Master, 0 without one
            float gainReductionDb = 0.0f;
        };

        static LoudnessMonitor& get();

        void markVisible();
        void requestByAgent();
        // once a frame: starts and stops the tap, drains it
        void update(bool playing);
        // what has been measured since Play started or the last reset; drained up to now
        Reading reading();
        // the integrated loudness and the true-peak hold start over
        void reset();

    private:
        std::unique_ptr<AudioMeter> meter;
        std::vector<float> scratch;
        bool visible = false;
        bool visibleLastFrame = false;
        bool agent = false;
        bool wasPlaying = false;

        void drain();
    };

}

#endif //LOUDNESSMONITOR_H
