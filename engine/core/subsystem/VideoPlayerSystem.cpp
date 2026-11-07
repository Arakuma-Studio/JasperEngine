// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoPlayerSystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/AudioSourceComponent.h"
#include "component/CameraComponent.h"
#include "component/ImageComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "htmlui/HtmlUIClock.h"
#include "media/IVideoSource.h"
#include "media/VideoAudio.h"
#include "media/VideoFormats.h"
#include "media/VideoPlayback.h"
#include "media/VideoSourceFactory.h"
#if JASPERENGINE_VIDEO
#include "media/VideoStream.h"
#endif
#include "media/VideoTextureConverter.h"
#include "subsystem/AudioSystem.h"
#include "texture/Material.h"
#include "object/VideoPlayer.h"
#include "texture/Framebuffer.h"

#include "soloud.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <vector>

using namespace jasperengine;

namespace {

    bool externalAudio = false;

    // the player is still there when its event is delivered (a handler before may have destroyed it)
    bool alive(Scene* scene, Entity entity){
        return scene->isEntityCreated(entity) && scene->findComponent<VideoPlayerComponent>(entity) != nullptr;
    }

    constexpr int DRIFT_SAMPLES = 8;

    // the media clock in 48 kHz sample frames from the start of loop iteration 0
    int64_t mediaFrames(int iteration, double position, int64_t durationNs){
        const long double perIteration = std::llround((long double)durationNs * VideoAudioRing::SAMPLE_RATE / 1e9L);
        return (int64_t)((long double)iteration * perIteration + std::llround((long double)position * VideoAudioRing::SAMPLE_RATE));
    }

    // a material texture slot MaterialOverride replaced, and what it had
    struct OverriddenSlot {
        Entity entity = NULL_ENTITY;
        int submesh = -1;           // -1: a UI component's texture
        std::string property;
        Texture saved;
        bool reload = false;        // the slot was empty: the renderer was loaded again with a texture
    };

    Texture* meshSlot(Material& material, const std::string& property, bool create){
        if (property == "baseColorTexture" || property == "texture")
            return &material.baseColorTexture;
        if (property == "emissiveTexture")
            return &material.emissiveTexture;
        if (property == "metallicRoughnessTexture")
            return &material.metallicRoughnessTexture;
        if (property == "occlusionTexture")
            return &material.occlusionTexture;
        if (property == "normalTexture")
            return &material.normalTexture;
        for (auto& [name, texture] : material.shaderTextures){
            if (name == property)
                return &texture;
        }
        if (create && !material.shader.empty()){
            material.shaderTextures.push_back({property, Texture()});
            return &material.shaderTextures.back().second;
        }
        return nullptr;
    }

    Texture* uiSlot(UIComponent& ui, const std::string& property, bool create){
        if (property == "baseColorTexture" || property == "texture")
            return &ui.texture;
        for (auto& [name, texture] : ui.shaderTextures){
            if (name == property)
                return &texture;
        }
        if (create){
            ui.shaderTextures.push_back({property, Texture()});
            return &ui.shaderTextures.back().second;
        }
        return nullptr;
    }

}

namespace {

    // one player's runtime
    struct PlayerEntry {
        std::unique_ptr<IVideoSource> source;
        std::string openedPath;
        bool openedAudio = true;
        VideoPlayback clock;
        std::vector<VideoPlayback::Event> events;
        double now = 0.0;
        bool prepared = false;
        bool playWanted = false;        // play() before it was prepared
        bool startedPending = false;    // onStarted waits for the first frame after play()
        bool awoken = false;            // playOnAwake was looked at
        bool userSeek = false;          // onSeekCompleted at the next SEEKED
        bool loopApplied = false;
        int lastIteration = 0;
        int64_t shownSequence = -1;
        int shownIndex = -1;
        bool stallWanted = false;
        MediaError error;
        bool errorReported = false;
        bool warnedEmpty = false;
        // audio
        std::unique_ptr<VideoAudioSource> voice;
        bool voice3d = false;
        bool audioSynced = false;
        double lastAudioNow = -1.0;
        double driftSamples[DRIFT_SAMPLES] = {};
        int driftCount = 0;
        // MaterialOverride
        std::vector<OverriddenSlot> overridden;
        bool overrideWarned = false;
    };

}

struct VideoPlayerSystem::State {
    std::unordered_map<Entity, PlayerEntry> entries;
    double lastUiNow = -1.0;
    std::vector<std::function<void()>> pendingEvents;
};

VideoPlayerSystem::VideoPlayerSystem(Scene* scene): SubSystem(scene), state(new State()){
}

VideoPlayerSystem::~VideoPlayerSystem(){
    destroy();
    delete state;
}

void VideoPlayerSystem::setAudioOutputForTesting(bool enabled){
    externalAudio = !enabled;
}

