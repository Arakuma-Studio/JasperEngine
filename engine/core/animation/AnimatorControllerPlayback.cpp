// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator Controller side of the player runtime (anim layers spec 11.2-11.3): loading,
// the state groups, parameters, play / crossFade / stop and state info. The per-frame driver
// (transitions, weights, blend trees, events) is ActionSystem::driveController.

#include "AnimationPlayerRuntime.h"

#include "animation/AnimatorController.h"
#include "pool/AnimatorControllerPool.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    using namespace jasperengine::anim;

    // a controller that failed to load is tried again this often
    constexpr float CONTROLLER_RETRY_SECONDS = 1.0f;

    bool warnOnce(ControllerRuntime& controller, const std::string& key){
        if (std::find(controller.warned.begin(), controller.warned.end(), key) != controller.warned.end()) return false;
        controller.warned.push_back(key);
        return true;
    }

    // the parameters for a (re)loaded controller: defaults, values kept by name
    void resetParameters(ControllerRuntime& runtime, const AnimatorController* previous){
        const ControllerData& data = runtime.controller->getData();
        std::vector<ControllerParamValue> params(data.parameters.size());
        for (size_t i = 0; i < data.parameters.size(); i++){
            params[i].value = data.parameters[i].defaultValue;
            if (previous){
                const int old = previous->getData().findParameter(data.parameters[i].name);
                if (old >= 0 && (size_t)old < runtime.params.size() && previous->getData().parameters[(size_t)old].type == data.parameters[i].type){
                    params[i] = runtime.params[(size_t)old];
                }
            }
        }
        runtime.params = std::move(params);
    }

    // a value converted to a parameter type
    float convert(float value, ControllerParameterType type){
        if (!std::isfinite(value)) value = 0.0f;
        switch (type){
            case ControllerParameterType::Float: return value;
            case ControllerParameterType::Int: return std::round(value);
            case ControllerParameterType::Bool:
            case ControllerParameterType::Trigger: return value != 0.0f ? 1.0f : 0.0f;
        }
        return value;
    }

    ControllerRuntime* loadedController(AnimationPlayerComponent& player){
        if (player.controller.empty()) return nullptr;
        AnimationPlayerRuntime& runtime = playerRuntime(player);
        if (!runtime.controller) return nullptr;
        return runtime.controller->controller ? runtime.controller.get() : nullptr;
    }

    // the controller of a player for an API call: loaded on demand (calls from init, before the
    // first update), else warned once
    ControllerRuntime* apiController(AnimationPlayerComponent& player, const std::string& who, const char* call){
        if (player.controller.empty()){
            AnimationPlayerRuntime& runtime = playerRuntime(player);
            const std::string key = std::string("nocontroller:") + call;
            if (std::find(runtime.warnedLayers.begin(), runtime.warnedLayers.end(), key) == runtime.warnedLayers.end()){
                runtime.warnedLayers.push_back(key);
                Log::warn("Animation Player '%s': %s needs an Animator Controller (set the player's controller)", who.c_str(), call);
            }
            return nullptr;
        }
        ensureController(player, who, 0.0f, false);
        return loadedController(player);
    }

    void stopGroup(AnimationPlayerRuntime& runtime, ControllerGroup& group){
        group.active = false;
        group.weight = 0.0f;
        group.time = 0.0;
        group.previous = 0.0;
        group.entered = false;
        group.startEvents = false;
        for (size_t index : group.states){
            if (index < runtime.states.size()) stopState(runtime.states[index]);
        }
    }

    void pendStateCallback(AnimationPlayerRuntime& runtime, int layer, const std::string& state, bool enter){
        PendingPlayerCallback callback;
        callback.stateEnter = enter;
        callback.stateExit = !enter;
        callback.layer = layer;
        callback.event.state = state;
        callback.when = 0.0f;
        runtime.pending.push_back(std::move(callback));
    }

    const std::string& stateName(const ControllerRuntime& controller, int layer, int state){
        return controller.controller->getData().layers[(size_t)layer].states[(size_t)state].name;
    }

}

