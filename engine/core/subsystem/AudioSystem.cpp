// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioSystem.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "PlatformConfig.h"
#include "manager/SceneManager.h"

#include "audio/AudioMixer.h"
#include "pool/SoundPool.h"
#include "Scene.h"

#include "soloud.h"
#include "soloud_wav.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

using namespace jasperengine;

bool AudioSystem::inited = false;
bool AudioSystem::outputUnavailable = false;
bool AudioSystem::offlineCapture = false;

float AudioSystem::globalVolume = 1.0;

namespace {

    // what started every voice the engine holds (sources, one-shots, playClipAtPoint)
    struct VoiceRecord {
        Scene* owner = nullptr;
        std::string groupId;
        int priority = 128;
        uint64_t serial = 0;
        bool isProtected = false;
    };

    std::unordered_map<unsigned int, VoiceRecord>& voices() {
        static auto* map = new std::unordered_map<unsigned int, VoiceRecord>();
        return *map;
    }

    uint64_t nextSerial = 1;
    // the Null backend's pump: the frames it mixed and the time it was given since the audio opened.
    // Each frame mixes up to round(time x rate), so no rounding piles up (144 fps at 44.1 kHz is
    // 306.25 frames a frame: rounded each time, 48 ms a minute behind the game)
    double pumpSeconds = 0.0;
    uint64_t pumpFrames = 0;
    AudioConfig config;
    AudioBackend backend = AudioBackend::Auto;
    bool backendFromEnvironment = false;
    bool outputAvailable = false;
    bool listenerPaused = false;

    // the clips whose one-shot waited too long for its load: warned once
    std::set<std::string>& slowClips() {
        static auto* set = new std::set<std::string>();
        return *set;
    }

    constexpr double oneShotLoadWait = 0.25;

    enum EventKind { EventStart, EventPause, EventStop };

    // Where a scene's camera hears from, in its own world: its position (a perspective camera), or
    // the centre of its view (an orthographic or UI camera, whose view spans [leftClip, rightClip]
    // x [bottomClip, topClip] from its position: 0 to the canvas by default, so a sound at the
    // centre of the screen is centred). forward / right / up: the camera's basis, unshaken
    // (steadyWorldUp: a Camera Shaker never turns what the player hears)
    struct CameraPose {
        bool valid = false;
        bool perspective = false;
        Vector3 position;
        Vector3 view;
        Vector3 worldUp;
        Vector3 forward = Vector3(0, 0, -1);
        Vector3 right = Vector3(1, 0, 0);
        Vector3 up = Vector3(0, 1, 0);
    };

    CameraPose cameraPose(Scene& scene) {
        CameraPose pose;
        const Entity entity = scene.getCamera();
        const CameraComponent* camera = scene.findComponent<CameraComponent>(entity);
        const Transform* transform = scene.findComponent<Transform>(entity);
        if (!camera || !transform) return pose;
        pose.valid = true;
        pose.perspective = camera->type == CameraType::CAMERA_PERSPECTIVE;
        pose.position = transform->worldPosition;
        pose.view = camera->worldTarget - pose.position;
        pose.worldUp = camera->steadyWorldUp;
        const Vector3 side = pose.view.crossProduct(pose.worldUp);
        if (pose.view.squaredLength() > 1e-12f && side.squaredLength() > 1e-12f) {
            pose.forward = pose.view.normalized();
            pose.right = side.normalized();
            pose.up = pose.right.crossProduct(pose.forward);
        }
        if (!pose.perspective) {
            pose.position += pose.right * ((camera->leftClip + camera->rightClip) * 0.5f) +
                             pose.up * ((camera->bottomClip + camera->topClip) * 0.5f);
        }
        return pose;
    }

    // SoLoud's velocity of what went from `from` to `to` in `seconds`: units a second, the other way
    // round (SoLoud's Doppler takes its velocities negated). None for a move faster than sound (a
    // teleport, a camera cut), and at most half the speed of sound over the Doppler factor, below
    // which SoLoud's Doppler never divides by zero.
    Vector3 dopplerVelocity(const Vector3& from, const Vector3& to, double seconds, float factor) {
        if (!(seconds > 0.0)) return Vector3(0, 0, 0);
        Vector3 velocity = (from - to) / (float)seconds;
        const float sound = AudioSystem::getSoloud().get3dSoundSpeed();
        const float speed = velocity.length();
        if (!(speed < sound)) return Vector3(0, 0, 0);
        const float limit = 0.5f * sound / std::max(1.0f, factor);
        if (speed > limit) velocity *= limit / speed;
        return velocity;
    }

    // x' = p_L + B_L B_S^T (x - p_S), B = [right up forward]: a point of a scene seen by its camera
    // S as the listener camera L would see it at the same place in its view
    Vector3 mapToListener(const Vector3& position, const CameraPose& own, const CameraPose& listener) {
        const Vector3 offset = position - own.position;
        return listener.position + listener.right * own.right.dotProduct(offset) +
               listener.up * own.up.dotProduct(offset) + listener.forward * own.forward.dotProduct(offset);
    }

    // 0 for a voice that could not start
    void registerVoice(unsigned int handle, Scene* owner, const std::string& groupId, int priority, bool isProtected) {
        if (handle == 0) return;
        VoiceRecord record;
        record.owner = owner;
        record.groupId = groupId;
        record.priority = priority;
        record.serial = nextSerial++;
        record.isProtected = isProtected;
        voices()[handle] = record;
    }

}

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

void AudioSystem::configure(const AudioConfig& newConfig){
    config = newConfig;
    config.maxRealVoices = std::clamp(config.maxRealVoices, 1, 255);
    config.maxVoices = std::clamp(config.maxVoices, 1, 1000);
    if (config.sampleRate < 0) config.sampleRate = 0;
    if (inited) {
        // the real voices apply at once; the sample rate at the next init
        AudioMixer::onAudioInit();
    }
}

const AudioConfig& AudioSystem::getConfig(){
    return config;
}

void AudioSystem::setBackend(AudioBackend newBackend){
    backend = newBackend;
    backendFromEnvironment = true;   // an explicit choice wins over the environment
    outputUnavailable = false;
}

AudioBackend AudioSystem::getBackend(){
    return backend;
}

