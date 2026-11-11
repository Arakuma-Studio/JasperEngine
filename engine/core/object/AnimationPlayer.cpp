// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationPlayer.h"

#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorInstance.h"
#include "animation/CompiledController.h"
#include "component/AnimatorComponent.h"
#include "object/Animator.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/AnimatorSystem.h"
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

void AnimationState::setMirror(bool mirror){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)) state->mirrorOverride = mirror ? 1 : 0;
}

bool AnimationState::getMirror() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state && anim::stateMirror(*state);
}

void AnimationState::setFootIK(bool footIK){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)) state->footIKOverride = footIK ? 1 : 0;
}

bool AnimationState::getFootIK() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state && anim::stateFootIK(*state);
}

void AnimationState::resetMirror(){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)) state->mirrorOverride = -1;
}

void AnimationState::resetFootIK(){
    if (anim::PlayerStateRuntime* state = findState(scene, entity, name)) state->footIKOverride = -1;
}

bool AnimationState::isHumanoid() const{
    const anim::PlayerStateRuntime* state = findState(scene, entity, name);
    return state && anim::stateIsHumanoid(*state);
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

namespace {

    // Upstream's Animation Player with a Controller (docs/specs/S21-animation.md "Upstream
    // absorption"): the controller plays on the GameObject's Animator. A player that is not
    // enabled on a GameObject with an Animator hands its playback calls to it (what the scene
    // migration makes of upstream's controller players); the controller calls always go to it.
    AnimatorComponent* animatorOf(Scene* scene, Entity entity){
        return scene && scene->isEntityCreated(entity) ? scene->findComponent<AnimatorComponent>(entity) : nullptr;
    }

    bool handsOver(Scene* scene, Entity entity, const AnimationPlayerComponent& player){
        return !player.enabled && animatorOf(scene, entity) != nullptr;
    }

    void warnOnce(AnimationPlayerComponent& player, const std::string& key, const std::string& message){
        anim::AnimationPlayerRuntime& runtime = anim::playerRuntime(player);
        if (std::find(runtime.warnedLayers.begin(), runtime.warnedLayers.end(), key) == runtime.warnedLayers.end()){
            runtime.warnedLayers.push_back(key);
            Log::warn("%s", message.c_str());
        }
    }

    // the Animator a controller call goes to; null (warned once per call) without one
    AnimatorComponent* controllerAnimator(Scene* scene, Entity entity, AnimationPlayerComponent& player, const char* call, const std::string& who){
        AnimatorComponent* animator = animatorOf(scene, entity);
        if (!animator){
            warnOnce(player, std::string("animator:") + call, "Animation Player '" + who + "': " + call +
                     " needs an Animator Controller: add an Animator with a controller (or setController)");
        }
        return animator;
    }

    // a handed-over play: the Animator plays again after a stop()
    void resumeAnimator(Scene* scene, Entity entity){
        if (AnimatorComponent* animator = animatorOf(scene, entity)) animator->enabled = true;
    }

    // blend and queue have no controller form: said once per player and call
    bool refusedByController(Scene* scene, Entity entity, AnimationPlayerComponent& player, const char* call, const std::string& who){
        if (!handsOver(scene, entity, player)) return false;
        warnOnce(player, std::string("controller:") + call, "Animation Player '" + who + "': " + call +
                 " is not available with an Animator Controller (use parameters, play or crossFade)");
        return true;
    }

    // a state name of the Animator's controller ("Layer.State" or "State") plays (current or next)
    bool animatorStatePlaying(Scene* scene, Entity entity, const std::string* name){
        AnimatorComponent* animator = animatorOf(scene, entity);
        if (!animator || !animator->enabled) return false;
        anim::AnimatorInstance* instance = scene->getSystem<AnimatorSystem>()->instance(entity);
        if (!instance || !instance->controller) return false;
        if (!name) return true;
        for (size_t l = 0; l < instance->layers.size(); l++){
            for (const anim::AnimatorInstance::StateInfo& info : {instance->currentState((int)l), instance->nextState((int)l)}){
                if (info.state >= 0 && (info.name == *name || info.fullPath == *name)) return true;
            }
        }
        return false;
    }

}

void AnimationPlayer::play(const std::string& name){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)){
        resumeAnimator(scene, entity);
        Animator(scene, entity).play(name);
        return;
    }
    anim::playState(player, name, 0.0f, getName());
}

