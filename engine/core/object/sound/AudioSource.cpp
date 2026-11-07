// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioSource.h"
#include "component/Transform.h"
#include "subsystem/AudioSystem.h"
#include "asset/AssetMap.h"
#include "audio/AudioMixer.h"
#include "pool/SoundPool.h"
#include "Scene.h"

#include "soloud.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
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

    // a play() not started yet counts (Unity: true right after Play())
    if (audio.startTrigger && !audio.stopTrigger) {
        return true;
    }
    if (audio.stopTrigger) {
        return false;
    }
    // the voices: a looping sound plays on past its length, whatever its pitch (spec A15)
    if (audio.state == AudioSourceState::Playing && audio.handle != 0 && AudioSystem::isInited() &&
        AudioSystem::getSoloud().isValidVoiceHandle(audio.handle)) {
        return true;
    }
    return scene->getSystem<AudioSystem>()->getPlayingVoiceCount(audio) > 0 && audio.state != AudioSourceState::Paused;
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

void AudioSource::setOutputGroup(const std::string& groupName){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    audio.outputGroupName = groupName;
    audio.outputGroup = AudioMixer::findGroupId(groupName);
    audio.groupWarned = false;
}

std::string AudioSource::getOutputGroup() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    if (!audio.outputGroup.empty()) {
        const std::string name = AudioMixer::getGroupName(audio.outputGroup);
        if (!name.empty()) return name;
    }
    return audio.outputGroupName;
}

void AudioSource::setLoadType(AudioLoadType loadType){
    getComponent<AudioSourceComponent>().loadType = loadType;
}

AudioLoadType AudioSource::getLoadType() const{
    return getComponent<AudioSourceComponent>().loadType;
}

AudioLoadType AudioSource::getResolvedLoadType() const{
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();
    if (audio.filename.empty()) return audio.loadType;
    return SoundPool::resolveLoadType(audio.filename, audio.loadType);
}

void AudioSource::setPriority(int priority){
    getComponent<AudioSourceComponent>().priority = std::clamp(priority, 0, 256);
}

int AudioSource::getPriority() const{
    return getComponent<AudioSourceComponent>().priority;
}

void AudioSource::setMaxVoices(int maxVoices){
    getComponent<AudioSourceComponent>().maxVoices = std::clamp(maxVoices, 1, 64);
}

int AudioSource::getMaxVoices() const{
    return getComponent<AudioSourceComponent>().maxVoices;
}

void AudioSource::setIgnoreListenerPause(bool ignore){
    getComponent<AudioSourceComponent>().ignoreListenerPause = ignore;
}

bool AudioSource::isIgnoreListenerPause() const{
    return getComponent<AudioSourceComponent>().ignoreListenerPause;
}

void AudioSource::setPitch(float pitch){
    setSpeed(pitch);
}

float AudioSource::getPitch() const{
    return getSpeed();
}

#if JASPERENGINE_SOUND_LAYER
void AudioSource::setEvent(const std::string& pathOrGuid){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();
    AssetRef ref;
    if (Guid::isValidText(pathOrGuid)) {
        ref.guid = Guid::parse(pathOrGuid);
        AssetMap::findPath(ref.guid, ref.path);
    } else {
        ref.path = pathOrGuid;
        ref.guid = AssetMap::findGuid(pathOrGuid);
    }
    audio.event = ref;
    audio.needUpdate = true;
}

std::string AudioSource::getEvent() const{
    return getComponent<AudioSourceComponent>().event.path;
}

void AudioSource::setPlayOn(uint32_t flags){
    getComponent<AudioSourceComponent>().playOn = flags & AudioPlayOn_All;
}

uint32_t AudioSource::getPlayOn() const{
    return getComponent<AudioSourceComponent>().playOn;
}

void AudioSource::setMinImpactSpeed(float speed){
    if (!std::isfinite(speed)) {
        Log::warn("AudioSource.minImpactSpeed: not a number, ignored");
        return;
    }
    getComponent<AudioSourceComponent>().minImpactSpeed = std::max(0.0f, speed);
}

float AudioSource::getMinImpactSpeed() const{
    return getComponent<AudioSourceComponent>().minImpactSpeed;
}

void AudioSource::setFullVolumeImpactSpeed(float speed){
    if (!std::isfinite(speed)) {
        Log::warn("AudioSource.fullVolumeImpactSpeed: not a number, ignored");
        return;
    }
    getComponent<AudioSourceComponent>().fullVolumeImpactSpeed = std::max(0.0f, speed);
}

float AudioSource::getFullVolumeImpactSpeed() const{
    return getComponent<AudioSourceComponent>().fullVolumeImpactSpeed;
}

void AudioSource::setPitchRandomSemitones(float semitones){
    if (!std::isfinite(semitones)) {
        Log::warn("AudioSource.pitchRandomSemitones: not a number, ignored");
        return;
    }
    getComponent<AudioSourceComponent>().pitchRandomSemitones = std::clamp(semitones, 0.0f, 24.0f);
}

float AudioSource::getPitchRandomSemitones() const{
    return getComponent<AudioSourceComponent>().pitchRandomSemitones;
}

void AudioSource::setVolumeRandomDb(float db){
    if (!std::isfinite(db)) {
        Log::warn("AudioSource.volumeRandomDb: not a number, ignored");
        return;
    }
    getComponent<AudioSourceComponent>().volumeRandomDb = std::clamp(db, 0.0f, 60.0f);
}

float AudioSource::getVolumeRandomDb() const{
    return getComponent<AudioSourceComponent>().volumeRandomDb;
}

#endif // JASPERENGINE_SOUND_LAYER

void AudioSource::playOneShot(const std::string& filename, float volumeScale){
    AudioSourceComponent& audio = getComponent<AudioSourceComponent>();

    scene->getSystem<AudioSystem>()->playOneShot(audio, filename, volumeScale);
}

void AudioSource::fadeIn(double seconds, bool unscaled){
    scene->getSystem<AudioSystem>()->fadeIn(getComponent<AudioSourceComponent>(), seconds, unscaled);
}

void AudioSource::fadeOut(double seconds, bool pause, bool unscaled){
    scene->getSystem<AudioSystem>()->fadeOut(getComponent<AudioSourceComponent>(), seconds, pause, unscaled);
}

void AudioSource::fadeTo(double volume, double seconds, bool unscaled){
    scene->getSystem<AudioSystem>()->fadeTo(getComponent<AudioSourceComponent>(), volume, seconds, unscaled);
}

bool AudioSource::isFading() const{
    return getComponent<AudioSourceComponent>().fade.active;
}

int AudioSource::getVoiceCount() const{
    return scene->getSystem<AudioSystem>()->getPlayingVoiceCount(getComponent<AudioSourceComponent>());
}

void AudioSource::playClipAtPoint(Scene* scene, const std::string& filename, Vector3 position, float volume, const std::string& group){
    if (!scene) {
        Log::error("AudioSource.playClipAtPoint: no scene");
        return;
    }
    scene->getSystem<AudioSystem>()->playClipAtPoint(filename, position, volume, group);
}
