// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AudioAnalysis.h"
#include "SoundSynth.h"
#include "json.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // "Make a placeholder like this" (spec S36 M10): the params of a recipe or a layered preset that
    // bring its render closest to a reference clip's band balance, envelope, length and attack, in at
    // most SoundTypes::FIT_RENDERS renders. No model and no audio of the reference is copied: only
    // these numbers reach the result. The distance says how alike two measurements are, not whether the
    // result is good. A pure function of the reference, the preset, the family and the seed.
    class SoundFit {
    public:
        using Json = nlohmann::json;

        static constexpr int BANDS = 26;

        struct Measure {
            unsigned int rate = 0;
            bool silent = true;
            double activeSeconds = 0.0;
            double attackMs = 0.0;
            // the power of each third-octave band, summed over the frames of the active region
            std::array<double, BANDS> bandPower = {};
            // RMS over consecutive 5 ms frames from the head, dB re the loudest, floored at -60
            std::vector<double> envelopeDb;
        };

        struct Quality {
            double distance = 0.0;
            double bandDb = 0.0;
            double envelopeDb = 0.0;
            double onsetMs = 0.0;
            double lengthRatio = 1.0;
            // close, similar or rough
            std::string grade;
        };

        struct Result {
            Json params = Json::object();
            std::string family;
            Quality quality;
            double startDistance = 0.0;
            int renders = 0;
            // the render with the fitted params
            Measure measure;
        };

        // A band's centre, Hz: 1000 x 10^(k / 10), k = -13..12
        static double bandCentre(int band);
        // Each band's share of the total in dB, floored at -60, over the bands both rates hold (a band
        // above 0.45 x the rate reads the floor); `other` is the rate of the file it is compared with
        static std::array<double, BANDS> bandLevels(const Measure& measure, unsigned int otherRate);

        // On the mono mix, over its active region: from its head to its tail at -60 dB re its peak, so
        // that no measure depends on the level.
        static Measure measure(const AudioAnalysis::Signal& signal);
        static Quality compare(const Measure& reference, const Measure& candidate);
        static std::string gradeOf(double distance);
        // "+4.2 dB at 500 Hz, -3.1 dB at 4 kHz": the two bands that differ the most, candidate against reference
        static std::string bandWords(const Measure& reference, const Measure& candidate);

        // Can this preset be fitted? False, with the sentence to refuse it with (sfxr presets, random).
        static bool canFit(const SoundSynth::Preset* preset, std::string& error);

        // family: fixes a layered preset's family (empty: the search tries each, starting at the first).
        // Runs on the calling thread; progress after each render, cancel checked between renders.
        static bool fit(const Measure& reference, const SoundSynth::Preset& preset, const std::string& family, uint32_t seed, Result& result,
                        std::string& error, const std::function<void(int renders, double best)>& progress = nullptr,
                        const std::atomic<bool>* cancel = nullptr);
    };

}
