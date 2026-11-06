// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Tempo, beat grid and downbeat detection of a music clip (spec S39 R16, sound-layer P6 §2.4.8):
// an onset-strength envelope from the analyzer's STFT, the tempo by its autocorrelation under a
// prior around 120 BPM, Ellis' (2007) dynamic-programming beat tracker, the beats moved onto the
// attacks and a least-squares line through them, and the bar's phase from the low band's accents.
// A pure function of the file's bytes, cached beside the analysis. The result is only shown:
// a click on Use, or an agent passing `fix` to set_audio_import_settings, writes it.
#pragma once

#include "audio/AudioAnalysis.h"
#include "audio/AudioImportSettings.h"

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class AudioMusicGrid {
    public:
        using Json = nlohmann::json;

        // Bump whenever any value changes for the same bytes: the cache file names carry it
        static constexpr int GRID_VERSION = 1;

        // One way to lay the grid: beat 1 at firstBeatMs, a beat every 60 / bpm s
        struct Candidate {
            double bpm = 0.0;
            double firstBeatMs = 0.0;
            int beatsPerBar = 4;
            // the autocorrelation of the onset envelope at that tempo's beat
            double score = 0.0;
        };

        struct Grid {
            std::string sha1;
            unsigned int sampleRate = 0;
            uint64_t frames = 0;
            // 0: none (too short to judge, or no steady pulse: notes say which)
            double bpm = 0.0;
            double bpmAlt = 0.0;
            double firstBeatMs = 0.0;
            int beatsPerBarGuess = 4;
            double downbeatConfidence = 0.0;
            double pulseClarity = 0.0;
            bool confident = false;
            // the detection first, then bpmAlt's grid
            std::vector<Candidate> candidates;
            // {id, text}: too_short, no_pulse
            Json notes = Json::array();

            bool hasTempo() const { return bpm > 0.0; }
            Json toJson() const;
            static bool fromJson(const Json& value, Grid& out);
        };

        // The grid of a decoded file (its mono mix, at its own rate, the whole file)
        static Grid detect(const AudioAnalysis::Signal& signal);
        // A file through the cache in cacheDirectory (<sha1>-grid-v<GRID_VERSION>.json): read, else
        // decoded, detected and written through io::writeFileAtomic. Safe on any thread.
        static bool get(const std::filesystem::path& file, const std::filesystem::path& cacheDirectory, Grid& out, std::string& error);
        // The same from bytes already read (a built-in clip)
        static bool getBytes(const std::string& bytes, const std::filesystem::path& cacheDirectory, Grid& out, std::string& error);
        static std::filesystem::path cacheFile(const std::filesystem::path& cacheDirectory, const std::string& sha1);

        // analyze_audio's `music`: the grid, loop_beats (the loop region of `current`, else of the
        // whole file, in beats) with a beat_drift note, `current` (the clip's music block or null),
        // `agrees` (current lays the same grid: bpm within AGREE_BPM, the same beats per bar, beat 1
        // within AGREE_MS of a bar line of the detection) and `fix`, the `settings` to pass to
        // set_audio_import_settings. current null: a built-in clip (no .meta): `agrees` null.
        // headMs, tailMs: the file's silence, for `auto` trims.
        static Json item(const Grid& grid, const AudioImportSettings* current, double headMs, double tailMs);
        // "tempo 120.00 bpm (alt 60.00), 4/4 guess, first beat 12.4 ms, confidence 0.62", or the reason there is none
        static std::string line(const Grid& grid);
        // set_audio_import_settings' music block of a candidate
        static AudioImportSettings::Music musicOf(const Candidate& candidate);
    };

}
