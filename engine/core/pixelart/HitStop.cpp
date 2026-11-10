// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/HitStop.h"

#include "Engine.h"
#include "Scene.h"
#include "camera/CameraShake.h"
#include "camera/ShakePreset.h"
#include "component/HitStopExemptComponent.h"
#include "component/Transform.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace{

    // what is left of a hit after a frame, below which it is over (float noise of fixed deltas)
    constexpr double kHitStopEpsilon = 1e-6;

    struct HitStopState{
        double remaining = 0.0;
        float scale = 1.0f;
        bool onlyMode = false;
        std::vector<HitStopTarget> exclude;
        std::vector<HitStopTarget> only;

        // this frame's values (beginFrame)
        double frameFactor = 1.0;
        double worldFactor = 1.0;
        bool frameActive = false;
        double unfrozenDelta = 0.0;
        uint64_t frame = 0;

        HitStopStats stats;

        Scene* scriptScene = nullptr;
        Entity scriptEntity = NULL_ENTITY;
    };

    HitStopState& state(){
        static HitStopState value;
        return value;
    }

    // One scene's resolved flags for one frame: the GameObjects whose own Hit Stop Exempt counts
    // (with or without its children), then every answer cached, so a frame walks each parent
    // chain once
    struct SceneFlags{
        uint64_t frame = UINT64_MAX;
        std::unordered_map<Entity, bool> exemptRoots;     // entity -> includeChildren
        std::unordered_set<Entity> excludeRoots;
        std::unordered_set<Entity> onlyRoots;
        std::unordered_map<Entity, bool> exemptCache;
        std::unordered_map<Entity, bool> onlyCache;
    };

    std::unordered_map<Scene*, SceneFlags>& sceneFlags(){
        static std::unordered_map<Scene*, SceneFlags> value;
        return value;
    }

    SceneFlags& resolve(Scene* scene){
        HitStopState& s = state();
        SceneFlags& flags = sceneFlags()[scene];
        if (flags.frame == s.frame) return flags;
        flags.frame = s.frame;
        flags.exemptRoots.clear();
        flags.excludeRoots.clear();
        flags.onlyRoots.clear();
        flags.exemptCache.clear();
        flags.onlyCache.clear();
        auto exempts = scene->getComponentArray<HitStopExemptComponent>();
        for (size_t i = 0; i < exempts->size(); i++){
            const HitStopExemptComponent& exempt = exempts->getComponentFromIndex(i);
            if (exempt.enabled) flags.exemptRoots[exempts->getEntity(i)] = exempt.includeChildren;
        }
        for (const HitStopTarget& target : s.exclude){
            if (target.scene == scene && target.entity != NULL_ENTITY) flags.excludeRoots.insert(target.entity);
        }
        for (const HitStopTarget& target : s.only){
            if (target.scene == scene && target.entity != NULL_ENTITY) flags.onlyRoots.insert(target.entity);
        }
        return flags;
    }

    Entity parentOf(Scene* scene, Entity entity){
        const Transform* transform = scene->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

    bool exemptIn(Scene* scene, SceneFlags& flags, Entity entity){
        if (flags.exemptRoots.empty() && flags.excludeRoots.empty()) return false;
        auto cached = flags.exemptCache.find(entity);
        if (cached != flags.exemptCache.end()) return cached->second;
        bool result = false;
        // a guard against a broken (cyclic) hierarchy
        int depth = 0;
        for (Entity e = entity; e != NULL_ENTITY && depth < 1024; e = parentOf(scene, e), depth++){
            auto root = flags.exemptRoots.find(e);
            if (root != flags.exemptRoots.end() && (e == entity || root->second)){
                result = true;
                break;
            }
            if (flags.excludeRoots.count(e)){
                result = true;
                break;
            }
        }
        flags.exemptCache[entity] = result;
        return result;
    }

    bool onlyIn(Scene* scene, SceneFlags& flags, Entity entity){
        if (flags.onlyRoots.empty()) return false;
        auto cached = flags.onlyCache.find(entity);
        if (cached != flags.onlyCache.end()) return cached->second;
        bool result = false;
        int depth = 0;
        for (Entity e = entity; e != NULL_ENTITY && depth < 1024; e = parentOf(scene, e), depth++){
            if (flags.onlyRoots.count(e)){
                result = true;
                break;
            }
        }
        flags.onlyCache[entity] = result;
        return result;
    }

    void appendTargets(std::vector<HitStopTarget>& to, const std::vector<HitStopTarget>& from){
        for (const HitStopTarget& target : from){
            const bool known = std::any_of(to.begin(), to.end(), [&](const HitStopTarget& t){
                return t.scene == target.scene && t.entity == target.entity;
            });
            if (!known) to.push_back(target);
        }
    }

}

void HitStop::play(float seconds){
    play(seconds, Options());
}