void VideoPlayerSystem::load(){
}

void VideoPlayerSystem::draw(){
    // the frames convert in RenderSystem's frame passes (processConversions)
}

namespace {

    // what a component plays: a URL or a path as the factory takes it ("" for none)
    std::string sourcePathOf(const VideoPlayerComponent& player){
        return player.source == VideoPlayerSource::URL ? player.url : player.clip;
    }

}

void VideoPlayerSystem::destroy(){
    if (!scene || !state)
        return;
    for (auto& [entity, entry] : state->entries){
        if (entry.voice)
            entry.voice->stop();
        entry.voice.reset();
        // the converter may hold the source for a conversion not run yet (the browser's element)
        if (scene->isComponentRegistered<VideoPlayerComponent>() && scene->isEntityCreated(entity)){
            if (VideoPlayerComponent* player = scene->findComponent<VideoPlayerComponent>(entity); player && player->output)
                player->output->forgetSource(entry.source.get());
        }
        entry.source.reset();
    }
    state->entries.clear();
    state->pendingEvents.clear();
}

void VideoPlayerSystem::setPaused(bool value){
    SubSystem::setPaused(value);
    for (auto& [entity, entry] : state->entries){
        if (entry.voice)
            entry.voice->setPaused(value || !entry.clock.isAdvancing());
        // a source that plays by itself (the browser's element) holds too
        if (entry.source)
            entry.source->setClock(!value && entry.clock.isAdvancing(), entry.clock.getPlaybackRate(), entry.clock.getPosition());
    }
}

void VideoPlayerSystem::onComponentAdded(Entity entity, ComponentId componentId){
    if (!scene->isComponentRegistered<VideoPlayerComponent>() || componentId != scene->getComponentId<VideoPlayerComponent>())
        return;
    state->entries[entity] = PlayerEntry();
    VideoPlayerComponent* player = scene->findComponent<VideoPlayerComponent>(entity);
    if (player && !player->output)
        player->output = std::make_shared<VideoTextureConverter>();
}

namespace {

    void restoreOverride(Scene* scene, std::vector<OverriddenSlot>& slots){
        for (OverriddenSlot& slot : slots){
            if (!scene->isEntityCreated(slot.entity))
                continue;
            if (slot.submesh >= 0){
                MeshComponent* mesh = scene->findComponent<MeshComponent>(slot.entity);
                if (!mesh || slot.submesh >= (int)std::max(mesh->numSubmeshes, 1u))
                    continue;
                Submesh& submesh = mesh->submeshes[slot.submesh];
                if (Texture* texture = meshSlot(submesh.material, slot.property, false))
                    *texture = slot.saved;
                submesh.needUpdateTexture = true;
                if (slot.reload)
                    mesh->needReload = true;
            }else{
                UIComponent* ui = scene->findComponent<UIComponent>(slot.entity);
                if (!ui)
                    continue;
                if (Texture* texture = uiSlot(*ui, slot.property, false))
                    *texture = slot.saved;
                ui->needUpdateTexture = true;
                if (slot.reload)
                    ui->needReload = true;
            }
        }
        slots.clear();
    }

}

void VideoPlayerSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (!scene->isComponentRegistered<VideoPlayerComponent>() || componentId != scene->getComponentId<VideoPlayerComponent>())
        return;
    auto it = state->entries.find(entity);
    if (it != state->entries.end()){
        restoreOverride(scene, it->second.overridden);
        if (it->second.voice)
            it->second.voice->stop();
        if (VideoPlayerComponent* player = scene->findComponent<VideoPlayerComponent>(entity); player && player->output)
            player->output->forgetSource(it->second.source.get());
        state->entries.erase(it);
    }
    if (VideoPlayerComponent* player = scene->findComponent<VideoPlayerComponent>(entity); player && player->output)
        player->output->release();
}

VideoTextureConverter* VideoPlayerSystem::getOutput(Entity entity){
    VideoPlayerComponent* player = scene->findComponent<VideoPlayerComponent>(entity);
    if (!player)
        return nullptr;
    if (!player->output)
        player->output = std::make_shared<VideoTextureConverter>();
    return player->output.get();
}

// ---------------------------------------------------------------- control

