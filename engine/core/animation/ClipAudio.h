// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S40-timeline-audio.md R1-R4)
// - Purpose: an animation clip's audio cues (the `.anim` "audio" array): at a time in the clip,
//   play a clip or a Sound Event (2D or following a GameObject of the animated hierarchy), start an
//   AudioSource of that hierarchy, or move the mixer to a snapshot. AnimationClipIO reads, checks
//   and writes them; the Animation Player, the Animator and montages fire them through
//   anim::collectCrossedCues with the clip events' crossing rules.
// - Thread-safety: plain values.
// - Invariants of a ClipData out of the loader: cues sorted by time (then kind, then their file
//   order), times on the 1e-6 s grid and >= 0, every cue of one kind with its own field set
//   (validateCue), volume and pitch inside their ranges.

#ifndef CLIP_AUDIO_H
#define CLIP_AUDIO_H

#include "Export.h"
#include "asset/AssetRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace jasperengine{

    enum class ClipCueKind : uint8_t { Sound, Source, Snapshot };
    // None: plays to its end. StateExit: stopped with a 10 ms fade when the state that fired it
    // stops, is left or finishes its crossfade out (a source cue stops its source)
    enum class ClipCueStop : uint8_t { None, StateExit };

    struct JASPERENGINE_API ClipAudioCue{
        float time = 0.0f;
        ClipCueKind kind = ClipCueKind::Sound;
        // sound: an audio clip or a .soundevent
        AssetRef sound;
        // sound: the GameObject a spatial cue follows; source: the GameObject with the AudioSource.
        // Relative to the animated root ("" = the root), the binding grammar of tracks
        std::string path;
        // sound: a mixer group name ("" = the event's, else Master)
        std::string group;
        // snapshot: a snapshot of the project's mixer
        std::string snapshot;
        // a label for the editor and the capture lines
        std::string name;
        bool spatial = false;
        // the voice's pitch also follows the state's speed
        bool followSpeed = false;
        // snapshot: unset = the mixer's update mode
        std::optional<bool> unscaled;
        float volumeDb = 0.0f;
        float pitchSemitones = 0.0f;
        // snapshot: the transition time
        float seconds = 0.0f;
        ClipCueStop stop = ClipCueStop::None;

        bool operator==(const ClipAudioCue& other) const;
        bool operator!=(const ClipAudioCue& other) const { return !(*this == other); }
    };

    namespace anim{
        constexpr float CUE_MIN_VOLUME_DB = -60.0f;
        constexpr float CUE_MAX_VOLUME_DB = 12.0f;
        constexpr float CUE_MAX_PITCH_SEMITONES = 24.0f;
        // the fade of a state_exit stop
        constexpr float CUE_EXIT_FADE_SECONDS = 0.01f;

        // "sound", "source", "snapshot"
        JASPERENGINE_API const char* cueKindName(ClipCueKind kind);
        JASPERENGINE_API bool cueKindFromName(const std::string& name, ClipCueKind& out);
        // "none", "state_exit"
        JASPERENGINE_API const char* cueStopName(ClipCueStop stop);
        JASPERENGINE_API bool cueStopFromName(const std::string& name, ClipCueStop& out);
        // What makes a cue unplayable, "" when none: a sound cue without a sound, a source cue
        // without a path... and a path the binding grammar does not read. Ranges are clamped by
        // the loader, not refused.
        JASPERENGINE_API std::string validateCue(const ClipAudioCue& cue);
        // the label the editor, the capture and the warnings show: its name, else what it plays
        JASPERENGINE_API std::string cueLabel(const ClipAudioCue& cue);
    }

}

#endif //CLIP_AUDIO_H
