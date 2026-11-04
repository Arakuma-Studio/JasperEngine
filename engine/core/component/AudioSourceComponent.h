// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIO_SOURCE_COMPONENT_H
#define AUDIO_SOURCE_COMPONENT_H

#include "asset/AssetRef.h"
#include "Engine.h"

#include <memory>
#include <vector>

namespace SoLoud{
    class AudioSource;
}

namespace jasperengine{

    enum class AudioSourceState{
        Playing,
        Paused,
        Stopped
    };

    // How the clip is held (Unity AudioClip load types, per source here; audio-mixer spec A8).
    // Auto: files of 1 MiB and more stream, smaller ones are decoded on load.
    enum class AudioLoadType{
        Auto,
        DecompressOnLoad,
        CompressedInMemory,
        Streaming
    };

    // what a running fade does when it reaches its volume
    enum class AudioFadeEnd{
        None,
        Stop,
        Pause
    };

    // A PlayOneShot voice of a source (same output group, volume x scale, pitch, pan, position)
    struct AudioOneShot{
        unsigned int handle = 0;
        std::shared_ptr<SoLoud::AudioSource> sample;   // held while the voice plays (a scene change may drop the pool's)
        float volumeScale = 1.0f;
        uint64_t serial = 0;          // start order: the oldest is stolen first
    };

    // A one-shot whose clip is still loading (async loading): started when it is, dropped after 0.25 s
    struct AudioPendingOneShot{
        std::string filename;
        float volumeScale = 1.0f;
        double waited = 0;
    };

    // fadeIn / fadeOut / fadeTo (spec A12): the source's volume animated on the main thread
    struct AudioFade{
        bool active = false;
        double from = 0;
        double to = 0;
        double duration = 0;
        double elapsed = 0;
        bool unscaled = false;
        AudioFadeEnd end = AudioFadeEnd::None;
        // fadeIn / fadeOut put the authored volume back when they end or are cut (stop)
        bool restore = false;
        double restoreVolume = 1;
    };

    enum class AudioSourceAttenuation{
        NO_ATTENUATION,
        INVERSE_DISTANCE,
        LINEAR_DISTANCE,
        EXPONENTIAL_DISTANCE
    };

    struct JASPERENGINE_API AudioSourceComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        std::shared_ptr<SoLoud::AudioSource> sample = nullptr;   // Wav (decoded) or WavStream
        // SoundPool's generation of the sample's file: a hot-reloaded file is taken on the next play
        uint32_t sampleGeneration = 0;
        unsigned int handle = 0; //Soloud handle

        AudioSourceState state = AudioSourceState::Stopped;

        AssetRef filename;
        bool loaded = false;
        bool loadFailed = false;

        bool enableClocked = false;
        // where a 3D source was last frame (its velocity is the move from there); runtime
        Vector3 lastPosition = Vector3(0, 0, 0);
        bool positionTracked = false;
        // the velocity its voices were last given (SoLoud's, units a second)
        Vector3 lastVelocity = Vector3(0, 0, 0);

        bool startTrigger = false;
        bool stopTrigger = false;
        bool pauseTrigger = false;

        FunctionSubscribe<void()> onStart;
        FunctionSubscribe<void()> onPause;
        FunctionSubscribe<void()> onStop;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onStart); f(onPause); f(onStop); }

        double volume = 1;
        float speed = 1;
        float pan = 0; // -1 is left, 0 is middle and 1 is right
        bool looping = false;
        double loopingPoint = 0;
        bool protectVoice = false;
        bool inaudibleBehaviorMustTick = false;
        bool inaudibleBehaviorKill = false;

        // for 3D sound
        float minDistance = 1.0;
        float maxDistance = 1000.0;
        AudioSourceAttenuation attenuationModel = AudioSourceAttenuation::NO_ATTENUATION;
        float attenuationRolloffFactor = 1.0; // means that end of max distance sound will be zero
        float dopplerFactor = 1.0;

        // Mixer group (audio-mixer spec A3): its id, and its name for readability and as the
        // fallback when the id is not in the mixer; both empty = Master
        std::string outputGroup;
        std::string outputGroupName;
        AudioLoadType loadType = AudioLoadType::Auto;
        // 0 = most important .. 256 = least (Unity): the least important voice is stolen first
        int priority = 128;
        // main + one-shot voices of this source; past it the oldest one-shot is stolen
        int maxVoices = 8;
        // keeps playing while AudioListener.pause is set (a pause menu's clicks)
        bool ignoreListenerPause = false;

        // setted by system
        double length = 0;
        double playingTime = 0;

        bool needUpdate = true;

        // ---- runtime (AudioSystem; not serialized)
        std::string sampleKey;                 // the SoundPool entry of `sample` ("<file>#<type>")
        AudioLoadType loadedType = AudioLoadType::Auto;   // what `sample` was loaded as (resolved)
        std::vector<AudioOneShot> oneShots;
        std::vector<AudioPendingOneShot> pendingOneShots;
        AudioFade fade;
        bool voicePaused = false;              // the pause last applied to the voices
        bool listenerPaused = false;           // paused by AudioListener.pause
        std::string resolvedGroup;             // the group id the voices play through ("" = Master)
        bool groupWarned = false;              // the one-time "group not in the mixer" warning
        double lastVolumeApplied = -1;         // the volume last written to the voices
    };

}

#endif //AUDIO_SOURCE_COMPONENT_H