void AnimationPlayer::play(const std::string& name, float fade){
    play(name, fade, PlayMode::StopSameLayer);
}

void AnimationPlayer::play(const std::string& name, float fade, PlayMode mode){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)){
        // a controller has no play modes: a fade crossfades (in seconds), else an instant switch
        resumeAnimator(scene, entity);
        if (fade > 0.0f) Animator(scene, entity).crossFadeInFixedTime(name, fade);
        else Animator(scene, entity).play(name);
        return;
    }
    anim::playState(player, name, fade, getName(), mode == PlayMode::StopAll ? PlayMode::StopAll : PlayMode::StopSameLayer);
}

void AnimationPlayer::playInLayer(const std::string& name, int layer){
    playInLayer(name, layer, 0.0f);
}

void AnimationPlayer::playInLayer(const std::string& name, int layer, float normalizedTime){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)){
        resumeAnimator(scene, entity);
        Animator(scene, entity).play(name, layer, normalizedTime);
        return;
    }
    // without a controller the layer is the state's own: a plain play from normalizedTime
    if (anim::playState(player, name, 0.0f, getName())){
        getState(name).setNormalizedTime(normalizedTime);
    }
}

void AnimationPlayer::crossFade(const std::string& name, float fade, PlayMode mode){
    play(name, fade > 0.0f ? fade : 0.0f, mode);
}

void AnimationPlayer::crossFadeInLayer(const std::string& name, float seconds, int layer){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)){
        resumeAnimator(scene, entity);
        Animator(scene, entity).crossFadeInFixedTime(name, seconds, layer);
        return;
    }
    anim::playState(player, name, seconds, getName());
}

void AnimationPlayer::crossFade(const std::string& name){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    crossFade(name, player.defaultFadeTime, PlayMode::StopSameLayer);
}

void AnimationPlayer::crossFade(const std::string& name, float fade){
    crossFade(name, fade, PlayMode::StopSameLayer);
}

void AnimationPlayer::blend(const std::string& name, float weight){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (refusedByController(scene, entity, player, "blend", getName())) return;
    anim::blendState(player, name, weight, player.defaultFadeTime, getName());
}

void AnimationPlayer::blend(const std::string& name, float weight, float fade){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (refusedByController(scene, entity, player, "blend", getName())) return;
    anim::blendState(player, name, weight, fade, getName());
}

void AnimationPlayer::queue(const std::string& name){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (refusedByController(scene, entity, player, "queue", getName())) return;
    anim::queueState(player, name, 0.0f, getName());
}

void AnimationPlayer::queue(const std::string& name, float fade){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (refusedByController(scene, entity, player, "queue", getName())) return;
    anim::queueState(player, name, fade, getName());
}

void AnimationPlayer::stop(){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)){
        // upstream's controller stop(): nothing plays until a play() (or an enable)
        if (AnimatorComponent* animator = animatorOf(scene, entity)) animator->enabled = false;
        return;
    }
    anim::stopStates(player, nullptr, getName());
}

void AnimationPlayer::stop(const std::string& name){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)){
        if (AnimatorComponent* animator = animatorOf(scene, entity)) animator->enabled = false;
        return;
    }
    anim::stopStates(player, &name, getName());
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
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)) return animatorStatePlaying(scene, entity, nullptr);
    return anim::isStatePlaying(player, nullptr);
}

bool AnimationPlayer::isPlaying(const std::string& name) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    // a controller state ("<Layer>.<State>" or "<State>") plays while it is current or next
    if (handsOver(scene, entity, player)) return animatorStatePlaying(scene, entity, &name);
    return anim::isStatePlaying(player, &name);
}

