// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationPlayer.h"

#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "subsystem/ActionSystem.h"
#include "Scene.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

// AnimationState: a (GameObject, state name) handle, looked up on every access

namespace {

    // the state of the handle, null when the GameObject, its player or the name is gone
    anim::PlayerStateRuntime* findState(Scene* scene, Entity entity, const std::string& name){
        if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return nullptr;
        AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
        if (!player) return nullptr;
        anim::AnimationPlayerRuntime& runtime = anim::playerRuntime(*player);
        int index = runtime.findState(name);
        return index >= 0 ? &runtime.states[(size_t)index] : nullptr;
    }

}

AnimationState::AnimationState(){
}

AnimationState::AnimationState(Scene* scene, Entity entity, const std::string& name):
        scene(scene), entity(entity), name(name){
}

bool AnimationState::isValid() const{
    return findState(scene, entity, name) != nullptr;
}

std::string AnimationState::getName() const{
    return name;
}

std::string AnimationState::getClip() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state ? state->clip : std::string();
}

void AnimationState::setTime(float time){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)){
        anim::setStateTime(*state, time);
    }
}

float AnimationState::getTime() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state ? anim::stateClipTime(*state) : 0.0f;
}

void AnimationState::setNormalizedTime(float normalizedTime){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)){
        anim::setStateTime(*state, normalizedTime * anim::stateLength(*state));
    }
}

float AnimationState::getNormalizedTime() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    if (!state) return 0.0f;
    const float length = anim::stateLength(*state);
    return length > 0.0f ? anim::stateClipTime(*state) / length : 0.0f;
}

void AnimationState::setSpeed(float speed){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)){
        state->speed = std::isfinite(speed) ? speed : 0.0f;
    }
}

float AnimationState::getSpeed() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state ? state->speed : 1.0f;
}

void AnimationState::setWeight(float weight){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)){
        weight = std::isfinite(weight) ? std::max(0.0f, weight) : 0.0f;
        state->weight = weight;
        state->targetWeight = weight;
        state->fadeSpeed = 0.0f;
        state->stopOnFadeOut = false;
    }
}

float AnimationState::getWeight() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state ? state->weight : 0.0f;
}

float AnimationState::getLength() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state ? anim::stateLength(*state) : 0.0f;
}

void AnimationState::setWrap(const std::string& wrap){
    anim::PlayerStateRuntime* state = findState(scene, entity, name);
    if (!state) return;
    ClipWrapMode mode;
    if (!anim::wrapFromName(wrap, mode)){
        Log::warn("AnimationState '%s': unknown wrap '%s' (once, loop, ping_pong, clamp_forever or default)", name.c_str(), wrap.c_str());
        return;
    }
    // keeps the time in the clip across the change of stored form
    const float time = anim::stateClipTime(*state);
    state->wrapOverride = mode;
    anim::setStateTime(*state, time);
}

std::string AnimationState::getWrap() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return anim::wrapName(state ? anim::stateWrap(*state) : ClipWrapMode::Once);
}

void AnimationState::setEnabled(bool enabled){
    anim::PlayerStateRuntime* state = findState(scene, entity, name);
    if (!state) return;
    // Unity: a disabled state keeps its time (paused) until enabled again
    state->enabled = enabled;
    if (enabled){
        state->playing = true;
        state->held = false;
    }
}

bool AnimationState::isEnabled() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state && state->enabled;
}

void AnimationState::setLayer(int layer){
    anim::PlayerStateRuntime* state = findState(scene, entity, name);
    if (!state) return;
    AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(entity);
    const int index = anim::layerIndex(player, layer, scene->getEntityName(entity));
    if (index >= 0) anim::setStateLayer(player, *state, index);
}

int AnimationState::getLayer() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    if (!state) return 0;
    return scene->getComponent<AnimationPlayerComponent>(entity).runtime->stateLayer(*state);
}

void AnimationState::addMixingTransform(const std::string& path){
    addMixingTransform(path, true);
}

void AnimationState::addMixingTransform(const std::string& path, bool recursive){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)){
        anim::addMixingTransform(*state, path, recursive);
    }
}

bool AnimationState::removeMixingTransform(const std::string& path){
    anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state && anim::removeMixingTransform(*state, path);
}

void AnimationState::clearMixingTransforms(){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)){
        anim::clearMixingTransforms(*state);
    }
}

