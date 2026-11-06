// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A clip's import settings (spec S36 R7): the `import.audioImporter` section of its S04 meta
// sidecar, Unity's AudioImporter in our names. What the bytes cannot say about how a clip plays:
// its type, gain, trims, click-safe fades, loop region and music tempo (S39 R3). Keys are written
// only when they differ from their default, in this order; unknown keys survive a round trip after
// them.
//
//   import:
//     audioImporter:
//       version: 1
//       type: footstep
//       gain_db: -3
//       trim_head_ms: auto
//       fade_out_ms: 20
//       loop_start_ms: 1250.5
//       music:
//         bpm: 120
//         beats_per_bar: 4
//         first_beat_ms: 12.4

#pragma once

#include "audio/AudioImport.h"

#include "json.hpp"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class AudioImportSettings {
    public:
        using Json = nlohmann::json;

        static constexpr const char* metaKey = "import.audioImporter";
        static constexpr int currentVersion = 1;

        static constexpr double GAIN_MIN_DB = -60.0;
        static constexpr double GAIN_MAX_DB = 24.0;
        static constexpr double FADE_MAX_MS = (double)CLIP_FADE_MAX_MS;
        // the shortest loop region a voice can wrap on without a burr, frames
        static constexpr int64_t LOOP_MIN_FRAMES = 32;

        // A trim: a length in milliseconds, or "auto": to the first (last) sample above -60 dBFS
        struct Trim {
            bool automatic = false;
            double ms = 0.0;

            bool isDefault() const { return !automatic && ms == 0.0; }
            bool operator==(const Trim& other) const { return automatic == other.automatic && ms == other.ms; }
        };

        static constexpr double BPM_MIN = 20.0;
        static constexpr double BPM_MAX = 400.0;
        static constexpr int BEATS_PER_BAR_MAX = 16;

        // A music clip's beat grid: beat 1 at firstBeatMs from the file's start (before trims), a
        // beat every 60 / bpm s, a bar every beatsPerBar beats. bpm 0: no tempo, the block unwritten.
        // It never changes the samples.
        struct Music {
            double bpm = 0.0;
            int beatsPerBar = 4;
            double firstBeatMs = 0.0;

            bool isDefault() const { return bpm == 0.0; }
            bool operator==(const Music& other) const {
                return bpm == other.bpm && beatsPerBar == other.beatsPerBar && firstBeatMs == other.firstBeatMs;
            }
            // {bpm, beats_per_bar, first_beat_ms}
            nlohmann::json toJson() const;
        };

        std::string type;           // SoundTypes.h vocabulary; empty: none
        double gainDb = 0.0;
        Trim trimHead;
        Trim trimTail;
        double fadeInMs = 0.0;
        double fadeOutMs = 0.0;
        double loopStartMs = -1.0;  // -1: the trimmed start
        double loopEndMs = -1.0;    // -1: the trimmed end
        Music music;
        // the keys this version does not read (a later version's), in file order
        YAML::Node unknown;

        // no key but `version` would be written
        bool isDefault() const;
        bool operator==(const AudioImportSettings& other) const;
        // the section differs in how the clip sounds (not its type, its tempo or an unknown key)
        bool sameSound(const AudioImportSettings& other) const;

        // The one validation of the values (Inspector, MCP, export); false with the reason
        bool validate(std::string& error) const;

        // Reads a section (an absent or empty node is the defaults); a known key of the wrong shape
        // or a value validate refuses fails with the reason
        static bool parse(const YAML::Node& section, AudioImportSettings& out, std::string& error);
        // The section of an asset's .meta. True with the defaults when there is none.
        static bool load(const std::filesystem::path& asset, AudioImportSettings& out, std::string& error);
        // The section's value, keys in order; a null node when isDefault(), which removes the section
        YAML::Node toYaml() const;
        // The keys as written (no `version`)
        Json toJson() const;
        // set_audio_import_settings' `settings`: each key set, null resetting one; an unknown name
        // or a wrong shape fails with the reason and names the keys
        bool apply(const Json& keys, std::string& error);
        // The R7 key names, in order
        static const std::vector<std::string>& keyNames();

        // To frames at the file's rate, round(ms x rate / 1000), `auto` trims from the file's head and
        // tail silence (AudioAnalysis::silence, in ms), the loop points checked against the trimmed
        // region. False with the reason when they do not fit the file. resolved, when given, gets
        // {gain_db, trim_head_frames, trim_tail_frames, fade_in_frames, fade_out_frames,
        // loop_start_frame, loop_end_frame, sample_rate, frames}, and {bpm, beats_per_bar,
        // first_beat_frame} with a tempo.
        bool resolve(uint64_t fileFrames, unsigned int sampleRate, double headSilenceMs, double tailSilenceMs, ClipImportSettings& out,
                     std::string& error, Json* resolved = nullptr) const;
        // A number as the section writes it: at most 4 decimals, no trailing zeros, no locale
        static std::string numberText(double value);
    };

}