AnimationState AnimationPlayer::getState(const std::string& name) const{
    return AnimationState(scene, entity, name);
}

std::vector<std::string> AnimationPlayer::getAnimatedProperties(const std::string& name) const{
    std::vector<std::string> out;
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    anim::AnimationPlayerRuntime& runtime = anim::playerRuntime(player);
    const int index = runtime.findState(name);
    if (index < 0) return out;
    const anim::ClipBindings& bindings = runtime.states[(size_t)index].bindings;
    if (!bindings.clip) return out;
    const std::vector<AnimationClip::Track>& tracks = bindings.clip->getTracks();
    for (size_t i = 0; i < tracks.size() && i < bindings.targets.size(); i++){
        if (tracks[i].channel != ClipChannel::Property || bindings.targets[i] == NULL_ENTITY) continue;
        out.push_back(tracks[i].path + ":" + tracks[i].component + "." + tracks[i].property);
    }
    return out;
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
            if (track.isSampled() && !isHumanoidChannel(track.channel)) info.trackCount++;
        }
    }
    info.humanoid = anim::stateIsHumanoid(state);
    info.retarget = state.retarget;
    info.mirror = anim::stateMirror(state);
    info.footIK = anim::stateFootIK(state);
    info.humanoidBones = state.bindings.humanoid ? state.bindings.humanoid->outputs.size() : 0;
    return info;
}

// authored fields (complete): a clip list change sets needsLoad

bool AnimationPlayer::addClip(const std::string& path){
    return addClip(path, "");
}

bool AnimationPlayer::addClip(const std::string& path, const std::string& name){
    return addClip(path, name, AnimationClipOptions());
}

bool AnimationPlayer::addClip(const std::string& path, const std::string& name, const AnimationClipOptions& options){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    AnimationPlayerClip entry;
    entry.clip = path;
    entry.name = name;
    entry.retarget = options.retarget;
    entry.mirror = options.mirror;
    entry.footIK = options.footIK;
    entry.layer = options.layer;
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

void AnimationPlayer::setPoseFrameRate(float fps){
    getComponent<AnimationPlayerComponent>().poseFrameRate = std::isfinite(fps) ? std::max(0.0f, fps) : 0.0f;
}

float AnimationPlayer::getPoseFrameRate() const{
    return getComponent<AnimationPlayerComponent>().poseFrameRate;
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
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)){
        // the Animator's checkbox; enabled again it starts from its default states (Unity's
        // Animator, upstream's controller player)
        AnimatorComponent* animator = animatorOf(scene, entity);
        if (enabled && !animator->enabled){
            if (anim::AnimatorInstance* instance = scene->getSystem<AnimatorSystem>()->instance(entity)) instance->restart();
        }
        animator->enabled = enabled;
        return;
    }
    player.enabled = enabled;
}

bool AnimationPlayer::isEnabled() const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)) return animatorOf(scene, entity)->enabled;
    return player.enabled;
}

bool AnimationPlayer::isLoaded() const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)) return Animator(scene, entity).isLoaded();
    if (!player.runtime || player.needsLoad) return false;
    for (const anim::PlayerStateRuntime& state : player.runtime->states){
        if (!state.bindings.clip || !state.bindings.resolved) return false;
    }
    return true;
}

// layers

namespace {

    anim::AnimationPlayerRuntime& layeredRuntime(AnimationPlayerComponent& player, const std::string& who){
        (void)who;
        return anim::playerRuntime(player);
    }

}

int AnimationPlayer::getLayerCount() const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)) return Animator(scene, entity).getLayerCount();
    return (int)layeredRuntime(player, getName()).layers.size();
}

std::string AnimationPlayer::getLayerName(int index) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)) return Animator(scene, entity).getLayerName(index);
    const anim::AnimationPlayerRuntime& runtime = layeredRuntime(player, getName());
    return index >= 0 && (size_t)index < runtime.layers.size() ? runtime.layers[(size_t)index].name : std::string();
}

