// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationPlayerRuntime.h"

#include "animation/AnimatorController.h"
#include "animation/ModelSubClips.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

std::string anim::defaultStateName(const AnimationPlayerClip& entry){
    if (!entry.name.empty()) return entry.name;
    SubClipReference reference;
    if (parseSubClipReference(entry.clip, reference)) return reference.clip;
    size_t slash = entry.clip.find_last_of("/\\");
    std::string file = slash == std::string::npos ? entry.clip : entry.clip.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
}

std::vector<std::string> anim::playerStateNames(const std::vector<AnimationPlayerClip>& clips){
    std::vector<std::string> names;
    names.reserve(clips.size());
    for (const AnimationPlayerClip& entry : clips){
        std::string base = defaultStateName(entry);
        std::string name = base;
        for (int k = 2; std::find(names.begin(), names.end(), name) != names.end(); k++){
            name = base + " " + std::to_string(k);
        }
        names.push_back(name);
    }
    return names;
}

int anim::AnimationPlayerRuntime::findState(const std::string& name) const{
    for (size_t i = 0; i < states.size(); i++){
        if (states[i].name == name) return (int)i;
    }
    return -1;
}

int anim::AnimationPlayerRuntime::findLayer(const std::string& name) const{
    for (size_t i = 0; i < layers.size(); i++){
        if (layers[i].name == name) return (int)i;
    }
    return -1;
}

int anim::AnimationPlayerRuntime::stateLayer(const PlayerStateRuntime& state) const{
    const int layer = state.layerOverride >= 0 ? state.layerOverride : state.layer;
    return (layer >= 0 && (size_t)layer < layers.size()) ? layer : 0;
}

float anim::AnimationPlayerRuntime::layerWeight(int layer) const{
    if (layer <= 0 || (size_t)layer >= layers.size()) return 1.0f;
    return layers[(size_t)layer].weight;
}

void anim::ClipPreviewSession::setRest(Entity entity, const ChannelRest& value){
    rest[entity] = value;
}

void anim::ClipPreviewSession::reset(){
    root = NULL_ENTITY;
    rest.clear();
    bindings.clear();
    written.clear();
    writtenProperties.clear();
    previousTimes.clear();
    scratch = PlayerPoseScratch();
    trackSlots.clear();
    trackMasks.clear();
    references.clear();
    humanoid.reset();
    humanoidRevision = 0;
}

// ---- playback ----

namespace {

    using namespace jasperengine::anim;

    float finiteOr(float value, float fallback){
        return std::isfinite(value) ? value : fallback;
    }

    PlayerStateRuntime* findNamed(AnimationPlayerComponent& player, const std::string& name, const std::string& who){
        AnimationPlayerRuntime& runtime = playerRuntime(player);
        int index = runtime.findState(name);
        if (index >= 0) return &runtime.states[(size_t)index];
        if (std::find(runtime.warnedStates.begin(), runtime.warnedStates.end(), name) == runtime.warnedStates.end()){
            runtime.warnedStates.push_back(name);
            Log::warn("Animation Player '%s': no state named '%s'", who.c_str(), name.c_str());
        }
        return nullptr;
    }

    // weight units per second that reach `to` from `from` in `fade` seconds
    float fadeRate(float from, float to, float fade){
        return fade > 0.0f ? std::abs(to - from) / fade : 0.0f;
    }

    // the queue of one layer (and the stops at cycle ends its states were given); -1: every layer
    void clearQueue(AnimationPlayerRuntime& runtime, int layer){
        for (size_t l = 0; l < runtime.layers.size(); l++){
            if (layer < 0 || (int)l == layer) runtime.layers[l].queue.clear();
        }
        for (PlayerStateRuntime& state : runtime.states){
            if (layer < 0 || runtime.stateLayer(state) == layer) state.stopAtCycleEnd = false;
        }
    }

