// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioSource.h"
#include "subsystem/AudioSystem.h"
#include "pool/SoundPool.h"

#include "Log.h"

#include <stdexcept>


using namespace jasperengine;

AudioSource::AudioSource(Scene* scene): EntityHandle(scene){
    addComponent<AudioSourceComponent>();
}

AudioSource::AudioSource(Scene* scene, bool is3D): EntityHandle(scene){
    addComponent<AudioSourceComponent>();
    if (is3D){
        addComponent<Transform>();
    }
}

AudioSource::AudioSource(Scene* scene, Entity entity): EntityHandle(scene, entity){
}


AudioSource::~AudioSource(){

}

AudioSource::AudioSource(const AudioSource& rhs) = default;
AudioSource& AudioSource::operator=(const AudioSource& rhs) = default;
AudioSource::AudioSource(AudioSource&& rhs) noexcept = default;
AudioSource& AudioSource::operator=(AudioSource&& rhs) noexcept = default;

GameObject AudioSource::getGameObject() const{
    if (!isSound3D()){
        Log::error("Cannot get object from 2D sound: enable 3D sound first");
        throw std::runtime_error("Cannot get object from 2D sound");
    }

    return GameObject(scene, entity);
}

int AudioSource::loadSound(const std::string& filename){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    // the loaded sample would keep playing the old file
    if (!audio.filename.empty() && audio.filename != filename) {
        scene->getSystem<AudioSystem>()->destroySound(audio);
    }

    audio.filename = filename;
    SoundPool::retry(filename);

    if (Engine::isViewLoaded())
        return scene->getSystem<AudioSystem>()->loadSound(audio);
    else
        return false;
}

void AudioSource::destroySound(){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    scene->getSystem<AudioSystem>()->destroySound(audio);
}

void AudioSource::play(){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    audio.startTrigger = true;
    audio.pauseTrigger = false;
    audio.stopTrigger = false;
}

void AudioSource::pause(){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    audio.startTrigger = false;
    audio.pauseTrigger = true;
    audio.stopTrigger = false;
}

void AudioSource::stop(){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    audio.startTrigger = false;
    audio.pauseTrigger = false;
    audio.stopTrigger = true;
}

void AudioSource::seek(double time){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    scene->getSystem<AudioSystem>()->seekSound(audio, time);
}

double AudioSource::getLength(){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.length;
}

double AudioSource::getPlayingTime(){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.playingTime;
}

bool AudioSource::isPlaying(){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if ((audio.length > audio.playingTime) && (audio.playingTime != 0) && (audio.state == AudioSourceState::Playing)) {
        return true;
    }
    return false;
}

bool AudioSource::isPaused(){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return (audio.state == AudioSourceState::Paused);
}

bool AudioSource::isStopped(){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return (audio.state == AudioSourceState::Stopped);
}

void AudioSource::setSound3D(bool sound3D){
    bool hasTransform = scene->findComponent<Transform>(entity) != nullptr;
    if (sound3D && !hasTransform){
        addComponent<Transform>();
    }else if (!sound3D && hasTransform){
        removeComponent<Transform>();
    }

    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();
    audio.needUpdate = true;
    markChanged<AudioSourceComponent>();
}

bool AudioSource::isSound3D() const{
    return scene->findComponent<Transform>(entity) != nullptr;
}

void AudioSource::setClockedSound(bool enableClocked){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    audio.enableClocked = enableClocked;
}

bool AudioSource::isClockedSound() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.enableClocked;
}

void AudioSource::setVolume(double volume){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.volume != volume){
        audio.volume = volume;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

double AudioSource::getVolume() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.volume;
}

void AudioSource::setSpeed(float speed){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.speed != speed){
        audio.speed = speed;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

float AudioSource::getSpeed() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.speed;
}

void AudioSource::setPan(float pan){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.pan != pan){
        audio.pan = pan;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

float AudioSource::getPan() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.pan;
}

void AudioSource::setLooping(bool looping){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.looping != looping){
        audio.looping = looping;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

bool AudioSource::isLooping() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.looping;
}

void AudioSource::setLoopingPoint(double loopingPoint){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.loopingPoint != loopingPoint){
        audio.loopingPoint = loopingPoint;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

double AudioSource::getLoopingPoint() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.loopingPoint;
}

void AudioSource::setProtectVoice(bool protectVoice){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.protectVoice != protectVoice){
        audio.protectVoice = protectVoice;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

bool AudioSource::isProtectVoice() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.protectVoice;
}

void AudioSource::setInaudibleBehavior(bool mustTick, bool kill){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.inaudibleBehaviorMustTick != mustTick || audio.inaudibleBehaviorKill != kill){
        audio.inaudibleBehaviorMustTick = mustTick;
        audio.inaudibleBehaviorKill = kill;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

void AudioSource::setMinMaxDistance(float minDistance, float maxDistance){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.minDistance != minDistance || audio.maxDistance != maxDistance){
        audio.minDistance = minDistance;
        audio.maxDistance = maxDistance;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

void AudioSource::setMinDistance(float minDistance){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.minDistance != minDistance){
        audio.minDistance = minDistance;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

float AudioSource::getMinDistance() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.minDistance;
}

void AudioSource::setMaxDistance(float maxDistance){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.maxDistance != maxDistance){
        audio.maxDistance = maxDistance;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

float AudioSource::getMaxDistance() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.maxDistance;
}

void AudioSource::setAttenuationModel(AudioSourceAttenuation attenuationModel){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.attenuationModel != attenuationModel){
        audio.attenuationModel = attenuationModel;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

AudioSourceAttenuation AudioSource::getAttenuationModel() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.attenuationModel;
}

void AudioSource::setAttenuationRolloffFactor(float attenuationRolloffFactor){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.attenuationRolloffFactor != attenuationRolloffFactor){
        audio.attenuationRolloffFactor = attenuationRolloffFactor;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

float AudioSource::getAttenuationRolloffFactor() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.attenuationRolloffFactor;
}

void AudioSource::setDopplerFactor(float dopplerFactor){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (audio.dopplerFactor != dopplerFactor){
        audio.dopplerFactor = dopplerFactor;
        audio.needUpdate = true;
        markChanged<AudioSourceComponent>();
    }
}

float AudioSource::getDopplerFactor() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    return audio.dopplerFactor;
}

void AudioSource::setEnabled(bool enabled){
    getComponent<AudioSourceComponent>().enabled = enabled;
}

bool AudioSource::isEnabled() const{
    return getComponent<AudioSourceComponent>().enabled;
}