int AnimationPlayer::getLayerIndex(const std::string& name) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)) return Animator(scene, entity).getLayerIndex(name);
    return layeredRuntime(player, getName()).findLayer(name);
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
    if (handsOver(scene, entity, player)){
        Animator(scene, entity).setLayerWeight(layer, weight, fade);
        return;
    }
    const int index = anim::layerIndex(player, layer, getName());
    if (index >= 0) anim::setLayerWeight(player, index, weight, fade, getName());
}

void AnimationPlayer::setLayerWeight(const std::string& layer, float weight){
    setLayerWeight(layer, weight, 0.0f);
}

void AnimationPlayer::setLayerWeight(const std::string& layer, float weight, float fade){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)){
        Animator animator(scene, entity);
        const int index = animator.getLayerIndex(layer);
        if (index >= 0) animator.setLayerWeight(index, weight, fade);
        else warnOnce(player, "animator:layer:" + layer, "Animation Player '" + getName() + "': the Animator has no layer named '" + layer + "'");
        return;
    }
    const int index = anim::layerIndex(player, layer, getName());
    if (index >= 0) anim::setLayerWeight(player, index, weight, fade, getName());
}

float AnimationPlayer::getLayerWeight(int layer) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)) return Animator(scene, entity).getLayerWeight(layer);
    const int index = anim::layerIndex(player, layer, getName());
    return index >= 0 ? player.runtime->layerWeight(index) : 0.0f;
}

float AnimationPlayer::getLayerWeight(const std::string& layer) const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    if (handsOver(scene, entity, player)){
        Animator animator(scene, entity);
        return animator.getLayerWeight(animator.getLayerIndex(layer));
    }
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

// Animator Controller (upstream's player API, on the GameObject's Animator)

void AnimationPlayer::setController(const std::string& controller){
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    AnimatorComponent* animator = animatorOf(scene, entity);
    if (controller.empty()){
        // the clip list plays again
        if (animator && handsOver(scene, entity, player)){
            player.enabled = animator->enabled;
            animator->controller = std::string();
            animator->needsLoad = true;
        }
        return;
    }
    // the Animator plays the controller (added when missing); the player hands over to it
    const bool wasEnabled = handsOver(scene, entity, player) ? animator->enabled : player.enabled;
    Animator(scene, entity).setController(controller);
    animatorOf(scene, entity)->enabled = wasEnabled;
    player.enabled = false;
}

std::string AnimationPlayer::getController() const{
    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>();
    return handsOver(scene, entity, player) ? animatorOf(scene, entity)->controller.path : std::string();
}

bool AnimationPlayer::hasController() const{
    return !getController().empty();
}

void AnimationPlayer::setFloat(const std::string& name, float value){
    if (controllerAnimator(scene, entity, getComponent<AnimationPlayerComponent>(), "setFloat", getName())) Animator(scene, entity).setFloat(name, value);
}

void AnimationPlayer::setFloat(const std::string& name, float value, float dampTime){
    if (!controllerAnimator(scene, entity, getComponent<AnimationPlayerComponent>(), "setFloat", getName())) return;
    // upstream's damping: toward the value once per update until another set
    anim::AnimatorInstance* instance = scene->getSystem<AnimatorSystem>()->instance(entity);
    if (!instance || !instance->setFloatTarget(name, value, dampTime)) Animator(scene, entity).setFloat(name, value);
}

float AnimationPlayer::getFloat(const std::string& name) const{
    return controllerAnimator(scene, entity, getComponent<AnimationPlayerComponent>(), "getFloat", getName()) ? Animator(scene, entity).getFloat(name) : 0.0f;
}

void AnimationPlayer::setInteger(const std::string& name, int value){
    if (controllerAnimator(scene, entity, getComponent<AnimationPlayerComponent>(), "setInteger", getName())) Animator(scene, entity).setInteger(name, value);
}

int AnimationPlayer::getInteger(const std::string& name) const{
    return controllerAnimator(scene, entity, getComponent<AnimationPlayerComponent>(), "getInteger", getName()) ? Animator(scene, entity).getInteger(name) : 0;
}

