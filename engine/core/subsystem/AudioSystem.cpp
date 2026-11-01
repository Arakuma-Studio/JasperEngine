// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioSystem.h"
#include "PlatformConfig.h"
#include "manager/SceneManager.h"

#include "pool/SoundPool.h"
#include "Scene.h"

#include "soloud.h"
#include "soloud_wav.h"

using namespace jasperengine;

bool AudioSystem::inited = false;
bool AudioSystem::outputUnavailable = false;
bool AudioSystem::offlineCapture = false;

float AudioSystem::globalVolume = 1.0;

AudioSystem::AudioSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<AudioSourceComponent>());
    // the type onComponentRemoved handles
    declareHookType(scene->getComponentId<AudioSourceComponent>());

    cameraLastPosition = Vector3(0, 0, 0);
}

SoLoud::Soloud& AudioSystem::getSoloud(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static SoLoud::Soloud* soloud = new SoLoud::Soloud();
    return *soloud;
};

bool AudioSystem::init(){
    if (!inited) {
        // Opened once: without an output device (a headless machine, a server, a CI
        // runner) every retry would probe all the audio backends again, for each sound
        if (outputUnavailable) {
            return false;
        }
        // --headless and --silent-audio open no device (docs/specs/S30-shipping.md R17)
        if (platformConfig().silentAudio) {
            outputUnavailable = true;
            Log::print("Audio output off (--silent-audio or --headless): sounds are muted");
            return false;
        }

        SoLoud::result result = getSoloud().init();
        if (result != SoLoud::SOLOUD_ERRORS::SO_NO_ERROR) {
            outputUnavailable = true;
            Log::warn("No audio output available (%s): sounds are muted", getSoloud().getErrorString(result));
            return false;
        }

        getSoloud().setGlobalVolume(globalVolume);


        inited = true;
    }

    return true;
}

bool AudioSystem::isOutputAvailable(){
    return !outputUnavailable;
}

bool AudioSystem::beginOfflineCapture(unsigned int sampleRate, unsigned int channels){
    // whatever plays now stops: the capture starts with the scene
    deInit();
    SoLoud::result result = getSoloud().init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, sampleRate,
                                             SoLoud::Soloud::AUTO, channels);
    if (result != SoLoud::SOLOUD_ERRORS::SO_NO_ERROR) {
        Log::warn("Offline audio capture unavailable (%s)", getSoloud().getErrorString(result));
        return false;
    }
    getSoloud().setGlobalVolume(globalVolume);
    inited = true;
    offlineCapture = true;
    return true;
}

void AudioSystem::endOfflineCapture(){
    if (!offlineCapture) return;
    deInit();
    offlineCapture = false;
}

bool AudioSystem::isOfflineCapture(){
    return offlineCapture;
}

void AudioSystem::mixOffline(float* out, unsigned int frames){
    if (!offlineCapture || frames == 0) return;
    getSoloud().mix(out, frames);
}

void AudioSystem::deInit(){
    if (inited){
        getSoloud().deinit();

        inited = false;
    }
}

bool AudioSystem::loadSoundSample(AudioSourceComponent& audio){
    if (audio.filename.empty()) {
        return false;
    }

    if (!audio.sample){
        SoundLoadResult result = SoundPool::loadFromFile(audio.filename, audio.filename);
        if (result.state == ResourceLoadState::Loading) {
            return false;
        }
        if (result.state == ResourceLoadState::Failed) {
            audio.loaded = false;
            audio.loadFailed = true;
            audio.length = 0;
            audio.startTrigger = false;
            audio.state = AudioSourceState::Stopped;
            audio.sample.reset();
            return false;
        }

        audio.sample = result.data;
        audio.sampleGeneration = SoundPool::generation(audio.filename);
        if (!audio.sample) {
            return false;
        }
    }

    audio.length = audio.sample->getLength();
    audio.loaded = true;
    audio.loadFailed = false;

    return true;
}

bool AudioSystem::loadSound(AudioSourceComponent& audio){
    if (!loadSoundSample(audio)) {
        return false;
    }

    return init();
}

void AudioSystem::preloadSoundAssets(){
    auto audios = scene->getComponentArray<AudioSourceComponent>();
    for (int i = 0; i < audios->size(); i++) {
        AudioSourceComponent& audio = audios->getComponentFromIndex(i);

        if (audio.filename.empty()) {
            if (audio.loaded || audio.handle != 0 || audio.sample) {
                destroySound(audio);
            }
            continue;
        }

        if (!audio.loaded) {
            loadSoundSample(audio);
        }
    }
}