    void warnOnce(AnimationPlayerRuntime& runtime, const std::string& key, const char* format, const std::string& who, const std::string& what){
        if (std::find(runtime.warnedLayers.begin(), runtime.warnedLayers.end(), key) != runtime.warnedLayers.end()) return;
        runtime.warnedLayers.push_back(key);
        Log::warn(format, who.c_str(), what.c_str());
    }

    float clampWeight(float weight){
        return std::isfinite(weight) ? std::min(std::max(weight, 0.0f), 1.0f) : 0.0f;
    }

}

// ---- layers ----

std::vector<AnimationPlayerLayer> anim::playerLayers(const AnimationPlayerComponent& player){
    if (!player.layers.empty()) return player.layers;
    AnimationPlayerLayer base;
    base.name = BASE_LAYER_NAME;
    return {base};
}

bool anim::isDefaultLayering(const AnimationPlayerComponent& player){
    if (!player.controller.empty()) return false;
    if (player.runtime && !player.needsLoad){
        const AnimationPlayerRuntime& runtime = *player.runtime;
        if (runtime.layers.size() > 1) return false;
        if (!runtime.layers.empty() && !runtime.layers[0].maskPath.empty()) return false;
        for (const PlayerStateRuntime& state : runtime.states){
            if (!state.mixing.empty()) return false;
        }
        return true;
    }
    return player.layers.size() <= 1 && (player.layers.empty() || player.layers[0].mask.empty());
}

const char* anim::layerBlendName(AnimationLayerBlend blend){
    return blend == AnimationLayerBlend::Additive ? "additive" : "override";
}

bool anim::layerBlendFromName(const std::string& name, AnimationLayerBlend& out){
    if (name == "override"){
        out = AnimationLayerBlend::Override;
        return true;
    }
    if (name == "additive"){
        out = AnimationLayerBlend::Additive;
        return true;
    }
    return false;
}

int anim::layerIndex(AnimationPlayerComponent& player, const std::string& name, const std::string& who){
    // a controller's layers are known once it loads (a call from init, before the first update)
    if (!player.controller.empty()) ensureController(player, who, 0.0f, false);
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    const int index = runtime.findLayer(name);
    if (index < 0) warnOnce(runtime, "name:" + name, "Animation Player '%s': no layer named '%s'", who, name);
    return index;
}

int anim::layerIndex(AnimationPlayerComponent& player, int index, const std::string& who){
    if (!player.controller.empty()) ensureController(player, who, 0.0f, false);
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    if (index >= 0 && (size_t)index < runtime.layers.size()) return index;
    warnOnce(runtime, "index:" + std::to_string(index), "Animation Player '%s': no layer %s", who, std::to_string(index));
    return -1;
}

void anim::setLayerWeight(AnimationPlayerComponent& player, int layer, float weight, float fade, const std::string& who){
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    if (layer < 0 || (size_t)layer >= runtime.layers.size()) return;
    if (layer == 0){
        warnOnce(runtime, "baseweight", "Animation Player '%s': the weight of '%s' (the base layer) is always 1", who, runtime.layers[0].name);
        return;
    }
    LayerRuntime& target = runtime.layers[(size_t)layer];
    weight = clampWeight(weight);
    fade = finiteOr(fade, 0.0f);
    target.targetWeight = weight;
    if (fade > 0.0f){
        target.fadeSpeed = fadeRate(target.weight, weight, fade);
        if (target.fadeSpeed == 0.0f) target.weight = weight;
    }else{
        target.fadeSpeed = 0.0f;
        target.weight = weight;
    }
}

void anim::setLayerMask(AnimationPlayerComponent& player, int layer, const std::string& path, const std::string& who){
    (void)who;
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    if (layer < 0 || (size_t)layer >= runtime.layers.size()) return;
    LayerRuntime& target = runtime.layers[(size_t)layer];
    if (target.maskPath == path) return;
    target.maskPath = path;
    target.mask.reset();
    target.maskLoading = !path.empty();
    target.maskFailed = false;
    target.maskGeneration = 0;
    target.maskError.clear();
    target.maskErrorLogged = false;
    target.maskRetry = 0.0f;
    target.maskEntitiesValid = false;
}