void AnimationPlayer::setBool(const std::string& name, bool value){
    if (controllerAnimator(scene, entity, getComponent<AnimationPlayerComponent>(), "setBool", getName())) Animator(scene, entity).setBool(name, value);
}

bool AnimationPlayer::getBool(const std::string& name) const{
    return controllerAnimator(scene, entity, getComponent<AnimationPlayerComponent>(), "getBool", getName()) && Animator(scene, entity).getBool(name);
}

void AnimationPlayer::setTrigger(const std::string& name){
    if (controllerAnimator(scene, entity, getComponent<AnimationPlayerComponent>(), "setTrigger", getName())) Animator(scene, entity).setTrigger(name);
}

void AnimationPlayer::resetTrigger(const std::string& name){
    if (controllerAnimator(scene, entity, getComponent<AnimationPlayerComponent>(), "resetTrigger", getName())) Animator(scene, entity).resetTrigger(name);
}

namespace {

    const anim::CompiledController* controllerOf(Scene* scene, Entity entity){
        if (!animatorOf(scene, entity)) return nullptr;
        anim::AnimatorInstance* instance = scene->getSystem<AnimatorSystem>()->instance(entity);
        return instance && instance->controller ? instance->controller.get() : nullptr;
    }

}

bool AnimationPlayer::hasParameter(const std::string& name) const{
    const anim::CompiledController* controller = controllerOf(scene, entity);
    return controller && controller->findParameter(name) >= 0;
}

std::string AnimationPlayer::getParameterType(const std::string& name) const{
    const anim::CompiledController* controller = controllerOf(scene, entity);
    const int index = controller ? controller->findParameter(name) : -1;
    return index >= 0 ? anim::parameterTypeName(controller->parameters[(size_t)index].type) : "";
}

std::string AnimationPlayer::getCurrentStateName() const{
    return getCurrentStateName(0);
}

std::string AnimationPlayer::getCurrentStateName(int layer) const{
    return getCurrentStateInfo(layer).name;
}

namespace {

    ControllerStateInfo stateInfoOf(Scene* scene, Entity entity, int layer, bool next){
        ControllerStateInfo info;
        if (!animatorOf(scene, entity)) return info;
        const AnimatorStateInfo state = next ? Animator(scene, entity).getNextState(layer) : Animator(scene, entity).getCurrentState(layer);
        info.isValid = state.valid;
        info.name = state.name;
        info.fullName = state.fullPath;
        info.normalizedTime = state.normalizedTime;
        info.length = state.length;
        info.speed = state.speed;
        info.loop = state.loop;
        return info;
    }

}

ControllerStateInfo AnimationPlayer::getCurrentStateInfo() const{
    return getCurrentStateInfo(0);
}

ControllerStateInfo AnimationPlayer::getCurrentStateInfo(int layer) const{
    return stateInfoOf(scene, entity, layer, false);
}

ControllerStateInfo AnimationPlayer::getNextStateInfo() const{
    return getNextStateInfo(0);
}

ControllerStateInfo AnimationPlayer::getNextStateInfo(int layer) const{
    return stateInfoOf(scene, entity, layer, true);
}

bool AnimationPlayer::isInTransition() const{
    return isInTransition(0);
}

bool AnimationPlayer::isInTransition(int layer) const{
    return animatorOf(scene, entity) && Animator(scene, entity).isInTransition(layer);
}

FunctionSubscribe<void(int, const std::string&)>& AnimationPlayer::getOnStateEnter(){
    return getComponent<AnimationPlayerComponent>().onStateEnter;
}

FunctionSubscribe<void(int, const std::string&)>& AnimationPlayer::getOnStateExit(){
    return getComponent<AnimationPlayerComponent>().onStateExit;
}

FunctionSubscribe<void(const AnimationEvent&)>& AnimationPlayer::getOnEvent(){
    return getComponent<AnimationPlayerComponent>().onEvent;
}

FunctionSubscribe<void(const std::string&)>& AnimationPlayer::getOnFinish(){
    return getComponent<AnimationPlayerComponent>().onFinish;
}
