// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationPlayerRuntime.h"

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

void anim::ClipPreviewSession::setRest(Entity entity, const ChannelRest& value){
    rest[entity] = value;
}

void anim::ClipPreviewSession::reset(){
    root = NULL_ENTITY;
    rest.clear();
    bindings.clear();
    written.clear();
    previousTimes.clear();
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

    void clearQueue(AnimationPlayerRuntime& runtime){
        runtime.queue.clear();
        for (PlayerStateRuntime& state : runtime.states){
            state.stopAtCycleEnd = false;
        }
    }

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

anim::AnimationPlayerRuntime& anim::playerRuntime(AnimationPlayerComponent& player){
    if (!player.runtime){
        player.runtime = std::make_shared<AnimationPlayerRuntime>();
        player.needsLoad = true;
    }
    AnimationPlayerRuntime& runtime = *player.runtime;
    if (player.needsLoad){
        player.needsLoad = false;
        // one state per entry; a surviving name keeps its playback, and its loaded clip and
        // bindings while the entry names the same clip
        std::vector<std::string> names = playerStateNames(player.clips);
        std::vector<PlayerStateRuntime> states(player.clips.size());
        for (size_t i = 0; i < player.clips.size(); i++){
            PlayerStateRuntime& state = states[i];
            int old = runtime.findState(names[i]);
            if (old >= 0){
                state = runtime.states[(size_t)old];
                if (state.clip != player.clips[i].clip){
                    state.bindings = ClipBindings();
                    state.loadFailed = false;
                    state.retryTimer = 0.0f;
                    state.errorLogged = false;
                    state.error.clear();
                }
            }
            state.name = names[i];
            state.entry = i;
            state.clip = player.clips[i].clip;
            state.entryWrap = player.clips[i].wrap;
        }
        // queued states follow their names
        std::deque<QueuedState> queue;
        for (const QueuedState& queued : runtime.queue){
            if (queued.state >= runtime.states.size()) continue;
            auto it = std::find(names.begin(), names.end(), runtime.states[queued.state].name);
            if (it != names.end()){
                QueuedState moved = queued;
                moved.state = (size_t)(it - names.begin());
                queue.push_back(moved);
            }
        }
        runtime.states = std::move(states);
        runtime.queue = std::move(queue);
        runtime.warnedStates.clear();
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

bool anim::playState(AnimationPlayerComponent& player, const std::string& name, float fade, const std::string& who){
    PlayerStateRuntime* target = findNamed(player, name, who);
    if (!target) return false;
    player.started = true;
    AnimationPlayerRuntime& runtime = *player.runtime;
    clearQueue(runtime);
    fade = finiteOr(fade, 0.0f);
    if (fade <= 0.0f){
        // Unity Play: the others stop (StopSameLayer), the state plays from its start at weight 1
        for (PlayerStateRuntime& state : runtime.states){
            if (&state != target) stopState(state);
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
        if (&state == target || (!state.enabled && !state.held)) continue;
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
    bool playing = false;
    for (const PlayerStateRuntime& state : runtime.states){
        if (state.enabled && state.targetWeight > 0.0f) playing = true;
    }
    // with nothing playing it plays now
    if (!playing){
        return playState(player, name, fade, who);
    }
    QueuedState queued;
    queued.state = (size_t)(target - runtime.states.data());
    queued.fade = std::max(0.0f, finiteOr(fade, 0.0f));
    runtime.queue.push_back(queued);
    return true;
}

void anim::stopStates(AnimationPlayerComponent& player, const std::string* name, const std::string& who){
    if (name){
        PlayerStateRuntime* target = findNamed(player, *name, who);
        if (target){
            stopState(*target);
            // R17: stop clears the queue
            clearQueue(*player.runtime);
            player.started = true;
        }
        return;
    }
    player.started = true;
    AnimationPlayerRuntime& runtime = playerRuntime(player);
    clearQueue(runtime);
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
    clearQueue(runtime);
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