void anim::setLayerBlend(AnimationPlayerComponent& player, int layer, AnimationLayerBlend blend, const std::string& who){
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    if (layer < 0 || (size_t)layer >= runtime.layers.size()) return;
    if (layer == 0){
        if (blend != AnimationLayerBlend::Override){
            warnOnce(runtime, "baseblend", "Animation Player '%s': '%s' (the base layer) always blends as override", who, runtime.layers[0].name);
        }
        return;
    }
    runtime.layers[(size_t)layer].blend = blend;
}

void anim::stopLayer(AnimationPlayerComponent& player, int layer, float fade){
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    if (layer < 0 || (size_t)layer >= runtime.layers.size()) return;
    if (runtime.controller){
        // the controller drives its states (spec 11.2): a layer's weight is the way to mute it
        if (std::find(runtime.warnedLayers.begin(), runtime.warnedLayers.end(), "controller:stopLayer") == runtime.warnedLayers.end()){
            runtime.warnedLayers.push_back("controller:stopLayer");
            Log::warn("Animation Player: stopLayer is not available with an Animator Controller (use setLayerWeight, play or stop)");
        }
        return;
    }
    player.started = true;
    clearQueue(runtime, layer);
    fade = finiteOr(fade, 0.0f);
    for (PlayerStateRuntime& state : runtime.states){
        if (runtime.stateLayer(state) != layer || (!state.enabled && !state.held)) continue;
        state.targetWeight = 0.0f;
        state.fadeSpeed = fadeRate(state.weight, 0.0f, fade);
        state.stopOnFadeOut = true;
        if (fade <= 0.0f || state.fadeSpeed == 0.0f) stopState(state);
    }
}

int anim::addLayer(AnimationPlayerComponent& player, const std::string& name, float weight, AnimationLayerBlend blend, const std::string& mask){
    if (name.empty()) return -1;
    std::vector<AnimationPlayerLayer> layers = playerLayers(player);
    for (const AnimationPlayerLayer& layer : layers){
        if (layer.name == name) return -1;
    }
    AnimationPlayerLayer added;
    added.name = name;
    added.weight = clampWeight(weight);
    added.blend = blend;
    added.mask = mask;
    layers.push_back(added);
    player.layers = layers;
    player.needsLoad = true;
    return (int)layers.size() - 1;
}

bool anim::removeLayer(AnimationPlayerComponent& player, int layer, const std::string& who){
    std::vector<AnimationPlayerLayer> layers = playerLayers(player);
    if (layer <= 0 || (size_t)layer >= layers.size()) return false;
    size_t moved = 0;
    for (AnimationPlayerClip& entry : player.clips){
        if (entry.layer == layer){
            entry.layer = 0;
            moved++;
        }else if (entry.layer > layer){
            entry.layer--;
        }
    }
    if (moved > 0){
        Log::warn("Animation Player '%s': layer '%s' removed: its %zu clip entr%s moved to '%s'", who.c_str(), layers[(size_t)layer].name.c_str(),
                  moved, moved == 1 ? "y" : "ies", layers[0].name.c_str());
    }
    layers.erase(layers.begin() + layer);
    // runtime moves (Lua state.layer) follow the layers that shift down
    if (player.runtime){
        for (PlayerStateRuntime& state : player.runtime->states){
            if (state.layerOverride == layer) state.layerOverride = -1;
            else if (state.layerOverride > layer) state.layerOverride--;
        }
    }
    player.layers = layers.size() == 1 && layers[0].name == BASE_LAYER_NAME && layers[0].mask.empty() ? std::vector<AnimationPlayerLayer>() : layers;
    player.needsLoad = true;
    return true;
}