// AnimationPlayer

AnimationPlayer::AnimationPlayer(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<AnimationPlayerComponent>(entity)){
        scene->addComponent<AnimationPlayerComponent>(entity, {});
    }
}

AnimationPlayer::~AnimationPlayer(){
}

AnimationPlayer::AnimationPlayer(const AnimationPlayer& rhs) = default;
AnimationPlayer& AnimationPlayer::operator=(const AnimationPlayer& rhs) = default;
AnimationPlayer::AnimationPlayer(AnimationPlayer&& rhs) noexcept = default;
AnimationPlayer& AnimationPlayer::operator=(AnimationPlayer&& rhs) noexcept = default;

// playback (anim:: playback functions; the system update loads, binds and samples)

void AnimationPlayer::play(const std::string& name){
    anim::playState(getComponent<AnimationPlayerComponent>(), name, 0.0f, getName());
}

void AnimationPlayer::play(const std::string& name, float fade){
    anim::playState(getComponent<AnimationPlayerComponent>(), name, fade, getName());
}

void AnimationPlayer::play(const std::string& name, float fade, PlayMode mode){
    anim::playState(getComponent<AnimationPlayerComponent>(), name, fade, getName(), mode == PlayMode::StopAll ? PlayMode::StopAll : PlayMode::StopSameLayer);
}

void AnimationPlayer::crossFade(const std::string& name, float fade, PlayMode mode){
    play(name, fade, mode);
}

void AnimationPlayer::crossFade(const std::string& name){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    anim::playState(player, name, player.defaultFadeTime, getName());
}

void AnimationPlayer::crossFade(const std::string& name, float fade){
    anim::playState(getComponent<AnimationPlayerComponent>(), name, fade, getName());
}

void AnimationPlayer::blend(const std::string& name, float weight){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    anim::blendState(player, name, weight, player.defaultFadeTime, getName());
}

void AnimationPlayer::blend(const std::string& name, float weight, float fade){
    anim::blendState(getComponent<AnimationPlayerComponent>(), name, weight, fade, getName());
}

void AnimationPlayer::queue(const std::string& name){
    anim::queueState(getComponent<AnimationPlayerComponent>(), name, 0.0f, getName());
}

void AnimationPlayer::queue(const std::string& name, float fade){
    anim::queueState(getComponent<AnimationPlayerComponent>(), name, fade, getName());
}

void AnimationPlayer::stop(){
    anim::stopStates(getComponent<AnimationPlayerComponent>(), nullptr, getName());
}

void AnimationPlayer::stop(const std::string& name){
    anim::stopStates(getComponent<AnimationPlayerComponent>(), &name, getName());
}

void AnimationPlayer::rewind(){
    anim::rewindStates(getComponent<AnimationPlayerComponent>(), nullptr, getName());
}

void AnimationPlayer::rewind(const std::string& name){
    anim::rewindStates(getComponent<AnimationPlayerComponent>(), &name, getName());
}

void AnimationPlayer::sample(){
    getComponent<AnimationPlayerComponent>();
    scene->getSystem<ActionSystem>()->samplePlayer(entity);
}

bool AnimationPlayer::isPlaying() const{
    return anim::isStatePlaying(getComponent<AnimationPlayerComponent>(), nullptr);
}

bool AnimationPlayer::isPlaying(const std::string& name) const{
    return anim::isStatePlaying(getComponent<AnimationPlayerComponent>(), &name);
}

AnimationState AnimationPlayer::getState(const std::string& name) const{
    return AnimationState(scene, entity, name);
}

AnimationStateInfo AnimationPlayer::getStateInfo(const std::string& name) const{
    AnimationStateInfo info;
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    anim::AnimationPlayerRuntime& runtime = anim::playerRuntime(player);
    int index = runtime.findState(name);
    if (index < 0) return info;
    const anim::PlayerStateRuntime& state = runtime.states[(size_t)index];
    info.name = state.name;
    info.clip = state.clip;
    info.loaded = state.bindings.clip != nullptr;
    info.error = state.error;
    info.time = anim::stateClipTime(state);
    info.length = anim::stateLength(state);
    info.normalizedTime = info.length > 0.0f ? info.time / info.length : 0.0f;
    info.speed = state.speed;
    info.weight = state.weight;
    info.targetWeight = state.targetWeight;
    info.wrap = anim::stateWrap(state);
    info.enabled = state.enabled;
    info.playing = state.playing;
    for (const anim::LayerRuntime& layer : runtime.layers){
        for (const anim::QueuedState& queued : layer.queue){
            if (queued.state == (size_t)index) info.queued = true;
        }
    }
    info.boundTracks = state.bindings.resolvedCount;
    if (state.bindings.clip){
        for (const AnimationClip::Track& track : state.bindings.clip->getTracks()){
            if (track.isSampled()) info.trackCount++;
        }
    }
    return info;
}

