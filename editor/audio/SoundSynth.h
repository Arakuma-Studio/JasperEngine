// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // Placeholder sounds for agents that cannot hear: SoLoud's
    // sfxr presets (rendered offline at 44.1 kHz) and six DSP recipes (48 kHz), mono, brought to a
    // loudness target and written as PCM16 WAV. Every render fades in and out (raised cosine), so
    // its first and last samples are exactly 0. Everything follows from the preset, the seed, the
    // params and the target, so equal requests give byte-identical files in one editor build.
    // Plain functions, no audio device, safe on any thread.
    class SoundSynth {
    public:
        using Json = nlohmann::json;

        // provenance.generator.version: bump whenever a request's output changes
        static constexpr int VERSION = 1;
        static constexpr unsigned int SFXR_RATE = 44100;
        static constexpr unsigned int RECIPE_RATE = 48000;
        // sfxr renders until its sound ends, at most this long
        static constexpr double MAX_SECONDS = 10.0;
        // the recipes' frequencies move by up to this share per seed (variations differ in pitch)
        static constexpr double PITCH_JITTER = 0.03;
        // the gain to the target stops where the true peak would pass this
        static constexpr double TRUE_PEAK_CEILING_DBTP = -1.0;
        // xorshift32 cannot start from 0: seed 0 starts here
        static constexpr uint32_t ZERO_SEED = 0x9E3779B9u;
        static constexpr const char* RANDOM = "random";

        struct Param {
            enum class Kind { Number, Integer, Boolean };
            const char* name;
            double min;
            double max;
            double defaultValue;
            Kind kind = Kind::Number;
        };

        struct Preset {
            const char* name;
            // the sound type of its files (SoundTypes.h): the loudness target and the kind
            const char* type;
            // SoLoud::Sfxr::SFXR_PRESETS, or -1 for a recipe
            int sfxrPreset;
            // a recipe's parameters (sfxr presets take sfxrParams())
            std::vector<Param> params;

            bool isSfxr() const { return sfxrPreset >= 0; }
        };

        // The seeded generator of the recipes (xorshift32: x ^= x << 13; x ^= x >> 17; x ^= x << 5)
        class Random {
        public:
            explicit Random(uint32_t seed);
            uint32_t next();
            // [0, 1)
            double uniform();
            // [-1, 1)
            double bipolar();

        private:
            uint32_t state;
        };

        // The 17 presets: coin, laser, explosion, powerup, hurt, jump, blip (sfxr), then click, pluck,
        // bell, whoosh, riser, thump (recipes), then ui_click, swoosh, impact, pop (layered, SoundDesign)
        static const std::vector<Preset>& presets();
        static const Preset* find(const std::string& name);
        // the 17 and "random"
        static std::vector<std::string> names();
        // SoLoud::SfxrParams by field name, with sfxr's own slider ranges
        static const std::vector<Param>& sfxrParams();
        // The parameters a preset takes
        static const std::vector<Param>& paramsOf(const Preset& preset);

        // How many presets "random" picks among: the sfxr presets and the recipes. The layered ones
        // came later, and a seed keeps the preset it always picked.
        static constexpr size_t RANDOM_PRESETS = 13;

        // The preset "random" stands for: the first xorshift32 output of the seed, modulo RANDOM_PRESETS
        static const Preset& pick(uint32_t seed);

        // params is an object of overrides, each a known name within its range; the error names
        // the valid list
        static bool checkParams(const Preset& preset, const Json& params, std::string& error);

        // The raw signal (mono, its own level, faded in and out) of a preset with checked params.
        // family: a layered preset's (SoundDesign::families; empty: its first)
        static bool render(const Preset& preset, uint32_t seed, const Json& params, std::vector<float>& samples,
                           unsigned int& sampleRate, std::string& error, const std::string& family = std::string());

        struct Output {
            const Preset* preset = nullptr;
            uint32_t seed = 0;
            double targetLufs = 0.0;
            // the gain applied to the raw signal
            double gainDb = 0.0;
            // true when the true peak ceiling, not the target, set the gain
            bool limitedByTruePeak = false;
            unsigned int sampleRate = 0;
            size_t frames = 0;
            // the PCM16 WAV file (fmt and data chunks only)
            std::string wav;
        };

        // A preset (not "random": pick() it first) with checked params, at targetLufs momentary max
        static bool generate(const Preset& preset, uint32_t seed, const Json& params, double targetLufs, Output& out,
                             std::string& error, const std::string& family = std::string());
    };

}