void anim::resetControllerLayers(AnimationPlayerRuntime& runtime){
    ControllerRuntime& controller = *runtime.controller;
    // what each layer was playing, by name (groupNames: the names the groups were built for); a
    // layer in a transition keeps its destination (the transition completes)
    struct Kept{ std::string layer; std::string state; double time; };
    std::vector<Kept> kept;
    for (size_t l = 0; l < controller.layers.size() && l < controller.groupNames.size(); l++){
        const ControllerLayerRuntime& layer = controller.layers[l];
        const std::vector<std::string>& names = controller.groupNames[l].second;
        const int state = layer.transition.active ? layer.transition.to : layer.current;
        if (!layer.started || state < 0 || (size_t)state >= names.size() || (size_t)state >= layer.groups.size()) continue;
        kept.push_back({controller.groupNames[l].first, names[(size_t)state], layer.groups[(size_t)state].time});
    }
    controller.layers.clear();
    controller.groupNames.clear();
    if (!controller.controller) return;
    const ControllerData& data = controller.controller->getData();
    controller.layers.resize(data.layers.size());
    // the runtime states were built from the controller in this same order (controllerSpecs):
    // positional, so names that happen to collide ("A.B" + "C" and "A" + "B.C") stay apart
    size_t next = 0;
    for (size_t l = 0; l < data.layers.size(); l++){
        const ControllerLayer& layerData = data.layers[l];
        ControllerLayerRuntime& layer = controller.layers[l];
        layer.groups.resize(layerData.states.size());
        std::vector<std::string> names;
        for (size_t s = 0; s < layerData.states.size(); s++){
            const ControllerState& state = layerData.states[s];
            names.push_back(state.name);
            ControllerGroup& group = layer.groups[s];
            const size_t count = !state.clip.empty() ? 1 : (state.hasBlendTree ? state.blendTree.children.size() : 0);
            for (size_t c = 0; c < count && next < runtime.states.size(); c++) group.states.push_back(next++);
        }
        controller.groupNames.push_back({layerData.name, names});
        for (const Kept& entry : kept){
            if (entry.layer != layerData.name) continue;
            const int state = layerData.findState(entry.state);
            if (state < 0) continue;
            layer.started = true;
            layer.current = state;
            layer.lastEntered = state;
            ControllerGroup& group = layer.groups[(size_t)state];
            group.active = true;
            group.weight = 1.0f;
            group.time = entry.time;
            group.previous = entry.time;
        }
    }
    // the driver gives the active groups their weights on its next step; anything else (a
    // transition's sources before the reload) stops now
    for (PlayerStateRuntime& state : runtime.states){
        if (state.driven) stopState(state);
    }
}

bool anim::ensureController(AnimationPlayerComponent& player, const std::string& who, float dt, bool poolChanged){
    if (player.controller.empty()) return false;
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    if (!runtime.controller || runtime.controller->path != player.controller){
        // the field changed without a reload request (the ECS component from Lua)
        player.needsLoad = true;
        playerRuntime(player);
    }
    ControllerRuntime& controller = *runtime.controller;
    bool fetch = !controller.controller;
    if (!fetch && poolChanged && AnimatorControllerPool::getGeneration(controller.path) != controller.generation) fetch = true;
    if (fetch && controller.failed){
        controller.retryTimer += dt;
        if (controller.retryTimer < CONTROLLER_RETRY_SECONDS) fetch = false;
        else controller.retryTimer = 0.0f;
    }
    if (fetch){
        std::string error;
        std::shared_ptr<const AnimatorController> loaded = AnimatorControllerPool::getOrLoad(controller.path, &error);
        if (!loaded){
            controller.failed = true;
            controller.error = error;
            if (!controller.errorLogged){
                controller.errorLogged = true;
                Log::error("Animation Player '%s': controller '%s': %s", who.c_str(), controller.path.c_str(), error.c_str());
            }
        }else if (loaded != controller.controller){
            std::shared_ptr<const AnimatorController> previous = controller.controller;
            controller.controller = loaded;
            controller.generation = AnimatorControllerPool::getGeneration(controller.path);
            controller.failed = false;
            controller.errorLogged = false;
            controller.error.clear();
            controller.retryTimer = 0.0f;
            resetParameters(controller, previous.get());
            // the states and layers follow the controller (spec 11.2)
            player.needsLoad = true;
            playerRuntime(player);
        }else{
            controller.generation = AnimatorControllerPool::getGeneration(controller.path);
        }
    }
    if (controller.controller && !player.clips.empty() && !controller.clipsWarned){
        controller.clipsWarned = true;
        Log::warn("Animation Player '%s': it has an Animator Controller: its %zu clip entr%s and layers are ignored", who.c_str(),
                  player.clips.size(), player.clips.size() == 1 ? "y" : "ies");
    }
    return controller.controller != nullptr;
}

