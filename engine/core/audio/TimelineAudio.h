// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TIMELINE_AUDIO_H
#define TIMELINE_AUDIO_H

#include "Export.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <optional>
#include <string>

namespace jasperengine {

    class Scene;
    struct ClipAudioCue;

    // Where a timeline sound came from: the capture's `cue` line (docs/specs/S40-timeline-audio.md
    // R8) and the one-per-Play warnings
    struct JASPERENGINE_API TimelineCueInfo {
        // "clip": a cue of a .anim (Animation Player, Animator, montage); "actions": an audio or
        // snapshot action of a legacy Animation
        const char* timeline = "clip";
        // the .anim (clip), or the Animation's GameObject name (actions)
        std::string clip;
        // the cue's index in the clip, or the frame's index in the Animation
        int cue = -1;
        Scene* scene = nullptr;
        // the Animation Player, Animator or Animation GameObject
        Entity player = NULL_ENTITY;
        std::string state;
        int layer = -1;
        // where it was crossed on the state's time line (seconds)
        double position = 0.0;
    };

    // A voice a timeline started, kept by its player for a state_exit stop
    struct JASPERENGINE_API TimelineVoice {
        // a sound cue's free voice (SoundHandle's id and generation)
        uint32_t id = 0;
        uint32_t generation = 0;
        // a source cue's AudioSource
        Entity source = NULL_ENTITY;

        bool empty() const { return id == 0 && source == NULL_ENTITY; }
    };

    // The one start path of timeline sound (R6, R19, R26): clip cues and the legacy timeline's
    // audio and snapshot actions place, start, stop and log their sounds here, so the same
    // footsteps placed either way give the same samples. Main thread. Part of the soundlayer
    // module: callers guard with JASPERENGINE_SOUND_LAYER.
    class JASPERENGINE_API TimelineAudio {
    public:
        // R5: the output frames a start waits inside the frame when crossed at `fraction` of it
        // (clamped to [0, 1]): round(fraction x the frame's unscaled seconds x the output rate)
        static uint32_t delayFrames(double fraction);
        // the same for a crossing `seconds` into an update of `span` seconds (any scale)
        static uint32_t delayFramesAt(double seconds, double span);

        // Fires one crossed cue of a clip playing on `root` (R6): a sound cue as a free voice, a
        // source cue through its AudioSource, a snapshot cue through the mixer. stateSpeed scales
        // a follow_speed cue's pitch. The voice a state_exit cue started, else an empty one.
        static TimelineVoice fireCue(Scene* scene, Entity root, const ClipAudioCue& cue, uint32_t delay, float stateSpeed,
                                     const TimelineCueInfo& info);
        // The capture's `cue` line of a start (R8): fireCue writes its own; audio and snapshot
        // actions write theirs here (kind "source" or "snapshot")
        static void logStart(const TimelineCueInfo& info, const char* kind, const std::string& name, uint32_t delay);
        // A state_exit stop (R7): a 10 ms fade, reason cue_exit
        static void stop(Scene* scene, const TimelineVoice& voice);

        // An audio action's source (R19): starts its main voice placed `delay` frames into the
        // frame at `speedScale` (a Paused source resumes, ignoring the delay); `target` is where a
        // 3D action without a Transform plays. False when the GameObject has no AudioSource.
        static bool startSource(Scene* scene, Entity source, uint32_t delay, float speedScale, Entity target, const TimelineCueInfo& info);
        static void pauseSource(Scene* scene, Entity source);
        // stops it with the cue_exit fade
        static void stopSource(Scene* scene, Entity source);
        // whether its voice still plays (or is about to start)
        static bool isSourcePlaying(Scene* scene, Entity source);

        // A snapshot cue or action (R6, R19): AudioMixer::transitionTo with cause "animation" in
        // the capture; an unknown snapshot warns once per Play
        static bool transition(const std::string& snapshot, float seconds, std::optional<bool> unscaled, const TimelineCueInfo& info);

        // The sound's played length at speed 1 (seconds; R9, R21): a clip's trimmed region, an
        // event's longest clip (layer mode: max(delay + length)); 0 when it does not resolve
        static double soundLength(const std::string& eventOrClip);

        // Play start and stop: the one-per-Play warnings again
        static void resetRuntime();
    };

}

#endif //TIMELINE_AUDIO_H
