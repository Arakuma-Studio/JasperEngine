// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "component/AudioSourceComponent.h"

#if JASPERENGINE_SOUND_LAYER

#include "SoundHandle.h"

#include "subsystem/AudioSystem.h"

using namespace jasperengine;

SoundHandle::SoundHandle(uint32_t id, uint32_t generation): id(id), generation(generation) {
}

bool SoundHandle::isValid() const { return AudioSystem::freeIsValid(id, generation); }
bool SoundHandle::isPlaying() const { return AudioSystem::freeIsPlaying(id, generation); }
void SoundHandle::stop() { AudioSystem::freeStop(id, generation); }
void SoundHandle::pause() { AudioSystem::freePause(id, generation, true); }
void SoundHandle::resume() { AudioSystem::freePause(id, generation, false); }

void SoundHandle::fadeOut(float seconds, bool pause, bool unscaled) {
    AudioSystem::freeFadeOut(id, generation, seconds, pause, unscaled);
}

void SoundHandle::fadeTo(float volume, float seconds, bool unscaled) {
    AudioSystem::freeFadeTo(id, generation, volume, seconds, unscaled);
}

float SoundHandle::getVolume() const { return AudioSystem::freeGet(id, generation, AudioSystem::FreeValue::Volume); }
void SoundHandle::setVolume(float volume) { AudioSystem::freeSet(id, generation, AudioSystem::FreeValue::Volume, volume); }
float SoundHandle::getPitch() const { return AudioSystem::freeGet(id, generation, AudioSystem::FreeValue::Pitch); }
void SoundHandle::setPitch(float pitch) { AudioSystem::freeSet(id, generation, AudioSystem::FreeValue::Pitch, pitch); }
float SoundHandle::getPan() const { return AudioSystem::freeGet(id, generation, AudioSystem::FreeValue::Pan); }
void SoundHandle::setPan(float pan) { AudioSystem::freeSet(id, generation, AudioSystem::FreeValue::Pan, pan); }
Vector3 SoundHandle::getPosition() const { return AudioSystem::freeGetPosition(id, generation); }
void SoundHandle::setPosition(const Vector3& position) { AudioSystem::freeSetPosition(id, generation, position); }
double SoundHandle::getTime() const { return AudioSystem::freeGetTime(id, generation); }
int SoundHandle::getVoiceCount() const { return AudioSystem::freeGetVoiceCount(id, generation); }

#endif // JASPERENGINE_SOUND_LAYER
