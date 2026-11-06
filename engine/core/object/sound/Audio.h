// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Audio_h
#define Audio_h

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "object/sound/SoundHandle.h"
#include "util/FunctionSubscribe.h"

#include <optional>
#include <string>

namespace jasperengine {

    class EntityHandle;
    class Scene;

    // Audio::play's options (docs/specs/S37-sound-events.md R13)
    struct JASPERENGINE_API PlayOptions {
        // 3D, following a GameObject; stops when it is destroyed
        Scene* targetScene = nullptr;
        Entity target = NULL_ENTITY;
        // 3D, at a fixed place
        std::optional<Vector3> position;
        // the owner, with whose pause it holds and with which it stops; default the target's, else the main scene
        Scene* scene = nullptr;
        // linear, as AudioSource.volume
        float volume = 1.0f;
        // relative speed, as AudioSource.pitch
        float pitch = 1.0f;
        // seconds before it starts
        float delay = 0.0f;
        // a group name, as AudioSource.outputGroup; default the event's, else Master
        std::string group;
        // the delay runs on unscaled time
        bool unscaled = false;

        PlayOptions& following(const EntityHandle& gameObject);
    };

    // Audio::playMusic's options (docs/specs/S39-music.md R10, R19)
    struct JASPERENGINE_API MusicOptions {
        // seconds of unscaled time of the equal-power crossfade from the switch point; 0 cuts
        float crossfade = 0.0f;
        // the switch point: "now", or the playing track's next "beat", "bar" or "section_end"
        std::string sync = "now";
        // a mixer snapshot reached at the switch point, over snapshotSeconds (< 0: the crossfade's)
        std::string snapshot;
        float snapshotSeconds = -1.0f;
        // a group name; default the event's output, else Project Settings audio.music_group, else Master
        std::string group;
        // linear, on top of the event's volume
        float volume = 1.0f;
        // plays on while AudioListener.pause holds the game's sounds
        bool ignoreListenerPause = false;
    };

    // where the music is (Audio::getMusicPosition): bar and beat 1-based (0 before the first beat),
    // the section 1-based
    struct JASPERENGINE_API MusicPosition {
        int section = 0;
        int bar = 0;
        int beat = 0;
        int beatsPerBar = 4;
        double beatFraction = 0.0;
        double bpm = 0.0;
    };

    // Free voices (with SoundHandle): sounds a game starts from code with no AudioSource.
    // Inside a physics callback or off the main thread a call defers to the frame's audio update:
    // its handle is valid at once, and invalid from then if nothing started.
    class JASPERENGINE_API Audio {
    public:
        // A Sound Event (GUID or path) or a clip path. An unknown name gives an invalid handle and
        // one warning per name per Play.
        static SoundHandle play(const std::string& eventOrClip, const PlayOptions& options = PlayOptions());
        // Stops the voices started by Audio.play, triggers and one-shots: all, or those routed to
        // `group` or a group below it. The count stopped (0 when deferred). AudioSystem.stopAll
        // stops the sources' main voices too.
        static int stopAll(const std::string& group = "");

        // ---- the music (docs/specs/S39-music.md): one track, a clip or a Sound Event as a playlist
        // Starts or switches the music; the playing track again does nothing. False with one warning
        // for a track that cannot play as music.
        static bool playMusic(const std::string& eventOrClip, const MusicOptions& options = MusicOptions());
        // a linear fade to silence, then it stops; sync: when the fade starts, as playMusic's
        static void stopMusic(float fade = 0.0f, const std::string& sync = "now");
        // the current section ends its loop at its next loop end; false without music
        static bool advanceMusic();
        // a clip or event on the next "now", "beat" or "bar" of the music, owned by the music: a
        // scene load does not cut it. Without music or tempo it plays now, warned once.
        static SoundHandle playStinger(const std::string& eventOrClip, const std::string& sync = "beat");
        // the track playing (the incoming one during a crossfade), "" without music
        static std::string getMusic();
        // seconds of the track played so far, all sections
        static double getMusicTime();
        // nothing without music or without a tempo (the clip's import settings music.bpm)
        static std::optional<MusicPosition> getMusicPosition();
        // (bar, beat) and (section, clip), on the main thread in the frame whose mix holds the beat or
        // the section's start: frame-accurate, not sample-accurate
        static FunctionSubscribe<void(int, int)> onMusicBeat;
        static FunctionSubscribe<void(int, const std::string&)> onMusicSection;
    };

}

#endif /* Audio_h */