void anim::setStateLayer(AnimationPlayerComponent& player, PlayerStateRuntime& state, int layer){
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    if (layer < 0 || (size_t)layer >= runtime.layers.size()) return;
    if (state.driven){
        // a controller state lives on its controller layer
        if (std::find(runtime.warnedLayers.begin(), runtime.warnedLayers.end(), "controller:layer") == runtime.warnedLayers.end()){
            runtime.warnedLayers.push_back("controller:layer");
            Log::warn("Animation Player: an Animator Controller state stays on its controller layer (state.layer is ignored)");
        }
        return;
    }
    const int from = runtime.stateLayer(state);
    state.layerOverride = layer == state.layer ? -1 : layer;
    if (from == layer) return;
    // a queued entry follows the state to its new layer's queue
    const size_t index = (size_t)(&state - runtime.states.data());
    std::deque<QueuedState>& queue = runtime.layers[(size_t)from].queue;
    for (auto it = queue.begin(); it != queue.end();){
        if (it->state == index){
            runtime.layers[(size_t)layer].queue.push_back(*it);
            it = queue.erase(it);
        }else{
            ++it;
        }
    }
    state.stopAtCycleEnd = false;
}

void anim::addMixingTransform(PlayerStateRuntime& state, const std::string& path, bool recursive){
    for (MixingTransform& mixing : state.mixing){
        if (mixing.path == path){
            if (mixing.recursive != recursive){
                mixing.recursive = recursive;
                state.mixingRevision++;
            }
            return;
        }
    }
    state.mixing.push_back({path, recursive});
    state.mixingRevision++;
}

bool anim::removeMixingTransform(PlayerStateRuntime& state, const std::string& path){
    for (auto it = state.mixing.begin(); it != state.mixing.end(); ++it){
        if (it->path == path){
            state.mixing.erase(it);
            state.mixingRevision++;
            return true;
        }
    }
    return false;
}

void anim::clearMixingTransforms(PlayerStateRuntime& state){
    if (state.mixing.empty()) return;
    state.mixing.clear();
    state.mixingRevision++;
}

float anim::effectiveSpeed(const AnimationPlayerComponent& player, const PlayerStateRuntime& state){
    return finiteOr(state.speed, 0.0f) * finiteOr(player.speed, 0.0f);
}

// Stop rewinds and keeps the pose (nothing writes the state's channels any more)
void anim::stopState(PlayerStateRuntime& state){
    state.enabled = false;
    state.playing = false;
    state.weight = 0.0f;
    state.targetWeight = 0.0f;
    state.fadeSpeed = 0.0f;
    state.stopOnFadeOut = false;
    state.stopAtCycleEnd = false;
    state.startedThisUpdate = false;
    state.finishNotified = false;
    state.startAtEnd = false;
    state.finishing = false;
    state.held = false;
    state.time = 0.0f;
    state.cycles = 0;
}

// a stopped (or paused) state starts from its beginning: 0, or its length when the effective
// speed is negative (R18)
void anim::startState(const AnimationPlayerComponent& player, PlayerStateRuntime& state){
    state.enabled = true;
    state.playing = true;
    state.cycles = 0;
    state.finishNotified = false;
    state.startedThisUpdate = true;
    state.stopAtCycleEnd = false;
    state.stopOnFadeOut = false;
    state.startAtEnd = false;
    state.finishing = false;
    state.held = false;
    state.time = 0.0f;
    if (effectiveSpeed(player, state) < 0.0f){
        if (state.bindings.clip){
            state.time = state.bindings.clip->getLength();
        }else{
            state.startAtEnd = true;
        }
    }
}

namespace {

