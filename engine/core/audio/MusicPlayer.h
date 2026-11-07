// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MUSICPLAYER_H
#define MUSICPLAYER_H

#include "Export.h"
#include "object/sound/Audio.h"

#include <optional>
#include <string>

namespace jasperengine{

    // The game's music, what Unity games write a music manager for (docs/specs/S39-music.md). One
    // track at a time: a clip (one section repeating its loop region until Audio.advanceMusic), or a
    // Sound Event played as a playlist (its clips are the sections, each repeating its loop region
    // `loops` times, picked by the event's mode; the event's loop repeats the playlist). A switch
    // crossfades with equal-power curves now or on the next beat, bar or section end of the playing
    // track, optionally moving the mixer to a snapshot at the switch; stingers land on the next beat
    // or bar. Its voices belong to AudioSystem's scene-less music system: they play through a mixer
    // group like any voice (ducking, faders, snapshots apply) and survive every scene load.
    // Main thread; Audio's music functions validate and defer, then call these.
    class JASPERENGINE_API MusicPlayer{
    public:
        // get_play_state's audio.music
        struct Status{
            bool playing = false;
            std::string track;
            int section = -1;
            std::string clip;
            double timeSeconds = 0.0;
            int bar = 0;
            int beat = 0;
            double bpm = 0.0;
            bool crossfading = false;
            // the group its voice plays through, by name ("Master")
            std::string group;
        };

        // starts or switches the music. False, with one warning naming why, for a track that does
        // not resolve or cannot play as music (a layer event, sections that differ in rate or channels)
        static bool play(const std::string& track, const MusicOptions& options);
        static void stop(float fadeSeconds, const std::string& sync);
        static bool advance();
        static SoundHandle stinger(const std::string& eventOrClip, const std::string& sync);

        // Once a frame (Engine::runFrame, after the scenes, before the deterministic mix) with the
        // frame's unscaled time: the sections the audio thread began, the beat and section
        // callbacks of what this frame mixes, switch points, crossfade and stop segments, the next
        // section of a playlist
        static void update(double unscaledFrameSeconds);
        // Every music voice and stinger stops (reason scene), the state, the warnings and the
        // callbacks go: the editor's Play start and stop
        static void reset();
        // The audio closed: its voices are gone with it
        static void forget();
        // An editor preview (the event view's Play as Music): it plays in edit mode, where the game's
        // events stay paused. Until the next reset.
        static void setPreview(bool preview);
        static bool isPreview();

        static std::string current();
        static double time();
        static std::optional<MusicPosition> position();
        static Status status();

        // "now", "beat", "bar", "section_end"
        static bool isSyncName(const std::string& name);
    };

}

#endif //MUSICPLAYER_H
