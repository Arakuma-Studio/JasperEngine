// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A clip's import settings (spec S36 R7): the `import.audioImporter` section of its S04 meta
// sidecar, Unity's AudioImporter in our names. What the bytes cannot say about how a clip plays:
// its type, gain, trims, click-safe fades and loop region. Keys are written only when they differ
// from their default, in this order; unknown keys survive a round trip after them.
//
//   import:
//     audioImporter:
//       version: 1
//       type: footstep
//       gain_db: -3
//       trim_head_ms: auto
//       fade_out_ms: 20
//       loop_start_ms: 1250.5

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

        std::string type;           // SoundTypes.h vocabulary; empty: none
        double gainDb = 0.0;
        Trim trimHead;
        Trim trimTail;
        double fadeInMs = 0.0;
        double fadeOutMs = 0.0;
        double loopStartMs = -1.0;  // -1: the trimmed start
        double loopEndMs = -1.0;    // -1: the trimmed end
        // the keys this version does not read (S39's music, a later version's), in file order
        YAML::Node unknown;

        // no key but `version` would be written
        bool isDefault() const;
        bool operator==(const AudioImportSettings& other) const;
        // the section differs in how the clip plays (not its type, not an unknown key)
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
        // loop_start_frame, loop_end_frame, sample_rate, frames}.
        bool resolve(uint64_t fileFrames, unsigned int sampleRate, double headSilenceMs, double tailSilenceMs, ClipImportSettings& out,
                     std::string& error, Json* resolved = nullptr) const;
        // A number as the section writes it: at most 4 decimals, no trailing zeros, no locale
        static std::string numberText(double value);
    };

}