std::string AudioSystem::getBackendName(){
    if (!inited) return "";
    const char* name = getSoloud().getBackendString();
    return name ? name : "";
}

bool AudioSystem::init(){
    if (!inited) {
        // Opened once: without any backend every retry would probe all of them again, for each sound
        if (outputUnavailable) {
            return false;
        }

        if (!backendFromEnvironment) {
            backendFromEnvironment = true;
            if (const char* env = std::getenv("JASPERENGINE_AUDIO_BACKEND")) {
                if (std::strcmp(env, "nosound") == 0) backend = AudioBackend::NoSound;
                else if (std::strcmp(env, "null") == 0) backend = AudioBackend::Null;
            }
        }

        // --headless and --silent-audio open no device but mix as a device would, on the null
        // driver pumped by the frames' unscaled time (docs/specs/S30-shipping.md R17): sounds play,
        // end and fire their events, so the game's logic is that of a normal run
        AudioBackend opened = backend;
        if (platformConfig().silentAudio && opened == AudioBackend::Auto) {
            opened = AudioBackend::Null;
            Log::print("Audio output off (--silent-audio or --headless): mixing without output");
        }

        SoLoud::Soloud& soloud = getSoloud();
        const unsigned int flags = SoLoud::Soloud::CLIP_ROUNDOFF;
        const unsigned int rate = config.sampleRate > 0 ? (unsigned int)config.sampleRate : (unsigned int)SoLoud::Soloud::AUTO;
        SoLoud::result result = SoLoud::SO_NO_ERROR + 1;

        outputAvailable = false;
        if (opened == AudioBackend::Auto) {
            result = soloud.init(flags, SoLoud::Soloud::MINIAUDIO, rate);
            outputAvailable = result == SoLoud::SO_NO_ERROR;
            if (!outputAvailable) {
                Log::warn("No audio output available (%s): mixing without output", soloud.getErrorString(result));
            }
        }
        if (result != SoLoud::SO_NO_ERROR && opened != AudioBackend::Null) {
            result = soloud.init(flags, SoLoud::Soloud::NOSOUND, rate == SoLoud::Soloud::AUTO ? 44100 : rate);
        }
        if (result != SoLoud::SO_NO_ERROR) {
            // no threads (a Web build without pthreads): mixed on the main thread each frame
            result = soloud.init(flags, SoLoud::Soloud::NULLDRIVER, rate == SoLoud::Soloud::AUTO ? 44100 : rate, 1024);
        }
        if (result != SoLoud::SO_NO_ERROR) {
            outputUnavailable = true;
            Log::warn("No audio backend opened (%s): sounds are muted", soloud.getErrorString(result));
            return false;
        }

        soloud.setGlobalVolume(globalVolume);

        inited = true;

        // the mixer's buses, before any voice
        AudioMixer::onAudioInit();
    }

    return true;
}

bool AudioSystem::isOutputAvailable(){
    return inited ? outputAvailable : (!outputUnavailable && backend == AudioBackend::Auto && !platformConfig().silentAudio);
}

bool AudioSystem::isInited(){
    return inited;
}

void AudioSystem::mixManually(double seconds){
    // a Recorder capture holds the null driver and mixes it itself (mixOffline), once a frame
    if (!inited || offlineCapture || getSoloud().getBackendId() != SoLoud::Soloud::NULLDRIVER || !(seconds > 0.0)) return;
    SoLoud::Soloud& soloud = getSoloud();
    const unsigned int channels = std::max(1u, soloud.getBackendChannels());
    pumpSeconds += seconds;
    const uint64_t due = (uint64_t)std::llround(pumpSeconds * soloud.getBackendSamplerate());
    uint64_t samples = due > pumpFrames ? due - pumpFrames : 0;
    pumpFrames += samples;
    static std::vector<float> buffer;
    constexpr unsigned int chunk = 1024;
    buffer.resize(chunk * channels);
    while (samples > 0) {
        const unsigned int now = (unsigned int)std::min<uint64_t>(samples, chunk);
        soloud.mix(buffer.data(), now);
        samples -= now;
    }
}

void AudioSystem::close(){
    deInit();
    outputUnavailable = false;
}

void AudioSystem::getVoiceCounts(uint32_t& active, uint32_t& max){
    active = 0;
    max = 0;
    if (!inited)
        return;
    // the engine's voices (sources, one-shots), as get_play_state's audio.voices: SoLoud's own
    // count has the mixer's group buses too
    active = getVoiceCount();
    max = getSoloud().getMaxActiveVoiceCount();
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
    // the mixer's buses, as init() makes them: the capture hears the groups, their effects and mutes
    AudioMixer::onAudioInit();
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
    pumpSeconds = 0.0;
    pumpFrames = 0;
    if (inited){
        AudioMixer::onAudioShutdown();
        getSoloud().deinit();
        voices().clear();

        inited = false;
    }
}

// ---------------------------------------------------------------- the voice budget

unsigned int AudioSystem::getVoiceCount(){
    pruneVoices();
    return (unsigned int)voices().size();
}

void AudioSystem::pruneVoices(){
    if (!inited) {
        voices().clear();
        return;
    }
    SoLoud::Soloud& soloud = getSoloud();
    for (auto it = voices().begin(); it != voices().end();) {
        if (!soloud.isValidVoiceHandle(it->first)) it = voices().erase(it);
        else ++it;
    }
}

void AudioSystem::stopVoice(unsigned int handle){
    if (handle == 0) return;
    if (inited) getSoloud().stop(handle);
    voices().erase(handle);
}

void AudioSystem::moveGroupVoices(const std::string& fromGroupId, const std::string& toGroupId){
    for (auto& [handle, record] : voices()) {
        if (record.groupId == fromGroupId) record.groupId = toGroupId;
    }
}

std::vector<unsigned int> AudioSystem::getGroupVoices(const std::string& groupId){
    std::vector<unsigned int> out;
    for (const auto& [handle, record] : voices()) {
        if (record.groupId == groupId) out.push_back(handle);
    }
    return out;
}

