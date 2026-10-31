// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ActionSystem's Animator Controller driver (anim layers spec 11.2): once per update, before
// the sampling, for every layer of a player with a controller: the parameters' damping, the
// states' normalized times, at most one transition (Any State first, then the current state's;
// during a transition only what its interruption source allows), the transition weights, the
// 1D blend trees, the player states' weights and clip times, and the events of the state that
// is not fading out (in a blend tree, only its heaviest child's).

#include "ActionSystem.h"
#include "ActionSystemPlayersDetail.h"

#include "Scene.h"
#include "Log.h"

#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorController.h"
#include "animation/KeyframeSampling.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    using namespace jasperengine::anim;

    // Unity's Mathf.SmoothDamp (critically damped, no max speed)
    float smoothDamp(float current, float target, float& velocity, float smoothTime, float dt){
        if (!(dt > 0.0f)) return current;
        smoothTime = std::max(0.0001f, smoothTime);
        const float omega = 2.0f / smoothTime;
        const float x = omega * dt;
        const float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        const float change = current - target;
        const float originalTo = target;
        const float temp = (velocity + omega * change) * dt;
        velocity = (velocity - omega * temp) * exp;
        float output = (current - change) + (change + temp) * exp;
        if ((originalTo - current > 0.0f) == (output > originalTo)){
            output = originalTo;
            velocity = 0.0f;
        }
        return output;
    }

    // a normalized time as the time in a clip of `length` under `wrap`
    float clipTimeOf(double normalized, float length, ClipWrapMode wrap){
        if (length <= 0.0f) return 0.0f;
        if (wrap == ClipWrapMode::Loop){
            double t = normalized - std::floor(normalized);
            return (float)(t * length);
        }
        if (wrap == ClipWrapMode::PingPong){
            double t = normalized - 2.0 * std::floor(normalized / 2.0);
            return (float)((t <= 1.0 ? t : 2.0 - t) * length);
        }
        return (float)(std::min(std::max(normalized, 0.0), 1.0) * length);
    }

    bool conditionHolds(const ControllerRuntime& controller, const AnimatorController::Condition& condition){
        if (condition.parameter < 0 || (size_t)condition.parameter >= controller.params.size()) return false;
        const float value = controller.params[(size_t)condition.parameter].value;
        switch (condition.mode){
            case ConditionMode::If: return value != 0.0f;
            case ConditionMode::IfNot: return value == 0.0f;
            case ConditionMode::Greater: return value > condition.threshold;
            case ConditionMode::Less: return value < condition.threshold;
            case ConditionMode::Equals: return std::round(value) == std::round(condition.threshold);
            case ConditionMode::NotEqual: return std::round(value) != std::round(condition.threshold);
        }
        return false;
    }

    // the group's normalized time crossed `exitTime` during this step: below 1 at every loop at
    // that fraction, from 1 once at that value (spec 11.2.1)
    bool crossedExitTime(const ControllerGroup& group, float exitTime){
        const double from = std::min(group.previous, group.time);
        const double to = std::max(group.previous, group.time);
        if (to == from) return false;
        if (exitTime < 1.0f){
            const double k = std::floor(to - exitTime);
            return k >= 0.0 && k + exitTime > from && k + exitTime <= to;
        }
        return exitTime > from && exitTime <= to;
    }

    void pendStateCallback(AnimationPlayerRuntime& runtime, int layer, const std::string& state, bool enter){
        PendingPlayerCallback callback;
        callback.stateEnter = enter;
        callback.stateExit = !enter;
        callback.layer = layer;
        callback.event.state = state;
        runtime.pending.push_back(std::move(callback));
    }

    void stopGroup(AnimationPlayerRuntime& runtime, ControllerGroup& group){
        group.active = false;
        group.weight = 0.0f;
        group.time = 0.0;
        group.previous = 0.0;
        group.entered = false;
        for (size_t index : group.states){
            if (index < runtime.states.size()) stopState(runtime.states[index]);
        }
    }

    void enterGroup(ControllerGroup& group, double time){
        group.active = true;
        group.weight = 0.0f;
        group.time = time;
        group.previous = time;
        group.entered = true;
    }

}