void VideoPlayerSystem::prepare(Entity entity){
    VideoPlayerComponent* player = scene->findComponent<VideoPlayerComponent>(entity);
    if (!player)
        return;
    PlayerEntry& entry = state->entries[entity];
    const std::string path = sourcePathOf(*player);
    if (path.empty()){
        if (!entry.warnedEmpty){
            entry.warnedEmpty = true;
            Log::warn("[Video] '%s': nothing to play (set its %s)", scene->getEntityName(entity).c_str(),
                      player->source == VideoPlayerSource::URL ? "url" : "clip");
        }
        return;
    }
    // a clip or url set since it opened (`vp.url = x; vp:prepare()` in one go): that one goes
    // ... and one that failed: prepare() tries again (a URL back online, a file fixed)
    if (entry.source && (entry.openedPath != path || entry.openedAudio != (player->audioOutputMode != VideoAudioOutputMode::NONE) ||
                         entry.source->getState() == IVideoSource::State::FAILED))
        stop(entity);
    if (entry.source)
        return;
    VideoSourceFactory::Options options;
#if JASPERENGINE_VIDEO
    options.threaded = VideoStream::platformHasThreads();
#endif
    options.audio = player->audioOutputMode != VideoAudioOutputMode::NONE;
    entry.source = VideoSourceFactory::create(path, options);
    entry.openedPath = path;
    entry.openedAudio = options.audio;
    entry.prepared = false;
    entry.error = MediaError();
    entry.errorReported = false;
    entry.clock.reset();
    entry.events.clear();
    entry.shownSequence = -1;
    entry.shownIndex = -1;
    entry.lastIteration = 0;
    entry.loopApplied = false;
    entry.audioSynced = false;
}

void VideoPlayerSystem::play(Entity entity){
    VideoPlayerComponent* player = scene->findComponent<VideoPlayerComponent>(entity);
    if (!player)
        return;
    PlayerEntry& entry = state->entries[entity];
    entry.awoken = true;
    if (entry.source && (entry.openedPath != sourcePathOf(*player) ||
                         entry.openedAudio != (player->audioOutputMode != VideoAudioOutputMode::NONE) ||
                         entry.source->getState() == IVideoSource::State::FAILED))
        stop(entity);
    if (!entry.source)
        prepare(entity);
    if (!entry.source)
        return;
    entry.startedPending = true;
    if (!entry.prepared){
        entry.playWanted = true;
        return;
    }
    entry.clock.play(entry.now, entry.events);
    // waitForFirstFrame: the clock starts once a frame of the time is shown
    entry.stallWanted = player->waitForFirstFrame;
    entry.audioSynced = false;
}

void VideoPlayerSystem::pause(Entity entity){
    auto it = state->entries.find(entity);
    if (it == state->entries.end())
        return;
    PlayerEntry& entry = it->second;
    entry.playWanted = false;
    if (entry.prepared)
        entry.clock.pause(entry.now, entry.events);
    if (entry.voice)
        entry.voice->setPaused(true);
}

void VideoPlayerSystem::stop(Entity entity){
    auto it = state->entries.find(entity);
    if (it == state->entries.end())
        return;
    PlayerEntry& entry = it->second;
    restoreOverride(scene, entry.overridden);
    if (entry.voice)
        entry.voice->stop();
    entry.voice.reset();
    if (VideoPlayerComponent* player = scene->findComponent<VideoPlayerComponent>(entity); player && player->output)
        player->output->forgetSource(entry.source.get());
    // the decoder goes; the texture keeps its last frame until the next prepare
    entry.source.reset();
    entry.openedPath.clear();
    entry.prepared = false;
    entry.playWanted = false;
    entry.startedPending = false;
    entry.userSeek = false;
    entry.clock.reset();
    entry.events.clear();
    entry.shownSequence = -1;
    entry.shownIndex = -1;
    entry.lastIteration = 0;
}

void VideoPlayerSystem::resetPlayers(){
    if (!scene || !state)
        return;
    std::vector<Entity> entities;
    for (const auto& [entity, entry] : state->entries)
        entities.push_back(entity);
    for (Entity entity : entities){
        stop(entity);
        PlayerEntry& entry = state->entries[entity];
        entry.awoken = false;
        entry.warnedEmpty = false;
        entry.error = MediaError();
        entry.errorReported = false;
        entry.now = 0.0;
        entry.lastAudioNow = -1.0;
    }
    state->pendingEvents.clear();
    state->lastUiNow = -1.0;
}

void VideoPlayerSystem::handOver(Entity entity, VideoPlayerSystem& to, Entity toEntity){
    if (!state || !to.state)
        return;
    auto it = state->entries.find(entity);
    if (it == state->entries.end())
        return;
    PlayerEntry moved = std::move(it->second);
    state->entries.erase(it);
    // Material Override slots belong to this scene's renderers: given back here, set again there
    restoreOverride(scene, moved.overridden);
    moved.overrideWarned = false;
    to.state->entries[toEntity] = std::move(moved);
}

void VideoPlayerSystem::setTime(Entity entity, double seconds){
    auto it = state->entries.find(entity);
    if (it == state->entries.end() || !it->second.prepared || !std::isfinite(seconds))
        return;
    PlayerEntry& entry = it->second;
    entry.clock.seek(seconds, entry.now, entry.events);
    entry.userSeek = true;
    entry.audioSynced = false;
}

