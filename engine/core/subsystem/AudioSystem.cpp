// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioSystem.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "PlatformConfig.h"
#include "manager/SceneManager.h"

#include "asset/AssetMap.h"
#include "audio/AudioCapture.h"
#include "audio/AudioClock.h"
#include "audio/AudioMixer.h"
#include "audio/AudioTap.h"
#include "audio/SoundEvent.h"
#if JASPERENGINE_SOUND_LAYER
#include "audio/SoundEventIO.h"
#include "audio/SoundEventLibrary.h"
#include "component/ButtonComponent.h"
#include "component/UIComponent.h"
#include "jobs/JobSystem.h"
#include "object/sound/Audio.h"
#include "object/sound/SoundHandle.h"
#endif
#include "pool/SoundPool.h"
#include "subsystem/PhysicsSystem.h"
#include "Scene.h"

#include "soloud.h"
#include "soloud_wav.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "json.hpp"

using namespace jasperengine;

bool AudioSystem::inited = false;
bool AudioSystem::outputUnavailable = false;

float AudioSystem::globalVolume = 1.0;

namespace {

    // what started every voice the engine holds (sources, one-shots, playClipAtPoint)
    struct VoiceRecord {
        Scene* owner = nullptr;
        std::string groupId;
        int priority = 128;
        uint64_t serial = 0;
        bool isProtected = false;
        // Audio.stopAll's: Audio.play, triggers, one-shots; not a source's main voice
        bool freeOrOneShot = false;
    };

    std::unordered_map<unsigned int, VoiceRecord>& voices() {
        static auto* map = new std::unordered_map<unsigned int, VoiceRecord>();
        return *map;
    }

    uint64_t nextSerial = 1;

#if JASPERENGINE_SOUND_LAYER
    // ---- free voices (docs/specs/S37-sound-events.md R13)

    struct FreeVoice {
        unsigned int handle = 0;
        std::shared_ptr<SoLoud::AudioSource> sample;
        float volumeScale = 1.0f;
        float speedScale = 1.0f;
        std::shared_ptr<const SoundEventDef> event;
    };

    // one play: an entry a SoundHandle names by index + 1 and generation
    struct FreePlay {
        bool used = false;
        uint32_t generation = 1;
        AudioSystem* owner = nullptr;
        // not started yet: a call made in a physics callback, or its delay running
        bool waiting = false;
        std::string name;
        const char* cause = "cpp";
        double delayLeft = 0.0;
        bool delayUnscaled = false;
        std::string group;
        std::vector<FreeVoice> voices;
        float volume = 1.0f;
        float pitch = 1.0f;
        float pan = 0.0f;
        Vector3 position = Vector3(0, 0, 0);
        bool is3D = false;
        bool wants3D = false;
        Scene* targetScene = nullptr;
        Entity target = NULL_ENTITY;
        bool paused = false;
        bool appliedPause = false;
        AudioFade fade;
        Vector3 lastHeard = Vector3(0, 0, 0);
        bool heardTracked = false;
    };

    std::vector<FreePlay>& freePlays() {
        static auto* plays = new std::vector<FreePlay>();
        return *plays;
    }

    // what is deferred comes from any thread; so do reads of a handle there
    std::recursive_mutex& freeMutex() {
        static auto* mutex = new std::recursive_mutex();
        return *mutex;
    }

    std::vector<std::function<void()>>& deferredCalls() {
        static auto* calls = new std::vector<std::function<void()>>();
        return *calls;
    }

    std::atomic<bool> deferredPending{false};
    thread_local int physicsCallbackDepth = 0;

    void deferCall(std::function<void()> call) {
        std::lock_guard<std::recursive_mutex> lock(freeMutex());
        deferredCalls().push_back(std::move(call));
        deferredPending.store(true, std::memory_order_relaxed);
    }

    FreePlay* freePlayOf(uint32_t id, uint32_t generation) {
        std::vector<FreePlay>& plays = freePlays();
        if (id == 0 || id > plays.size()) return nullptr;
        FreePlay& play = plays[id - 1];
        return play.used && play.generation == generation ? &play : nullptr;
    }

    void releaseFreePlay(FreePlay& play) {
        const uint32_t generation = play.generation + 1;
        play = FreePlay();
        play.generation = generation;
    }

    // a voice of it still valid; the entry goes when none is and it is not about to start
    bool freePlayLive(FreePlay& play) {
        if (play.waiting) return true;
        if (AudioSystem::isInited()) {
            SoLoud::Soloud& soloud = AudioSystem::getSoloud();
            for (const FreeVoice& voice : play.voices) {
                if (soloud.isValidVoiceHandle(voice.handle)) return true;
            }
        }
        releaseFreePlay(play);
        return false;
    }

#endif

    // ---- Sound Events (docs/specs/S37-sound-events.md)

#if JASPERENGINE_SOUND_LAYER
    // a live play of an event with a limit (R6.2), over every scene
    struct EventInstance {
        uint64_t id = 0;
        std::string event;
        std::vector<unsigned int> handles;
        int priority = 128;
        bool isProtected = false;
        uint64_t serial = 0;
    };

    std::map<uint64_t, EventInstance>& eventInstances() {
        static auto* map = new std::map<uint64_t, EventInstance>();
        return *map;
    }

    uint64_t nextInstance = 1;
#endif

    // the frames mixed through the null driver since it opened, by the pump or a capture
    uint64_t mixedFrames = 0;
#if JASPERENGINE_SOUND_LAYER
    bool soundEventSeedChosen = false;

    // one warning per clip, event or group per Play
    std::set<std::string>& soundEventWarnings() {
        static auto* set = new std::set<std::string>();
        return *set;
    }
#endif

    std::function<void(const AudioVoiceLine&)>& voiceObserver() {
        static auto* observer = new std::function<void(const AudioVoiceLine&)>();
        return *observer;
    }

    bool voiceObserverSet = false;

    // JASPERENGINE_AUDIO_TRACE=1: every voice line goes to the log as JSON, for tests and for an
    // agent reading the console, with no capture running
    bool voiceTraceRead = false;
    bool voiceTraceOn = false;

    bool voiceLinesWanted() {
        if (!voiceTraceRead) {
            voiceTraceRead = true;
            const char* env = std::getenv("JASPERENGINE_AUDIO_TRACE");
            voiceTraceOn = env && std::strcmp(env, "1") == 0;
        }
        return voiceObserverSet || voiceTraceOn;
    }

    void traceVoiceLine(const AudioVoiceLine& line) {
        nlohmann::ordered_json json;
        json["type"] = line.kind == AudioVoiceLine::Kind::Start ? "voice_start" : line.kind == AudioVoiceLine::Kind::Stop ? "voice_stop" :
                       line.kind == AudioVoiceLine::Kind::Reject ? "voice_reject" : line.kind == AudioVoiceLine::Kind::Pause ? "voice_pause" : "voice_resume";
        json["sample"] = line.sample;
        if (line.reason[0]) json["reason"] = line.reason;
        if (line.handle) json["voice"] = line.handle;
        json["entity"] = line.entity == NULL_ENTITY ? (int64_t)-1 : (int64_t)line.entity;
        if (!line.clip.empty()) json["clip"] = line.clip;
        if (!line.event.empty()) json["event"] = line.event;
        if (!line.eventPath.empty()) json["event_path"] = line.eventPath;
        if (line.variation >= 0) json["variation"] = line.variation;
        if (line.layer >= 0) json["layer"] = line.layer;
        if (line.instance) json["instance"] = line.instance;
        if (line.byInstance) json["by_instance"] = line.byInstance;
        json["owner"] = line.owner;
        json["cause"] = line.cause;
        if (line.trigger[0]) json["trigger"] = line.trigger;
        if (line.impactSpeed >= 0.0f) json["impact_speed"] = line.impactSpeed;
        json["gain"] = line.gain;
        json["pitch"] = line.pitch;
        json["priority"] = line.priority;
        json["group"] = line.group;
        if (line.delayFrames) json["delay_frames"] = line.delayFrames;
        Log::print("[audio] %s", json.dump().c_str());
    }

    void emitVoiceLine(AudioVoiceLine& line) {
        line.sample = mixedFrames;
        if (voiceObserverSet) voiceObserver()(line);
        if (voiceTraceOn) traceVoiceLine(line);
    }

    // why the stops made while one lives happen: the voice lines' reason
    struct StopReason {
        static inline const char* current = "stop";
        static inline uint64_t byInstance = 0;
        const char* previous;
        uint64_t previousBy;
        explicit StopReason(const char* reason, uint64_t by = 0): previous(current), previousBy(byInstance) {
            current = reason;
            byInstance = by;
        }
        ~StopReason() {
            current = previous;
            byInstance = previousBy;
        }
        StopReason(const StopReason&) = delete;
        StopReason& operator=(const StopReason&) = delete;
    };

    void stopVoiceBecause(unsigned int handle, const char* reason, uint64_t byInstance) {
        const StopReason because(reason, byInstance);
        AudioSystem::stopVoice(handle);
    }

    void pauseVoiceLine(unsigned int handle, bool paused) {
        if (handle == 0 || !voiceLinesWanted()) return;
        AudioVoiceLine line;
        line.kind = paused ? AudioVoiceLine::Kind::Pause : AudioVoiceLine::Kind::Resume;
        line.handle = handle;
        emitVoiceLine(line);
    }

#if JASPERENGINE_SOUND_LAYER
    uint64_t chooseSoundEventSeed() {
        if (!soundEventSeedChosen) {
            soundEventSeedChosen = true;
            // what the run is depends on the output: a source's sample may be held from before it opened
            AudioSystem::init();
            if (AudioSystem::isDeterministic()) SoundEventLibrary::setSeed(AudioSystem::getConfig().seed);
        }
        return SoundEventLibrary::getSeed();
    }

    SoundEventClock soundEventClock() {
        SoundEventClock clock;
        clock.samples = AudioSystem::isDeterministic();
        clock.cursor = mixedFrames;
        clock.rate = AudioSystem::isInited() ? AudioSystem::getSoloud().getBackendSamplerate() : 48000;
        // with a device: the mixer's clock, in step with what the player hears
        clock.seconds = AudioMixer::getClockTime();
        return clock;
    }

    // what the limit's steal needs: the instance's loudest voice now
    float instanceLoudness(const EventInstance& instance) {
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        float loudest = 0.0f;
        for (unsigned int handle : instance.handles) {
            loudest = std::max(loudest, soloud.getOverallVolume(handle));
        }
        return loudest;
    }