void AudioSystem::getLoadCount(size_t& loaded, size_t& total) const{
    loaded = 0;
    total = 0;

    auto audios = scene->getComponentArray<AudioSourceComponent>();
    for (int i = 0; i < audios->size(); i++) {
        const AudioSourceComponent& audio = audios->getComponentFromIndex(i);
        if (audio.filename.empty()) {
            continue;
        }

        total++;
        // a missing file must not hold the load
        if (audio.loaded || audio.loadFailed) {
            loaded++;
        }
    }
}

void AudioSystem::destroySound(AudioSourceComponent& audio){
    if (inited && audio.handle != 0) {
        getSoloud().stop(audio.handle);
    }

    audio.handle = 0;
    audio.playingTime = 0;
    audio.startTrigger = false;
    audio.pauseTrigger = false;
    audio.stopTrigger = false;
    audio.loaded = false;
    audio.loadFailed = false;
    audio.length = 0;
    audio.sample.reset();
    if (!audio.filename.empty()) {
        SoundPool::remove(audio.filename);
    }
}

bool AudioSystem::seekSound(AudioSourceComponent& audio, double time){
    return getSoloud().seek(audio.handle, time) == SoLoud::SOLOUD_ERRORS::SO_NO_ERROR;
}

void AudioSystem::applySoundPause(bool pause) {
    if (inited) {
        auto audios = scene->getComponentArray<AudioSourceComponent>();
        for (int i = 0; i < audios->size(); i++) {
            AudioSourceComponent& audio = audios->getComponentFromIndex(i);
            if (audio.handle != 0) {
                getSoloud().setPause(audio.handle, pause);
            }
        }
    }
}

void AudioSystem::setPaused(bool paused) {
    this->paused = paused;
    applySoundPause(paused || !schedulerEnabled);
}

void AudioSystem::onSchedulerEnabledChanged(bool enabled) {
    schedulerEnabled = enabled;
    applySoundPause(paused || !schedulerEnabled);
}

void AudioSystem::stopSceneSounds() {
    auto audios = scene->getComponentArray<AudioSourceComponent>();
    for (int i = 0; i < audios->size(); i++) {
        AudioSourceComponent& audio = audios->getComponentFromIndex(i);

        if (inited && audio.handle != 0) {
            getSoloud().stop(audio.handle);
        }

        audio.handle = 0;
        audio.playingTime = 0;
        audio.startTrigger = false;
        audio.pauseTrigger = false;
        audio.stopTrigger = false;
    }
}

void AudioSystem::stopAll(){
    if (inited){
        getSoloud().stopAll();
    }
}

void AudioSystem::pauseAll(){
    if (inited) {
        getSoloud().setPauseAll(true);
    }
}

void AudioSystem::resumeAll(){
    if (inited) {
        getSoloud().setPauseAll(false);
    }
}

void AudioSystem::checkActive(){
    if (inited) {
        if (getSoloud().getVoiceCount() == 0){
            deInit();
        }
    }
}

// using global volume in a static var to save because init/deinit reset volume value
void AudioSystem::setGlobalVolume(float volume){
    globalVolume = volume;
    getSoloud().setGlobalVolume(globalVolume);
}

float AudioSystem::getGlobalVolume(){
    return globalVolume;
}

void AudioSystem::load(){
    // a file that failed before is tried again by each scene that loads it
    auto audios = scene->getComponentArray<AudioSourceComponent>();
    for (int i = 0; i < audios->size(); i++) {
        const AudioSourceComponent& audio = audios->getComponentFromIndex(i);
        if (!audio.filename.empty() && !audio.loaded) {
            SoundPool::retry(audio.filename);
        }
    }

    preloadSoundAssets();
}

void AudioSystem::destroy(){
}