    // the runtime layers for the effective authored ones: a surviving name keeps its runtime
    // weight, blend and mask, unless the authored value changed since (then that is taken)
    void rebuildLayers(AnimationPlayerRuntime& runtime, const std::vector<AnimationPlayerLayer>& authored){
        std::vector<LayerRuntime> layers(authored.size());
        for (size_t i = 0; i < authored.size(); i++){
            const AnimationPlayerLayer& source = authored[i];
            LayerRuntime& layer = layers[i];
            const float weight = i == 0 ? 1.0f : clampWeight(source.weight);
            const AnimationLayerBlend blend = i == 0 ? AnimationLayerBlend::Override : source.blend;
            int old = runtime.findLayer(source.name);
            if (old >= 0){
                LayerRuntime& previous = runtime.layers[(size_t)old];
                layer.weight = previous.weight;
                layer.targetWeight = previous.targetWeight;
                layer.fadeSpeed = previous.fadeSpeed;
                layer.blend = previous.blend;
                layer.maskPath = previous.maskPath;
                layer.mask = previous.mask;
                layer.maskLoading = previous.maskLoading;
                layer.maskFailed = previous.maskFailed;
                layer.maskGeneration = previous.maskGeneration;
                layer.maskError = previous.maskError;
                layer.maskErrorLogged = previous.maskErrorLogged;
                layer.maskRetry = previous.maskRetry;
                if (previous.authoredWeight != weight || i == 0){
                    layer.weight = weight;
                    layer.targetWeight = weight;
                    layer.fadeSpeed = 0.0f;
                }
                if (previous.authoredBlend != blend || i == 0) layer.blend = blend;
                if (previous.authoredMask != source.mask){
                    layer.maskPath = source.mask;
                    layer.mask.reset();
                    layer.maskLoading = !source.mask.empty();
                    layer.maskFailed = false;
                    layer.maskGeneration = 0;
                    layer.maskError.clear();
                    layer.maskErrorLogged = false;
                    layer.maskRetry = 0.0f;
                }
            }else{
                layer.weight = weight;
                layer.targetWeight = weight;
                layer.blend = blend;
                layer.maskPath = source.mask;
                layer.maskLoading = !source.mask.empty();
            }
            layer.name = source.name;
            layer.authoredWeight = weight;
            layer.authoredBlend = blend;
            layer.authoredMask = source.mask;
        }
        runtime.layers = std::move(layers);
    }

}

namespace {

    // what one runtime state is built from: a clip entry, or an Animator Controller state (or
    // blend tree child)
    struct StateSpec{
        std::string name;
        std::string clip;
        ClipWrapMode wrap = ClipWrapMode::Default;
        int layer = 0;
        size_t entry = SIZE_MAX;
        bool driven = false;
        bool hidden = false;
    };

    // the controller's states: "<Layer>.<State>" per clip state, "<Layer>.<State>#<i>" per
    // blend tree child (spec 11.2)
    std::vector<StateSpec> controllerSpecs(const AnimatorController& controller){
        std::vector<StateSpec> specs;
        const ControllerData& data = controller.getData();
        for (size_t l = 0; l < data.layers.size(); l++){
            const ControllerLayer& layer = data.layers[l];
            for (const ControllerState& state : layer.states){
                const std::string name = layer.name + "." + state.name;
                if (!state.clip.empty()){
                    specs.push_back({name, state.clip, state.wrap, (int)l, SIZE_MAX, true, false});
                }else if (state.hasBlendTree){
                    for (size_t c = 0; c < state.blendTree.children.size(); c++){
                        specs.push_back({name + "#" + std::to_string(c), state.blendTree.children[c].clip, state.wrap, (int)l, SIZE_MAX, true, true});
                    }
                }
            }
        }
        return specs;
    }

    std::vector<AnimationPlayerLayer> controllerLayers(const AnimatorController& controller){
        std::vector<AnimationPlayerLayer> layers;
        for (const ControllerLayer& source : controller.getData().layers){
            AnimationPlayerLayer layer;
            layer.name = source.name;
            layer.weight = source.weight;
            layer.blend = source.blend;
            layer.mask = source.mask;
            layer.maskGuid = source.maskGuid;
            layers.push_back(layer);
        }
        return layers;
    }

}

std::vector<AnimationPlayerLayer> anim::effectivePlayerLayers(const AnimationPlayerComponent& player){
    if (!player.controller.empty()){
        if (player.runtime && player.runtime->controller && player.runtime->controller->controller){
            return controllerLayers(*player.runtime->controller->controller);
        }
        AnimationPlayerLayer base;
        base.name = BASE_LAYER_NAME;
        return {base};
    }
    return playerLayers(player);
}