// authored fields (complete): a clip list change sets needsLoad

bool AnimationPlayer::addClip(const std::string& path){
    return addClip(path, "");
}

bool AnimationPlayer::addClip(const std::string& path, const std::string& name){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    AnimationPlayerClip entry;
    entry.clip = path;
    entry.name = name;
    std::vector<std::string> names = anim::playerStateNames(player.clips);
    if (std::find(names.begin(), names.end(), anim::defaultStateName(entry)) != names.end()) return false;
    player.clips.push_back(entry);
    player.needsLoad = true;
    return true;
}

bool AnimationPlayer::removeClip(const std::string& name){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    std::vector<std::string> names = anim::playerStateNames(player.clips);
    for (size_t i = 0; i < names.size(); i++){
        if (names[i] == name){
            player.clips.erase(player.clips.begin() + (long)i);
            player.needsLoad = true;
            return true;
        }
    }
    return false;
}

int AnimationPlayer::getClipCount() const{
    return (int)getComponent<AnimationPlayerComponent>().clips.size();
}

std::string AnimationPlayer::getClipName(int index) const{
    std::vector<std::string> names = anim::playerStateNames(getComponent<AnimationPlayerComponent>().clips);
    return index >= 0 && (size_t)index < names.size() ? names[(size_t)index] : std::string();
}

void AnimationPlayer::setSpeed(float speed){
    getComponent<AnimationPlayerComponent>().speed = speed;
}

float AnimationPlayer::getSpeed() const{
    return getComponent<AnimationPlayerComponent>().speed;
}

void AnimationPlayer::setDefaultClip(const std::string& name){
    getComponent<AnimationPlayerComponent>().defaultClip = name;
}

std::string AnimationPlayer::getDefaultClip() const{
    return getComponent<AnimationPlayerComponent>().defaultClip;
}

void AnimationPlayer::setDefaultFadeTime(float fade){
    getComponent<AnimationPlayerComponent>().defaultFadeTime = fade;
}

float AnimationPlayer::getDefaultFadeTime() const{
    return getComponent<AnimationPlayerComponent>().defaultFadeTime;
}

void AnimationPlayer::setPlayAutomatically(bool playAutomatically){
    getComponent<AnimationPlayerComponent>().playAutomatically = playAutomatically;
}

bool AnimationPlayer::isPlayAutomatically() const{
    return getComponent<AnimationPlayerComponent>().playAutomatically;
}

void AnimationPlayer::setEnabled(bool enabled){
    getComponent<AnimationPlayerComponent>().enabled = enabled;
}

bool AnimationPlayer::isEnabled() const{
    return getComponent<AnimationPlayerComponent>().enabled;
}

bool AnimationPlayer::isLoaded() const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (!player.runtime || player.needsLoad) return false;
    for (const anim::PlayerStateRuntime& state : player.runtime->states){
        if (!state.bindings.clip || !state.bindings.resolved) return false;
    }
    return true;
}

// layers

int AnimationPlayer::getLayerCount() const{
    return (int)anim::playerRuntime(getComponent<AnimationPlayerComponent>()).layers.size();
}

std::string AnimationPlayer::getLayerName(int index) const{
    const anim::AnimationPlayerRuntime& runtime = anim::playerRuntime(getComponent<AnimationPlayerComponent>());
    return index >= 0 && (size_t)index < runtime.layers.size() ? runtime.layers[(size_t)index].name : std::string();
}

int AnimationPlayer::getLayerIndex(const std::string& name) const{
    return anim::playerRuntime(getComponent<AnimationPlayerComponent>()).findLayer(name);
}

int AnimationPlayer::addLayer(const std::string& name){
    return addLayer(name, 1.0f, "override", "");
}

int AnimationPlayer::addLayer(const std::string& name, float weight){
    return addLayer(name, weight, "override", "");
}