    // The live instances of `event` (forgetting the ended), and which gives way to one of `priority`
    // (nullptr: none may). `full`: at its limit.
    const EventInstance* limitCandidate(const SoundEventDef& def, const std::string& event, int priority, bool& full) {
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        std::vector<const EventInstance*> live;
        std::map<uint64_t, EventInstance>& instances = eventInstances();
        for (auto it = instances.begin(); it != instances.end();) {
            EventInstance& instance = it->second;
            if (instance.event != event) {
                ++it;
                continue;
            }
            instance.handles.erase(std::remove_if(instance.handles.begin(), instance.handles.end(),
                                                  [&](unsigned int handle) { return !soloud.isValidVoiceHandle(handle); }),
                                   instance.handles.end());
            if (instance.handles.empty()) {
                it = instances.erase(it);
                continue;
            }
            live.push_back(&instance);
            ++it;
        }
        full = (int)live.size() >= def.maxInstances;
        if (!full || def.steal == AudioStealPolicy::None) return nullptr;

        const EventInstance* chosen = nullptr;
        for (const EventInstance* instance : live) {
            if (instance->isProtected || instance->priority < priority) continue;
            if (!chosen) {
                chosen = instance;
                continue;
            }
            bool better = false;
            if (def.steal == AudioStealPolicy::Oldest) {
                better = instance->serial < chosen->serial;
            } else if (def.steal == AudioStealPolicy::Newest) {
                better = instance->serial > chosen->serial;
            } else {
                const float a = instanceLoudness(*instance);
                const float b = instanceLoudness(*chosen);
                better = a < b || (a == b && instance->id < chosen->id);
            }
            if (better) chosen = instance;
        }
        return chosen;
    }
#endif
    // the Null backend's pump: the frames it mixed and the time it was given since the audio opened.
    // Each frame mixes up to round(time x rate), so no rounding piles up (144 fps at 44.1 kHz is
    // 306.25 frames a frame: rounded each time, 48 ms a minute behind the game)
    double pumpSeconds = 0.0;
    uint64_t pumpFrames = 0;
    AudioConfig config;
    AudioBackend backend = AudioBackend::Auto;
    bool backendFromEnvironment = false;
    // the deterministic driver is open, its clock, and who holds it (beginDeterministic)
    bool deterministicDriver = false;
    AudioClock deterministicClock;
    int deterministicHolds = 0;
    bool deterministicSwitched = false;
    AudioBackend backendBeforeDeterministic = AudioBackend::Auto;
    bool deviceBeforeDeterministic = false;
    constexpr unsigned int deterministicRate = 48000;
    constexpr unsigned int deterministicChunk = 512;
    constexpr unsigned int deterministicChannels = 2;

    void readBackendEnvironment() {
        if (backendFromEnvironment) return;
        backendFromEnvironment = true;
        if (const char* env = std::getenv("JASPERENGINE_AUDIO_BACKEND")) {
            if (std::strcmp(env, "nosound") == 0) backend = AudioBackend::NoSound;
            else if (std::strcmp(env, "null") == 0) backend = AudioBackend::Null;
            else if (std::strcmp(env, "deterministic") == 0) backend = AudioBackend::Deterministic;
        }
    }
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
    void registerVoice(unsigned int handle, Scene* owner, const std::string& groupId, int priority, bool isProtected, bool freeOrOneShot) {
        if (handle == 0) return;
        VoiceRecord record;
        record.owner = owner;
        record.groupId = groupId;
        record.priority = priority;
        record.serial = nextSerial++;
        record.isProtected = isProtected;
        record.freeOrOneShot = freeOrOneShot;
        voices()[handle] = record;
    }

}

AudioSystem::AudioSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<AudioSourceComponent>());
    // the type onComponentRemoved handles
    declareHookType(scene->getComponentId<AudioSourceComponent>());

    cameraLastPosition = Vector3(0, 0, 0);
    triggerSink = std::make_shared<TriggerSink>();
    triggerSink->system = this;
}

AudioSystem::~AudioSystem(){
    triggerSink->system = nullptr;
    forgetFreeVoices();
}

SoLoud::Soloud& AudioSystem::getSoloud(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static SoLoud::Soloud* soloud = new SoLoud::Soloud();
    return *soloud;
};

void AudioSystem::configure(const AudioConfig& newConfig){
    const AudioOutputStage stageBefore = config.outputStage;
    config = newConfig;
    config.maxRealVoices = std::clamp(config.maxRealVoices, 1, 255);
    config.maxVoices = std::clamp(config.maxVoices, 1, 1000);
    if (config.sampleRate < 0) config.sampleRate = 0;
    if (inited) {
        // the real voices and the output stage apply at once; the sample rate at the next init
        AudioMixer::onAudioInit();
        if (config.outputStage != stageBefore) applyOutputStage();
    }
}

AudioOutputStage AudioSystem::outputStageFromName(const std::string& name){
    return name == "linear" ? AudioOutputStage::Linear : AudioOutputStage::Roundoff;
}

IosAudioSession AudioSystem::iosSessionFromName(const std::string& name){
    if (name == "ambient") return IosAudioSession::Ambient;
    if (name == "solo_ambient") return IosAudioSession::SoloAmbient;
    if (name == "playback") return IosAudioSession::Playback;
    return IosAudioSession::Default;
}

void AudioSystem::applyOutputStage(){
    SoLoud::Soloud& soloud = getSoloud();
    const bool linear = config.outputStage == AudioOutputStage::Linear;
    // the mixer reads both once per mix
    soloud.mFlags = linear ? (soloud.mFlags & ~(unsigned int)SoLoud::Soloud::CLIP_ROUNDOFF)
                           : (soloud.mFlags | (unsigned int)SoLoud::Soloud::CLIP_ROUNDOFF);
    soloud.setPostClipScaler(linear ? 1.0f : 0.95f);
    Log::print("Audio output stage: %s", config.outputStage == AudioOutputStage::Linear ? "linear" : "roundoff");
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
    readBackendEnvironment();
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

        readBackendEnvironment();

        // --headless and --silent-audio open no device but mix as a device would, on the
        // deterministic driver (docs/specs/S30-shipping.md R17): sounds play, end and fire their
        // events, so the game's logic is that of a normal run, the same on every run
        AudioBackend opened = backend;
        if (platformConfig().silentAudio && opened == AudioBackend::Auto) {
            opened = AudioBackend::Deterministic;
        }

        SoLoud::Soloud& soloud = getSoloud();
        const unsigned int flags = config.outputStage == AudioOutputStage::Linear ? 0 : (unsigned int)SoLoud::Soloud::CLIP_ROUNDOFF;
#if defined(__APPLE__) && TARGET_OS_IOS
        soloud.mIosSessionCategory = (unsigned int)config.iosSession;
#endif
        const unsigned int rate = config.sampleRate > 0 ? (unsigned int)config.sampleRate : (unsigned int)SoLoud::Soloud::AUTO;
        SoLoud::result result = SoLoud::SO_NO_ERROR + 1;

        outputAvailable = false;
        if (opened == AudioBackend::Deterministic) {
            result = soloud.init(flags, SoLoud::Soloud::NULLDRIVER, deterministicRate, deterministicChunk, deterministicChannels);
            if (result == SoLoud::SO_NO_ERROR) {
                deterministicDriver = true;
                deterministicClock.reset(deterministicRate);
                Log::print("Audio: deterministic driver (%u Hz, game-time pump)", deterministicRate);
            }
        }
        if (opened == AudioBackend::Auto) {
            result = soloud.init(flags, SoLoud::Soloud::MINIAUDIO, rate);
            outputAvailable = result == SoLoud::SO_NO_ERROR;
            if (!outputAvailable) {
                Log::warn("No audio output available (%s): mixing without output", soloud.getErrorString(result));
            }
        }
        if (result != SoLoud::SO_NO_ERROR && opened != AudioBackend::Null && opened != AudioBackend::Deterministic) {
            result = soloud.init(flags, SoLoud::Soloud::NOSOUND, rate == SoLoud::Soloud::AUTO ? 44100 : rate);
        }
        if (result != SoLoud::SO_NO_ERROR && opened != AudioBackend::Deterministic) {
            // no threads (a Web build without pthreads): mixed on the main thread each frame
            result = soloud.init(flags, SoLoud::Soloud::NULLDRIVER, rate == SoLoud::Soloud::AUTO ? 44100 : rate, 1024);
        }
        if (result != SoLoud::SO_NO_ERROR) {
            outputUnavailable = true;
            Log::warn("No audio backend opened (%s): sounds are muted", soloud.getErrorString(result));
            return false;
        }

        soloud.setGlobalVolume(globalVolume);
        applyOutputStage();

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
    // the deterministic driver has its own pump, after the scenes' update
    if (!inited || deterministicDriver || getSoloud().getBackendId() != SoLoud::Soloud::NULLDRIVER || !(seconds > 0.0)) return;
    SoLoud::Soloud& soloud = getSoloud();
    const unsigned int channels = std::max(1u, soloud.getBackendChannels());
    pumpSeconds += seconds;
    const uint64_t due = (uint64_t)std::llround(pumpSeconds * soloud.getBackendSamplerate());
    uint64_t samples = due > pumpFrames ? due - pumpFrames : 0;
    pumpFrames += samples;
    mixedFrames += samples;
    static std::vector<float> buffer;
    constexpr unsigned int chunk = 1024;
    buffer.resize(chunk * channels);
    while (samples > 0) {
        const unsigned int now = (unsigned int)std::min<uint64_t>(samples, chunk);
        soloud.mix(buffer.data(), now);
        samples -= now;
    }
}

AudioOutputMode AudioSystem::getOutputMode(){
    if (inited) {
        if (deterministicDriver) return AudioOutputMode::Deterministic;
        return outputAvailable ? AudioOutputMode::Device : AudioOutputMode::NoOutput;
    }
    readBackendEnvironment();
    if (backend == AudioBackend::Deterministic || (backend == AudioBackend::Auto && platformConfig().silentAudio)) return AudioOutputMode::Deterministic;
    return isOutputAvailable() ? AudioOutputMode::Device : AudioOutputMode::NoOutput;
}

void AudioSystem::pumpDeterministic(double unscaledSeconds){
    if (!deterministicDriver) return;
    uint32_t frames = deterministicClock.advance(unscaledSeconds);
    if (frames == 0) return;
    static float buffer[deterministicChunk * deterministicChannels];
    SoLoud::Soloud& soloud = getSoloud();
    uint64_t first = deterministicClock.cursor() - frames;
    while (frames > 0) {
        const uint32_t now = std::min(frames, deterministicChunk);
        soloud.mix(buffer, now);
        mixedFrames += now;
        AudioTap::publish(buffer, now, first);
        first += now;
        frames -= now;
    }
#if JASPERENGINE_AUDIO_CAPTURE
    if (AudioCapture::isActive()) AudioCapture::frameMixed();
#endif
}

uint64_t AudioSystem::getMixedFrames(){
    return mixedFrames;
}

unsigned int AudioSystem::getDeterministicLatency(const std::string& groupId){
    return 1u + (unsigned int)AudioMixer::getGroupDepth(groupId) + AudioMixer::getOutputLatencyFrames(groupId);
}

bool AudioSystem::beginDeterministic(){
    if (deterministicHolds++ == 0) {
        readBackendEnvironment();
        deterministicSwitched = getOutputMode() != AudioOutputMode::Deterministic;
        if (deterministicSwitched) {
            backendBeforeDeterministic = backend;
            deviceBeforeDeterministic = hasDevice();
            backend = AudioBackend::Deterministic;
            deInit();
            outputUnavailable = false;
        }
    }
    return init() && deterministicDriver;
}

bool AudioSystem::isDeterministicHeld(){
    return deterministicHolds > 0;
}

AudioBackend AudioSystem::getSelectedBackend(){
    readBackendEnvironment();
    return deterministicHolds > 0 && deterministicSwitched ? backendBeforeDeterministic : backend;
}

bool AudioSystem::hasDevice(){
    if (deterministicHolds > 0 && deterministicSwitched && backend == AudioBackend::Deterministic) return deviceBeforeDeterministic;
    return init() && outputAvailable;
}

