// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "audio/AudioImport.h"
#include "audio/SoundEvent.h"
#include "audio/SoundEventLibrary.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Offline auditions of a Sound Event (docs/specs/S37-sound-events.md R16, R20): the next plays
    // a Play with this seed makes, each rendered alone. The picks are SoundEventLibrary's rules from
    // a fresh state. Each rendition is mixed by a private SoLoud on the null driver: it never touches
    // the game's SoLoud, pool or mixer. A rendition is the event's voices dry and centred, as their
    // group receives them.
    class SoundEventRender {
    public:
        // What a render reads, taken on the main thread: where the clips are, and how their .meta's
        // import settings (S36) say they play, by the clip's assets-relative path
        struct Input {
            std::filesystem::path assetsRoot;
            std::map<std::string, ClipImportSettings> settings;
            // SoundSearch::eventProblems' reads
            std::vector<std::filesystem::path> clipFiles;
            std::filesystem::path cacheDirectory;
        };
        static Input prepare(const Project& project, const SoundEventDef& event);

        // a rendition stops here (a looping event: its first pass)
        static constexpr double MAX_SECONDS = 10.0;
        static constexpr unsigned int BLOCK_FRAMES = 512;
        static constexpr unsigned int CHANNELS = 2;
        static constexpr unsigned int SAMPLE_RATE = 48000;
        static constexpr int MAX_COUNT = 16;

        struct Voice {
            int variation = 0;
            int layer = 0;
            // assets-relative
            std::string clip;
            double pitchSemitones = 0.0;
            double volumeDb = 0.0;
            double delayMs = 0.0;
            bool played = false;
            // why it did not (missing_clip in a game)
            std::string error;
        };

        struct Rendition {
            uint64_t playIndex = 0;
            std::vector<Voice> voices;
            // interleaved, CHANNELS per frame, at SAMPLE_RATE
            std::vector<float> samples;
            // cut at MAX_SECONDS
            bool truncated = false;
            // the rendition's loudness (S38's meter): LUFS-M max and true peak; silence is -inf-like
            double lufs = -120.0;
            double peakDbtp = -120.0;
        };

        // One clip, decoded and measured alone: Match Levels' numbers
        struct ClipLevel {
            bool loaded = false;
            double lufs = -120.0;
            double peakDbtp = -120.0;
        };

        // the `count` (1..MAX_COUNT) renditions from a fresh state under `seed`. `key` is the event's
        // GUID; a clip that does not load is a voice with its error, the other voices still play.
        // False with error when the private mixer cannot start. Any thread.
        static bool render(const SoundEventDef& event, const std::string& key, const Input& input, int count, uint64_t seed,
                           std::vector<Rendition>& out, std::string& error);
        static ClipLevel measureClip(const std::string& clip, const Input& input);

        // Match Levels (R16): each clip's volume_db set so all play at their average LUFS-M, none
        // past -1 dBTP. False + error for a layer event or when no clip can be measured.
        static bool matchLevels(SoundEventDef& event, const Input& input, std::string& error);

    };

}
