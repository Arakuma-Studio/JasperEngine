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
    for (const anim::QueuedState& queued : runtime.queue){
        if (queued.state == (size_t)index) info.queued = true;
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

FunctionSubscribe<void(const AnimationEvent&)>& AnimationPlayer::getOnEvent(){
    return getComponent<AnimationPlayerComponent>().onEvent;
}

FunctionSubscribe<void(const std::string&)>& AnimationPlayer::getOnFinish(){
    return getComponent<AnimationPlayerComponent>().onFinish;
}
