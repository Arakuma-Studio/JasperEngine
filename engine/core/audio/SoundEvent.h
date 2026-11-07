// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SoundEvent_h
#define SoundEvent_h

#include "Export.h"
#include "asset/AssetRef.h"
#include "component/AudioSourceComponent.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine {

    // the scene and MCP name of one AudioPlayOn flag (ui_click ... sensor_3d); "" for none or several
    JASPERENGINE_API const char* audioPlayOnName(uint32_t flag);
    // a flag by its name; 0 for an unknown name
    JASPERENGINE_API uint32_t audioPlayOnFromName(const std::string& name);

    // How a Sound Event picks its clips (Unity's Audio Random Container, docs/specs/S37-sound-events.md
    // R1): a weighted pick, a shuffle bag, the list in order, or every clip at once
    enum class SoundEventMode {
        Random,
        Shuffle,
        Sequence,
        Layer
    };

    // which live instance gives way when an event is at its max_instances
    enum class AudioStealPolicy {
        Oldest,
        Quietest,
        Newest,
        None
    };

    // keys of a file this engine does not know, as read (compact JSON), written back after the known ones
    using SoundEventUnknownKeys = std::vector<std::pair<std::string, std::string>>;

    struct JASPERENGINE_API SoundEventClip {
        // assets-relative, as AudioSourceComponent::filename; the GUID decides when it resolves
        AssetRef clip;
        // random mode only
        float weight = 1.0f;
        float volumeDb = 0.0f;
        float pitchSemitones = 0.0f;
        float delayMs = 0.0f;
        // music only (S39): passes of its loop region a section plays, counting the first; 0 = until
        // Audio.advanceMusic
        int loops = 1;
        SoundEventUnknownKeys unknownKeys;

        bool operator==(const SoundEventClip& o) const;
        bool operator!=(const SoundEventClip& o) const { return !(*this == o); }
    };

    // replaces the playing source's 3D settings when an event has it
    struct JASPERENGINE_API SoundEventSpatial {
        float minDistance = 1.0f;
        float maxDistance = 1000.0f;
        AudioSourceAttenuation attenuation = AudioSourceAttenuation::NO_ATTENUATION;
        float rolloff = 1.0f;
        float doppler = 1.0f;

        bool operator==(const SoundEventSpatial& o) const;
        bool operator!=(const SoundEventSpatial& o) const { return !(*this == o); }
    };

    // A .soundevent as the engine plays it (SoundEventIO reads and writes it). The file holds no
    // GUID: that lives in its .meta (S04), SoundEventLibrary finds it through AssetMap.
    struct JASPERENGINE_API SoundEventDef {
        SoundEventMode mode = SoundEventMode::Random;
        std::vector<SoundEventClip> clips;
        // random mode: the clip picked last is left out of the next pick
        bool avoidRepeat = true;
        // each play's draws, uniform in [min, max]
        float pitchMin = 0.0f, pitchMax = 0.0f;
        float volumeMin = 0.0f, volumeMax = 0.0f;
        float delayMin = 0.0f, delayMax = 0.0f;
        // both empty = the playing source's group (an Audio.play voice: its group option, else Master)
        std::string outputGroupId, outputGroupName;
        // instances at once, over every scene; 0 = no limit
        int maxInstances = 0;
        AudioStealPolicy steal = AudioStealPolicy::Oldest;
        float cooldownMs = 0.0f;
        // 0..256; 128 = the playing source's
        int priority = 128;
        bool loop = false;
        std::optional<SoundEventSpatial> spatial;
        // a sound type of the library's vocabulary ("" = none)
        std::string type;
        SoundEventUnknownKeys unknownKeys;

        bool operator==(const SoundEventDef& o) const;
        bool operator!=(const SoundEventDef& o) const { return !(*this == o); }

        static const char* modeName(SoundEventMode mode);
        static bool modeFromName(const std::string& name, SoundEventMode& mode);
        static const char* stealName(AudioStealPolicy steal);
        static bool stealFromName(const std::string& name, AudioStealPolicy& steal);
        // "no_attenuation", "inverse_distance", "linear_distance", "exponential_distance"
        static const char* attenuationName(AudioSourceAttenuation attenuation);
        static bool attenuationFromName(const std::string& name, AudioSourceAttenuation& attenuation);
    };

    // The generator of a play, exactly (tools/mcp/audio_refs.py reproduces every draw): its state
    // is splitmix64(seed ^ fnv1a64(key) ^ playIndex * golden), its draws xorshift64*, each uniform
    // u = (next() >> 11) x 2^-53 in [0, 1). Draw order: the pick, then pitch, volume, delay.
    class JASPERENGINE_API SoundEventRng {
    public:
        static constexpr uint64_t GOLDEN = 0x9E3779B97F4A7C15ull;

        static uint64_t fnv1a64(const std::string& bytes);
        static uint64_t splitmix64(uint64_t x);

        // key: an event's GUID, or a clip-mode source's "entity:<id>"
        SoundEventRng(uint64_t seed, const std::string& key, uint64_t playIndex);

        uint64_t next();
        double uniform();
        // min + u (max - min)
        double range(double min, double max);

    private:
        uint64_t state;
    };

}

#endif /* SoundEvent_h */
