// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOIMPORT_H
#define AUDIOIMPORT_H

#include "Export.h"

#include <cstdint>
#include <optional>
#include <string>

namespace jasperengine{

    // A clip's import settings (the `.meta` section import.audioImporter, resolved to frames at the
    // file's own rate from the file's first frame). A default-constructed value is the whole file at
    // its own level, unfaded: it plays bit for bit, unwrapped.
    struct JASPERENGINE_API ClipImportSettings{
        // applied to every sample the clip gives; 0 dB leaves the samples bit for bit
        float gainDb = 0.0f;
        // frames left out at the start and at the end
        uint32_t trimHeadFrames = 0;
        uint32_t trimTailFrames = 0;
        // frames of the trimmed region shaped by a raised-cosine fade at its start and at its end
        // (clipFadeWeight; AudioClipSource clamps each to the region's length); 0: none. Fades never
        // shape a looping voice's wrap: the fade-in only before its first wrap, the fade-out only
        // on a pass that ends the region.
        uint32_t fadeInFrames = 0;
        uint32_t fadeOutFrames = 0;
        // the loop region of a looping voice: from the start frame (-1: the trimmed start) up to
        // the end frame, exclusive (-1: the trimmed end)
        int64_t loopStartFrame = -1;
        int64_t loopEndFrame = -1;
        // the music tempo (S39): beats per minute (0: no tempo), beats in a bar and beat 1's frame
        // from the file's first frame. They never change the clip's samples.
        float bpm = 0.0f;
        int beatsPerBar = 4;
        int64_t firstBeatFrame = 0;

        bool operator==(const ClipImportSettings& other) const;
        bool operator!=(const ClipImportSettings& other) const { return !(*this == other); }

        // no key differs from its default
        bool isDefault() const;
        // the clip's samples are the file's (the tempo may be set): it is not wrapped
        bool playsAsFile() const;
        // FNV-1a of what changes the samples or the beat grid: names the wrapped sample and the
        // analysis' cache file. Stable across runs, platforms and builds.
        uint64_t hash() const;
    };

    // the longest fade the importer gives (fade_in_ms, fade_out_ms)
    constexpr float CLIP_FADE_MAX_MS = 1000.0f;

    // The weight of frame k of an n-frame fade (computed in double, rounded to float once):
    // fade-in, k from the region's start: 0.5 x (1 - cos(pi x k / n)), so frame 0 is exactly 0;
    // fade-out, k from the first of the region's last n frames: 0.5 x (1 + cos(pi x (k + 1) / n)),
    // so the region's last frame is exactly 0. 1 outside the fade (k >= n, or n = 0). AudioClipSource's
    // tables and the analysis of a clip as it plays both take their weights from here.
    JASPERENGINE_API float clipFadeWeight(uint32_t k, uint32_t n, bool fadeIn);

    // The import settings of the clips that have any, by the path Audio Sources name them with:
    // the editor registers the project's for Play, a game reads je-audio-imports.json at start. Empty
    // in every project without settings: a look-up then costs one atomic load. Thread-safe (the
    // pool's load jobs read it).
    class JASPERENGINE_API AudioImportTable{
    public:
        // the export's file, an ordinary asset (loose or in the pack)
        static constexpr const char* FILE_NAME = "je-audio-imports.json";
        static constexpr const char* FORMAT = "jasperengine.audio-imports";
        static constexpr int VERSION = 1;

        // Reads the table of a game; a missing file is an empty table. False when it does not read.
        static bool load(const std::string& assetPath);
        // From the file's text (false, the table unchanged, when it is not one, with the reason).
        static bool parse(const std::string& text, std::string* error = nullptr);
        // A default value removes the clip. A change makes the clip's next voice take a new sample
        // (SoundPool::invalidate); voices playing keep theirs.
        static void set(const std::string& clip, const ClipImportSettings& settings);
        static void clear();
        static std::optional<ClipImportSettings> find(const std::string& clip);
        static bool isEmpty();
    };

}

#endif /* AUDIOIMPORT_H */