void HitStop::play(float seconds, const Options& options){
    HitStopState& s = state();
    if (!std::isfinite(seconds) || seconds <= 0.0f) return;
    float scale = std::isfinite(options.timeScale) ? options.timeScale : 0.0f;
    scale = std::min(1.0f, std::max(0.0f, scale));

    s.stats.requests++;
    s.stats.totalSeconds += seconds;
    s.stats.longestSeconds = std::max(s.stats.longestSeconds, (double)seconds);

    const bool running = s.remaining > kHitStopEpsilon;
    const bool onlyMode = !options.only.empty();
    if (running){
        s.remaining = std::max(s.remaining, (double)seconds);
        s.scale = std::min(s.scale, scale);
        if (onlyMode != s.onlyMode){
            s.exclude.clear();
            s.only.clear();
        }
    }else{
        s.remaining = seconds;
        s.scale = scale;
        s.exclude.clear();
        s.only.clear();
    }
    s.onlyMode = onlyMode;
    appendTargets(s.exclude, options.exclude);
    appendTargets(s.only, options.only);
    // the lists changed: this frame's flags are resolved again
    s.frame++;

    // the shake runs on unscaled time, so it shakes during the freeze (pixelfx act.js)
    ShakeOptions shakeOptions;
    shakeOptions.unscaled = 1;
    if (options.shakeAmplitudePx > 0.0f && options.shakeSeconds > 0.0f){
        CameraShake::shake(ShakeSource(camera::makeKickPreset(options.shakeAmplitudePx, options.shakeSeconds)), shakeOptions);
    }else if (!options.shakePreset.empty()){
        CameraShake::shake(ShakeSource(options.shakePreset), shakeOptions);
    }
}

void HitStop::stop(){
    HitStopState& s = state();
    s.remaining = 0.0;
    s.scale = 1.0f;
    s.onlyMode = false;
    s.exclude.clear();
    s.only.clear();
    s.frame++;
}

bool HitStop::isActive(){
    return state().remaining > kHitStopEpsilon;
}

float HitStop::getRemaining(){
    const double remaining = state().remaining;
    return remaining > kHitStopEpsilon ? (float)remaining : 0.0f;
}

float HitStop::getFactor(){
    const HitStopState& s = state();
    return s.remaining > kHitStopEpsilon ? s.scale : 1.0f;
}

double HitStop::getUnfrozenDeltaTime(){
    return state().unfrozenDelta;
}

bool HitStop::isFrameActive(){
    return state().frameActive;
}

bool HitStop::isExempt(Scene* scene, Entity entity){
    const HitStopState& s = state();
    if (!scene || entity == NULL_ENTITY || (!s.frameActive && s.remaining <= kHitStopEpsilon)) return false;
    return exemptIn(scene, resolve(scene), entity);
}

double HitStop::deltaFor(Scene* scene, Entity entity, double dt){
    const HitStopState& s = state();
    if (!s.frameActive || !scene || entity == NULL_ENTITY) return dt;
    SceneFlags& flags = resolve(scene);
    if (s.onlyMode){
        // an exempt GameObject inside an `only` target still runs
        if (onlyIn(scene, flags, entity) && !exemptIn(scene, flags, entity)) return dt * s.frameFactor;
        return dt;
    }
    if (!exemptIn(scene, flags, entity)) return dt;
    // the delta the system got, without this frame's factor; frozen outright, the engine's
    return s.frameFactor > 0.0 ? dt / s.frameFactor : s.unfrozenDelta;
}

double HitStop::beginFrame(double unscaledDelta, double unfrozenDelta){
    HitStopState& s = state();
    s.unfrozenDelta = unfrozenDelta;
    if (s.remaining <= kHitStopEpsilon){
        // idle: one compare (R35)
        if (s.frameActive){
            s.frameActive = false;
            s.frameFactor = 1.0;
            s.worldFactor = 1.0;
            s.remaining = 0.0;
            s.exclude.clear();
            s.only.clear();
            s.onlyMode = false;
            sceneFlags().clear();
        }
        return 1.0;
    }
    s.frame++;
    s.frameActive = true;
    if (unscaledDelta > 0.0){
        // the part of the frame the hit covers runs at its scale, the rest at 1
        const double covered = std::min(s.remaining, unscaledDelta);
        s.frameFactor = (s.scale * covered + (unscaledDelta - covered)) / unscaledDelta;
        s.remaining -= covered;
        if (s.remaining <= kHitStopEpsilon) s.remaining = 0.0;
    }else{
        s.frameFactor = s.scale;
    }
    s.worldFactor = s.onlyMode ? 1.0 : s.frameFactor;
    return s.worldFactor;
}

double HitStop::getWorldFactor(){
    return state().frameActive ? state().worldFactor : 1.0;
}

HitStop::ScriptScope::ScriptScope(Scene* scene, Entity entity){
    HitStopState& s = state();
    previousScene = s.scriptScene;
    previousEntity = s.scriptEntity;
    s.scriptScene = scene;
    s.scriptEntity = entity;
}

HitStop::ScriptScope::~ScriptScope(){
    HitStopState& s = state();
    s.scriptScene = previousScene;
    s.scriptEntity = previousEntity;
}

double HitStop::getScriptDeltaTime(){
    const HitStopState& s = state();
    const double dt = Engine::getDeltatime();
    if (!s.frameActive || !s.scriptScene) return dt;
    return deltaFor(s.scriptScene, s.scriptEntity, dt);
}

HitStopStats HitStop::getStats(){
    return state().stats;
}

void HitStop::resetStats(){
    state().stats = HitStopStats();
}

void HitStop::reset(){
    HitStopState& s = state();
    const uint64_t frame = s.frame + 1;
    s = HitStopState();
    s.frame = frame;
    sceneFlags().clear();
}