void AudioSystem::moveSourceVoices(Scene* from, AudioSourceComponent& source, Scene* to, AudioSourceComponent& target){
    std::vector<unsigned int> handles;
    if (source.handle) handles.push_back(source.handle);
    for (const AudioOneShot& shot : source.oneShots) handles.push_back(shot.handle);
    for (unsigned int handle : handles) {
        auto it = voices().find(handle);
        if (it != voices().end() && it->second.owner == from) it->second.owner = to;
    }

    target.sample = source.sample;
    target.sampleKey = source.sampleKey;
    target.loadedType = source.loadedType;
    target.loaded = source.loaded;
    target.length = source.length;
    target.handle = source.handle;
    target.state = source.state;
    target.playingTime = source.playingTime;
    target.lastPosition = source.lastPosition;
    target.positionTracked = source.positionTracked;
    target.lastVelocity = source.lastVelocity;
    target.oneShots = source.oneShots;
    target.pendingOneShots = source.pendingOneShots;
    target.fade = source.fade;
    target.voicePaused = source.voicePaused;
    target.listenerPaused = source.listenerPaused;
    target.resolvedGroup = source.resolvedGroup;
    target.lastVolumeApplied = source.lastVolumeApplied;
    target.startTrigger = source.startTrigger;
    target.pauseTrigger = source.pauseTrigger;
    target.stopTrigger = source.stopTrigger;

    // the source keeps nothing that could stop them
    source.handle = 0;
    source.oneShots.clear();
    source.pendingOneShots.clear();
    source.fade = AudioFade();
    source.startTrigger = false;
    source.pauseTrigger = false;
    source.stopTrigger = false;
}

void AudioSystem::setListenerEnabled(bool enabled){
    listenerEnabled = enabled;
}

bool AudioSystem::isListenerEnabled() const{
    return listenerEnabled;
}

// ---------------------------------------------------------------- samples

bool AudioSystem::loadSoundSample(AudioSourceComponent& audio){
    if (audio.filename.empty()) {
        return false;
    }

    // a changed load type (or a file that crossed the Auto size) loads the sample again
    if (audio.sample && SoundPool::resolveLoadType(audio.filename, audio.loadType) != audio.loadedType) {
        stopVoices(audio);
        releaseSample(audio);
    }
    // voices of a sample the component let go (its file changed): they go before the new one
    if (!audio.sample && (audio.handle != 0 || !audio.oneShots.empty())) {
        stopVoices(audio);
    }

    if (!audio.sample){
        SoundLoadResult result = SoundPool::loadClip(audio.filename, audio.loadType);
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
            audio.pendingOneShots.clear();
            return false;
        }

        audio.sample = result.data;
        audio.sampleGeneration = SoundPool::generation(audio.filename);
        audio.sampleKey = result.id;
        audio.loadedType = result.loadType;
        if (!audio.sample) {
            return false;
        }
    }

    audio.length = SoundPool::getLength(audio.sample.get());
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

bool AudioSystem::preload(const std::string& filename, AudioLoadType loadType){
    SoundLoadResult result = SoundPool::loadClip(filename, loadType);
    if (result.state == ResourceLoadState::Failed) return false;
    if (result.data) SoundPool::pin(result.id);
    // async loading: the pool finishes it; the next call (or the first play) holds it
    return true;
}

