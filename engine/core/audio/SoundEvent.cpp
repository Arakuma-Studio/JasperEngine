// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "component/AudioSourceComponent.h"

#if JASPERENGINE_SOUND_LAYER

#include "SoundEvent.h"

using namespace jasperengine;

namespace {
    struct PlayOnName {
        uint32_t flag;
        const char* name;
    };
    constexpr PlayOnName PLAY_ON_NAMES[] = {
        {AudioPlayOn_UIClick, "ui_click"},
        {AudioPlayOn_UIPress, "ui_press"},
        {AudioPlayOn_UIHover, "ui_hover"},
        {AudioPlayOn_Collision2D, "collision_2d"},
        {AudioPlayOn_Collision3D, "collision_3d"},
        {AudioPlayOn_Sensor2D, "sensor_2d"},
        {AudioPlayOn_Sensor3D, "sensor_3d"},
    };
}

const char* jasperengine::audioPlayOnName(uint32_t flag) {
    for (const PlayOnName& entry : PLAY_ON_NAMES) {
        if (entry.flag == flag) return entry.name;
    }
    return "";
}

uint32_t jasperengine::audioPlayOnFromName(const std::string& name) {
    for (const PlayOnName& entry : PLAY_ON_NAMES) {
        if (name == entry.name) return entry.flag;
    }
    return 0;
}

bool SoundEventClip::operator==(const SoundEventClip& o) const {
    return clip.path == o.clip.path && clip.guid == o.clip.guid && weight == o.weight && volumeDb == o.volumeDb &&
           pitchSemitones == o.pitchSemitones && delayMs == o.delayMs && unknownKeys == o.unknownKeys;
}

bool SoundEventSpatial::operator==(const SoundEventSpatial& o) const {
    return minDistance == o.minDistance && maxDistance == o.maxDistance && attenuation == o.attenuation &&
           rolloff == o.rolloff && doppler == o.doppler;
}

bool SoundEventDef::operator==(const SoundEventDef& o) const {
    return mode == o.mode && clips == o.clips && avoidRepeat == o.avoidRepeat && pitchMin == o.pitchMin &&
           pitchMax == o.pitchMax && volumeMin == o.volumeMin && volumeMax == o.volumeMax && delayMin == o.delayMin &&
           delayMax == o.delayMax && outputGroupId == o.outputGroupId && outputGroupName == o.outputGroupName &&
           maxInstances == o.maxInstances && steal == o.steal && cooldownMs == o.cooldownMs && priority == o.priority &&
           loop == o.loop && spatial == o.spatial && type == o.type && unknownKeys == o.unknownKeys;
}

const char* SoundEventDef::modeName(SoundEventMode mode) {
    switch (mode) {
        case SoundEventMode::Random: return "random";
        case SoundEventMode::Shuffle: return "shuffle";
        case SoundEventMode::Sequence: return "sequence";
        case SoundEventMode::Layer: return "layer";
    }
    return "random";
}

bool SoundEventDef::modeFromName(const std::string& name, SoundEventMode& mode) {
    for (SoundEventMode candidate : {SoundEventMode::Random, SoundEventMode::Shuffle, SoundEventMode::Sequence, SoundEventMode::Layer}) {
        if (name == modeName(candidate)) {
            mode = candidate;
            return true;
        }
    }
    return false;
}

const char* SoundEventDef::stealName(AudioStealPolicy steal) {
    switch (steal) {
        case AudioStealPolicy::Oldest: return "oldest";
        case AudioStealPolicy::Quietest: return "quietest";
        case AudioStealPolicy::Newest: return "newest";
        case AudioStealPolicy::None: return "none";
    }
    return "oldest";
}

bool SoundEventDef::stealFromName(const std::string& name, AudioStealPolicy& steal) {
    for (AudioStealPolicy candidate : {AudioStealPolicy::Oldest, AudioStealPolicy::Quietest, AudioStealPolicy::Newest, AudioStealPolicy::None}) {
        if (name == stealName(candidate)) {
            steal = candidate;
            return true;
        }
    }
    return false;
}

const char* SoundEventDef::attenuationName(AudioSourceAttenuation attenuation) {
    switch (attenuation) {
        case AudioSourceAttenuation::NO_ATTENUATION: return "no_attenuation";
        case AudioSourceAttenuation::INVERSE_DISTANCE: return "inverse_distance";
        case AudioSourceAttenuation::LINEAR_DISTANCE: return "linear_distance";
        case AudioSourceAttenuation::EXPONENTIAL_DISTANCE: return "exponential_distance";
    }
    return "no_attenuation";
}

bool SoundEventDef::attenuationFromName(const std::string& name, AudioSourceAttenuation& attenuation) {
    for (AudioSourceAttenuation candidate : {AudioSourceAttenuation::NO_ATTENUATION, AudioSourceAttenuation::INVERSE_DISTANCE,
                                             AudioSourceAttenuation::LINEAR_DISTANCE, AudioSourceAttenuation::EXPONENTIAL_DISTANCE}) {
        if (name == attenuationName(candidate)) {
            attenuation = candidate;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- the generator

uint64_t SoundEventRng::fnv1a64(const std::string& bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : bytes) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t SoundEventRng::splitmix64(uint64_t x) {
    x += GOLDEN;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

SoundEventRng::SoundEventRng(uint64_t seed, const std::string& key, uint64_t playIndex) {
    state = splitmix64(seed ^ fnv1a64(key) ^ (playIndex * GOLDEN));
    // xorshift never leaves 0
    if (state == 0) {
        state = GOLDEN;
    }
}

uint64_t SoundEventRng::next() {
    uint64_t x = state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

double SoundEventRng::uniform() {
    return (double)(next() >> 11) * (1.0 / 9007199254740992.0);
}

double SoundEventRng::range(double min, double max) {
    return min + uniform() * (max - min);
}

#endif // JASPERENGINE_SOUND_LAYER