void VideoPlayerSystem::setFrame(Entity entity, int64_t frame){
    auto it = state->entries.find(entity);
    if (it == state->entries.end() || !it->second.prepared)
        return;
    const VideoInfo& info = it->second.source->getInfo();
    const double rate = info.frameRate > 0.0 ? info.frameRate : 30.0;
    const int64_t clamped = std::clamp<int64_t>(frame, 0, std::max(info.frameCount - 1, 0));
    // a little into the frame: its own time, not the end of the one before
    setTime(entity, ((double)clamped + 1e-3) / rate);
}

void VideoPlayerSystem::stepForward(Entity entity){
    auto it = state->entries.find(entity);
    if (it == state->entries.end() || !it->second.prepared)
        return;
    PlayerEntry& entry = it->second;
    if (!entry.clock.isPaused())
        entry.clock.pause(entry.now, entry.events);
    const int64_t next = entry.shownIndex < 0 ? 0 : entry.shownIndex + 1;
    if (next >= entry.source->getInfo().frameCount)
        return;
    setFrame(entity, next);
}

VideoPlayerSystem::Status VideoPlayerSystem::getStatus(Entity entity) const{
    Status status;
    auto it = state->entries.find(entity);
    if (it == state->entries.end())
        return status;
    const PlayerEntry& entry = it->second;
    status.error = entry.error.message;
    if (!entry.source)
        return status;
    status.backend = entry.source->backendName();
    status.preparing = !entry.prepared && !entry.error;
    if (!entry.prepared){
        const double download = entry.source->getDownloadProgress();
        status.prepareProgress = download < 0.0 ? 0.0 : download * 0.95;
        return status;
    }
    const VideoInfo& info = entry.source->getInfo();
    status.prepared = true;
    status.prepareProgress = 1.0;
    status.canSetTime = true;
    status.paused = entry.clock.isPaused() && !entry.clock.isEnded();
    status.playing = !entry.clock.isPaused() && !entry.clock.isEnded();
    status.time = entry.clock.getPosition();
    status.frame = entry.shownIndex;
    status.frameCount = info.frameCount;
    status.frameRate = info.frameRate;
    status.length = info.duration;
    status.width = info.displayWidth;
    status.height = info.displayHeight;
    status.bufferedSeconds = entry.source->getBufferedSeconds();
    status.videoCodec = info.videoCodec.empty() ? std::string() : VideoFormats::codecName(info.videoCodec);
    status.audioCodec = info.audioCodec.empty() ? std::string() : VideoFormats::codecName(info.audioCodec);
    return status;
}

int VideoPlayerSystem::getOpenSources() const{
    int count = 0;
    for (const auto& [entity, entry] : state->entries)
        count += entry.source ? 1 : 0;
    return count;
}


namespace {