void AudioSystem::releaseSample(AudioSourceComponent& audio){
    audio.loaded = false;
    audio.loadFailed = false;
    audio.length = 0;
    audio.sample.reset();
    if (!audio.sampleKey.empty()) {
        SoundPool::remove(audio.sampleKey);
        audio.sampleKey.clear();
    }
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

void AudioSystem::stopVoices(AudioSourceComponent& audio){
    if (audio.handle != 0) {
        stopVoice(audio.handle);
    }
    for (const AudioOneShot& shot : audio.oneShots) {
        stopVoice(shot.handle);
    }
    audio.handle = 0;
    audio.oneShots.clear();
    audio.pendingOneShots.clear();
    audio.playingTime = 0;
    audio.voicePaused = false;
    audio.lastVolumeApplied = -1;
}

void AudioSystem::destroySound(AudioSourceComponent& audio){
    stopVoices(audio);

    audio.startTrigger = false;
    audio.pauseTrigger = false;
    audio.stopTrigger = false;
    // a fade dies with the voice; fadeIn / fadeOut leave the authored volume
    if (audio.fade.active && audio.fade.restore) audio.volume = audio.fade.restoreVolume;
    audio.fade = AudioFade();
    releaseSample(audio);
}

bool AudioSystem::seekSound(AudioSourceComponent& audio, double time){
    if (!inited || audio.handle == 0) return false;
    return getSoloud().seek(audio.handle, time) == SoLoud::SOLOUD_ERRORS::SO_NO_ERROR;
}

// ---------------------------------------------------------------- pause

bool AudioSystem::desiredPause(const AudioSourceComponent& audio) const{
    if (audio.state == AudioSourceState::Paused) return true;
    // the system paused, or skipped by the scheduler (Systems window): its sounds hold too
    if (paused || !schedulerEnabled) return true;
    return listenerPaused && !audio.ignoreListenerPause;
}

void AudioSystem::applyPause(AudioSourceComponent& audio){
    const bool pause = desiredPause(audio);
    audio.listenerPaused = listenerPaused && !audio.ignoreListenerPause;
    if (!inited || pause == audio.voicePaused) return;
    if (audio.handle != 0) getSoloud().setPause(audio.handle, pause);
    for (const AudioOneShot& shot : audio.oneShots) getSoloud().setPause(shot.handle, pause);
    audio.voicePaused = pause;
}

void AudioSystem::applySoundPause() {
    // only what this system paused comes back: a source the game paused stays paused
    auto audios = scene->getComponentArray<AudioSourceComponent>();
    for (int i = 0; i < audios->size(); i++) {
        applyPause(audios->getComponentFromIndex(i));
    }
    if (inited) {
        for (PointVoice& voice : pointVoices) {
            const bool pause = paused || !schedulerEnabled || listenerPaused;
            if (voice.paused != pause) {
                getSoloud().setPause(voice.handle, pause);
                voice.paused = pause;
            }
        }
    }
}

void AudioSystem::setPaused(bool paused) {
    this->paused = paused;
    applySoundPause();
}

void AudioSystem::onSchedulerEnabledChanged(bool enabled) {
    schedulerEnabled = enabled;
    applySoundPause();
}

void AudioSystem::setListenerPaused(bool paused){
    listenerPaused = paused;
}

bool AudioSystem::isListenerPaused(){
    return listenerPaused;
}

void AudioSystem::stopSceneSounds() {
    auto audios = scene->getComponentArray<AudioSourceComponent>();
    for (int i = 0; i < audios->size(); i++) {
        AudioSourceComponent& audio = audios->getComponentFromIndex(i);

        stopVoices(audio);
        audio.startTrigger = false;
        audio.pauseTrigger = false;
        audio.stopTrigger = false;
        if (audio.fade.active && audio.fade.restore) audio.volume = audio.fade.restoreVolume;
        audio.fade = AudioFade();
    }

    for (const PointVoice& voice : pointVoices) stopVoice(voice.handle);
    pointVoices.clear();
    pendingPointVoices.clear();

    // and anything else this scene started (a voice whose component went first)
    std::vector<unsigned int> owned;
    for (const auto& [handle, record] : voices()) {
        if (record.owner == scene) owned.push_back(handle);
    }
    for (unsigned int handle : owned) stopVoice(handle);
}

void AudioSystem::stopAll(){
    if (inited){
        std::vector<unsigned int> all;
        for (const auto& [handle, record] : voices()) all.push_back(handle);
        for (unsigned int handle : all) stopVoice(handle);
    }
}

void AudioSystem::pauseAll(){
    AudioMixer::setMasterPaused(true);
}

void AudioSystem::resumeAll(){
    AudioMixer::setMasterPaused(false);
}

void AudioSystem::setPlatformPaused(bool paused){
    AudioMixer::setPlatformPaused(paused);
}

void AudioSystem::checkActive(){
    if (inited) {
        // the mixer's own buses do not keep the audio open
        if (getSoloud().getVoiceCount() <= AudioMixer::getBusVoiceCount()){
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

// ---------------------------------------------------------------- voices

const std::string& AudioSystem::resolveGroup(AudioSourceComponent& audio, Entity entity){
    // the id first (a renamed group keeps its sources), unless the name says another group
    // that exists; then the name; then Master (spec A3)
    std::string id;
    const std::string byId = audio.outputGroup.empty() ? "" : AudioMixer::findGroupId(audio.outputGroup);
    const std::string byName = audio.outputGroupName.empty() ? "" : AudioMixer::findGroupId(audio.outputGroupName);
    if (!byId.empty() && (byName.empty() || byName == byId)) {
        id = byId;
    } else if (!byName.empty()) {
        id = byName;
    } else if (!byId.empty()) {
        id = byId;
    }
    const bool asked = !audio.outputGroup.empty() || !audio.outputGroupName.empty();
    if (asked && id.empty() && !audio.groupWarned) {
        audio.groupWarned = true;
        Log::warn("Audio Source '%s': mixer group '%s' is not in the mixer, it plays through Master",
                  scene->getEntityName(entity).c_str(),
                  (audio.outputGroupName.empty() ? audio.outputGroup : audio.outputGroupName).c_str());
    }
    if (!id.empty()) audio.groupWarned = false;
    audio.resolvedGroup = id;
    return audio.resolvedGroup;
}

void AudioSystem::apply3dParameters(AudioSourceComponent& audio, unsigned int handle, const Vector3& position, const Vector3& velocity){
    SoLoud::Soloud& soloud = getSoloud();
    unsigned int attModel = SoLoud::AudioSource::NO_ATTENUATION;
    if (audio.attenuationModel == AudioSourceAttenuation::INVERSE_DISTANCE)
        attModel = SoLoud::AudioSource::INVERSE_DISTANCE;
    if (audio.attenuationModel == AudioSourceAttenuation::LINEAR_DISTANCE)
        attModel = SoLoud::AudioSource::LINEAR_DISTANCE;
    if (audio.attenuationModel == AudioSourceAttenuation::EXPONENTIAL_DISTANCE)
        attModel = SoLoud::AudioSource::EXPONENTIAL_DISTANCE;

    soloud.set3dSourceMinMaxDistance(handle, audio.minDistance, audio.maxDistance);
    soloud.set3dSourceAttenuation(handle, attModel, audio.attenuationRolloffFactor);
    soloud.set3dSourceDopplerFactor(handle, audio.dopplerFactor);
    soloud.set3dSourceParameters(handle, position.x, position.y, position.z, velocity.x, velocity.y, velocity.z);
}

void AudioSystem::applyVoiceParameters(AudioSourceComponent& audio, unsigned int handle, bool mainVoice, bool use3D){
    SoLoud::Soloud& soloud = getSoloud();
    soloud.setRelativePlaySpeed(handle, audio.speed > 0.0f ? audio.speed : 0.0001f);
    // a 3D voice's pan is SoLoud's (play3d, update3dAudio): a 2D pan here would centre its channel
    // volumes until the frame's 3D pass, and a block mixed in between ramps it toward the centre
    if (!use3D) {
        soloud.setPan(handle, audio.pan);
    }
    if (mainVoice) {
        soloud.setLooping(handle, audio.looping);
        soloud.setLoopPoint(handle, audio.loopingPoint);
    }
    soloud.setProtectVoice(handle, audio.protectVoice);
    soloud.setInaudibleBehavior(handle, audio.inaudibleBehaviorMustTick, audio.inaudibleBehaviorKill);
}

unsigned int AudioSystem::startVoice(AudioSourceComponent& audio, SoLoud::AudioSource& sample, float volume, bool use3D,
                                     const Vector3& position, bool mainVoice){
    if (!init()) return 0;

    // the global budget (spec A11): past maxVoices the least important voice goes, the oldest
    // of them first; a new voice less important than all of them does not start
    pruneVoices();
    if ((int)voices().size() >= config.maxVoices) {
        unsigned int victim = 0;
        const VoiceRecord* worst = nullptr;
        for (const auto& [handle, record] : voices()) {
            if (record.isProtected) continue;
            if (!worst || record.priority > worst->priority ||
                (record.priority == worst->priority && record.serial < worst->serial)) {
                worst = &record;
                victim = handle;
            }
        }
        if (!worst || audio.priority > worst->priority) {
            return 0;
        }
        stopVoice(victim);
    }

    const bool clocked = mainVoice && audio.enableClocked;
    // the game time it was played at: SoLoud delays it from the first clocked start of the buffer
    unsigned int handle = AudioMixer::play(audio.resolvedGroup, sample, use3D, position, volume, clocked, AudioMixer::getClockTime());
    if (handle == 0) return 0;

    applyVoiceParameters(audio, handle, mainVoice, use3D);
    if (use3D) {
        apply3dParameters(audio, handle, position, Vector3(0, 0, 0));
    }
    registerVoice(handle, scene, audio.resolvedGroup, audio.priority, audio.protectVoice);

    // started paused, except a clocked voice (SoLoud has no paused clocked play): held at once
    const bool pause = desiredPause(audio);
    getSoloud().setPause(handle, pause);
    return handle;
}

bool AudioSystem::playOneShot(AudioSourceComponent& audio, const std::string& filename, float volumeScale){
    if (filename.empty()) return false;
    AudioPendingOneShot pending;
    pending.filename = filename;
    pending.volumeScale = std::max(0.0f, volumeScale);
    audio.pendingOneShots.push_back(pending);
    return true;
}

void AudioSystem::startPendingOneShots(AudioSourceComponent& audio, bool use3D, const Vector3& position, double unscaledDt){
    for (size_t i = 0; i < audio.pendingOneShots.size();) {
        AudioPendingOneShot& pending = audio.pendingOneShots[i];
        SoundLoadResult result = SoundPool::loadClip(pending.filename, audio.loadType);
        if (result.state == ResourceLoadState::Loading) {
            pending.waited += unscaledDt;
            if (pending.waited > oneShotLoadWait) {
                if (slowClips().insert(pending.filename).second) {
                    Log::warn("PlayOneShot of '%s' dropped: the clip was still loading after %.2f s (AudioSystem.preload it)",
                              pending.filename.c_str(), oneShotLoadWait);
                }
                audio.pendingOneShots.erase(audio.pendingOneShots.begin() + i);
                continue;
            }
            i++;
            continue;
        }
        const AudioPendingOneShot shot = pending;
        audio.pendingOneShots.erase(audio.pendingOneShots.begin() + i);
        if (result.state == ResourceLoadState::Failed || !result.data) continue;

        // the source's voices are capped: the oldest one-shot gives its place (spec A10)
        const int mainVoices = audio.handle != 0 ? 1 : 0;
        const int cap = std::max(1, audio.maxVoices);
        while (!audio.oneShots.empty() && mainVoices + (int)audio.oneShots.size() >= cap) {
            auto oldest = std::min_element(audio.oneShots.begin(), audio.oneShots.end(),
                                           [](const AudioOneShot& a, const AudioOneShot& b) { return a.serial < b.serial; });
            stopVoice(oldest->handle);
            audio.oneShots.erase(oldest);
        }
        if (mainVoices + (int)audio.oneShots.size() >= cap) continue;

        // the pool entry stays while a voice of it plays: the sample is held by the source's one-shots
        const float volume = (float)audio.volume * shot.volumeScale;
        unsigned int handle = startVoice(audio, *result.data, volume, use3D, position, false);
        if (handle == 0) continue;
        AudioOneShot oneShot;
        oneShot.handle = handle;
        oneShot.sample = result.data;
        oneShot.volumeScale = shot.volumeScale;
        oneShot.serial = nextSerial++;
        audio.oneShots.push_back(oneShot);
    }
}

int AudioSystem::getPlayingVoiceCount(const AudioSourceComponent& audio) const{
    if (!inited) return 0;
    int count = 0;
    if (audio.handle != 0 && getSoloud().isValidVoiceHandle(audio.handle)) count++;
    for (const AudioOneShot& shot : audio.oneShots) {
        if (getSoloud().isValidVoiceHandle(shot.handle)) count++;
    }
    return count;
}

void AudioSystem::playClipAtPoint(const std::string& filename, const Vector3& position, float volume, const std::string& group){
    if (filename.empty()) return;
    PendingPointVoice pending;
    pending.filename = filename;
    pending.position = position;
    pending.volume = std::max(0.0f, volume);
    pending.group = group;
    pendingPointVoices.push_back(pending);
}

void AudioSystem::updatePointVoices(double unscaledDt, bool hasListener, const std::function<Vector3(const Vector3&)>& heard){
    if (inited) {
        for (size_t i = 0; i < pointVoices.size();) {
            if (!getSoloud().isValidVoiceHandle(pointVoices[i].handle)) {
                voices().erase(pointVoices[i].handle);
                pointVoices.erase(pointVoices.begin() + i);
            } else {
                const bool pause = paused || listenerPaused;
                if (pointVoices[i].paused != pause) {
                    getSoloud().setPause(pointVoices[i].handle, pause);
                    pointVoices[i].paused = pause;
                }
                i++;
            }
        }
    }

    for (size_t i = 0; i < pendingPointVoices.size();) {
        PendingPointVoice& pending = pendingPointVoices[i];
        SoundLoadResult result = SoundPool::loadClip(pending.filename, AudioLoadType::Auto);
        if (result.state == ResourceLoadState::Loading) {
            pending.waited += unscaledDt;
            if (pending.waited > oneShotLoadWait) {
                if (slowClips().insert(pending.filename).second) {
                    Log::warn("PlayClipAtPoint of '%s' dropped: the clip was still loading after %.2f s (AudioSystem.preload it)",
                              pending.filename.c_str(), oneShotLoadWait);
                }
                pendingPointVoices.erase(pendingPointVoices.begin() + i);
                continue;
            }
            i++;
            continue;
        }
        const PendingPointVoice shot = pending;
        pendingPointVoices.erase(pendingPointVoices.begin() + i);
        if (result.state == ResourceLoadState::Failed || !result.data || !init()) continue;

        AudioSourceComponent defaults;
        defaults.resolvedGroup = AudioMixer::findGroupId(shot.group);
        if (!shot.group.empty() && defaults.resolvedGroup.empty()) {
            Log::warn("PlayClipAtPoint: mixer group '%s' is not in the mixer, it plays through Master", shot.group.c_str());
        }
        // Unity's PlayClipAtPoint: a 3D source with a logarithmic rolloff
        defaults.attenuationModel = AudioSourceAttenuation::INVERSE_DISTANCE;
        const Vector3 position = heard(shot.position);
        const unsigned int handle = startVoice(defaults, *result.data, shot.volume, hasListener, position, false);
        if (handle == 0) continue;
        PointVoice voice;
        voice.handle = handle;
        voice.sample = result.data;
        voice.is3D = hasListener;
        voice.position = position;
        voice.paused = paused || listenerPaused;
        if (voice.paused) getSoloud().setPause(handle, true);
        pointVoices.push_back(voice);
    }
}

// ---------------------------------------------------------------- fades

void AudioSystem::fadeTo(AudioSourceComponent& audio, double volume, double seconds, bool unscaled){
    volume = std::max(0.0, volume);
    if (!(seconds > 0.0)) {
        audio.fade = AudioFade();
        audio.volume = volume;
        audio.needUpdate = true;
        return;
    }
    AudioFade fade;
    fade.active = true;
    fade.from = audio.volume;
    fade.to = volume;
    fade.duration = seconds;
    fade.unscaled = unscaled;
    audio.fade = fade;
}

void AudioSystem::fadeIn(AudioSourceComponent& audio, double seconds, bool unscaled){
    // from silence to the volume it has (a fadeIn over a running fadeOut goes back to its level)
    const double target = audio.fade.active && audio.fade.restore ? audio.fade.restoreVolume : audio.volume;
    const bool playing = audio.state == AudioSourceState::Playing && !audio.stopTrigger && !audio.pauseTrigger;
    if (!playing || audio.handle == 0) {
        audio.startTrigger = true;
        audio.pauseTrigger = false;
        audio.stopTrigger = false;
        audio.volume = 0;
    }
    audio.needUpdate = true;
    if (!(seconds > 0.0)) {
        audio.fade = AudioFade();
        audio.volume = target;
        return;
    }
    AudioFade fade;
    fade.active = true;
    fade.from = audio.volume;
    fade.to = target;
    fade.duration = seconds;
    fade.unscaled = unscaled;
    fade.restore = true;
    fade.restoreVolume = target;
    audio.fade = fade;
}

void AudioSystem::fadeOut(AudioSourceComponent& audio, double seconds, bool pause, bool unscaled){
    const double restoreVolume = audio.fade.active && audio.fade.restore ? audio.fade.restoreVolume : audio.volume;
    AudioFade fade;
    fade.active = true;
    fade.from = audio.volume;
    fade.to = 0;
    fade.duration = std::max(0.0, seconds);
    fade.unscaled = unscaled;
    fade.end = pause ? AudioFadeEnd::Pause : AudioFadeEnd::Stop;
    fade.restore = true;
    fade.restoreVolume = restoreVolume;
    audio.fade = fade;
}

void AudioSystem::updateFade(AudioSourceComponent& audio, double scaledDt, double unscaledDt, std::vector<std::pair<Entity, int>>& events, Entity entity){
    AudioFade& fade = audio.fade;
    if (!fade.active) return;
    fade.elapsed += fade.unscaled ? unscaledDt : scaledDt;
    const double t = fade.duration > 0.0 ? std::min(1.0, fade.elapsed / fade.duration) : 1.0;
    audio.volume = fade.from + (fade.to - fade.from) * t;
    if (t < 1.0) return;

    const AudioFade done = fade;
    fade = AudioFade();
    if (done.end == AudioFadeEnd::Stop) {
        const bool wasPlaying = audio.state != AudioSourceState::Stopped || audio.handle != 0;
        stopVoices(audio);
        audio.state = AudioSourceState::Stopped;
        if (wasPlaying) events.push_back({entity, EventStop});
    } else if (done.end == AudioFadeEnd::Pause) {
        if (audio.state == AudioSourceState::Playing) {
            audio.state = AudioSourceState::Paused;
            events.push_back({entity, EventPause});
        }
    }
    // the next play() is at the authored level
    if (done.restore && done.end != AudioFadeEnd::None) audio.volume = done.restoreVolume;
    else if (done.restore) audio.volume = done.to;
}

// ---------------------------------------------------------------- the frame

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
        // what moves while it holds is not a speed
        motionFresh = false;
        listenerTracked = false;
        return;
    }

    const double unscaledDt = AudioMixer::getFrameUnscaledDelta();

    auto audios = scene->getComponentArray<AudioSourceComponent>();

    // open the output early, a device started by the first play cuts that sound
    if (!outputRequested && (audios->size() > 0 || !pendingPointVoices.empty())) {
        outputRequested = true;
        init();
    }

    // nothing to place: the next frame's speeds start afresh
    if (audios->size() == 0 && pendingPointVoices.empty() && pointVoices.empty()) {
        motionFresh = false;
        listenerTracked = false;
        return;
    }

    // One listener, the main scene's camera, hears every scene's 3D sources, so without a camera
    // they play in 2D. The persistent scene's (DontDestroyOnLoad) positions are world positions
    // of the main scene. A layer scene (a HUD, a scene drawn over another) is seen through its own
    // camera: its sources are heard where they are in that camera's view, mapped into the main
    // camera's (a layer without the main scene, or over a main scene without a camera, hears
    // through its own)
    const bool persistent = scene == SceneManager::getPersistentScene();
    Scene* mainScene = Engine::getMainScene();
    Scene* listenerScene = scene;
    if (persistent) {
        listenerScene = mainScene;
    } else if (mainScene && mainScene != scene && cameraPose(*mainScene).valid) {
        listenerScene = mainScene;
    }
    const bool mapped = !persistent && listenerScene != scene;
    const CameraPose listenerPose = listenerScene ? cameraPose(*listenerScene) : CameraPose();
    const CameraPose ownPose = mapped ? cameraPose(*scene) : listenerPose;
    const bool hasListener = listenerPose.valid && ownPose.valid;
    auto heard = [&](const Vector3& position) {
        return mapped ? mapToListener(position, ownPose, listenerPose) : position;
    };
    // Doppler under perspective cameras only: 2D and UI scenes move in pixels, where everyday
    // speeds are near the speed of sound (343.3 units a second)
    const bool doppler = hasListener && listenerPose.perspective && ownPose.perspective;
    bool any3D = false;

    // the listener's velocity: where it heard from last frame to where it hears now, in units a
    // second (none for its first frame, another camera, or a frame after the system held)
    listenerVelocity = Vector3(0, 0, 0);
    if (hasListener) {
        const Vector3 position = listenerPose.position;
        const Entity cameraEntity = listenerScene->getCamera();
        if (doppler && motionFresh && listenerTracked && listenerTrackedScene == listenerScene && listenerTrackedCamera == cameraEntity) {
            listenerVelocity = dopplerVelocity(cameraLastPosition, position, unscaledDt, 1.0f);
        }
        cameraLastPosition = position;
        listenerTracked = true;
        listenerTrackedScene = listenerScene;
        listenerTrackedCamera = cameraEntity;
    } else {
        listenerTracked = false;
    }

    // The persistent and layer scenes' 3D sounds are heard by the main scene's camera every frame
    // (the main scene's own system places it only when it plays 3D voices), before they are
    // placed against it
    if (listenerScene != scene && hasListener && listenerEnabled){
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

    // the callbacks run after the loop: a script may add or remove components
    std::vector<std::pair<Entity, int>> events;

    for (int i = 0; i < audios->size(); i++){
		AudioSourceComponent& audio = audios->getComponentFromIndex(i);

        Entity entity = audios->getEntity(i);
        Signature signature = scene->getSignature(entity);
        bool use3DAudio = hasListener && signature.test(scene->getComponentId<Transform>());

        // where the listener hears it (its world position, or mapped from a layer's camera)
        Vector3 worldPosition = Vector3(0, 0, 0);
        if (use3DAudio){
            Transform& transform = scene->getComponent<Transform>(entity);

            worldPosition = heard(transform.worldPosition);
        }

        // no clip: nothing of its own plays (its one-shots still do, Unity's PlayOneShot needs no clip)
        const bool hasClip = !audio.filename.empty();
        if (!hasClip && (audio.loaded || audio.handle != 0 || audio.sample)) {
            if (audio.handle != 0) stopVoice(audio.handle);
            audio.handle = 0;
            audio.playingTime = 0;
            releaseSample(audio);
        }
        if (!hasClip) {
            if (audio.state != AudioSourceState::Stopped) audio.state = AudioSourceState::Stopped;
            audio.startTrigger = false;
            audio.pauseTrigger = false;
        }

        // Unity: disabling an Audio Source or its GameObject stops it and a play request
        // is ignored. A source left Playing starts over on reactivation (play on awake).
        if (!audio.enabled || !scene->isEntityActiveInHierarchy(entity)) {
            if (audio.handle != 0 || !audio.oneShots.empty() || !audio.pendingOneShots.empty()) {
                stopVoices(audio);
            }
            if (audio.state == AudioSourceState::Paused) {
                audio.state = AudioSourceState::Stopped;
            }
            audio.startTrigger = false;
            audio.pauseTrigger = false;
            audio.stopTrigger = false;
            if (audio.fade.active && audio.fade.restore) audio.volume = audio.fade.restoreVolume;
            audio.fade = AudioFade();
            audio.positionTracked = false;
            continue;
        }

        // its velocity (units a second) from where it was last frame, tracked while it is silent
        // too: a one-shot or a play of a moving source starts at its speed
        Vector3 velocity(0, 0, 0);
        bool moved = false;
        if (use3DAudio) {
            if (doppler && motionFresh && audio.positionTracked) {
                velocity = dopplerVelocity(audio.lastPosition, worldPosition, unscaledDt, audio.dopplerFactor);
            }
            moved = !audio.positionTracked || audio.lastPosition != worldPosition;
            audio.lastPosition = worldPosition;
            audio.positionTracked = true;
        } else {
            audio.positionTracked = false;
        }

        const std::string groupBefore = audio.resolvedGroup;
        resolveGroup(audio, entity);

        // a voice that ended by itself (or was stolen): Stopped, onStop (spec A15)
        if (inited && audio.handle != 0 && !getSoloud().isValidVoiceHandle(audio.handle)) {
            voices().erase(audio.handle);
            audio.handle = 0;
            audio.playingTime = 0;
            if (audio.state != AudioSourceState::Stopped && !audio.startTrigger) {
                audio.state = AudioSourceState::Stopped;
                events.push_back({entity, EventStop});
            }
        }
        if (inited) {
            for (size_t s = 0; s < audio.oneShots.size();) {
                if (!getSoloud().isValidVoiceHandle(audio.oneShots[s].handle)) {
                    voices().erase(audio.oneShots[s].handle);
                    audio.oneShots.erase(audio.oneShots.begin() + s);
                } else {
                    s++;
                }
            }
        }

        // the state says Stopped (an undo, a scene edit in Play): its voice stops too
        if (audio.state == AudioSourceState::Stopped && audio.handle != 0 && !audio.startTrigger) {
            stopVoice(audio.handle);
            audio.handle = 0;
            audio.playingTime = 0;
        }

        bool pendingStart = audio.startTrigger || (audio.state == AudioSourceState::Playing && audio.handle == 0);

        if (hasClip && (audio.state == AudioSourceState::Playing || pendingStart)){
            if (!audio.loaded){
                loadSound(audio);
            }
        }

        if (audio.pauseTrigger){
            audio.pauseTrigger = false;
            if (audio.state == AudioSourceState::Playing) {
                audio.state = AudioSourceState::Paused;
                events.push_back({entity, EventPause});
            }
        }
        if (audio.stopTrigger){
            audio.stopTrigger = false;

            // stop() stops the one-shots too (Unity)
            const bool hadVoices = audio.handle != 0 || !audio.oneShots.empty();
            stopVoices(audio);
            if (audio.fade.active && audio.fade.restore) audio.volume = audio.fade.restoreVolume;
            audio.fade = AudioFade();
            if (audio.state != AudioSourceState::Stopped || hadVoices) {
                events.push_back({entity, EventStop});
            }
            audio.state = AudioSourceState::Stopped;
        }
        // after the stop: a stop() before the first start keeps it stopped
        pendingStart = audio.startTrigger || (audio.state == AudioSourceState::Playing && audio.handle == 0);

        if (hasClip && audio.loaded){
            if (pendingStart){
                const bool resume = audio.state == AudioSourceState::Paused && audio.handle != 0 && audio.startTrigger;
                audio.startTrigger = false;

                // the file was hot reloaded: this play takes the new sample (one still
                // playing finishes on the old, which its other sources hold)
                if (!resume && audio.sampleGeneration != SoundPool::generation(audio.filename)) {
                    if (std::shared_ptr<SoLoud::AudioSource> fresh = SoundPool::get(audio.sampleKey)) {
                        audio.sample = fresh;
                        audio.sampleGeneration = SoundPool::generation(audio.filename);
                        audio.length = SoundPool::getLength(audio.sample.get());
                    }
                }

                // a play() over a fadeOut: the fade is cut (it would stop or pause the new start)
                // and the authored volume comes back
                if (audio.fade.active && audio.fade.end != AudioFadeEnd::None) {
                    if (audio.fade.restore) audio.volume = audio.fade.restoreVolume;
                    audio.fade = AudioFade();
                }

                if (resume) {
                    audio.state = AudioSourceState::Playing;
                    events.push_back({entity, EventStart});
                } else {
                    // play() on a playing source starts it over (its own voice only; spec A9)
                    if (audio.handle != 0) {
                        stopVoice(audio.handle);
                        audio.handle = 0;
                    }
                    audio.state = AudioSourceState::Playing;
                    audio.voicePaused = desiredPause(audio);
                    audio.handle = startVoice(audio, *audio.sample, (float)audio.volume, use3DAudio, worldPosition, true);
                    if (audio.handle == 0) {
                        // no audio, or the voice budget refused it: as if it ended at once
                        audio.playingTime = 0;
                        audio.state = AudioSourceState::Stopped;
                    } else {
                        audio.lastVolumeApplied = audio.volume;
                        audio.needUpdate = true;
                        events.push_back({entity, EventStart});
                    }
                }
            }
        }
        startPendingOneShots(audio, use3DAudio, worldPosition, unscaledDt);

        // the group changed (the mixer was edited, outputGroup set): the voices move to its bus
        if (inited && groupBefore != audio.resolvedGroup) {
            if (SoLoud::Bus* bus = AudioMixer::getBus(audio.resolvedGroup)) {
                std::vector<unsigned int> handles;
                if (audio.handle) handles.push_back(audio.handle);
                for (const AudioOneShot& shot : audio.oneShots) handles.push_back(shot.handle);
                for (unsigned int handle : handles) {
                    bus->annexSound(handle);
                    auto it = voices().find(handle);
                    if (it != voices().end()) it->second.groupId = audio.resolvedGroup;
                }
            }
        }

        updateFade(audio, dt, unscaledDt, events, entity);
        applyPause(audio);

        if (!inited) continue;
        SoLoud::Soloud& soloud = getSoloud();

        const bool hasVoices = audio.handle != 0 || !audio.oneShots.empty();
        if (!hasVoices) {
            audio.needUpdate = false;
            // a new voice starts without a velocity
            audio.lastVelocity = Vector3(0, 0, 0);
            continue;
        }

        if (audio.handle != 0) {
            audio.playingTime = soloud.getStreamPosition(audio.handle);
        }

        // the volume (fades, scripts): one-shots follow it, scaled
        if (audio.volume != audio.lastVolumeApplied) {
            const double frame = std::max(0.0, unscaledDt);
            const bool smooth = audio.fade.active && frame > 0.0;
            auto write = [&](unsigned int handle, float volume) {
                if (smooth) soloud.fadeVolume(handle, volume, frame);
                else soloud.setVolume(handle, volume);
            };
            if (audio.handle != 0) write(audio.handle, (float)audio.volume);
            for (const AudioOneShot& shot : audio.oneShots) write(shot.handle, (float)audio.volume * shot.volumeScale);
            audio.lastVolumeApplied = audio.volume;
        }

        if (audio.needUpdate) {
            if (audio.handle != 0) applyVoiceParameters(audio, audio.handle, true, use3DAudio);
            for (const AudioOneShot& shot : audio.oneShots) applyVoiceParameters(audio, shot.handle, false, use3DAudio);
        }

        // 3D voices follow the source (its one-shots too, Unity)
        // and when it stops, a velocity of none (its pitch comes back)
        if (use3DAudio && (audio.needUpdate || moved || velocity != audio.lastVelocity)) {
            if (audio.handle != 0) apply3dParameters(audio, audio.handle, worldPosition, velocity);
            for (const AudioOneShot& shot : audio.oneShots) apply3dParameters(audio, shot.handle, worldPosition, velocity);
            audio.lastVelocity = velocity;
        }
        if (use3DAudio) {
            any3D = true;
        }

        audio.needUpdate = false;
    }

    // the next frame's velocities measure from this one's positions
    motionFresh = true;

    updatePointVoices(unscaledDt, hasListener, heard);
    for (const PointVoice& voice : pointVoices) any3D = any3D || voice.is3D;

    // the listener at the camera, once a frame while a 3D voice plays (the persistent scene's
    // system too: the main scene's own places it only when it has 3D voices)
    if (inited && hasListener && listenerEnabled && any3D) {
        placeListener(*listenerScene);
    } else if (inited && any3D) {
        getSoloud().update3dAudio();
    }

    for (const auto& [entity, kind] : events) {
        AudioSourceComponent* audio = scene->findComponent<AudioSourceComponent>(entity);
        if (!audio) continue;
        if (kind == EventStart) audio->onStart.call();
        else if (kind == EventPause) audio->onPause.call();
        else audio->onStop.call();
    }
}

void AudioSystem::placeListener(Scene& listenerScene){
    const CameraPose pose = cameraPose(listenerScene);
    const Vector3& position = pose.position;
    const Vector3& view = pose.view;
    const Vector3& up = pose.worldUp;
    const Vector3& velocity = listenerVelocity;
    getSoloud().set3dListenerParameters(position.x, position.y, position.z, view.x, view.y, view.z,
                                        up.x, up.y, up.z, velocity.x, velocity.y, velocity.z);
    if (inited){
        getSoloud().update3dAudio();
    }
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