int AnimationPlayer::addLayer(const std::string& name, float weight, const std::string& blend){
    return addLayer(name, weight, blend, "");
}

int AnimationPlayer::addLayer(const std::string& name, float weight, const std::string& blend, const std::string& mask){
    AnimationLayerBlend mode = AnimationLayerBlend::Override;
    if (!anim::layerBlendFromName(blend, mode)){
        Log::warn("Animation Player '%s': unknown blend '%s' (override or additive): using override", getName().c_str(), blend.c_str());
    }
    return anim::addLayer(getComponent<AnimationPlayerComponent>(), name, weight, mode, mask);
}

bool AnimationPlayer::removeLayer(int layer){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (layer == 0){
        Log::warn("Animation Player '%s': the base layer cannot be removed", getName().c_str());
        return false;
    }
    return anim::removeLayer(player, layer, getName());
}

bool AnimationPlayer::removeLayer(const std::string& layer){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    return index >= 0 && removeLayer(index);
}

void AnimationPlayer::setLayerWeight(int layer, float weight){
    setLayerWeight(layer, weight, 0.0f);
}

void AnimationPlayer::setLayerWeight(int layer, float weight, float fade){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    if (index >= 0) anim::setLayerWeight(player, index, weight, fade, getName());
}

void AnimationPlayer::setLayerWeight(const std::string& layer, float weight){
    setLayerWeight(layer, weight, 0.0f);
}

void AnimationPlayer::setLayerWeight(const std::string& layer, float weight, float fade){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    if (index >= 0) anim::setLayerWeight(player, index, weight, fade, getName());
}

float AnimationPlayer::getLayerWeight(int layer) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    return index >= 0 ? player.runtime->layerWeight(index) : 0.0f;
}

float AnimationPlayer::getLayerWeight(const std::string& layer) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    return index >= 0 ? player.runtime->layerWeight(index) : 0.0f;
}

void AnimationPlayer::setLayerMask(int layer, const std::string& mask){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    if (index >= 0) anim::setLayerMask(player, index, mask, getName());
}

void AnimationPlayer::setLayerMask(const std::string& layer, const std::string& mask){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    if (index >= 0) anim::setLayerMask(player, index, mask, getName());
}

std::string AnimationPlayer::getLayerMask(int layer) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    return index >= 0 ? player.runtime->layers[(size_t)index].maskPath : std::string();
}

std::string AnimationPlayer::getLayerMask(const std::string& layer) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    return index >= 0 ? player.runtime->layers[(size_t)index].maskPath : std::string();
}

void AnimationPlayer::setLayerBlendMode(int layer, const std::string& mode){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    AnimationLayerBlend blend;
    if (index < 0) return;
    if (!anim::layerBlendFromName(mode, blend)){
        Log::warn("Animation Player '%s': unknown blend mode '%s' (override or additive)", getName().c_str(), mode.c_str());
        return;
    }
    anim::setLayerBlend(player, index, blend, getName());
}

void AnimationPlayer::setLayerBlendMode(const std::string& layer, const std::string& mode){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    if (index >= 0) setLayerBlendMode(index, mode);
}

std::string AnimationPlayer::getLayerBlendMode(int layer) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    return index >= 0 ? anim::layerBlendName(player.runtime->layers[(size_t)index].blend) : "";
}

std::string AnimationPlayer::getLayerBlendMode(const std::string& layer) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    return index >= 0 ? anim::layerBlendName(player.runtime->layers[(size_t)index].blend) : "";
}

void AnimationPlayer::stopLayer(int layer){
    stopLayer(layer, 0.0f);
}

void AnimationPlayer::stopLayer(int layer, float fade){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    if (index >= 0) anim::stopLayer(player, index, fade);
}

void AnimationPlayer::stopLayer(const std::string& layer){
    stopLayer(layer, 0.0f);
}

void AnimationPlayer::stopLayer(const std::string& layer, float fade){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    const int index = anim::layerIndex(player, layer, getName());
    if (index >= 0) anim::stopLayer(player, index, fade);
}

FunctionSubscribe<void(const AnimationEvent&)>& AnimationPlayer::getOnEvent(){
    return getComponent<AnimationPlayerComponent>().onEvent;
}

FunctionSubscribe<void(const std::string&)>& AnimationPlayer::getOnFinish(){
    return getComponent<AnimationPlayerComponent>().onFinish;
}