void AudioSystem::update(double dt){
    preloadSoundAssets();

    if (paused) {
        return;
    }

    auto audios = scene->getComponentArray<AudioSourceComponent>();

    // open the output early, a device started by the first play cuts that sound
    if (!outputRequested && audios->size() > 0) {
        outputRequested = true;
        init();
    }

    // 3D sources are placed against the main camera, so without one they play in 2D. The
    // persistent scene's (DontDestroyOnLoad) are heard by the loaded main scene's camera, which
    // that scene's system places: their positions are world positions too
    const bool persistent = scene == SceneManager::getPersistentScene();
    Scene* listenerScene = persistent ? Engine::getMainScene() : scene;
    bool hasListener = listenerScene && listenerScene->findComponent<CameraComponent>(listenerScene->getCamera()) &&
                       listenerScene->findComponent<Transform>(listenerScene->getCamera());

    // The persistent scene's 3D sounds are heard by the loaded main scene's camera, every frame
    // (the main scene's own system places it only when one of its sources updates), before
    // they are placed against it
    if (persistent && hasListener){
        bool playing3D = false;
        for (int i = 0; i < audios->size() && !playing3D; i++){
            const AudioSourceComponent& audio = audios->getComponentFromIndex(i);
            playing3D = audio.enabled && audio.state == AudioSourceState::Playing &&
                        scene->getSignature(audios->getEntity(i)).test(scene->getComponentId<Transform>());
        }
        if (playing3D){
            placeListener(*listenerScene);
        }
    }

    for (int i = 0; i < audios->size(); i++){
		AudioSourceComponent& audio = audios->getComponentFromIndex(i);

        Entity entity = audios->getEntity(i);
        Signature signature = scene->getSignature(entity);
        bool use3DAudio = hasListener && signature.test(scene->getComponentId<Transform>());

        Vector3 worldPosition = Vector3(0, 0, 0);
        if (use3DAudio){
            Transform& transform = scene->getComponent<Transform>(entity);

            worldPosition = transform.worldPosition;
        }

        if (audio.filename.empty()) {
            if (audio.loaded || audio.handle != 0 || audio.sample) {
                destroySound(audio);
            }
            continue;
        }

        // Unity: disabling an Audio Source or its GameObject stops it and a play request
        // is ignored. A source left Playing starts over on reactivation (play on awake).
        if (!audio.enabled || !scene->isEntityActiveInHierarchy(entity)) {
            if (audio.handle != 0) {
                if (inited) {
                    getSoloud().stop(audio.handle);
                }
                audio.handle = 0;
                audio.playingTime = 0;
            }
            if (audio.state == AudioSourceState::Paused) {
                audio.state = AudioSourceState::Stopped;
            }
            audio.startTrigger = false;
            audio.pauseTrigger = false;
            audio.stopTrigger = false;
            continue;
        }

        bool pendingStart = audio.startTrigger || (audio.state == AudioSourceState::Playing && audio.handle == 0);

        if (audio.state == AudioSourceState::Playing || pendingStart){
            if (!audio.loaded){
                loadSound(audio);
            }
        }

        if (audio.loaded){
            if (audio.pauseTrigger){
                audio.pauseTrigger = false;

                getSoloud().setPause(audio.handle, true);
                audio.state = AudioSourceState::Paused;
            }
            if (audio.stopTrigger){
                audio.stopTrigger = false;

                getSoloud().stop(audio.handle);
                audio.handle = 0;
                audio.playingTime = 0;
                audio.state = AudioSourceState::Stopped;
            }
            if (pendingStart){
                audio.startTrigger = false;

                // the file was hot reloaded: this play takes the new sample (one still
                // playing finishes on the old, which its other sources hold)
                if (audio.state != AudioSourceState::Paused && audio.sampleGeneration != SoundPool::generation(audio.filename)) {
                    if (std::shared_ptr<SoLoud::Wav> fresh = SoundPool::get(audio.filename)) {
                        audio.sample = fresh;
                        audio.sampleGeneration = SoundPool::generation(audio.filename);
                        audio.length = audio.sample->getLength();
                    }
                }

                if (audio.state != AudioSourceState::Paused || audio.handle == 0) {
                    if (!init()) {
                        // no output: the sound plays silently, as if it ended at once,
                        // rather than asking for the device again every frame
                        audio.handle = 0;
                        audio.playingTime = 0;
                        audio.state = AudioSourceState::Stopped;
                        continue;
                    }

                    if (use3DAudio){
                        if (audio.enableClocked){
                            audio.handle = getSoloud().play3dClocked(Engine::getDeltatime(), *audio.sample, worldPosition.x, worldPosition.y, worldPosition.z);
                        }else{
                            audio.handle = getSoloud().play3d(*audio.sample, worldPosition.x, worldPosition.y, worldPosition.z);
                        }
                    }else{
                        if (audio.enableClocked){
                            audio.handle = getSoloud().playClocked(Engine::getDeltatime(), *audio.sample);
                        }else{
                            audio.handle = getSoloud().play(*audio.sample);
                        }
                    }
                    // a new voice starts with the sample defaults
                    audio.needUpdate = true;
                }else{
                    getSoloud().setPause(audio.handle, false);
                }
                if (use3DAudio){
                    Transform& cameraTransform =  listenerScene->getComponent<Transform>(listenerScene->getCamera());

                    audio.lastPosition = worldPosition;
                    cameraLastPosition = cameraTransform.worldPosition;
                }

                audio.state = AudioSourceState::Playing;
            }

            if (audio.state == AudioSourceState::Playing){
                audio.playingTime = getSoloud().getStreamTime(audio.handle);

                if (audio.needUpdate){
                    getSoloud().setVolume(audio.handle, audio.volume);
                    getSoloud().setRelativePlaySpeed(audio.handle, audio.speed);
                    getSoloud().setPan(audio.handle, audio.pan);
                    getSoloud().setLooping(audio.handle, audio.looping);
                    getSoloud().setLoopPoint(audio.handle, audio.loopingPoint);
                    getSoloud().setProtectVoice(audio.handle, audio.protectVoice);
                    getSoloud().setInaudibleBehavior(audio.handle, audio.inaudibleBehaviorMustTick, audio.inaudibleBehaviorKill);

                    if (use3DAudio){
                        CameraComponent& camera =  listenerScene->getComponent<CameraComponent>(listenerScene->getCamera());
                        Transform& cameraTransform =  listenerScene->getComponent<Transform>(listenerScene->getCamera());

                        Vector3 velocity = audio.lastPosition - worldPosition;

                        Vector3 camWorldPos = cameraTransform.worldPosition;
                        Vector3 camWorldView = camera.worldTarget - camWorldPos;
                        Vector3 camWorldUp = camera.worldUp;
                        Vector3 camVelocity = cameraLastPosition - camWorldPos;

                        unsigned int attModel = SoLoud::AudioSource::NO_ATTENUATION;
                        if (audio.attenuationModel == AudioSourceAttenuation::INVERSE_DISTANCE)
                            attModel = SoLoud::AudioSource::INVERSE_DISTANCE;
                        if (audio.attenuationModel == AudioSourceAttenuation::LINEAR_DISTANCE)
                            attModel = SoLoud::AudioSource::LINEAR_DISTANCE;
                        if (audio.attenuationModel == AudioSourceAttenuation::EXPONENTIAL_DISTANCE)
                            attModel = SoLoud::AudioSource::EXPONENTIAL_DISTANCE;

                        getSoloud().set3dSourceMinMaxDistance(audio.handle, audio.minDistance, audio.maxDistance);
                        getSoloud().set3dSourceAttenuation(audio.handle, attModel, audio.attenuationRolloffFactor);
                        getSoloud().set3dSourceDopplerFactor(audio.handle, audio.dopplerFactor);
                        getSoloud().set3dSourceParameters(
                            audio.handle, 
                            worldPosition.x, worldPosition.y, worldPosition.z, 
                            velocity.x, velocity.y, velocity.z);
                        if (!persistent){
                            getSoloud().set3dListenerParameters(
                                camWorldPos.x, camWorldPos.y, camWorldPos.z,
                                camWorldView.x, camWorldView.y, camWorldView.z,
                                camWorldUp.x, camWorldUp.y, camWorldUp.z,
                                camVelocity.x, camVelocity.y, camVelocity.z);
                        }

                        getSoloud().update3dAudio();

                        audio.lastPosition = worldPosition;
                        cameraLastPosition = camWorldPos;
                    }

                    audio.needUpdate = false;
                }
            }
        }
    }
}

void AudioSystem::placeListener(Scene& listenerScene){
    const CameraComponent& camera = listenerScene.getComponent<CameraComponent>(listenerScene.getCamera());
    const Transform& cameraTransform = listenerScene.getComponent<Transform>(listenerScene.getCamera());
    const Vector3 position = cameraTransform.worldPosition;
    const Vector3 view = camera.worldTarget - position;
    const Vector3 up = camera.worldUp;
    const Vector3 velocity = cameraLastPosition - position;
    getSoloud().set3dListenerParameters(position.x, position.y, position.z, view.x, view.y, view.z,
                                        up.x, up.y, up.z, velocity.x, velocity.y, velocity.z);
    if (inited){
        getSoloud().update3dAudio();
    }
    cameraLastPosition = position;
}

void AudioSystem::draw(){

}

void AudioSystem::onComponentAdded(Entity entity, ComponentId componentId) {

}

void AudioSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
	if (componentId == scene->getComponentId<AudioSourceComponent>()) {
		AudioSourceComponent& audio = scene->getComponent<AudioSourceComponent>(entity);
		destroySound(audio);
	}
}