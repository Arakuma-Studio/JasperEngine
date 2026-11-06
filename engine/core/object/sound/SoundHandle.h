// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SoundHandle_h
#define SoundHandle_h

#include "Export.h"
#include "math/Vector3.h"

#include <cstdint>

namespace jasperengine {

    // What Audio::play returns (docs/specs/S37-sound-events.md R13): a value naming one play, an
    // event instance (a layered one has several voices) or a clip. Copy it freely. Once every voice
    // of the play ended its entry is reused under a new generation, so a kept handle is then simply
    // invalid: getters give 0 (the zero vector), setters do nothing.
    class JASPERENGINE_API SoundHandle {
    public:
        SoundHandle() = default;
        SoundHandle(uint32_t id, uint32_t generation);

        // any voice of it still plays or is held, or it is about to start
        bool isValid() const;
        // valid and not paused by its own pause (a pause of the game may still hold it)
        bool isPlaying() const;
        void stop();
        // its own pause, on top of the scene's, the listener's and the game's
        void pause();
        void resume();
        // to silence over `seconds`, then stops (pause: pauses); the volume is put back
        void fadeOut(float seconds, bool pause = false, bool unscaled = false);
        // the volume moves to `volume` over `seconds`
        void fadeTo(float volume, float seconds, bool unscaled = false);

        // linear, on every voice (as AudioSource.volume)
        float getVolume() const;
        void setVolume(float volume);
        // relative speed, on every voice (as AudioSource.pitch)
        float getPitch() const;
        void setPitch(float pitch);
        // -1 left .. 1 right; for 2D voices
        float getPan() const;
        void setPan(float pan);
        // 3D voices; setting it stops following a GameObject
        Vector3 getPosition() const;
        void setPosition(const Vector3& position);
        // seconds into its first valid voice
        double getTime() const;
        int getVoiceCount() const;

        uint32_t getId() const { return id; }
        uint32_t getGeneration() const { return generation; }

        bool operator==(const SoundHandle& o) const { return id == o.id && generation == o.generation; }
        bool operator!=(const SoundHandle& o) const { return !(*this == o); }

    private:
        // 0: no play
        uint32_t id = 0;
        uint32_t generation = 0;
    };

}

#endif /* SoundHandle_h */