    // the player's sound: a SoLoud voice reading the source's ring, kept in step with the clock
    // (the WebM path's sync controller); Direct at directVolume, AudioSource through that
    // component's volume and 3D settings (at its GameObject's position)
    void updateAudio(Scene* scene, Entity entity, VideoPlayerComponent& player, PlayerEntry& entry, bool frameExact){
        IVideoSource& source = *entry.source;
        const VideoInfo& info = source.getInfo();
        if (!info.hasAudio || player.audioOutputMode == VideoAudioOutputMode::NONE)
            return;
        AudioSourceComponent* target = nullptr;
        Entity targetEntity = player.targetAudioSource == NULL_ENTITY ? entity : player.targetAudioSource;
        float volume = player.directMute ? 0.0f : std::clamp(player.directVolume, 0.0f, 1.0f);
        if (player.audioOutputMode == VideoAudioOutputMode::AUDIO_SOURCE){
            target = scene->isEntityCreated(targetEntity) ? scene->findComponent<AudioSourceComponent>(targetEntity) : nullptr;
            volume = target && target->enabled ? (float)std::clamp(target->volume, 0.0, 1.0) : 0.0f;
        }
        // Chromium's audible range of rates
        const double rate = entry.clock.getPlaybackRate();
        if (rate < 0.25 || rate > 4.0)
            volume = 0.0f;
        if (source.playsOwnAudio()){
            source.setOutputVolume(volume * AudioSystem::getGlobalVolume());
            return;
        }
        std::shared_ptr<VideoAudioRing> ring = source.getAudioRing();
        if (!ring)
            return;
        const double now = entry.now;
        const bool moving = entry.clock.isAdvancing() && entry.lastAudioNow >= 0.0 && now > entry.lastAudioNow;
        const bool clockRuns = entry.clock.isAdvancing() && (entry.lastAudioNow < 0.0 || now > entry.lastAudioNow);
        entry.lastAudioNow = now;

        int64_t latency = 0;
        if (!externalAudio){
            const bool want3d = target != nullptr && scene->findComponent<Transform>(targetEntity) != nullptr;
            if (entry.voice && entry.voice3d != want3d){
                entry.voice->stop();
                entry.voice.reset();
            }
            if (!entry.voice && entry.clock.isAdvancing()){
                entry.voice = std::make_unique<VideoAudioSource>(ring);
                bool started = false;
                if (want3d){
                    const Vector3 p = scene->getComponent<Transform>(targetEntity).worldPosition;
                    started = entry.voice->start3d(volume, p.x, p.y, p.z);
                }else{
                    started = entry.voice->start(volume);
                }
                entry.voice3d = want3d;
                if (!started)
                    entry.voice.reset();
                entry.audioSynced = false;
            }
            if (!entry.voice)
                return;
            if (!entry.voice->isPlaying()){
                // SoLoud was torn down: a new voice next time
                entry.voice.reset();
                return;
            }
            latency = entry.voice->getLatencyFrames();
            entry.voice->setVolume(volume);
            entry.voice->setSpeed((float)std::max(rate, 0.0625));
            if (entry.voice3d){
                const Vector3 p = scene->getComponent<Transform>(targetEntity).worldPosition;
                float listener[9] = {0, 0, 0, 0, 0, -1, 0, 1, 0};
                const Entity cameraEntity = scene->getCamera();
                if (CameraComponent* camera = scene->findComponent<CameraComponent>(cameraEntity)){
                    const Vector3 eye = scene->getComponent<Transform>(cameraEntity).worldPosition;
                    const Vector3 at = camera->worldTarget - eye;
                    const float values[9] = {eye.x, eye.y, eye.z, at.x, at.y, at.z, camera->worldUp.x, camera->worldUp.y, camera->worldUp.z};
                    std::copy(values, values + 9, listener);
                }
                unsigned int model = SoLoud::AudioSource::NO_ATTENUATION;
                if (target->attenuationModel == AudioSourceAttenuation::INVERSE_DISTANCE)
                    model = SoLoud::AudioSource::INVERSE_DISTANCE;
                else if (target->attenuationModel == AudioSourceAttenuation::LINEAR_DISTANCE)
                    model = SoLoud::AudioSource::LINEAR_DISTANCE;
                else if (target->attenuationModel == AudioSourceAttenuation::EXPONENTIAL_DISTANCE)
                    model = SoLoud::AudioSource::EXPONENTIAL_DISTANCE;
                entry.voice->set3d(p.x, p.y, p.z, target->minDistance, target->maxDistance, model, target->attenuationRolloffFactor, listener);
            }
        }

        const int64_t media = mediaFrames(entry.clock.getIteration(), entry.clock.getPosition(), info.durationNs);
        if (!entry.audioSynced){
            source.syncAudio(media + latency);
            entry.audioSynced = true;
            entry.driftCount = 0;
            if (frameExact)
                source.waitAudioSync();
        }else if (moving && !source.isAudioSyncPending()){
            const VideoAudioSync::Decision decision = VideoAudioSync::decide(ring->getReadPosition(), ring->getPendingCorrection(), media, latency);
            if (decision.action == VideoAudioSync::Action::RESYNC){
                source.syncAudio(decision.frames);
                entry.driftCount = 0;
            }else{
                // the mixer reads in blocks: the mean of the last updates outside the dead band is corrected
                entry.driftSamples[entry.driftCount % DRIFT_SAMPLES] = decision.drift;
                entry.driftCount++;
                if (entry.driftCount >= DRIFT_SAMPLES){
                    double mean = 0.0;
                    for (double sample : entry.driftSamples)
                        mean += sample;
                    mean /= DRIFT_SAMPLES;
                    if (std::fabs(mean) > VideoAudioSync::DEAD_BAND){
                        ring->requestCorrection(ring->getPendingCorrection() + (int64_t)std::llround(mean * VideoAudioRing::SAMPLE_RATE));
                        entry.driftCount = 0;
                    }
                }
            }
        }
        if (entry.voice)
            entry.voice->setPaused(!clockRuns);
    }