anim::AnimationPlayerRuntime& anim::playerRuntime(AnimationPlayerComponent& player){
    if (!player.runtime){
        player.runtime = std::make_shared<AnimationPlayerRuntime>();
        player.needsLoad = true;
    }
    AnimationPlayerRuntime& runtime = *player.runtime;
    if (player.needsLoad){
        player.needsLoad = false;
        // the queues by state name, before the states and layers change
        std::vector<std::pair<std::string, float>> queued;
        for (const LayerRuntime& layer : runtime.layers){
            for (const QueuedState& entry : layer.queue){
                if (entry.state < runtime.states.size()) queued.push_back({runtime.states[entry.state].name, entry.fade});
            }
        }
        // a controller owns the states (spec 5.10): its runtime follows the component's path
        if (player.controller.empty()){
            runtime.controller.reset();
        }else if (!runtime.controller || runtime.controller->path != player.controller){
            // another controller: its own parameters, from its defaults (loaded in the update)
            runtime.controller = std::make_shared<ControllerRuntime>();
            runtime.controller->path = player.controller;
        }
        rebuildLayers(runtime, effectivePlayerLayers(player));
        const int layerCount = (int)runtime.layers.size();

        std::vector<StateSpec> specs;
        runtime.duplicateStateNames.clear();
        std::string badLayers;
        if (runtime.controller){
            if (runtime.controller->controller) specs = controllerSpecs(*runtime.controller->controller);
        }else{
            std::vector<std::string> names = playerStateNames(player.clips);
            for (size_t i = 0; i < player.clips.size(); i++){
                const AnimationPlayerClip& entry = player.clips[i];
                const std::string base = defaultStateName(entry);
                if (names[i] != base){
                    if (!runtime.duplicateStateNames.empty()) runtime.duplicateStateNames += ", ";
                    runtime.duplicateStateNames += "'" + base + "' (clip " + std::to_string(i + 1) + ") plays as '" + names[i] + "'";
                }
                StateSpec spec;
                spec.name = names[i];
                spec.clip = entry.clip;
                spec.wrap = entry.wrap;
                spec.entry = i;
                spec.layer = entry.layer;
                if (entry.layer < 0 || entry.layer >= layerCount){
                    if (!badLayers.empty()) badLayers += ", ";
                    badLayers += "'" + names[i] + "' (layer " + std::to_string(entry.layer) + ")";
                    spec.layer = 0;
                }
                specs.push_back(spec);
            }
        }

        // one state per spec; a surviving name keeps its playback, and its loaded clip and
        // bindings while it names the same clip
        std::vector<PlayerStateRuntime> states(specs.size());
        for (size_t i = 0; i < specs.size(); i++){
            const StateSpec& spec = specs[i];
            PlayerStateRuntime& state = states[i];
            int old = runtime.findState(spec.name);
            if (old >= 0){
                state = runtime.states[(size_t)old];
                if (state.clip != spec.clip){
                    state.bindings = ClipBindings();
                    state.loadFailed = false;
                    state.retryTimer = 0.0f;
                    state.errorLogged = false;
                    state.error.clear();
                }
            }
            state.name = spec.name;
            state.entry = spec.entry;
            state.clip = spec.clip;
            state.entryWrap = spec.wrap;
            state.layer = spec.layer;
            state.driven = spec.driven;
            state.hidden = spec.hidden;
            if (state.layerOverride >= layerCount || spec.driven) state.layerOverride = -1;
        }
        // entries on layers the player does not have play on the base layer (said once per list)
        if (badLayers != runtime.warnedEntryLayers){
            runtime.warnedEntryLayers = badLayers;
            if (!badLayers.empty()){
                Log::warn("Animation Player: clip entries on missing layers play on '%s': %s",
                          runtime.layers[0].name.c_str(), badLayers.c_str());
            }
        }
        runtime.states = std::move(states);
        // queued states follow their names, on their (new) layer's queue, in their order
        for (const auto& entry : queued){
            int index = runtime.findState(entry.first);
            if (index < 0) continue;
            QueuedState moved;
            moved.state = (size_t)index;
            moved.fade = entry.second;
            runtime.layers[(size_t)runtime.stateLayer(runtime.states[(size_t)index])].queue.push_back(moved);
        }
        if (runtime.controller) resetControllerLayers(runtime);
        runtime.scratch.dirty = true;
        runtime.warnedStates.clear();
        runtime.warnedLayers.clear();
    }
    return runtime;
}

