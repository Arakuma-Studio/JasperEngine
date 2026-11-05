// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "asset/AssetRef.h"
#include "audio/AudioWaveform.h"
#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace SoLoud {
    class AudioSource;
}

namespace jasperengine::editor {

    class Project;

    // What the Animation window shows of a timeline sound (docs/specs/S40-timeline-audio.md R9, R10):
    // its played length at speed 1, its waveform and its peak, from S36's cached analysis and
    // overview. A clip is analysed on a worker (never on the drawing thread); a Sound Event reads its
    // file and takes its clips' numbers: its longest clip (layer mode: the longest delay + length), its
    // first clip's waveform and peak (plus that clip's delay in layer mode).
    struct TimelineSoundInfo {
        // still being analysed: draw a placeholder
        bool pending = true;
        std::string error;
        bool event = false;
        // an event with several clips: its block shows a stacked outline
        bool stacked = false;
        // seconds; 0 when unknown
        double length = 0.0;
        // the peak from the start of what plays (the trimmed head taken off), ms; < 0 when unknown
        double peakMs = -1.0;
        // the file whose waveform the block shows (absolute; an event: its first clip), and the
        // second of that file at the block's start (its trimmed head; minus a layer's delay)
        std::filesystem::path waveFile;
        double waveOffset = 0.0;
    };

    class TimelineSounds {
    public:
        // an assets-relative clip or .soundevent path; main thread
        static TimelineSoundInfo get(Project* project, const std::string& assetPath);
        // its waveform (pending while the overview is made)
        static AudioWaveform::Result waveform(Project* project, const TimelineSoundInfo& info);
        // the start time that puts the peak (R10's Align Peak) on `playhead`; false (and why) when
        // the sound has no known peak
        static bool peakAlignedTime(const TimelineSoundInfo& info, float playhead, float& time, std::string* why);
        // A sound's block between min and max (R9): its colour, the waveform's min / max between its
        // edges (`secondsPerPixel` of the sound), the peak tick, a stacked outline for an event of
        // several clips, its label clipped inside. True when the waveform was drawn.
        static bool drawBlock(ImDrawList* drawList, Project* project, const TimelineSoundInfo& info, ImVec2 min, ImVec2 max,
                              double secondsPerPixel, ImU32 fill, bool selected, const std::string& label);
        // an assets-relative sound as a reference: its GUID from the asset database
        static AssetRef reference(Project* project, const std::string& assetPath);
        // the project's sounds (audio clips and Sound Events), assets-relative, sorted; `needle`
        // (lower case) filters by path
        static std::vector<std::string> projectSounds(Project* project, const std::string& needle);
        static void clear();
    };

    // The editor preview of timeline audio (R11, R23): while the Animation window's preview plays,
    // each crossed cue or audio action starts a preview voice, placed inside the frame, outside the
    // mixer's groups (as the Sound Browser's auditions). Snapshots preview nothing, scrubbing is
    // silent, and nothing plays while Play captures audio (AudioCapture): its mix is the game's.
    class TimelineAudioPreview {
    public:
        ~TimelineAudioPreview();

        // "Audio preview is off while Play captures audio" while it is off, else ""
        static std::string blocker();

        // A voice of `assetPath` (a clip, or one pick of a Sound Event), `delayFrames` output
        // frames into the next mix; `exitStop` voices stop on stopExit(). False when nothing played.
        bool play(Project* project, const std::string& assetPath, float volumeDb, float pitch, uint32_t delayFrames, bool exitStop);
        // the state_exit voices stop with a 10 ms fade (the preview stopped, looped or was sought)
        void stopExit();
        void stopAll();
        // the preview voices playing now (tests, the window's status)
        int voiceCount();
        // the voices started since the last reset (tests)
        uint64_t startedCount() const { return started; }
        void resetCount() { started = 0; }

        // the frames a crossing `seconds` into an update of `span` seconds waits, at the output rate
        static uint32_t delayFrames(double seconds, double span, double frameSeconds);

    private:
        struct Voice {
            unsigned int handle = 0;
            std::shared_ptr<SoLoud::AudioSource> sample;
            bool exitStop = false;
        };
        std::vector<Voice> voices;
        uint64_t started = 0;
        // a Sound Event's pick counter: each play draws the next pick
        uint64_t picks = 0;
        void prune();
    };

}