    // MaterialOverride: the player's texture in the renderer's slot while the player has a
    // picture; the slot's own texture again when it stops, changes mode or goes
    void updateOverride(Scene* scene, Entity entity, VideoPlayerComponent& player, PlayerEntry& entry){
        const bool wanted = player.renderMode == VideoRenderMode::MATERIAL_OVERRIDE && player.output && player.output->hasPicture();
        const Entity targetEntity = player.targetMaterialRenderer == NULL_ENTITY ? entity : player.targetMaterialRenderer;
        // already applied to this target and property
        if (wanted && !entry.overridden.empty() && entry.overridden.front().entity == targetEntity &&
            entry.overridden.front().property == player.targetMaterialProperty)
            return;
        restoreOverride(scene, entry.overridden);
        if (!wanted || !scene->isEntityCreated(targetEntity))
            return;
        Texture video;
        video.setFramebuffer(player.output->getFramebuffer());
        const std::string& property = player.targetMaterialProperty.empty() ? std::string("baseColorTexture") : player.targetMaterialProperty;
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(targetEntity)){
            const unsigned int count = std::max(mesh->numSubmeshes, 1u);
            for (unsigned int s = 0; s < count; s++){
                if (player.targetSubmesh >= 0 && (int)s != player.targetSubmesh)
                    continue;
                Submesh& submesh = mesh->submeshes[s];
                Texture* slot = meshSlot(submesh.material, property, true);
                if (!slot)
                    continue;
                OverriddenSlot saved;
                saved.entity = targetEntity;
                saved.submesh = (int)s;
                saved.property = property;
                saved.saved = *slot;
                // an untextured submesh's variant samples nothing: loaded again with the texture
                saved.reload = slot->empty();
                *slot = video;
                submesh.needUpdateTexture = true;
                if (saved.reload)
                    mesh->needReload = true;
                entry.overridden.push_back(saved);
            }
        }else if (UIComponent* ui = scene->findComponent<UIComponent>(targetEntity)){
            if (Texture* slot = uiSlot(*ui, property, true)){
                OverriddenSlot saved;
                saved.entity = targetEntity;
                saved.submesh = -1;
                saved.property = property;
                saved.saved = *slot;
                saved.reload = slot->empty();
                *slot = video;
                ui->needUpdateTexture = true;
                if (saved.reload)
                    ui->needReload = true;
                entry.overridden.push_back(saved);
            }
        }
        if (entry.overridden.empty() && !entry.overrideWarned){
            entry.overrideWarned = true;
            Log::error("[Video] '%s': Material Override found no '%s' texture on '%s' (a Mesh, Sprite or Image with that material "
                       "texture or shader @texture uniform)", scene->getEntityName(entity).c_str(), property.c_str(),
                       scene->getEntityName(targetEntity).c_str());
        }
    }

}

// ---------------------------------------------------------------- render (after update)

void VideoPlayerSystem::processConversions(){
    if (!scene->isComponentRegistered<VideoPlayerComponent>())
        return;
    auto players = scene->getComponentArray<VideoPlayerComponent>();
    for (size_t i = 0; i < players->size(); i++){
        VideoPlayerComponent& player = players->getComponentFromIndex(i);
        if (player.output && player.output->hasPending())
            player.output->convert();
    }
}

bool VideoPlayerSystem::hasCameraPlanes(bool farPlane) const{
    if (!scene->isComponentRegistered<VideoPlayerComponent>())
        return false;
    const VideoRenderMode mode = farPlane ? VideoRenderMode::CAMERA_FAR_PLANE : VideoRenderMode::CAMERA_NEAR_PLANE;
    auto players = scene->getComponentArray<VideoPlayerComponent>();
    for (size_t i = 0; i < players->size(); i++){
        const VideoPlayerComponent& player = players->getComponentFromIndex(i);
        if (player.renderMode == mode && player.enabled && player.output && player.output->hasPicture() &&
            scene->isEntityActiveInHierarchy(players->getEntity(i)))
            return true;
    }
    return false;
}

void VideoPlayerSystem::drawCameraPlanes(Entity camera, bool farPlane, const RenderTargetFormat& target, CameraRender& pass,
                                         const Rect& viewport, bool flipY){
    if (!scene->isComponentRegistered<VideoPlayerComponent>())
        return;
    const VideoRenderMode mode = farPlane ? VideoRenderMode::CAMERA_FAR_PLANE : VideoRenderMode::CAMERA_NEAR_PLANE;
    auto players = scene->getComponentArray<VideoPlayerComponent>();
    for (size_t i = 0; i < players->size(); i++){
        VideoPlayerComponent& player = players->getComponentFromIndex(i);
        if (player.renderMode != mode || !player.enabled || !player.output || !scene->isEntityActiveInHierarchy(players->getEntity(i)))
            continue;
        // the target camera, else the scene's main one
        const Entity wanted = player.targetCamera != NULL_ENTITY && scene->isEntityCreated(player.targetCamera) &&
                              scene->findComponent<CameraComponent>(player.targetCamera) ? player.targetCamera : scene->getCamera();
        if (wanted != camera)
            continue;
        player.output->drawPlane(target, pass, viewport, player.aspectRatio, player.targetCameraAlpha, flipY);
    }
}

// ---------------------------------------------------------------- update