void anim::blendTreeWeights(const std::vector<float>& thresholds, float value, std::vector<float>& out){
    out.assign(thresholds.size(), 0.0f);
    if (thresholds.empty()) return;
    if (!std::isfinite(value)) value = 0.0f;
    if (value <= thresholds.front()){
        out.front() = 1.0f;
        return;
    }
    if (value >= thresholds.back()){
        out.back() = 1.0f;
        return;
    }
    for (size_t i = 0; i + 1 < thresholds.size(); i++){
        const float a = thresholds[i];
        const float b = thresholds[i + 1];
        if (value >= a && value <= b){
            const float u = b > a ? (value - a) / (b - a) : 0.0f;
            out[i] = 1.0f - u;
            out[i + 1] = u;
            return;
        }
    }
}

namespace {

    // the blend tree's child weights at the parameter's current value (main thread: the scratch
    // is reused, no allocation per frame)
    void treeWeights(const AnimationPlayerRuntime& runtime, const ControllerState& state, int parameter, std::vector<float>& out){
        static std::vector<float> thresholds;
        thresholds.clear();
        for (const BlendTreeChild& child : state.blendTree.children) thresholds.push_back(child.threshold);
        const float value = parameter >= 0 && (size_t)parameter < runtime.controller->params.size() ? runtime.controller->params[(size_t)parameter].value : 0.0f;
        blendTreeWeights(thresholds, value, out);
    }

}

float anim::controllerStateLength(const AnimationPlayerRuntime& runtime, int layer, int state){
    if (!runtime.controller || !runtime.controller->controller) return 0.0f;
    const ControllerRuntime& controller = *runtime.controller;
    const AnimatorController::Layer& layerData = controller.controller->getLayers()[(size_t)layer];
    const AnimatorController::State& stateData = layerData.states[(size_t)state];
    const ControllerGroup& group = controller.layers[(size_t)layer].groups[(size_t)state];
    if (stateData.data->isEmpty()) return 1.0f;
    // a clip that failed to load makes an empty state (L11): 1 s, so its time and exit times go on
    auto failed = [&](size_t index){ return runtime.states[index].loadFailed && !runtime.states[index].bindings.clip; };
    if (!stateData.data->hasBlendTree){
        if (group.states.empty() || failed(group.states[0])) return 1.0f;
        return stateLength(runtime.states[group.states[0]]);
    }
    // a blend tree: the cycle of its weighted children (1 / Σ w_c speed_c / length_c); children
    // without weight or without a clip (failed) do not count; 0 while a weighted one loads
    static std::vector<float> weights;
    treeWeights(runtime, *stateData.data, stateData.blendParameter, weights);
    double rate = 0.0;
    bool any = false;
    for (size_t c = 0; c < weights.size() && c < group.states.size(); c++){
        if (!(weights[c] > 0.0f) || failed(group.states[c])) continue;
        const float length = stateLength(runtime.states[group.states[c]]);
        if (length <= 0.0f) return 0.0f;
        rate += (double)weights[c] * stateData.data->blendTree.children[c].speed / length;
        any = true;
    }
    if (!any) return 1.0f;
    return rate > 0.0 ? (float)(1.0 / rate) : 0.0f;
}

float anim::controllerStateSpeed(const AnimationPlayerRuntime& runtime, int layer, int state){
    if (!runtime.controller || !runtime.controller->controller) return 1.0f;
    const ControllerRuntime& controller = *runtime.controller;
    const AnimatorController::State& stateData = controller.controller->getLayers()[(size_t)layer].states[(size_t)state];
    float speed = stateData.data->speed;
    if (stateData.speedParameter >= 0 && (size_t)stateData.speedParameter < controller.params.size()){
        speed *= controller.params[(size_t)stateData.speedParameter].value;
    }
    return std::isfinite(speed) ? speed : 0.0f;
}

bool anim::controllerStateLoops(const AnimationPlayerRuntime& runtime, int layer, int state){
    if (!runtime.controller || !runtime.controller->controller) return false;
    const ControllerRuntime& controller = *runtime.controller;
    const ControllerState& data = *controller.controller->getLayers()[(size_t)layer].states[(size_t)state].data;
    if (data.wrap != ClipWrapMode::Default) return data.wrap == ClipWrapMode::Loop || data.wrap == ClipWrapMode::PingPong;
    if (data.isEmpty()) return true;
    const ControllerGroup& group = controller.layers[(size_t)layer].groups[(size_t)state];
    if (group.states.empty()) return false;
    for (size_t index : group.states){
        const ClipWrapMode wrap = stateWrap(runtime.states[index]);
        if (wrap != ClipWrapMode::Loop && wrap != ClipWrapMode::PingPong) return false;
    }
    return true;
}

int anim::findControllerParameter(AnimationPlayerComponent& player, const std::string& name){
    ControllerRuntime* controller = loadedController(player);
    return controller ? controller->controller->getData().findParameter(name) : -1;
}