void ActionSystem::driveController(Entity entity, AnimationPlayerComponent& player, float dt){
    anim::AnimationPlayerRuntime& runtime = *player.runtime;
    anim::ControllerRuntime& controller = *runtime.controller;
    if (!controller.controller || controller.stopped) return;
    const AnimatorController& ac = *controller.controller;
    const ControllerData& data = ac.getData();
    (void)entity;

    // parameters damped by setFloat(name, v, dampTime)
    for (anim::ControllerParamValue& param : controller.params){
        if (param.damping) param.value = smoothDamp(param.value, param.target, param.velocity, param.dampTime, dt);
    }
    const float playerSpeed = std::isfinite(player.speed) ? player.speed : 0.0f;
    // reused between players and frames (no allocation per frame)
    std::vector<float>& weights = controllerWeights;
    std::vector<float>& thresholds = controllerThresholds;

    for (size_t l = 0; l < controller.layers.size() && l < ac.getLayers().size(); l++){
        anim::ControllerLayerRuntime& layer = controller.layers[l];
        const AnimatorController::Layer& layerData = ac.getLayers()[l];
        if (layerData.states.empty() || layer.groups.size() != layerData.states.size()) continue;
        const int li = (int)l;

        // start: the default state at weight 1 (spec 11.2 Start)
        if (!layer.started){
            const int start = layerData.defaultState;
            for (anim::ControllerGroup& group : layer.groups) stopGroup(runtime, group);
            layer.transition = anim::ControllerTransitionRuntime();
            enterGroup(layer.groups[(size_t)start], layerData.states[(size_t)start].data->cycleOffset);
            layer.groups[(size_t)start].weight = 1.0f;
            layer.current = start;
            layer.lastEntered = start;
            layer.started = true;
            pendStateCallback(runtime, li, layerData.states[(size_t)start].data->name, true);
        }

        // normalized times
        for (size_t g = 0; g < layer.groups.size(); g++){
            anim::ControllerGroup& group = layer.groups[g];
            group.previous = group.time;
            if (!group.active || group.entered) continue;
            const float length = anim::controllerStateLength(runtime, li, (int)g);
            if (length > 0.0f) group.time += (double)dt * anim::controllerStateSpeed(runtime, li, (int)g) * playerSpeed / length;
        }
        if (layer.transition.active) layer.transition.elapsed += dt;

        // at most one transition this update
        auto fires = [&](int index, int fromState) -> bool {
            const AnimatorController::Transition& transition = layerData.transitions[(size_t)index];
            for (const AnimatorController::Condition& condition : transition.conditions){
                if (!conditionHolds(controller, condition)) return false;
            }
            if (transition.data->hasExitTime){
                if (fromState < 0 || !crossedExitTime(layer.groups[(size_t)fromState], transition.data->exitTime)) return false;
            }
            return true;
        };
        int chosen = -1;
        auto pick = [&](const std::vector<int>& list, int fromState, bool any){
            for (int index : list){
                if (chosen >= 0) return;
                const AnimatorController::Transition& transition = layerData.transitions[(size_t)index];
                if (any){
                    if (layer.transition.active && transition.to == layer.transition.to) continue;
                    if (!layer.transition.active && transition.to == layer.current && !transition.data->canTransitionToSelf) continue;
                }else if (layer.transition.active && transition.to == layer.transition.to){
                    continue;
                }
                if (fires(index, fromState)) chosen = index;
            }
        };
        TransitionInterruption interruption = TransitionInterruption::None;
        if (layer.transition.active && layer.transition.transition >= 0){
            interruption = layerData.transitions[(size_t)layer.transition.transition].data->interruption;
        }
        if (!layer.transition.active){
            pick(layerData.anyTransitions, layer.current, true);
            if (layer.current >= 0) pick(layerData.states[(size_t)layer.current].transitions, layer.current, false);
        }else if (interruption != TransitionInterruption::None){
            const int exitState = layer.transition.to;
            pick(layerData.anyTransitions, exitState, true);
            if (interruption == TransitionInterruption::CurrentState && layer.current >= 0){
                pick(layerData.states[(size_t)layer.current].transitions, layer.current, false);
            }else if (interruption == TransitionInterruption::NextState){
                pick(layerData.states[(size_t)layer.transition.to].transitions, layer.transition.to, false);
            }
        }

        if (chosen >= 0){
            const AnimatorController::Transition& transition = layerData.transitions[(size_t)chosen];
            const ControllerTransition& transitionData = *transition.data;
            // the source of its duration and the state whose transitions are checked next: the
            // transition's own `from` (Any State: the current state)
            const int source = transition.from >= 0 ? transition.from : layer.current;
            if (layer.transition.active && interruption == TransitionInterruption::NextState && transition.from == layer.transition.to){
                layer.current = transition.from;
            }
            anim::ControllerTransitionRuntime started;
            started.active = true;
            started.to = transition.to;
            started.transition = chosen;
            const float sourceLength = source >= 0 ? anim::controllerStateLength(runtime, li, source) : 0.0f;
            started.duration = transitionData.fixedDuration ? transitionData.duration : transitionData.duration * sourceLength;
            // every state playing now keeps its weight at the start, scaled by (1 - x)
            for (size_t g = 0; g < layer.groups.size(); g++){
                anim::ControllerGroup& group = layer.groups[g];
                if ((int)g == transition.to){
                    if (group.active) stopGroup(runtime, group);
                    continue;
                }
                if (group.active && group.weight > 0.0f) started.sources.push_back({(int)g, group.weight});
                else if (group.active) stopGroup(runtime, group);
            }
            const ControllerState& destination = *layerData.states[(size_t)transition.to].data;
            enterGroup(layer.groups[(size_t)transition.to], transitionData.offset > 0.0f ? transitionData.offset : destination.cycleOffset);
            if (!layer.transition.active && layer.current < 0) layer.current = source;
            layer.transition = started;
            if (layer.lastEntered >= 0) pendStateCallback(runtime, li, layerData.states[(size_t)layer.lastEntered].data->name, false);
            pendStateCallback(runtime, li, destination.name, true);
            layer.lastEntered = transition.to;
            // the triggers it read are consumed
            for (const AnimatorController::Condition& condition : transition.conditions){
                if (condition.parameter >= 0 && data.parameters[(size_t)condition.parameter].type == ControllerParameterType::Trigger){
                    controller.params[(size_t)condition.parameter].value = 0.0f;
                }
            }
        }

        // weights: the destination x, the sources (1 - x) of their start weights; at 1 the
        // sources stop and rewind
        if (layer.transition.active){
            anim::ControllerTransitionRuntime& transition = layer.transition;
            const float x = transition.duration > 0.0f ? std::min(1.0f, std::max(0.0f, transition.elapsed / transition.duration)) : 1.0f;
            if (x >= 1.0f){
                for (size_t g = 0; g < layer.groups.size(); g++){
                    if ((int)g != transition.to && layer.groups[g].active) stopGroup(runtime, layer.groups[g]);
                }
                layer.groups[(size_t)transition.to].weight = 1.0f;
                layer.current = transition.to;
                layer.transition = anim::ControllerTransitionRuntime();
            }else{
                layer.groups[(size_t)transition.to].weight = x;
                for (const std::pair<int, float>& entry : transition.sources){
                    layer.groups[(size_t)entry.first].weight = entry.second * (1.0f - x);
                }
            }
        }else if (layer.current >= 0){
            layer.groups[(size_t)layer.current].weight = 1.0f;
        }

        // the player states: weights and clip times; events of the state that is not fading out
        const int playing = layer.transition.active ? layer.transition.to : layer.current;
        const float lambda = runtime.layerWeight(li);
        for (size_t g = 0; g < layer.groups.size(); g++){
            anim::ControllerGroup& group = layer.groups[g];
            const AnimatorController::State& stateData = layerData.states[g];
            if (!group.active){
                continue;
            }
            const ControllerState& state = *stateData.data;
            const size_t children = group.states.size();
            weights.assign(children, children == 1 ? 1.0f : 0.0f);
            if (state.hasBlendTree){
                thresholds.clear();
                for (const BlendTreeChild& child : state.blendTree.children) thresholds.push_back(child.threshold);
                const float value = stateData.blendParameter >= 0 ? controller.params[(size_t)stateData.blendParameter].value : 0.0f;
                anim::blendTreeWeights(thresholds, value, weights);
                weights.resize(children, 0.0f);
            }
            size_t heaviest = 0;
            for (size_t c = 1; c < children; c++){
                if (weights[c] > weights[heaviest]) heaviest = c;
            }
            for (size_t c = 0; c < children; c++){
                anim::PlayerStateRuntime& playerState = runtime.states[group.states[c]];
                const float length = anim::stateLength(playerState);
                const ClipWrapMode wrap = anim::stateWrap(playerState);
                const float weight = group.weight * weights[c];
                const float clipTime = clipTimeOf(group.time, length, wrap);
                playerState.weight = weight;
                playerState.targetWeight = weight;
                playerState.fadeSpeed = 0.0f;
                playerState.enabled = weight > 0.0f;
                playerState.playing = playerState.enabled;
                playerState.held = false;
                playerState.finishing = false;
                playerState.time = clipTime;
                playerState.drivenTime = clipTime;

                // events (R16 with the layer rule): the playing state's heaviest child only
                if ((int)g != playing || c != heaviest || !(weight > 0.0f) || !(lambda > 0.0f) || !playerState.bindings.clip || length <= 0.0f) continue;
                const AnimationClip& clip = *playerState.bindings.clip;
                if (clip.getEvents().empty()) continue;
                double from = 0.0, to = 0.0;
                if (wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong){
                    const double period = wrap == ClipWrapMode::PingPong ? 2.0 : 1.0;
                    const double base = period * std::floor(std::min(group.previous, group.time) / period);
                    from = (group.previous - base) * length;
                    to = (group.time - base) * length;
                }else{
                    from = std::min(std::max(group.previous, 0.0), 1.0) * length;
                    to = std::min(std::max(group.time, 0.0), 1.0) * length;
                }
                if (from == to && !group.entered) continue;
                std::vector<anim::CrossedEvent> crossed;
                anim::collectCrossedEvents(clip, wrap, (float)from, (float)to, group.entered, crossed);
                for (const anim::CrossedEvent& event : crossed){
                    const ClipEventData& eventData = clip.getEvents()[event.index];
                    anim::PendingPlayerCallback callback;
                    callback.event.functionName = eventData.function;
                    callback.event.args = eventData.args;
                    callback.event.state = playerState.name;
                    callback.event.time = eventData.time;
                    callback.when = to != from ? dt * (float)std::min(1.0, std::max(0.0, (event.position - from) / (to - from))) : 0.0f;
                    callback.clip = playerState.clip;
                    runtime.pending.push_back(std::move(callback));
                }
            }
            group.entered = false;
        }
    }
}