void VideoPlayerSystem::update(double dt){
    // unscaled time: the UI clock's (it runs at time scale 0 and stops with a paused app)
    const double uiNow = HtmlUIClock::now();
    const double unscaled = state->lastUiNow < 0.0 ? 0.0 : std::max(uiNow - state->lastUiNow, 0.0);
    state->lastUiNow = uiNow;
    if (paused || !scene->isComponentRegistered<VideoPlayerComponent>())
        return;

    auto players = scene->getComponentArray<VideoPlayerComponent>();
    const bool frameExact = HtmlUIClock::getFixedTime().has_value() || Engine::getFixedDeltatime() > 0.0;
    for (size_t i = 0; i < players->size(); i++){
        VideoPlayerComponent& player = players->getComponentFromIndex(i);
        const Entity entity = players->getEntity(i);
        PlayerEntry& entry = state->entries[entity];
        if (!player.output)
            player.output = std::make_shared<VideoTextureConverter>();
        const bool active = player.enabled && scene->isEntityActiveInHierarchy(entity);

        // a new clip or url (or the audio switched on/off): what played stops
        const bool audioWanted = player.audioOutputMode != VideoAudioOutputMode::NONE;
        if (entry.source && (sourcePathOf(player) != entry.openedPath || audioWanted != entry.openedAudio))
            stop(entity);

        if (!active){
            // a disabled or inactive player holds (its sound pauses)
            if (entry.voice)
                entry.voice->setPaused(true);
            continue;
        }
        if (!entry.awoken){
            entry.awoken = true;
            if (player.playOnAwake && !sourcePathOf(player).empty())
                play(entity);
        }

        entry.now += player.timeUpdateMode == VideoTimeUpdateMode::GAME_TIME ? dt : unscaled;
        if (!entry.source)
            continue;

        IVideoSource& source = *entry.source;
        IVideoSource::State sourceState = source.getState();
        if (sourceState == IVideoSource::State::OPENING || sourceState == IVideoSource::State::BUFFERING){
            source.pump(1);
            if (frameExact && sourceState == IVideoSource::State::OPENING)
                source.waitOpen();
            sourceState = source.getState();
        }
        if (sourceState == IVideoSource::State::FAILED && !entry.errorReported){
            entry.errorReported = true;
            entry.error = source.getError();
            if (!entry.error){
                entry.error.code = MediaErrorCode::DECODE;
                entry.error.message = "the video failed";
            }
            entry.playWanted = false;
            const std::string message = entry.error.message;
            Log::error("[Video] '%s': %s", scene->getEntityName(entity).c_str(), message.c_str());
            if (entry.prepared)
                entry.clock.pause(entry.now, entry.events);
            FunctionSubscribe<void(VideoPlayer, const std::string&)> handler = player.onErrorReceived;
            Scene* owner = scene;
            state->pendingEvents.push_back([owner, entity, handler, message]() mutable {
                if (alive(owner, entity))
                    handler.call(VideoPlayer(owner, entity), message);
            });
            continue;
        }
        if (sourceState != IVideoSource::State::READY)
            continue;

        const VideoInfo& info = source.getInfo();
        if (!entry.prepared){
            entry.prepared = true;
            entry.clock.setDuration(info.duration);
            entry.loopApplied = !player.isLooping;   // applied below
            FunctionSubscribe<void(VideoPlayer)> handler = player.onPrepareCompleted;
            Scene* owner = scene;
            state->pendingEvents.push_back([owner, entity, handler]() mutable {
                if (alive(owner, entity))
                    handler.call(VideoPlayer(owner, entity));
            });
            if (entry.playWanted){
                entry.playWanted = false;
                entry.clock.play(entry.now, entry.events);
                entry.stallWanted = player.waitForFirstFrame;
            }
        }
        if (entry.loopApplied != player.isLooping){
            entry.loopApplied = player.isLooping;
            entry.clock.setLoop(player.isLooping);
            source.setLoop(player.isLooping);
        }
        const double speed = std::clamp((double)player.playbackSpeed, VideoPlayback::MIN_RATE, VideoPlayback::MAX_RATE);
        if (std::fabs(entry.clock.getPlaybackRate() - speed) > 1e-9){
            entry.clock.setPlaybackRate(speed, entry.now, entry.events);
            entry.audioSynced = false;
        }
        if (entry.stallWanted){
            entry.stallWanted = false;
            if (entry.shownSequence < 0 || entry.clock.isSeeking())
                entry.clock.setStalled(true, entry.now, entry.events);
        }

        entry.clock.update(entry.now, entry.events);
        source.setClock(entry.clock.isAdvancing() && !paused, entry.clock.getPlaybackRate(), entry.clock.getPosition());

        // the frame of the clock's time
        bool shown = false;
        bool exact = false;
        if (info.hasVideo){
            VideoFrame frame;
            // only frame-exact play (fixed time steps) waits for the frame, bounded; real-time play
            // never blocks: without skipOnDrop the clock waits for the decoder instead (below)
            const bool wait = frameExact;
            const IVideoSource::FrameResult result = source.acquireFrame(entry.clock.getIteration(), entry.clock.getPosition(), wait,
                                                                         entry.shownSequence, frame);
            switch (result){
                case IVideoSource::FrameResult::NEW:
                case IVideoSource::FrameResult::STALE:
                    entry.shownSequence = frame.sequence;
                    if (source.rendersToTexture()){
                        // the browser's element uploads at its own size (renderTextureWidth/Height do not apply)
                        player.output->submitSource(&source, info.displayWidth, info.displayHeight);
                        entry.shownIndex = source.frameIndexFor(entry.clock.getPosition());
                        shown = true;
                    }else if (!frame.y.empty()){
                        entry.shownIndex = frame.index;
                        player.output->submit(std::move(frame), (int)player.renderTextureWidth, (int)player.renderTextureHeight, player.aspectRatio);
                        shown = true;
                    }else{
                        entry.shownIndex = frame.index;
                    }
                    exact = result == IVideoSource::FrameResult::NEW;
                    break;
                case IVideoSource::FrameResult::SAME:
                    exact = true;
                    break;
                case IVideoSource::FrameResult::NOT_READY:
                case IVideoSource::FrameResult::FAILED:
                    break;
            }
        }else{
            exact = true;
        }
        if (exact){
            entry.clock.setStalled(false, entry.now, entry.events);
            if (entry.clock.isSeeking())
                entry.clock.completeSeek(entry.now, entry.events);
            entry.clock.notifyReady(entry.now, entry.events);
        }else if (!entry.clock.isPaused() && !entry.clock.isSeeking() && info.hasVideo){
            // real time a quarter of a second behind the decoder: the clock waits
            const int64_t target = source.sequenceFor(entry.clock.getIteration(), entry.clock.getPosition());
            const double frameSeconds = info.frameRate > 0.0 ? 1.0 / info.frameRate : 1.0 / 30.0;
            const int64_t lag = std::max<int64_t>(target - std::max<int64_t>(entry.shownSequence, 0), 0);
            // skipOnDrop off (Unity): every frame shows, the clock slows down to the decoder
            if (entry.shownSequence < 0 || !player.skipOnDrop || (double)lag * frameSeconds > 0.25)
                entry.clock.setStalled(true, entry.now, entry.events);
        }

        // events of the clock
        Scene* owner = scene;
        for (VideoPlayback::Event event : entry.events){
            if (event == VideoPlayback::Event::PLAYING && entry.startedPending){
                entry.startedPending = false;
                FunctionSubscribe<void(VideoPlayer)> handler = player.onStarted;
                state->pendingEvents.push_back([owner, entity, handler]() mutable {
                    if (alive(owner, entity))
                        handler.call(VideoPlayer(owner, entity));
                });
            }else if (event == VideoPlayback::Event::SEEKED && entry.userSeek){
                entry.userSeek = false;
                FunctionSubscribe<void(VideoPlayer)> handler = player.onSeekCompleted;
                state->pendingEvents.push_back([owner, entity, handler]() mutable {
                    if (alive(owner, entity))
                        handler.call(VideoPlayer(owner, entity));
                });
            }else if (event == VideoPlayback::Event::ENDED){
                FunctionSubscribe<void(VideoPlayer)> handler = player.onLoopPointReached;
                state->pendingEvents.push_back([owner, entity, handler]() mutable {
                    if (alive(owner, entity))
                        handler.call(VideoPlayer(owner, entity));
                });
            }else if (event == VideoPlayback::Event::SEEKING || event == VideoPlayback::Event::SEEKED){
                entry.audioSynced = false;
            }
        }
        entry.events.clear();
        // each wrap of a loop
        if (entry.clock.getIteration() > entry.lastIteration){
            const int wraps = entry.clock.getIteration() - entry.lastIteration;
            FunctionSubscribe<void(VideoPlayer)> handler = player.onLoopPointReached;
            for (int w = 0; w < wraps; w++){
                state->pendingEvents.push_back([owner, entity, handler]() mutable {
                    if (alive(owner, entity))
                        handler.call(VideoPlayer(owner, entity));
                });
            }
        }
        entry.lastIteration = entry.clock.getIteration();
        if (shown && player.onFrameReady.hasSubscribers()){
            FunctionSubscribe<void(VideoPlayer, int)> handler = player.onFrameReady;
            const int index = entry.shownIndex;
            state->pendingEvents.push_back([owner, entity, handler, index]() mutable {
                if (alive(owner, entity))
                    handler.call(VideoPlayer(owner, entity), index);
            });
        }

        updateAudio(scene, entity, player, entry, frameExact);
        updateOverride(scene, entity, player, entry);
    }

    // the events after every player moved: a handler may stop, destroy or add players
    std::vector<std::function<void()>> events;
    events.swap(state->pendingEvents);
    for (std::function<void()>& event : events)
        event();
}