bool anim::setControllerParameter(AnimationPlayerComponent& player, const std::string& name, ControllerParameterType as, float value, float dampTime,
                                  const std::string& who){
    ControllerRuntime* controller = apiController(player, who, "parameters");
    if (!controller) return false;
    const ControllerData& data = controller->controller->getData();
    const int index = data.findParameter(name);
    if (index < 0){
        if (warnOnce(*controller, "unknown:" + name)){
            Log::warn("Animation Player '%s': the controller has no parameter '%s'", who.c_str(), name.c_str());
        }
        return false;
    }
    const ControllerParameterType type = data.parameters[(size_t)index].type;
    const bool fits = type == as || (as == ControllerParameterType::Trigger && type == ControllerParameterType::Bool);
    if (!fits && warnOnce(*controller, std::string("type:") + parameterTypeName(as) + ":" + name)){
        Log::warn("Animation Player '%s': parameter '%s' is a %s, set as a %s: converted", who.c_str(), name.c_str(),
                  parameterTypeName(type), parameterTypeName(as));
    }
    ControllerParamValue& param = controller->params[(size_t)index];
    const float converted = convert(value, type);
    if (type == ControllerParameterType::Float && dampTime > 0.0f && std::isfinite(dampTime)){
        // SmoothDamp toward it (Unity Animator.SetFloat with a damp time); the update moves it
        param.damping = true;
        param.target = converted;
        param.dampTime = dampTime;
    }else{
        param.damping = false;
        param.velocity = 0.0f;
        param.value = converted;
    }
    return true;
}

float anim::getControllerParameter(AnimationPlayerComponent& player, const std::string& name, ControllerParameterType as, const std::string& who){
    ControllerRuntime* controller = apiController(player, who, "parameters");
    if (!controller) return 0.0f;
    const ControllerData& data = controller->controller->getData();
    const int index = data.findParameter(name);
    if (index < 0){
        if (warnOnce(*controller, "unknown:" + name)){
            Log::warn("Animation Player '%s': the controller has no parameter '%s'", who.c_str(), name.c_str());
        }
        return 0.0f;
    }
    const ControllerParameterType type = data.parameters[(size_t)index].type;
    if (type != as && !(as == ControllerParameterType::Bool && type == ControllerParameterType::Trigger) &&
        warnOnce(*controller, std::string("get:") + parameterTypeName(as) + ":" + name)){
        Log::warn("Animation Player '%s': parameter '%s' is a %s, read as a %s: converted", who.c_str(), name.c_str(),
                  parameterTypeName(type), parameterTypeName(as));
    }
    return convert(controller->params[(size_t)index].value, as);
}

bool anim::findControllerState(const AnimationPlayerRuntime& runtime, const std::string& name, int layer, int& outLayer, int& outState){
    if (!runtime.controller || !runtime.controller->controller) return false;
    const ControllerData& data = runtime.controller->controller->getData();
    for (size_t l = 0; l < data.layers.size(); l++){
        if (layer >= 0 && (size_t)layer != l) continue;
        const ControllerLayer& layerData = data.layers[l];
        int state = layerData.findState(name);
        if (state < 0 && name.size() > layerData.name.size() + 1 && name.compare(0, layerData.name.size(), layerData.name) == 0 &&
            name[layerData.name.size()] == '.'){
            state = layerData.findState(name.substr(layerData.name.size() + 1));
        }
        if (state >= 0){
            outLayer = (int)l;
            outState = state;
            return true;
        }
    }
    return false;
}

bool anim::controllerPlay(AnimationPlayerComponent& player, const std::string& name, int layer, float normalizedTime, const std::string& who){
    ControllerRuntime* controller = apiController(player, who, "play");
    if (!controller) return false;
    AnimationPlayerRuntime& runtime = *player.runtime;
    int l = -1, s = -1;
    if (!findControllerState(runtime, name, layer, l, s)){
        if (warnOnce(*controller, "state:" + name)){
            Log::warn("Animation Player '%s': the controller has no state '%s'", who.c_str(), name.c_str());
        }
        return false;
    }
    player.started = true;
    if (controller->stopped){
        // the other layers enter their default states again
        controller->stopped = false;
        for (ControllerLayerRuntime& other : controller->layers) other.started = false;
    }
    ControllerLayerRuntime& target = controller->layers[(size_t)l];
    target.transition = ControllerTransitionRuntime();
    if (target.started && target.lastEntered >= 0) pendStateCallback(runtime, l, stateName(*controller, l, target.lastEntered), false);
    for (ControllerGroup& group : target.groups) stopGroup(runtime, group);
    ControllerGroup& group = target.groups[(size_t)s];
    group.active = true;
    group.weight = 1.0f;
    group.time = std::isfinite(normalizedTime) ? (double)normalizedTime : 0.0;
    group.previous = group.time;
    group.entered = true;
    group.startEvents = true;
    group.start = group.time;
    target.current = s;
    target.lastEntered = s;
    target.started = true;
    pendStateCallback(runtime, l, stateName(*controller, l, s), true);
    return true;
}