ClipWrapMode anim::stateWrap(const PlayerStateRuntime& state){
    if (state.wrapOverride != ClipWrapMode::Default) return state.wrapOverride;
    if (state.entryWrap != ClipWrapMode::Default) return state.entryWrap;
    return state.bindings.clip ? state.bindings.clip->getWrap() : ClipWrapMode::Once;
}

float anim::stateLength(const PlayerStateRuntime& state){
    return state.bindings.clip ? state.bindings.clip->getLength() : 0.0f;
}

float anim::stateClipTime(const PlayerStateRuntime& state){
    ClipWrapMode wrap = stateWrap(state);
    if (wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong){
        return wrapClipTime(state.time, stateLength(state), wrap);
    }
    return state.time;
}

void anim::setStateTime(PlayerStateRuntime& state, float time){
    state.time = finiteOr(time, 0.0f);
    state.cycles = 0;
    state.finishNotified = false;
    state.startAtEnd = false;
    ClipWrapMode wrap = stateWrap(state);
    float length = stateLength(state);
    if ((wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong) && length > 0.0f){
        // the stored form: [0, 2 * length)
        const double period = 2.0 * (double)length;
        double t = std::fmod((double)state.time, period);
        if (t < 0.0) t += period;
        if (t >= period) t = 0.0;
        state.time = (float)t;
    }else if (state.bindings.clip){
        state.time = std::min(std::max(state.time, 0.0f), length);
    }
}

bool anim::playState(AnimationPlayerComponent& player, const std::string& name, float fade, const std::string& who, PlayMode mode){
    PlayerStateRuntime* target = findNamed(player, name, who);
    if (!target) return false;
    player.started = true;
    AnimationPlayerRuntime& runtime = *player.runtime;
    // StopSameLayer: the target's layer only; StopAll: every layer
    const int layer = mode == PlayMode::StopAll ? -1 : runtime.stateLayer(*target);
    auto affected = [&](const PlayerStateRuntime& state){
        return layer < 0 || runtime.stateLayer(state) == layer;
    };
    clearQueue(runtime, layer);
    fade = finiteOr(fade, 0.0f);
    if (fade <= 0.0f){
        // Unity Play: the others stop, the state plays from its start at weight 1
        for (PlayerStateRuntime& state : runtime.states){
            if (&state != target && affected(state)) stopState(state);
        }
        startState(player, *target);
        target->weight = 1.0f;
        target->targetWeight = 1.0f;
        target->fadeSpeed = 0.0f;
        return true;
    }
    // CrossFade: the others (held ones too, R1b) fade out and stop, the state fades in from its
    // current weight (a playing state keeps its time; a held one restarts at its weight)
    for (PlayerStateRuntime& state : runtime.states){
        if (&state == target || (!state.enabled && !state.held) || !affected(state)) continue;
        state.targetWeight = 0.0f;
        state.fadeSpeed = fadeRate(state.weight, 0.0f, fade);
        state.stopOnFadeOut = true;
        if (state.fadeSpeed == 0.0f) stopState(state);
    }
    if (!target->enabled){
        const bool held = target->held;
        startState(player, *target);
        if (!held) target->weight = 0.0f;
    }
    target->stopOnFadeOut = false;
    target->targetWeight = 1.0f;
    target->fadeSpeed = fadeRate(target->weight, 1.0f, fade);
    if (target->fadeSpeed == 0.0f) target->weight = 1.0f;
    return true;
}