void AudioSystem::endDeterministic(){
    if (deterministicHolds == 0 || --deterministicHolds > 0) return;
    if (!deterministicSwitched) return;
    deterministicSwitched = false;
    backend = backendBeforeDeterministic;
    deInit();
    outputUnavailable = false;
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

void AudioSystem::deInit(){
    pumpSeconds = 0.0;
    pumpFrames = 0;
    mixedFrames = 0;
    deterministicDriver = false;
    if (inited){
        AudioMixer::onAudioShutdown();
        getSoloud().deinit();
        voices().clear();
#if JASPERENGINE_SOUND_LAYER
        eventInstances().clear();
        {
            std::lock_guard<std::recursive_mutex> lock(freeMutex());
            for (FreePlay& play : freePlays()) {
                if (play.used) releaseFreePlay(play);
            }
        }
#endif

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
    // (a handle that ended or was stopped before has gone from the list)
    if (voiceLinesWanted() && voices().count(handle)) {
        AudioVoiceLine line;
        line.kind = AudioVoiceLine::Kind::Stop;
        line.reason = StopReason::current;
        line.handle = handle;
        line.byInstance = StopReason::byInstance;
        emitVoiceLine(line);
    }
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
    for (const AudioOneShot& shot : source.mainLayers) handles.push_back(shot.handle);
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
    target.mainVolumeScale = source.mainVolumeScale;
    target.mainSpeedScale = source.mainSpeedScale;
    target.mainInstance = source.mainInstance;
    target.mainEventSample = source.mainEventSample;
    target.mainEvent = source.mainEvent;
    target.mainEventName = source.mainEventName;
    target.mainLayers = source.mainLayers;
    target.startCount = source.startCount;
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
    source.mainLayers.clear();
    source.mainEvent.reset();
    source.mainEventSample.reset();
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

    const StopReason changed("clip_changed");
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

        // the source plays its event: its clip is not loaded
        if (JASPERENGINE_SOUND_LAYER && !audio.event.empty()) {
            if (audio.loaded || audio.sample) releaseSample(audio);
            if (audio.preloadedEvent != audio.event.path) preloadEvent(audio);
            continue;
        }

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
        if (audio.filename.empty() || !audio.event.empty()) {
            continue;
        }

        total++;
        // a missing file must not hold the load
        if (audio.loaded || audio.loadFailed) {
            loaded++;
        }
    }
}

void AudioSystem::stopMainVoices(AudioSourceComponent& audio){
    if (audio.handle != 0) {
        stopVoice(audio.handle);
    }
    for (const AudioOneShot& layer : audio.mainLayers) {
        stopVoice(layer.handle);
    }
    audio.handle = 0;
    audio.mainLayers.clear();
    audio.mainVolumeScale = 1.0f;
    audio.mainSpeedScale = 1.0f;
    audio.mainInstance = 0;
    audio.mainEvent.reset();
    audio.mainEventSample.reset();
    audio.mainEventName.clear();
    audio.playingTime = 0;
}

// a layer that ended goes; the first left is the main voice when `handle` ended
void AudioSystem::pruneMainVoices(AudioSourceComponent& audio){
    SoLoud::Soloud& soloud = getSoloud();
    for (size_t i = 0; i < audio.mainLayers.size();) {
        if (!soloud.isValidVoiceHandle(audio.mainLayers[i].handle)) {
            voices().erase(audio.mainLayers[i].handle);
            audio.mainLayers.erase(audio.mainLayers.begin() + i);
        } else {
            i++;
        }
    }
    if (audio.handle != 0 && !soloud.isValidVoiceHandle(audio.handle) && !audio.mainLayers.empty()) {
        voices().erase(audio.handle);
        const AudioOneShot next = audio.mainLayers.front();
        audio.mainLayers.erase(audio.mainLayers.begin());
        audio.handle = next.handle;
        audio.mainVolumeScale = next.volumeScale;
        audio.mainSpeedScale = next.speedScale;
        audio.mainEventSample = next.sample;
    }
}

void AudioSystem::stopVoices(AudioSourceComponent& audio){
    stopMainVoices(audio);
    for (const AudioOneShot& shot : audio.oneShots) {
        stopVoice(shot.handle);
    }
    audio.oneShots.clear();
    audio.pendingOneShots.clear();
    audio.playingTime = 0;
    audio.voicePaused = false;
    audio.lastVolumeApplied = -1;
}

void AudioSystem::destroySound(AudioSourceComponent& audio){
    const StopReason removed("removed");
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
    for (const AudioOneShot& layer : audio.mainLayers) getSoloud().seek(layer.handle, time);
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
    if (audio.handle != 0) {
        getSoloud().setPause(audio.handle, pause);
        pauseVoiceLine(audio.handle, pause);
    }
    for (const AudioOneShot& layer : audio.mainLayers) {
        getSoloud().setPause(layer.handle, pause);
        pauseVoiceLine(layer.handle, pause);
    }
    for (const AudioOneShot& shot : audio.oneShots) {
        getSoloud().setPause(shot.handle, pause);
        pauseVoiceLine(shot.handle, pause);
    }
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
                pauseVoiceLine(voice.handle, pause);
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
    const StopReason sceneStops("scene");
    auto audios = scene->getComponentArray<AudioSourceComponent>();
    for (int i = 0; i < audios->size(); i++) {
        AudioSourceComponent& audio = audios->getComponentFromIndex(i);

        stopVoices(audio);
        audio.startTrigger = false;
        audio.pauseTrigger = false;
        audio.stopTrigger = false;
        if (audio.fade.active && audio.fade.restore) audio.volume = audio.fade.restoreVolume;
        audio.fade = AudioFade();
        audio.startCount = 0;
    }

    for (const PointVoice& voice : pointVoices) stopVoice(voice.handle);
    pointVoices.clear();
    pendingPointVoices.clear();
    forgetFreeVoices();
    removeTriggerSubscriptions();

    // and anything else this scene started (a voice whose component went first)
    std::vector<unsigned int> owned;
    for (const auto& [handle, record] : voices()) {
        if (record.owner == scene) owned.push_back(handle);
    }
    for (unsigned int handle : owned) stopVoice(handle);
}

void AudioSystem::stopAll(){
    const StopReason everything("stop_all");
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

void AudioSystem::apply3dParameters(AudioSourceComponent& audio, unsigned int handle, const Vector3& position, const Vector3& velocity,
                                    const SoundEventSpatial* spatial){
    SoLoud::Soloud& soloud = getSoloud();
    const AudioSourceAttenuation attenuation = spatial ? spatial->attenuation : audio.attenuationModel;
    unsigned int attModel = SoLoud::AudioSource::NO_ATTENUATION;
    if (attenuation == AudioSourceAttenuation::INVERSE_DISTANCE)
        attModel = SoLoud::AudioSource::INVERSE_DISTANCE;
    if (attenuation == AudioSourceAttenuation::LINEAR_DISTANCE)
        attModel = SoLoud::AudioSource::LINEAR_DISTANCE;
    if (attenuation == AudioSourceAttenuation::EXPONENTIAL_DISTANCE)
        attModel = SoLoud::AudioSource::EXPONENTIAL_DISTANCE;

    soloud.set3dSourceMinMaxDistance(handle, spatial ? spatial->minDistance : audio.minDistance, spatial ? spatial->maxDistance : audio.maxDistance);
    soloud.set3dSourceAttenuation(handle, attModel, spatial ? spatial->rolloff : audio.attenuationRolloffFactor);
    soloud.set3dSourceDopplerFactor(handle, spatial ? spatial->doppler : audio.dopplerFactor);
    soloud.set3dSourceParameters(handle, position.x, position.y, position.z, velocity.x, velocity.y, velocity.z);
}

void AudioSystem::applyVoiceParameters(AudioSourceComponent& audio, unsigned int handle, bool mainVoice, bool use3D, float speedScale, bool eventLoop, SoLoud::AudioSource* sample){
    SoLoud::Soloud& soloud = getSoloud();
    const float speed = audio.speed * speedScale;
    soloud.setRelativePlaySpeed(handle, speed > 0.0f ? speed : 0.0001f);
    // a 3D voice's pan is SoLoud's (play3d, update3dAudio): a 2D pan here would centre its channel
    // volumes until the frame's 3D pass, and a block mixed in between ramps it toward the centre
    if (!use3D) {
        soloud.setPan(handle, audio.pan);
    }
    if (mainVoice) {
        soloud.setLooping(handle, audio.looping || eventLoop);
        soloud.setLoopPoint(handle, SoundPool::getLoopPoint(sample ? sample : audio.sample.get(), audio.loopingPoint));
    } else if (eventLoop) {
        soloud.setLooping(handle, true);
        soloud.setLoopPoint(handle, SoundPool::getLoopPoint(sample, 0.0));
    }
    soloud.setProtectVoice(handle, audio.protectVoice);
    soloud.setInaudibleBehavior(handle, audio.inaudibleBehaviorMustTick, audio.inaudibleBehaviorKill);
}

unsigned int AudioSystem::startVoice(AudioSourceComponent& audio, SoLoud::AudioSource& sample, float volume, bool use3D,
                                     const Vector3& position, bool mainVoice, const VoiceExtras& extras){
    if (!init()) return 0;

    const int priority = extras.priority >= 0 ? extras.priority : audio.priority;
    const std::string& groupId = extras.groupId ? *extras.groupId : audio.resolvedGroup;

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
        if (!worst || priority > worst->priority) {
            if (voiceLinesWanted()) {
                AudioVoiceLine line;
                line.kind = AudioVoiceLine::Kind::Reject;
                line.reason = "budget";
                line.scene = scene;
                line.entity = extras.entity;
                line.clip = extras.clip;
                line.event = extras.event;
                line.eventPath = extras.eventPath;
                line.owner = extras.owner;
                line.cause = extras.cause;
                line.trigger = extras.trigger;
                line.priority = priority;
                line.group = groupId;
                emitVoiceLine(line);
            }
            return 0;
        }
        stopVoiceBecause(victim, "stolen_budget", 0);
    }

    // a delay is SoLoud's delay of a voice not started yet; a clocked play has its own. On the
    // deterministic driver every start of a frame is queued before that frame's mix, at its first sample
    const bool clocked = mainVoice && audio.enableClocked && extras.delayFrames == 0 && !deterministicDriver;
    // the game time it was played at: SoLoud delays it from the first clocked start of the buffer
    unsigned int handle = AudioMixer::play(groupId, sample, use3D, position, volume, clocked, AudioMixer::getClockTime());
    if (handle == 0) return 0;

    applyVoiceParameters(audio, handle, mainVoice, use3D, extras.speedScale, extras.eventLoop, &sample);
    if (use3D) {
        apply3dParameters(audio, handle, position, Vector3(0, 0, 0), extras.spatial);
    }
    if (extras.delayFrames > 0) {
        getSoloud().setDelaySamples(handle, extras.delayFrames);
    }
    const bool freeOrOneShot = std::strcmp(extras.owner, "one_shot") == 0 || std::strcmp(extras.owner, "free") == 0;
    registerVoice(handle, scene, groupId, priority, audio.protectVoice, freeOrOneShot);

    if (voiceLinesWanted()) {
        AudioVoiceLine line;
        line.handle = handle;
        line.scene = scene;
        line.entity = extras.entity;
        line.clip = extras.clip;
        line.event = extras.event;
        line.eventPath = extras.eventPath;
        line.variation = extras.variation;
        line.layer = extras.layer;
        line.instance = extras.instance;
        line.owner = extras.owner;
        line.cause = extras.cause;
        line.trigger = extras.trigger;
        line.impactSpeed = extras.impactSpeed;
        line.gain = volume;
        line.pitch = audio.speed * extras.speedScale;
        line.pan = audio.pan;
        line.looping = (mainVoice && audio.looping) || extras.eventLoop;
        line.spatial = use3D;
        line.position = position;
        line.priority = priority;
        line.group = groupId;
        line.delayFrames = extras.delayFrames;
        line.loadDelay = extras.loadDelay;
        emitVoiceLine(line);
    }

    // started paused, except a clocked voice (SoLoud has no paused clocked play): held at once
    const bool pause = desiredPause(audio);
    getSoloud().setPause(handle, pause);
    return handle;
}

bool AudioSystem::makeSourceRoom(AudioSourceComponent& audio, uint64_t instance){
    SoLoud::Soloud& soloud = getSoloud();
    for (size_t s = 0; s < audio.oneShots.size();) {
        if (!soloud.isValidVoiceHandle(audio.oneShots[s].handle)) {
            voices().erase(audio.oneShots[s].handle);
            audio.oneShots.erase(audio.oneShots.begin() + s);
        } else {
            s++;
        }
    }
    const int cap = std::max(1, audio.maxVoices);
    const int mainVoices = (audio.handle != 0 ? 1 : 0) + (int)audio.mainLayers.size();
    while (mainVoices + (int)audio.oneShots.size() >= cap) {
        // the oldest one-shot gives its place (spec A10), never one of the play that asks
        auto oldest = audio.oneShots.end();
        for (auto it = audio.oneShots.begin(); it != audio.oneShots.end(); ++it) {
            if ((instance == 0 || it->instance != instance) && (oldest == audio.oneShots.end() || it->serial < oldest->serial)) oldest = it;
        }
        if (oldest == audio.oneShots.end()) return false;
        stopVoiceBecause(oldest->handle, "stolen_source", instance);
        audio.oneShots.erase(oldest);
    }
    return true;
}

// ---------------------------------------------------------------- Sound Events

bool AudioSystem::isDeterministic(){
    return inited && getSoloud().getBackendId() == SoLoud::Soloud::NULLDRIVER;
}

void AudioSystem::setVoiceObserver(std::function<void(const AudioVoiceLine&)> observer){
    voiceObserverSet = (bool)observer;
    voiceObserver() = std::move(observer);
}

void AudioSystem::logReject(const char* reason, Entity entity, const std::string& clip, const SoundEventAsset* event, const std::string& eventName,
                            const char* owner, const char* cause, const char* trigger, float impactSpeed, int priority, const std::string& group){
    if (!voiceLinesWanted()) return;
    AudioVoiceLine line;
    line.kind = AudioVoiceLine::Kind::Reject;
    line.reason = reason;
    line.scene = scene;
    line.entity = entity;
    line.clip = clip;
#if JASPERENGINE_SOUND_LAYER
    line.event = event ? event->key : eventName;
    line.eventPath = event ? event->path : std::string();
#else
    (void)event;
    (void)eventName;
#endif
    line.owner = owner;
    line.cause = cause;
    line.trigger = trigger;
    line.impactSpeed = impactSpeed;
    line.priority = priority;
    line.group = group;
    emitVoiceLine(line);
}

#if JASPERENGINE_SOUND_LAYER

void AudioSystem::resetSoundEventRuntime(){
    SoundEventLibrary::resetRuntime();
    soundEventWarnings().clear();
    eventInstances().clear();
    soundEventSeedChosen = false;
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    deferredCalls().clear();
    deferredPending.store(false, std::memory_order_relaxed);
}

uint64_t AudioSystem::getSoundEventSeed(){
    return chooseSoundEventSeed();
}

const SoundEventAsset* AudioSystem::resolveSourceEvent(const std::string& name, Entity entity, const char* owner, const char* cause, const char* trigger, float impactSpeed){
    const SoundEventAsset* event = SoundEventLibrary::find(name);
    if (event) return event;
    if (soundEventWarnings().insert("event:" + name).second) {
        Log::warn("Sound Event '%s' does not resolve: nothing plays", name.c_str());
    }
    logReject("missing_event", entity, "", nullptr, name, owner, cause, trigger, impactSpeed, 128, "");
    return nullptr;
}

std::string AudioSystem::eventGroup(const SoundEventAsset& event, const std::string& fallback){
    const SoundEventDef& def = *event.def;
    if (def.outputGroupId.empty() && def.outputGroupName.empty()) return fallback;
    // an AudioSource's rule: the id first, unless the name says another group that exists
    const std::string byId = def.outputGroupId.empty() ? "" : AudioMixer::findGroupId(def.outputGroupId);
    const std::string byName = def.outputGroupName.empty() ? "" : AudioMixer::findGroupId(def.outputGroupName);
    if (!byId.empty() && (byName.empty() || byName == byId)) return byId;
    if (!byName.empty()) return byName;
    if (soundEventWarnings().insert("group:" + event.key).second) {
        Log::warn("Sound Event '%s': mixer group '%s' is not in the mixer, it plays through Master", event.path.c_str(),
                  (def.outputGroupName.empty() ? def.outputGroupId : def.outputGroupName).c_str());
    }
    return "";
}

void AudioSystem::preloadEvent(AudioSourceComponent& audio){
    audio.preloadedEvent = audio.event.path;
    const std::string name = audio.event.guid.isNull() ? audio.event.path : audio.event.guid.str();
    const SoundEventAsset* event = SoundEventLibrary::find(name);
    if (!event) return;
    for (const SoundEventClip& clip : event->def->clips) {
        if (!clip.clip.path.empty()) SoundPool::loadClip(clip.clip.path, audio.loadType);
    }
}

void AudioSystem::clipVariation(AudioSourceComponent& audio, Entity entity, float& volumeScale, float& speedScale){
    const uint64_t playIndex = audio.startCount++;
    const float pitch = std::isfinite(audio.pitchRandomSemitones) ? std::clamp(audio.pitchRandomSemitones, 0.0f, 24.0f) : 0.0f;
    const float volume = std::isfinite(audio.volumeRandomDb) ? std::clamp(audio.volumeRandomDb, 0.0f, 60.0f) : 0.0f;
    if (pitch == 0.0f && volume == 0.0f) return;
    // only the ranges that are not 0 draw: pitch uniform in [-p, p], then volume in [-v, 0] dB
    SoundEventRng rng(chooseSoundEventSeed(), "entity:" + std::to_string(entity), playIndex);
    if (pitch != 0.0f) speedScale = (float)((double)speedScale * std::pow(2.0, rng.range(-(double)pitch, (double)pitch) / 12.0));
    if (volume != 0.0f) volumeScale = (float)((double)volumeScale * std::pow(10.0, rng.range(-(double)volume, 0.0) / 20.0));
}

AudioSystem::StartedPlay AudioSystem::startEvent(const SoundEventAsset& event, const EventPlay& play){
    StartedPlay started;
    if (!init() || !play.audio) return started;
    AudioSourceComponent& audio = *play.audio;
    const std::shared_ptr<const SoundEventDef> defHold = event.def;
    const SoundEventDef& def = *defHold;
    const bool oneShot = play.owner == EventPlay::Owner::OneShot;
    const char* owner = play.ownerName ? play.ownerName : (oneShot ? "one_shot" : "main");
    const int priority = def.priority != 128 ? def.priority : audio.priority;
    const std::string group = play.group ? *play.group : eventGroup(event, audio.resolvedGroup);
    chooseSoundEventSeed();
    const SoundEventClock now = soundEventClock();

    // 1. the cooldown; nothing changes
    if (!SoundEventLibrary::cooledDown(event, now)) {
        logReject("cooldown", play.entity, "", &event, "", owner, play.cause, play.trigger, play.impactSpeed, priority, group);
        return started;
    }

    // 2. the limit: one instance stops before the new voices start, or the play is rejected
    const uint64_t instance = nextInstance++;
    if (def.maxInstances > 0) {
        bool full = false;
        const EventInstance* candidate = limitCandidate(def, event.key, priority, full);
        if (full && !candidate) {
            logReject("instance_limit", play.entity, "", &event, "", owner, play.cause, play.trigger, play.impactSpeed, priority, group);
            return started;
        }
        if (candidate) {
            const std::vector<unsigned int> stolen = candidate->handles;
            eventInstances().erase(candidate->id);
            for (unsigned int handle : stolen) stopVoiceBecause(handle, "stolen_instance", instance);
        }
    }

    // 3. the pick, with the play's generator: consumed whatever happens to its voices
    const SoundEventPick pick = SoundEventLibrary::pick(event, now);

    // 4. a voice per clip of the pick, each through the source's cap and the global budget
    const unsigned int rate = getSoloud().getBackendSamplerate();
    const SoundEventSpatial* spatial = def.spatial ? &*def.spatial : nullptr;
    for (size_t layer = 0; layer < pick.clips.size(); layer++) {
        const int index = pick.clips[layer];
        const SoundEventClip& clip = def.clips[index];
        // 5. a clip that does not load is skipped; the other picks do not change
        const SoundLoadResult loaded = SoundPool::loadClipNow(clip.clip.path, audio.loadType);
        if (loaded.state != ResourceLoadState::Finished || !loaded.data) {
            if (soundEventWarnings().insert("clip:" + event.key + ":" + clip.clip.path).second) {
                Log::warn("Sound Event '%s': clip '%s' does not load", event.path.c_str(), clip.clip.path.c_str());
            }
            logReject("missing_clip", play.entity, clip.clip.path, &event, "", owner, play.cause, play.trigger, play.impactSpeed, priority, group);
            continue;
        }
        const bool firstMain = !oneShot && started.handles.empty();
        if (!firstMain && !makeSourceRoom(audio, instance)) {
            logReject("max_voices", play.entity, clip.clip.path, &event, "", owner, play.cause, play.trigger, play.impactSpeed, priority, group);
            continue;
        }

        const float volumeScale = (float)(std::pow(10.0, (pick.volumeDb + (double)clip.volumeDb) / 20.0) * (double)play.impactGain * (double)play.volumeScale);
        const float speedScale = (float)std::pow(2.0, (pick.pitchSemitones + (double)clip.pitchSemitones) / 12.0);
        const double delayMs = std::max(pick.delayMs + (double)clip.delayMs, 0.0);

        VoiceExtras extras;
        extras.speedScale = speedScale;
        extras.priority = priority;
        extras.groupId = &group;
        extras.eventLoop = def.loop;
        extras.delayFrames = (uint32_t)std::llround(delayMs * (double)rate / 1000.0);
        extras.spatial = spatial;
        extras.owner = owner;
        extras.cause = play.cause;
        extras.trigger = play.trigger;
        extras.impactSpeed = play.impactSpeed;
        extras.entity = play.entity;
        extras.clip = clip.clip.path.c_str();
        extras.event = event.key.c_str();
        extras.eventPath = event.path.c_str();
        extras.variation = index;
        extras.layer = (int)layer;
        extras.instance = instance;
        const unsigned int handle = startVoice(audio, *loaded.data, (float)audio.volume * volumeScale, play.use3D, play.position, !oneShot, extras);
        if (handle == 0) continue;

        started.handles.push_back(handle);
        if (!oneShot && firstMain) {
            audio.handle = handle;
            audio.mainVolumeScale = volumeScale;
            audio.mainSpeedScale = speedScale;
            audio.mainInstance = instance;
            audio.mainEventSample = loaded.data;
            audio.mainEvent = defHold;
            audio.length = SoundPool::getLength(loaded.data.get());
        } else {
            AudioOneShot voice;
            voice.handle = handle;
            voice.sample = loaded.data;
            voice.volumeScale = volumeScale;
            voice.speedScale = speedScale;
            voice.serial = nextSerial++;
            voice.instance = instance;
            voice.event = defHold;
            (oneShot ? audio.oneShots : audio.mainLayers).push_back(std::move(voice));
        }
    }

    // an instance whose every voice was rejected is not live; its pick stays consumed
    if (!started.handles.empty()) {
        started.instance = instance;
        if (def.maxInstances > 0) {
            EventInstance& live = eventInstances()[instance];
            live.id = instance;
            live.event = event.key;
            live.handles = started.handles;
            live.priority = priority;
            live.isProtected = audio.protectVoice;
            live.serial = nextSerial++;
        }
    }
    return started;
}

bool AudioSystem::startMainEvent(AudioSourceComponent& audio, Entity entity, bool use3D, const Vector3& position){
    const std::string name = audio.event.guid.isNull() ? audio.event.path : audio.event.guid.str();
    const SoundEventAsset* event = resolveSourceEvent(name, entity, "main", "event", "", -1.0f);
    if (!event) return false;
    EventPlay play;
    play.audio = &audio;
    play.entity = entity;
    play.use3D = use3D;
    play.position = position;
    const StartedPlay started = startEvent(*event, play);
    if (started.handles.empty()) return false;
    audio.mainEventName = audio.event.path;
    return true;
}

#else // the module is out: nothing plays an event, every clip plays as it is

void AudioSystem::resetSoundEventRuntime(){
}

void AudioSystem::preloadEvent(AudioSourceComponent&){
}

void AudioSystem::clipVariation(AudioSourceComponent&, Entity, float&, float&){
}

bool AudioSystem::startMainEvent(AudioSourceComponent&, Entity, bool, const Vector3&){
    return false;
}

AudioSystem::StartedPlay AudioSystem::startEvent(const SoundEventAsset&, const EventPlay&){
    return StartedPlay();
}

const SoundEventAsset* AudioSystem::resolveSourceEvent(const std::string&, Entity, const char*, const char*, const char*, float){
    return nullptr;
}

#endif // JASPERENGINE_SOUND_LAYER

bool AudioSystem::playOneShot(AudioSourceComponent& audio, const std::string& filename, float volumeScale){
    if (filename.empty()) return false;
    AudioPendingOneShot pending;
    pending.filename = filename;
    pending.volumeScale = std::max(0.0f, volumeScale);
#if JASPERENGINE_SOUND_LAYER
    pending.isEvent = SoundEventLibrary::isEventName(filename);
#endif
    audio.pendingOneShots.push_back(pending);
    return true;
}

void AudioSystem::startPendingOneShots(AudioSourceComponent& audio, Entity entity, bool use3D, const Vector3& position, double unscaledDt){
    for (size_t i = 0; i < audio.pendingOneShots.size();) {
        AudioPendingOneShot& pending = audio.pendingOneShots[i];
        if (pending.isEvent) {
            const AudioPendingOneShot shot = pending;
            audio.pendingOneShots.erase(audio.pendingOneShots.begin() + i);
            const char* cause = shot.trigger[0] ? "trigger" : "event";
            const SoundEventAsset* event = resolveSourceEvent(shot.filename, entity, "one_shot", cause, shot.trigger, shot.impactSpeed);
            if (!event) continue;
            EventPlay play;
            play.owner = EventPlay::Owner::OneShot;
            play.audio = &audio;
            play.entity = entity;
            play.use3D = use3D;
            play.position = position;
            play.volumeScale = shot.volumeScale;
            play.impactGain = shot.impactGain;
            play.impactSpeed = shot.impactSpeed;
            play.cause = cause;
            play.trigger = shot.trigger;
            startEvent(*event, play);
            continue;
        }
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
        const char* cause = shot.trigger[0] ? "trigger" : "play";
        if (!makeSourceRoom(audio, 0)) {
            logReject("max_voices", entity, shot.filename, nullptr, "", "one_shot", cause, shot.trigger, shot.impactSpeed, audio.priority, audio.resolvedGroup);
            continue;
        }

        // every start of a source in clip mode varies (R11)
        float volumeScale = shot.volumeScale * shot.impactGain;
        float speedScale = 1.0f;
        if (audio.event.empty()) clipVariation(audio, entity, volumeScale, speedScale);

        // the pool entry stays while a voice of it plays: the sample is held by the source's one-shots
        const float volume = (float)audio.volume * volumeScale;
        VoiceExtras extras;
        extras.speedScale = speedScale;
        extras.owner = "one_shot";
        extras.cause = cause;
        extras.trigger = shot.trigger;
        extras.impactSpeed = shot.impactSpeed;
        extras.loadDelay = (uint32_t)std::llround(shot.waited * deterministicRate);
        extras.entity = entity;
        extras.clip = shot.filename.c_str();
        unsigned int handle = startVoice(audio, *result.data, volume, use3D, position, false, extras);
        if (handle == 0) continue;
        AudioOneShot oneShot;
        oneShot.handle = handle;
        oneShot.sample = result.data;
        oneShot.volumeScale = volumeScale;
        oneShot.speedScale = speedScale;
        oneShot.serial = nextSerial++;
        audio.oneShots.push_back(oneShot);
    }
}

int AudioSystem::getPlayingVoiceCount(const AudioSourceComponent& audio) const{
    if (!inited) return 0;
    int count = 0;
    if (audio.handle != 0 && getSoloud().isValidVoiceHandle(audio.handle)) count++;
    for (const AudioOneShot& layer : audio.mainLayers) {
        if (getSoloud().isValidVoiceHandle(layer.handle)) count++;
    }
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
                    pauseVoiceLine(pointVoices[i].handle, pause);
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
        VoiceExtras extras;
        extras.owner = "point";
        extras.loadDelay = (uint32_t)std::llround(shot.waited * deterministicRate);
        extras.clip = shot.filename.c_str();
        const unsigned int handle = startVoice(defaults, *result.data, shot.volume, hasListener, position, false, extras);
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
        const StopReason faded("fade_out");
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
#if JASPERENGINE_SOUND_LAYER
    if (deferredPending.load(std::memory_order_relaxed)) {
        std::vector<std::function<void()>> calls;
        {
            std::lock_guard<std::recursive_mutex> lock(freeMutex());
            calls.swap(deferredCalls());
            deferredPending.store(false, std::memory_order_relaxed);
        }
        for (const std::function<void()>& call : calls) call();
    }
#endif

    preloadSoundAssets();

    if (paused) {
        // what moves while it holds is not a speed
        motionFresh = false;
        listenerTracked = false;
        // a trigger while the scene's audio is paused is dropped
        triggerQueue.clear();
        contactQueue3D.clear();
        return;
    }

    const double unscaledDt = AudioMixer::getFrameUnscaledDelta();

    auto audios = scene->getComponentArray<AudioSourceComponent>();

    // open the output early, a device started by the first play cuts that sound
    if (!outputRequested && (audios->size() > 0 || !pendingPointVoices.empty())) {
        outputRequested = true;
        init();
    }

    // the sources' last one went: nothing needs the physics signals
    if (physicsNeeded != 0 && audios->size() == 0) syncPhysicsTriggers(0);
    if (!triggerQueue.empty() || !contactQueue3D.empty()) drainTriggers();

    bool anyFree = false;
#if JASPERENGINE_SOUND_LAYER
    {
        std::lock_guard<std::recursive_mutex> lock(freeMutex());
        for (const FreePlay& play : freePlays()) anyFree = anyFree || (play.used && play.owner == this);
    }
#endif

    // nothing to place: the next frame's speeds start afresh
    if (audios->size() == 0 && pendingPointVoices.empty() && pointVoices.empty() && !anyFree) {
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
    uint32_t neededPlayOn = 0;

    for (int i = 0; i < audios->size(); i++){
		AudioSourceComponent& audio = audios->getComponentFromIndex(i);

        Entity entity = audios->getEntity(i);

        // only what a source needs is subscribed; a scene without flags pays this one test
        if ((audio.playOn | audio.boundPlayOn) != 0) {
            const uint32_t desired = audio.enabled && scene->isEntityActiveInHierarchy(entity) ? (audio.playOn & AudioPlayOn_All) : 0u;
            neededPlayOn |= desired;
            syncUiTriggers(audio, entity, desired);
        }
        Signature signature = scene->getSignature(entity);
        bool use3DAudio = hasListener && signature.test(scene->getComponentId<Transform>());

        // where the listener hears it (its world position, or mapped from a layer's camera)
        Vector3 worldPosition = Vector3(0, 0, 0);
        if (use3DAudio){
            Transform& transform = scene->getComponent<Transform>(entity);

            worldPosition = heard(transform.worldPosition);
        }

        // set, the event is what the source plays: its clip is ignored, with no fallback
        const bool eventMode = JASPERENGINE_SOUND_LAYER && !audio.event.empty();
        // main voices of something else (the event was set, cleared or changed): they stop, and a
        // source left Playing starts over with what it has now
        if (audio.handle != 0 && (audio.mainEvent != nullptr) != eventMode) {
            const StopReason changed("clip_changed");
            stopMainVoices(audio);
        } else if (audio.handle != 0 && eventMode && audio.mainEventName != audio.event.path) {
            const StopReason changed("clip_changed");
            stopMainVoices(audio);
        }

        // no clip: nothing of its own plays (its one-shots still do, Unity's PlayOneShot needs no clip)
        const bool hasClip = !audio.filename.empty() && !eventMode;
        if (!hasClip && (audio.loaded || audio.sample || (audio.handle != 0 && !eventMode))) {
            const StopReason changed("clip_changed");
            if (!eventMode) stopMainVoices(audio);
            releaseSample(audio);
        }
        if (!hasClip && !eventMode) {
            if (audio.state != AudioSourceState::Stopped) audio.state = AudioSourceState::Stopped;
            audio.startTrigger = false;
            audio.pauseTrigger = false;
        }

        // Unity: disabling an Audio Source or its GameObject stops it and a play request
        // is ignored. A source left Playing starts over on reactivation (play on awake).
        if (!audio.enabled || !scene->isEntityActiveInHierarchy(entity)) {
            if (audio.handle != 0 || !audio.oneShots.empty() || !audio.pendingOneShots.empty()) {
                const StopReason disabled("disabled");
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

        // a voice that ended by itself (or was stolen): Stopped, onStop (spec A15); a layered
        // event's when none of its voices is left (R10)
        if (inited && !audio.mainLayers.empty()) pruneMainVoices(audio);
        if (inited && audio.handle != 0 && !getSoloud().isValidVoiceHandle(audio.handle)) {
            voices().erase(audio.handle);
            audio.handle = 0;
            audio.playingTime = 0;
            audio.mainVolumeScale = 1.0f;
            audio.mainSpeedScale = 1.0f;
            audio.mainEvent.reset();
            audio.mainEventSample.reset();
            audio.mainEventName.clear();
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
            stopMainVoices(audio);
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

        if (eventMode || (hasClip && audio.loaded)){
            if (pendingStart){
                const bool resume = audio.state == AudioSourceState::Paused && audio.handle != 0 && audio.startTrigger;
                const bool playTriggered = audio.startTrigger;
                audio.startTrigger = false;

                // the file was hot reloaded: this play takes the new sample (one still
                // playing finishes on the old, which its other sources hold)
                // (or its import settings changed: the sample is another entry of the pool)
                if (!resume && !eventMode && audio.sampleGeneration != SoundPool::generation(audio.filename)) {
                    const SoundLoadResult fresh = SoundPool::loadClip(audio.filename, audio.loadType);
                    if (fresh.data) {
                        const std::string previousKey = audio.sampleKey;
                        audio.sample = fresh.data;
                        audio.sampleKey = fresh.id;
                        audio.sampleGeneration = SoundPool::generation(audio.filename);
                        audio.length = SoundPool::getLength(audio.sample.get());
                        if (previousKey != fresh.id) SoundPool::remove(previousKey);
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
                    const char* cause = playTriggered ? "play" : audio.startCount > 0 ? "resume_restart" : "play_on_awake";
                    if (audio.handle != 0) {
                        const StopReason replaced("replaced");
                        stopMainVoices(audio);
                    }
                    audio.state = AudioSourceState::Playing;
                    audio.voicePaused = desiredPause(audio);
                    if (eventMode) {
                        startMainEvent(audio, entity, use3DAudio, worldPosition);
                    } else {
                        audio.mainVolumeScale = 1.0f;
                        audio.mainSpeedScale = 1.0f;
                        clipVariation(audio, entity, audio.mainVolumeScale, audio.mainSpeedScale);
                        VoiceExtras extras;
                        extras.speedScale = audio.mainSpeedScale;
                        extras.cause = cause;
                        extras.entity = entity;
                        extras.clip = audio.filename.c_str();
                        audio.handle = startVoice(audio, *audio.sample, (float)audio.volume * audio.mainVolumeScale, use3DAudio, worldPosition, true, extras);
                    }
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
        startPendingOneShots(audio, entity, use3DAudio, worldPosition, unscaledDt);

        // the group changed (the mixer was edited, outputGroup set): the voices move to its bus
        if (inited && groupBefore != audio.resolvedGroup) {
            if (SoLoud::Bus* bus = AudioMixer::getBus(audio.resolvedGroup)) {
                // (an event with its own output group stays on it)
                auto follows = [](const std::shared_ptr<const SoundEventDef>& def) {
                    return !def || (def->outputGroupId.empty() && def->outputGroupName.empty());
                };
                std::vector<unsigned int> handles;
                if (audio.handle && follows(audio.mainEvent)) handles.push_back(audio.handle);
                for (const AudioOneShot& layer : audio.mainLayers) {
                    if (follows(layer.event)) handles.push_back(layer.handle);
                }
                for (const AudioOneShot& shot : audio.oneShots) {
                    if (follows(shot.event)) handles.push_back(shot.handle);
                }
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
            if (audio.handle != 0) write(audio.handle, (float)audio.volume * audio.mainVolumeScale);
            for (const AudioOneShot& layer : audio.mainLayers) write(layer.handle, (float)audio.volume * layer.volumeScale);
            for (const AudioOneShot& shot : audio.oneShots) write(shot.handle, (float)audio.volume * shot.volumeScale);
            audio.lastVolumeApplied = audio.volume;
        }

        if (audio.needUpdate) {
            if (audio.handle != 0) applyVoiceParameters(audio, audio.handle, true, use3DAudio, audio.mainSpeedScale, audio.mainEvent && audio.mainEvent->loop, audio.mainEventSample.get());
            for (const AudioOneShot& layer : audio.mainLayers) applyVoiceParameters(audio, layer.handle, true, use3DAudio, layer.speedScale, layer.event && layer.event->loop, layer.sample.get());
            for (const AudioOneShot& shot : audio.oneShots) applyVoiceParameters(audio, shot.handle, false, use3DAudio, shot.speedScale, shot.event && shot.event->loop, shot.sample.get());
        }

        // 3D voices follow the source (its one-shots too, Unity)
        // and when it stops, a velocity of none (its pitch comes back)
        if (use3DAudio && (audio.needUpdate || moved || velocity != audio.lastVelocity)) {
            auto eventSpatial = [](const std::shared_ptr<const SoundEventDef>& def) -> const SoundEventSpatial* {
                return def && def->spatial ? &*def->spatial : nullptr;
            };
            if (audio.handle != 0) apply3dParameters(audio, audio.handle, worldPosition, velocity, eventSpatial(audio.mainEvent));
            for (const AudioOneShot& layer : audio.mainLayers) apply3dParameters(audio, layer.handle, worldPosition, velocity, eventSpatial(layer.event));
            for (const AudioOneShot& shot : audio.oneShots) apply3dParameters(audio, shot.handle, worldPosition, velocity, eventSpatial(shot.event));
            audio.lastVelocity = velocity;
        }
        if (use3DAudio) {
            any3D = true;
        }

        audio.needUpdate = false;
    }

    if (neededPlayOn != physicsNeeded) syncPhysicsTriggers(neededPlayOn);

    updatePointVoices(unscaledDt, hasListener, heard);
    for (const PointVoice& voice : pointVoices) any3D = any3D || voice.is3D;
    if (anyFree) updateFreeVoices(dt, unscaledDt, hasListener, heard, doppler && motionFresh, any3D);

    // the next frame's velocities measure from this one's positions
    motionFresh = true;

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
		if (audio.boundPlayOn != 0) syncUiTriggers(audio, entity, 0);
	}
}

#if JASPERENGINE_SOUND_LAYER

// ---------------------------------------------------------------- triggers (R12)

namespace {

    constexpr uint32_t BOUND_HIT_2D = 1, BOUND_BEGIN_2D = 2, BOUND_SENSOR_2D = 4, BOUND_CONTACT_3D = 8;
    constexpr const char* PHYSICS_TRIGGER_TAG = "jasperengine:audio:physics;";

    std::string uiTriggerTag(Entity entity) {
        return "jasperengine:audio:e" + std::to_string(entity) + ";";
    }

}

void AudioSystem::queueTrigger(uint32_t flag, Entity entity, Entity other, float speed){
    if (paused || !schedulerEnabled) return;
    TriggerEvent event;
    event.flag = flag;
    event.entity = entity;
    event.other = other;
    event.speed = speed;
    triggerQueue.push_back(event);
}

void AudioSystem::syncUiTriggers(AudioSourceComponent& audio, Entity entity, uint32_t desired){
    const std::string tag = uiTriggerTag(entity);
    const std::shared_ptr<TriggerSink> sink = triggerSink;
    if (UIComponent* ui = scene->findComponent<UIComponent>(entity)) {
        // added only where the signal lacks the tag: the user's subscribers keep their order, and a
        // rebuilt component (empty signals) gets it again
        if (desired & AudioPlayOn_UIClick) {
            if (!ui->onClick.has(tag)) {
                ui->onClick.add(tag, std::function<void(float, float)>([sink, entity](float, float) {
                    if (sink->system) sink->system->queueTrigger(AudioPlayOn_UIClick, entity, NULL_ENTITY, -1.0f);
                }));
            }
        } else if (audio.boundPlayOn & AudioPlayOn_UIClick) {
            ui->onClick.remove(tag);
        }
        if (desired & AudioPlayOn_UIHover) {
            if (!ui->onPointerEnter.has(tag)) {
                ui->onPointerEnter.add(tag, std::function<void(float, float)>([sink, entity](float, float) {
                    if (sink->system) sink->system->queueTrigger(AudioPlayOn_UIHover, entity, NULL_ENTITY, -1.0f);
                }));
            }
        } else if (audio.boundPlayOn & AudioPlayOn_UIHover) {
            ui->onPointerEnter.remove(tag);
        }
    }
    if (ButtonComponent* button = scene->findComponent<ButtonComponent>(entity)) {
        if (desired & AudioPlayOn_UIPress) {
            if (!button->onPress.has(tag)) {
                button->onPress.add(tag, std::function<void()>([sink, entity]() {
                    if (sink->system) sink->system->queueTrigger(AudioPlayOn_UIPress, entity, NULL_ENTITY, -1.0f);
                }));
            }
        } else if (audio.boundPlayOn & AudioPlayOn_UIPress) {
            button->onPress.remove(tag);
        }
    }
    audio.boundPlayOn = desired;
}

void AudioSystem::syncPhysicsTriggers(uint32_t needed){
    uint32_t wanted = 0;
#ifdef JASPERENGINE_PHYSICS_2D
    if (needed & AudioPlayOn_Collision2D) wanted |= BOUND_HIT_2D | BOUND_BEGIN_2D;
    if (needed & AudioPlayOn_Sensor2D) wanted |= BOUND_SENSOR_2D;
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    if (needed & (AudioPlayOn_Collision3D | AudioPlayOn_Sensor3D)) wanted |= BOUND_CONTACT_3D;
#endif
    physicsNeeded = needed;
    if (wanted == physicsBound) return;
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    const uint32_t adding = wanted & ~physicsBound;
    const uint32_t removing = physicsBound & ~wanted;
    const std::shared_ptr<TriggerSink> sink = triggerSink;
    Scene* owner = scene;
    (void)adding;
    (void)removing;
    (void)physics;
    (void)sink;
    (void)owner;
#ifdef JASPERENGINE_PHYSICS_2D
    if (adding & BOUND_HIT_2D) {
        physics->hitContact2D.add(PHYSICS_TRIGGER_TAG, std::function<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>(
            [sink, owner](Rigidbody2D bodyA, unsigned long, Rigidbody2D bodyB, unsigned long, Vector2, Vector2, float approachSpeed) {
                if (!sink->system) return;
                // Box2D's speed is in metres a second: in scene units, as the 3D one
                const float scale = owner->getSystem<PhysicsSystem>()->getPointsToMeterScale2D();
                sink->system->queueTrigger(AudioPlayOn_Collision2D, bodyA.getEntity(), bodyB.getEntity(), approachSpeed * scale);
            }));
    }
    if (adding & BOUND_BEGIN_2D) {
        physics->beginContact2D.add(PHYSICS_TRIGGER_TAG, std::function<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>(
            [sink, owner](Rigidbody2D bodyA, unsigned long shapeA, Rigidbody2D bodyB, unsigned long shapeB) {
                if (!sink->system) return;
                // one signal per pair: a pair with hit events plays on its hit event only
                auto hitEvents = [owner](Entity entity, unsigned long shape) {
                    const Rigidbody2DComponent* body = owner->findComponent<Rigidbody2DComponent>(entity);
                    return body && shape < body->colliders.size() && body->colliders[shape].enableHitEvents;
                };
                if (hitEvents(bodyA.getEntity(), shapeA) || hitEvents(bodyB.getEntity(), shapeB)) return;
                sink->system->queueTrigger(AudioPlayOn_Collision2D, bodyA.getEntity(), bodyB.getEntity(), -1.0f);
            }));
    }
    if (adding & BOUND_SENSOR_2D) {
        physics->beginSensorContact2D.add(PHYSICS_TRIGGER_TAG, std::function<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>(
            [sink](Rigidbody2D sensor, unsigned long, Rigidbody2D visitor, unsigned long) {
                if (sink->system) sink->system->queueTrigger(AudioPlayOn_Sensor2D, sensor.getEntity(), visitor.getEntity(), -1.0f);
            }));
    }
    if (removing & BOUND_HIT_2D) physics->hitContact2D.remove(PHYSICS_TRIGGER_TAG);
    if (removing & BOUND_BEGIN_2D) physics->beginContact2D.remove(PHYSICS_TRIGGER_TAG);
    if (removing & BOUND_SENSOR_2D) physics->beginSensorContact2D.remove(PHYSICS_TRIGGER_TAG);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    if (adding & BOUND_CONTACT_3D) {
        physics->onContactAdded3D.add(PHYSICS_TRIGGER_TAG, std::function<void(Rigidbody, Rigidbody, Contact3D)>(
            [sink](Rigidbody body1, Rigidbody body2, Contact3D contact) {
                if (!sink->system || sink->system->paused || !sink->system->schedulerEnabled) return;
                const bool sensor = contact.isSensor();
                sink->system->contactQueue3D.push_back({sensor ? (uint32_t)AudioPlayOn_Sensor3D : (uint32_t)AudioPlayOn_Collision3D,
                                                        body1.getEntity(), body2.getEntity(), sensor ? -1.0f : contact.getApproachSpeed()});
            }));
    }
    if (removing & BOUND_CONTACT_3D) physics->onContactAdded3D.remove(PHYSICS_TRIGGER_TAG);
#endif
    physicsBound = wanted;
}

void AudioSystem::removeTriggerSubscriptions(){
    triggerQueue.clear();
    contactQueue3D.clear();
    auto audios = scene->getComponentArray<AudioSourceComponent>();
    for (int i = 0; i < audios->size(); i++) {
        AudioSourceComponent& audio = audios->getComponentFromIndex(i);
        if (audio.boundPlayOn != 0) syncUiTriggers(audio, audios->getEntity(i), 0);
    }
    if (physicsNeeded != 0) syncPhysicsTriggers(0);
}

void AudioSystem::drainTriggers(){
    std::vector<TriggerEvent> events;
    events.swap(triggerQueue);
    std::vector<TriggerEvent> contacts;
    contacts.swap(contactQueue3D);

    // the game's pause drops them (the scene's own dropped them as they came)
    if (AudioMixer::isMasterPaused() || AudioMixer::isPlatformPaused()) return;

    // one per body pair and flag per frame, with the largest speed
    for (TriggerEvent& event : contacts) {
        if (event.other < event.entity) std::swap(event.entity, event.other);
    }
    std::stable_sort(contacts.begin(), contacts.end(), [](const TriggerEvent& a, const TriggerEvent& b) {
        if (a.entity != b.entity) return a.entity < b.entity;
        if (a.other != b.other) return a.other < b.other;
        return a.flag < b.flag;
    });
    std::vector<TriggerEvent> reduced;
    for (const TriggerEvent& event : contacts) {
        if (!reduced.empty() && reduced.back().entity == event.entity && reduced.back().other == event.other && reduced.back().flag == event.flag) {
            reduced.back().speed = std::max(reduced.back().speed, event.speed);
        } else {
            reduced.push_back(event);
        }
    }

    // both bodies of a pair play when both have the flag
    for (const std::vector<TriggerEvent>* list : {&events, &reduced}) {
        for (const TriggerEvent& event : *list) {
            playTrigger(event, event.entity);
            if (event.other != NULL_ENTITY && event.other != event.entity) playTrigger(event, event.other);
        }
    }
}

void AudioSystem::playTrigger(const TriggerEvent& event, Entity entity){
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return;
    AudioSourceComponent* audio = scene->findComponent<AudioSourceComponent>(entity);
    if (!audio || !(audio->playOn & event.flag) || !audio->enabled || !scene->isEntityActiveInHierarchy(entity)) return;
    if (listenerPaused && !audio->ignoreListenerPause) return;

    const bool eventMode = !audio->event.empty();
    const std::string name = eventMode ? (audio->event.guid.isNull() ? audio->event.path : audio->event.guid.str()) : audio->filename.path;
    if (name.empty()) return;

    AudioPendingOneShot pending;
    pending.filename = name;
    pending.isEvent = eventMode;
    pending.trigger = audioPlayOnName(event.flag);
    // a contact's speed: the gate, then a gain proportional to it
    if (event.speed >= 0.0f) {
        pending.impactSpeed = event.speed;
        if (event.speed < audio->minImpactSpeed) {
            const SoundEventAsset* asset = eventMode ? SoundEventLibrary::find(name) : nullptr;
            logReject("impact_gate", entity, eventMode ? "" : name, asset, eventMode ? name : "", "one_shot", "trigger", pending.trigger,
                      event.speed, audio->priority, audio->resolvedGroup);
            return;
        }
        if (audio->fullVolumeImpactSpeed > 0.0f) pending.impactGain = std::min(1.0f, event.speed / audio->fullVolumeImpactSpeed);
    }
    audio->pendingOneShots.push_back(pending);
}

// ---------------------------------------------------------------- free voices (R13)

AudioSystem::PhysicsCallbackScope::PhysicsCallbackScope(){
    physicsCallbackDepth++;
}

AudioSystem::PhysicsCallbackScope::~PhysicsCallbackScope(){
    physicsCallbackDepth--;
}

bool AudioSystem::isDeferringCalls(){
    return physicsCallbackDepth > 0 || !jobs::isMainThread();
}

SoundHandle AudioSystem::playFree(const std::string& eventOrClip, const PlayOptions& options, const char* cause){
    if (!std::isfinite(options.volume) || !std::isfinite(options.pitch) || !std::isfinite(options.delay) ||
        (options.position && (!std::isfinite(options.position->x) || !std::isfinite(options.position->y) || !std::isfinite(options.position->z)))) {
        Log::warn("Audio.play('%s'): a value is not a number; nothing plays", eventOrClip.c_str());
        return SoundHandle();
    }
    Scene* owner = options.scene ? options.scene : (options.targetScene ? options.targetScene : Engine::getMainScene());
    if (!owner) {
        Log::warn("Audio.play('%s'): no scene to play in", eventOrClip.c_str());
        return SoundHandle();
    }
    AudioSystem* system = owner->getSystem<AudioSystem>().get();

    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    std::vector<FreePlay>& plays = freePlays();
    uint32_t slot = (uint32_t)plays.size();
    for (uint32_t i = 0; i < plays.size(); i++) {
        if (!plays[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == plays.size()) plays.emplace_back();
    FreePlay& play = plays[slot];
    play.used = true;
    play.owner = system;
    play.waiting = true;
    play.name = eventOrClip;
    play.cause = cause;
    play.delayLeft = std::max(0.0, (double)options.delay);
    play.delayUnscaled = options.unscaled;
    play.group = options.group;
    play.volume = std::max(0.0f, options.volume);
    play.pitch = options.pitch > 0.0f ? options.pitch : 0.0001f;
    play.targetScene = options.targetScene;
    play.target = options.target;
    play.wants3D = options.target != NULL_ENTITY || options.position.has_value();
    if (options.position) play.position = *options.position;
    const SoundHandle handle(slot + 1, play.generation);
    const uint32_t generation = play.generation;

    if (isDeferringCalls() || play.delayLeft > 0.0) {
        // a delay is counted by the owner's updates; a deferred call by the frame's first
        if (play.delayLeft <= 0.0) {
            deferCall([slot, generation, system]() {
                if (FreePlay* waiting = freePlayOf(slot + 1, generation)) {
                    if (waiting->owner == system) system->startFree(slot);
                }
            });
        }
        return handle;
    }
    return system->startFree(slot) ? handle : SoundHandle();
}

bool AudioSystem::startFree(uint32_t slot){
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay& play = freePlays()[slot];
    play.waiting = false;
    if (!init()) {
        releaseFreePlay(play);
        return false;
    }

    // the voices' values, as a one-shot of a source that is nowhere else
    AudioSourceComponent defaults;
    defaults.volume = play.volume;
    defaults.speed = play.pitch;
    defaults.maxVoices = 64;
    // Unity's PlayClipAtPoint: a 3D source with a logarithmic rolloff
    defaults.attenuationModel = AudioSourceAttenuation::INVERSE_DISTANCE;
    std::string groupId;
    if (!play.group.empty()) {
        groupId = AudioMixer::findGroupId(play.group);
        if (groupId.empty() && soundEventWarnings().insert("group:" + play.group).second) {
            Log::warn("Audio.play('%s'): mixer group '%s' is not in the mixer, it plays through Master", play.name.c_str(), play.group.c_str());
        }
    }
    defaults.resolvedGroup = groupId;

    // where: following its target, at its position, or 2D
    Vector3 position = play.position;
    Transform* transform = nullptr;
    if (play.target != NULL_ENTITY) {
        transform = play.targetScene && play.targetScene->isEntityCreated(play.target) ? play.targetScene->findComponent<Transform>(play.target) : nullptr;
        if (!transform) {
            Log::warn("Audio.play('%s'): its target is gone; nothing plays", play.name.c_str());
            releaseFreePlay(play);
            return false;
        }
        position = transform->worldPosition;
        play.position = position;
    }
    play.is3D = play.wants3D && cameraPose(*scene).valid;

    if (SoundEventLibrary::isEventName(play.name)) {
        const SoundEventAsset* event = SoundEventLibrary::find(play.name);
        if (!event) {
            if (soundEventWarnings().insert("event:" + play.name).second) {
                Log::warn("Audio.play: Sound Event '%s' does not resolve: nothing plays", play.name.c_str());
            }
            logReject("missing_event", NULL_ENTITY, "", nullptr, play.name, "free", play.cause, "", -1.0f, 128, groupId);
            releaseFreePlay(play);
            return false;
        }
        EventPlay request;
        request.owner = EventPlay::Owner::OneShot;
        request.audio = &defaults;
        request.use3D = play.is3D;
        request.position = position;
        request.cause = play.cause;
        request.ownerName = "free";
        if (!play.group.empty()) request.group = &groupId;
        startEvent(*event, request);
        for (const AudioOneShot& voice : defaults.oneShots) {
            FreeVoice free;
            free.handle = voice.handle;
            free.sample = voice.sample;
            free.volumeScale = voice.volumeScale;
            free.speedScale = voice.speedScale;
            free.event = voice.event;
            play.voices.push_back(std::move(free));
        }
    } else {
        std::string clipPath = play.name;
        if (SoundEventLibrary::isGuid(clipPath)) AssetMap::findPath(Guid::parse(play.name), clipPath);
        const SoundLoadResult loaded = SoundPool::loadClipNow(clipPath, AudioLoadType::Auto);
        if (loaded.state != ResourceLoadState::Finished || !loaded.data) {
            if (soundEventWarnings().insert("clip:" + play.name).second) {
                Log::warn("Audio.play: clip '%s' does not load: nothing plays", play.name.c_str());
            }
            logReject("missing_clip", NULL_ENTITY, play.name, nullptr, "", "free", play.cause, "", -1.0f, 128, groupId);
            releaseFreePlay(play);
            return false;
        }
        VoiceExtras extras;
        extras.owner = "free";
        extras.cause = play.cause;
        extras.clip = play.name.c_str();
        const unsigned int handle = startVoice(defaults, *loaded.data, play.volume, play.is3D, position, false, extras);
        if (handle != 0) {
            FreeVoice free;
            free.handle = handle;
            free.sample = loaded.data;
            play.voices.push_back(std::move(free));
        }
    }

    if (play.voices.empty()) {
        // nothing started (a cooldown, a limit, the budget): an invalid handle
        releaseFreePlay(play);
        return false;
    }
    play.appliedPause = desiredPause(defaults);
    return true;
}

void AudioSystem::updateFreeVoices(double scaledDt, double unscaledDt, bool hasListener, const std::function<Vector3(const Vector3&)>& heard,
                                   bool doppler, bool& any3D){
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    std::vector<FreePlay>& plays = freePlays();
    const bool held = paused || !schedulerEnabled || listenerPaused;
    for (uint32_t slot = 0; slot < plays.size(); slot++) {
        if (!plays[slot].used || plays[slot].owner != this) continue;

        if (plays[slot].waiting) {
            FreePlay& waiting = plays[slot];
            if (waiting.delayLeft <= 0.0) continue;
            waiting.delayLeft -= waiting.delayUnscaled ? unscaledDt : scaledDt;
            if (waiting.delayLeft > 0.0) continue;
            if (!startFree(slot)) continue;
        }

        FreePlay& play = plays[slot];
        if (!freePlayLive(play)) continue;
        SoLoud::Soloud& soloud = getSoloud();

        // its target gone: it stops; alive, it follows
        if (play.target != NULL_ENTITY) {
            Transform* transform = play.targetScene && play.targetScene->isEntityCreated(play.target)
                                       ? play.targetScene->findComponent<Transform>(play.target) : nullptr;
            if (!transform) {
                for (const FreeVoice& voice : play.voices) stopVoice(voice.handle);
                releaseFreePlay(play);
                continue;
            }
            play.position = transform->worldPosition;
        }

        const bool pause = held || play.paused;
        if (pause != play.appliedPause) {
            for (const FreeVoice& voice : play.voices) {
                soloud.setPause(voice.handle, pause);
                pauseVoiceLine(voice.handle, pause);
            }
            play.appliedPause = pause;
        }

        if (play.fade.active) {
            AudioFade& fade = play.fade;
            fade.elapsed += fade.unscaled ? unscaledDt : scaledDt;
            const double t = fade.duration > 0.0 ? std::min(1.0, fade.elapsed / fade.duration) : 1.0;
            play.volume = (float)(fade.from + (fade.to - fade.from) * t);
            const double frame = std::max(0.0, unscaledDt);
            for (const FreeVoice& voice : play.voices) {
                if (frame > 0.0) soloud.fadeVolume(voice.handle, play.volume * voice.volumeScale, frame);
                else soloud.setVolume(voice.handle, play.volume * voice.volumeScale);
            }
            if (t >= 1.0) {
                const AudioFade done = fade;
                fade = AudioFade();
                if (done.end == AudioFadeEnd::Stop) {
                    for (const FreeVoice& voice : play.voices) stopVoiceBecause(voice.handle, "fade_out", 0);
                    releaseFreePlay(play);
                    continue;
                }
                if (done.end == AudioFadeEnd::Pause) {
                    play.paused = true;
                    play.volume = (float)done.restoreVolume;
                    for (const FreeVoice& voice : play.voices) {
                        soloud.setPause(voice.handle, true);
                        pauseVoiceLine(voice.handle, true);
                        soloud.setVolume(voice.handle, play.volume * voice.volumeScale);
                    }
                    play.appliedPause = true;
                }
            }
        }

        if (play.is3D && hasListener) {
            const Vector3 place = heard(play.position);
            Vector3 velocity(0, 0, 0);
            if (doppler && play.heardTracked) {
                velocity = dopplerVelocity(play.lastHeard, place, unscaledDt, 1.0f);
            }
            for (const FreeVoice& voice : play.voices) {
                soloud.set3dSourceParameters(voice.handle, place.x, place.y, place.z, velocity.x, velocity.y, velocity.z);
            }
            play.lastHeard = place;
            play.heardTracked = true;
            any3D = true;
        }
    }
}

void AudioSystem::forgetFreeVoices(){
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    for (FreePlay& play : freePlays()) {
        if (!play.used || play.owner != this) continue;
        if (inited) {
            for (const FreeVoice& voice : play.voices) stopVoice(voice.handle);
        }
        releaseFreePlay(play);
    }
}

int AudioSystem::stopFreeAndOneShots(const std::string& group){
    if (isDeferringCalls()) {
        deferCall([group]() { stopFreeAndOneShots(group); });
        return 0;
    }
    if (!inited) return 0;
    // every group is below Master
    bool all = group.empty() || group == "Master";
    std::set<std::string> groups;
    if (!all) {
        const std::string id = AudioMixer::findGroupId(group);
        const AudioMixerDef& def = AudioMixer::getDef();
        if (id.empty()) {
            if (soundEventWarnings().insert("stopAll:" + group).second) {
                Log::warn("Audio.stopAll: mixer group '%s' is not in the mixer: nothing stops", group.c_str());
            }
            return 0;
        }
        all = def.master() && def.master()->id == id;
        groups.insert(id);
        for (bool grew = true; grew;) {
            grew = false;
            for (const AudioMixerGroupDef& candidate : def.groups) {
                if (groups.count(candidate.parentId) && groups.insert(candidate.id).second) grew = true;
            }
        }
    }
    pruneVoices();
    std::vector<unsigned int> stopping;
    for (const auto& [handle, record] : voices()) {
        if (record.freeOrOneShot && (all || groups.count(record.groupId))) stopping.push_back(handle);
    }
    for (unsigned int handle : stopping) stopVoiceBecause(handle, "stop_all", 0);
    return (int)stopping.size();
}

bool AudioSystem::freeIsValid(uint32_t id, uint32_t generation){
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    return play && freePlayLive(*play);
}

bool AudioSystem::freeIsPlaying(uint32_t id, uint32_t generation){
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    return play && freePlayLive(*play) && !play->paused;
}

void AudioSystem::freeStop(uint32_t id, uint32_t generation){
    if (isDeferringCalls()) {
        deferCall([id, generation]() { freeStop(id, generation); });
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    if (!play) return;
    if (inited) {
        for (const FreeVoice& voice : play->voices) stopVoiceBecause(voice.handle, "stop", 0);
    }
    releaseFreePlay(*play);
}

void AudioSystem::freePause(uint32_t id, uint32_t generation, bool pausedNow){
    if (isDeferringCalls()) {
        deferCall([id, generation, pausedNow]() { freePause(id, generation, pausedNow); });
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    // its update applies it, with the scene's and the listener's
    if (play) play->paused = pausedNow;
}

void AudioSystem::freeFadeOut(uint32_t id, uint32_t generation, float seconds, bool pause, bool unscaled){
    if (isDeferringCalls()) {
        deferCall([=]() { freeFadeOut(id, generation, seconds, pause, unscaled); });
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    if (!play || !std::isfinite(seconds)) return;
    const double restoreVolume = play->fade.active && play->fade.restore ? play->fade.restoreVolume : play->volume;
    AudioFade fade;
    fade.active = true;
    fade.from = play->volume;
    fade.to = 0;
    fade.duration = std::max(0.0f, seconds);
    fade.unscaled = unscaled;
    fade.end = pause ? AudioFadeEnd::Pause : AudioFadeEnd::Stop;
    fade.restore = true;
    fade.restoreVolume = restoreVolume;
    play->fade = fade;
}

void AudioSystem::freeFadeTo(uint32_t id, uint32_t generation, float volume, float seconds, bool unscaled){
    if (isDeferringCalls()) {
        deferCall([=]() { freeFadeTo(id, generation, volume, seconds, unscaled); });
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    if (!play || !std::isfinite(volume) || !std::isfinite(seconds)) return;
    AudioFade fade;
    fade.active = true;
    fade.from = play->volume;
    fade.to = std::max(0.0f, volume);
    fade.duration = std::max(0.0f, seconds);
    fade.unscaled = unscaled;
    play->fade = fade;
}

float AudioSystem::freeGet(uint32_t id, uint32_t generation, FreeValue value){
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    if (!play || !freePlayLive(*play)) return 0.0f;
    switch (value) {
        case FreeValue::Volume: return play->volume;
        case FreeValue::Pitch: return play->pitch;
        case FreeValue::Pan: return play->pan;
    }
    return 0.0f;
}

void AudioSystem::freeSet(uint32_t id, uint32_t generation, FreeValue value, float amount){
    if (!std::isfinite(amount)) {
        Log::warn("SoundHandle: a value that is not a number is ignored");
        return;
    }
    if (isDeferringCalls()) {
        deferCall([=]() { freeSet(id, generation, value, amount); });
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    if (!play || !freePlayLive(*play)) return;
    SoLoud::Soloud& soloud = getSoloud();
    switch (value) {
        case FreeValue::Volume:
            play->volume = std::max(0.0f, amount);
            play->fade = AudioFade();
            for (const FreeVoice& voice : play->voices) soloud.setVolume(voice.handle, play->volume * voice.volumeScale);
            break;
        case FreeValue::Pitch:
            play->pitch = amount > 0.0f ? amount : 0.0001f;
            for (const FreeVoice& voice : play->voices) soloud.setRelativePlaySpeed(voice.handle, play->pitch * voice.speedScale);
            break;
        case FreeValue::Pan:
            play->pan = std::clamp(amount, -1.0f, 1.0f);
            if (!play->is3D) {
                for (const FreeVoice& voice : play->voices) soloud.setPan(voice.handle, play->pan);
            }
            break;
    }
}

Vector3 AudioSystem::freeGetPosition(uint32_t id, uint32_t generation){
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    return play && freePlayLive(*play) ? play->position : Vector3(0, 0, 0);
}

void AudioSystem::freeSetPosition(uint32_t id, uint32_t generation, const Vector3& position){
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        Log::warn("SoundHandle: a position that is not a number is ignored");
        return;
    }
    if (isDeferringCalls()) {
        deferCall([=]() { freeSetPosition(id, generation, position); });
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    if (!play || !freePlayLive(*play)) return;
    play->target = NULL_ENTITY;
    play->position = position;
}

double AudioSystem::freeGetTime(uint32_t id, uint32_t generation){
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    if (!play || !freePlayLive(*play) || !inited) return 0.0;
    SoLoud::Soloud& soloud = getSoloud();
    for (const FreeVoice& voice : play->voices) {
        if (soloud.isValidVoiceHandle(voice.handle)) return soloud.getStreamPosition(voice.handle);
    }
    return 0.0;
}

int AudioSystem::freeGetVoiceCount(uint32_t id, uint32_t generation){
    std::lock_guard<std::recursive_mutex> lock(freeMutex());
    FreePlay* play = freePlayOf(id, generation);
    if (!play || !freePlayLive(*play) || !inited) return 0;
    SoLoud::Soloud& soloud = getSoloud();
    int count = 0;
    for (const FreeVoice& voice : play->voices) {
        if (soloud.isValidVoiceHandle(voice.handle)) count++;
    }
    return count;
}

#else // the module is out: no subscriptions, no free voices

void AudioSystem::syncUiTriggers(AudioSourceComponent& audio, Entity, uint32_t){
    audio.boundPlayOn = 0;
}

void AudioSystem::syncPhysicsTriggers(uint32_t){
}

void AudioSystem::removeTriggerSubscriptions(){
}

void AudioSystem::drainTriggers(){
}

void AudioSystem::forgetFreeVoices(){
}

void AudioSystem::updateFreeVoices(double, double, bool, const std::function<Vector3(const Vector3&)>&, bool, bool&){
}

AudioSystem::PhysicsCallbackScope::PhysicsCallbackScope(){
}

AudioSystem::PhysicsCallbackScope::~PhysicsCallbackScope(){
}

#endif // JASPERENGINE_SOUND_LAYER