bool anim::controllerCrossFade(AnimationPlayerComponent& player, const std::string& name, float seconds, int layer, const std::string& who){
    ControllerRuntime* controller = apiController(player, who, "crossFade");
    if (!controller) return false;
    AnimationPlayerRuntime& runtime = *player.runtime;
    int l = -1, s = -1;
    if (!findControllerState(runtime, name, layer, l, s)){
        if (warnOnce(*controller, "state:" + name)){
            Log::warn("Animation Player '%s': the controller has no state '%s'", who.c_str(), name.c_str());
        }
        return false;
    }
    ControllerLayerRuntime& target = controller->layers[(size_t)l];
    if (controller->stopped || !target.started || !(seconds > 0.0f)) return controllerPlay(player, name, l, 0.0f, who);
    player.started = true;
    // the states playing now are its sources at their weights (as an interrupted transition)
    ControllerTransitionRuntime transition;
    transition.active = true;
    transition.to = s;
    transition.transition = -1;
    transition.duration = seconds;
    for (size_t g = 0; g < target.groups.size(); g++){
        ControllerGroup& group = target.groups[g];
        if ((int)g == s){
            // a crossFade to a playing state restarts it, keeping its weight (no copy state)
            if (group.active){
                transition.selfWeight = group.weight;
                stopGroup(runtime, group);
            }
            continue;
        }
        if (group.active && group.weight > 0.0f) transition.sources.push_back({(int)g, group.weight});
        else if (group.active) stopGroup(runtime, group);
    }
    if (target.lastEntered >= 0) pendStateCallback(runtime, l, stateName(*controller, l, target.lastEntered), false);
    ControllerGroup& group = target.groups[(size_t)s];
    group.active = true;
    group.weight = 0.0f;
    group.time = 0.0;
    group.previous = 0.0;
    group.entered = true;
    group.startEvents = true;
    group.start = 0.0;
    target.transition = transition;
    target.lastEntered = s;
    pendStateCallback(runtime, l, stateName(*controller, l, s), true);
    return true;
}

void anim::controllerStop(AnimationPlayerComponent& player){
    if (player.controller.empty() || !player.runtime || !player.runtime->controller) return;
    AnimationPlayerRuntime& runtime = *player.runtime;
    ControllerRuntime& controller = *runtime.controller;
    player.started = true;
    controller.stopped = true;
    for (ControllerLayerRuntime& layer : controller.layers){
        layer.transition = ControllerTransitionRuntime();
        for (ControllerGroup& group : layer.groups) stopGroup(runtime, group);
        layer.current = -1;
        layer.lastEntered = -1;
        layer.started = false;
    }
    for (PlayerStateRuntime& state : runtime.states) stopState(state);
}

anim::ControllerStateView anim::controllerStateInfo(const AnimationPlayerComponent& player, int layer, bool next){
    ControllerStateView view;
    if (!player.runtime || !player.runtime->controller || !player.runtime->controller->controller) return view;
    const AnimationPlayerRuntime& runtime = *player.runtime;
    const ControllerRuntime& controller = *runtime.controller;
    if (layer < 0 || (size_t)layer >= controller.layers.size()) return view;
    const ControllerLayerRuntime& layerRuntime = controller.layers[(size_t)layer];
    const int state = next ? (layerRuntime.transition.active ? layerRuntime.transition.to : -1) : layerRuntime.current;
    if (state < 0 || !layerRuntime.started) return view;
    const ControllerLayer& layerData = controller.controller->getData().layers[(size_t)layer];
    view.valid = true;
    view.name = layerData.states[(size_t)state].name;
    view.fullName = layerData.name + "." + view.name;
    view.normalizedTime = (float)layerRuntime.groups[(size_t)state].time;
    view.length = controllerStateLength(runtime, layer, state);
    view.speed = controllerStateSpeed(runtime, layer, state);
    view.loop = controllerStateLoops(runtime, layer, state);
    return view;
}

bool anim::controllerInTransition(const AnimationPlayerComponent& player, int layer){
    if (!player.runtime || !player.runtime->controller) return false;
    const ControllerRuntime& controller = *player.runtime->controller;
    return layer >= 0 && (size_t)layer < controller.layers.size() && controller.layers[(size_t)layer].transition.active;
}