bool anim::blendState(AnimationPlayerComponent& player, const std::string& name, float weight, float fade, const std::string& who){
    PlayerStateRuntime* target = findNamed(player, name, who);
    if (!target) return false;
    player.started = true;
    weight = std::max(0.0f, finiteOr(weight, 0.0f));
    fade = finiteOr(fade, 0.0f);
    if (target->held && weight <= 0.0f){
        // blending a held state to 0 fades it out (R1b)
        target->targetWeight = 0.0f;
        target->fadeSpeed = fadeRate(target->weight, 0.0f, fade);
        target->stopOnFadeOut = true;
        if (fade <= 0.0f || target->fadeSpeed == 0.0f) stopState(*target);
        return true;
    }
    if (!target->enabled){
        startState(player, *target);
        target->weight = 0.0f;
    }
    target->stopOnFadeOut = false;
    target->targetWeight = weight;
    target->fadeSpeed = fade > 0.0f ? fadeRate(target->weight, weight, fade) : 0.0f;
    if (target->fadeSpeed == 0.0f) target->weight = weight;
    return true;
}

bool anim::queueState(AnimationPlayerComponent& player, const std::string& name, float fade, const std::string& who){
    PlayerStateRuntime* target = findNamed(player, name, who);
    if (!target) return false;
    player.started = true;
    AnimationPlayerRuntime& runtime = *player.runtime;
    const int layer = runtime.stateLayer(*target);
    bool playing = false;
    for (const PlayerStateRuntime& state : runtime.states){
        if (state.enabled && state.targetWeight > 0.0f && runtime.stateLayer(state) == layer) playing = true;
    }
    // with nothing playing on its layer it plays now
    if (!playing){
        return playState(player, name, fade, who);
    }
    QueuedState queued;
    queued.state = (size_t)(target - runtime.states.data());
    queued.fade = std::max(0.0f, finiteOr(fade, 0.0f));
    runtime.layers[(size_t)layer].queue.push_back(queued);
    return true;
}

void anim::stopStates(AnimationPlayerComponent& player, const std::string* name, const std::string& who){
    if (name){
        PlayerStateRuntime* target = findNamed(player, *name, who);
        if (target){
            // R17: stop clears the queue (of the state's layer)
            clearQueue(*player.runtime, player.runtime->stateLayer(*target));
            stopState(*target);
            player.started = true;
        }
        return;
    }
    player.started = true;
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    clearQueue(runtime, -1);
    for (PlayerStateRuntime& state : runtime.states){
        stopState(state);
    }
}

void anim::rewindStates(AnimationPlayerComponent& player, const std::string* name, const std::string& who){
    auto rewind = [&player](PlayerStateRuntime& state){
        setStateTime(state, 0.0f);
        if (effectiveSpeed(player, state) < 0.0f){
            if (state.bindings.clip){
                state.time = state.bindings.clip->getLength();
            }else{
                state.startAtEnd = true;
            }
        }
        state.startedThisUpdate = state.enabled;
    };
    if (name){
        PlayerStateRuntime* target = findNamed(player, *name, who);
        if (target) rewind(*target);
        return;
    }
    for (PlayerStateRuntime& state : playerRuntime(player).states){
        rewind(state);
    }
}

void anim::fadeOutStates(AnimationPlayerComponent& player, float fade){
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    clearQueue(runtime, -1);
    fade = finiteOr(fade, 0.0f);
    for (PlayerStateRuntime& state : runtime.states){
        if (!state.enabled && !state.held) continue;
        state.targetWeight = 0.0f;
        state.fadeSpeed = fadeRate(state.weight, 0.0f, fade);
        state.stopOnFadeOut = true;
        if (fade <= 0.0f || state.fadeSpeed == 0.0f) stopState(state);
    }
}

bool anim::isStatePlaying(const AnimationPlayerComponent& player, const std::string* name){
    if (!player.runtime) return false;
    for (const PlayerStateRuntime& state : player.runtime->states){
        if ((!name || state.name == *name) && state.enabled) return true;
    }
    return false;
}
