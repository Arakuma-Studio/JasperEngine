// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ActionSystem's Animation Players (animation clips spec 3.3, 9.R1/R2/R9/R10/R16-R21): loading
// and binding, states, fades, queue, events, the rest term in the pose blend, edit-mode preview.

#include "ActionSystem.h"

#include "Scene.h"
#include "Log.h"

#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "animation/KeyframeSampling.h"
#include "lattice/LatticeSolver.h"
#include "pool/AnimationClipPool.h"
#include "script/LuaBinding.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;

namespace {

    // a clip that failed to load is tried again this often (PoseAnimator's pace)
    constexpr float FAILED_RETRY_SECONDS = 1.0f;
    // unresolved paths are looked up again at most this often after hierarchy changes (R9)
    constexpr float UNRESOLVED_RETRY_SECONDS = 0.25f;

    // the entity's place in the Transform array (SIZE_MAX without a Transform): one lookup
    size_t transformIndex(Scene* scene, Entity entity){
        auto transforms = scene->getComponentArray<Transform>();
        Transform* transform = entity == NULL_ENTITY ? nullptr : transforms->findComponent(entity);
        return transform ? (size_t)(transform - transforms->findComponentFromIndex(0)) : SIZE_MAX;
    }

    Entity parentOf(Scene* scene, Entity entity){
        Transform* transform = scene->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

    // a path's entity, the paths it goes through resolved once for all tracks
    Entity resolveCached(Scene* scene, Entity root, const std::string& path, std::unordered_map<std::string, Entity>& cache){
        if (path.empty()) return root;
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
        Entity entity = NULL_ENTITY;
        const size_t slash = path.rfind('/');
        if (slash == std::string::npos){
            entity = anim::resolvePath(scene, root, path);
        }else{
            Entity parent = resolveCached(scene, root, path.substr(0, slash), cache);
            if (parent != NULL_ENTITY && slash + 1 < path.size()){
                entity = anim::resolvePath(scene, parent, path.substr(slash + 1));
            }
        }
        cache[path] = entity;
        return entity;
    }

    // the scene value of the channels of `entity` (R2) that have none yet
    void completeRest(Scene* scene, Entity entity, anim::ChannelRest& rest, ClipChannel channel){
        switch (channel){
            case ClipChannel::Position:
            case ClipChannel::Rotation:
            case ClipChannel::Scale:
                if (!rest.hasTransform){
                    if (Transform* transform = scene->findComponent<Transform>(entity)){
                        rest.hasTransform = true;
                        rest.position = transform->position;
                        rest.rotation = transform->rotation;
                        rest.scale = transform->scale;
                    }
                }
                break;
            case ClipChannel::Morph:
                if (!rest.hasMorph){
                    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
                        rest.hasMorph = true;
                        rest.morphWeights.assign(mesh->morphWeights, mesh->morphWeights + MAX_MORPHTARGETS);
                    }
                }
                break;
            case ClipChannel::Lattice:
                if (!rest.hasLattice){
                    if (LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(entity)){
                        rest.hasLattice = true;
                        const size_t pointCount = (size_t)lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
                        rest.latticeOffsets.assign(pointCount, Vector3(0, 0, 0));
                        for (size_t i = 0; i < pointCount && i < latticecomp->offsets.size(); i++){
                            rest.latticeOffsets[i] = latticecomp->offsets[i];
                        }
                    }
                }
                break;
            case ClipChannel::Property:
                break;
        }
    }

    // the entity's rest entry (made on its first use)
    anim::ChannelRest& restOf(Entity entity, anim::RestMap& rest){
        return rest[entity];
    }

    size_t sampledTrackCount(const AnimationClip& clip){
        size_t count = 0;
        for (const AnimationClip::Track& track : clip.getTracks()){
            if (track.isSampled()) count++;
        }
        return count;
    }

    // Every track of the bindings' clip resolved again from `root`; the rest of every newly bound
    // channel is taken now, before anything writes it
    void resolveBindings(Scene* scene, Entity root, anim::ClipBindings& bindings, anim::RestMap* rest){
        const std::vector<AnimationClip::Track>& tracks = bindings.clip->getTracks();
        const size_t count = tracks.size();
        bindings.targets.assign(count, NULL_ENTITY);
        bindings.boundNames.assign(count, std::string());
        bindings.boundParents.assign(count, NULL_ENTITY);
        bindings.boundIndices.assign(count, SIZE_MAX);
        bindings.cursors.assign(count, anim::KeyCursor());
        bindings.resolvedCount = 0;
        std::unordered_map<std::string, Entity> cache;
        for (size_t i = 0; i < count; i++){
            if (!tracks[i].isSampled()) continue;
            Entity target = resolveCached(scene, root, tracks[i].path, cache);
            if (target == NULL_ENTITY) continue;
            bindings.targets[i] = target;
            bindings.boundNames[i] = scene->getEntityName(target);
            bindings.boundParents[i] = parentOf(scene, target);
            bindings.boundIndices[i] = transformIndex(scene, target);
            bindings.resolvedCount++;
            if (rest) completeRest(scene, target, restOf(target, *rest), tracks[i].channel);
        }
        // every entity a resolved path went through (the targets and their ancestors)
        bindings.pathEntities.clear();
        bindings.pathNames.clear();
        bindings.pathParents.clear();
        bindings.pathIndices.clear();
        for (const auto& entry : cache){
            if (entry.second == NULL_ENTITY) continue;
            bindings.pathEntities.push_back(entry.second);
        }
        std::sort(bindings.pathEntities.begin(), bindings.pathEntities.end());
        bindings.pathEntities.erase(std::unique(bindings.pathEntities.begin(), bindings.pathEntities.end()), bindings.pathEntities.end());
        for (Entity entity : bindings.pathEntities){
            bindings.pathNames.push_back(scene->getEntityName(entity));
            bindings.pathParents.push_back(parentOf(scene, entity));
            bindings.pathIndices.push_back(transformIndex(scene, entity));
        }
        bindings.rootIndex = transformIndex(scene, root);
        bindings.hierarchyRevision = scene->hierarchyRevision();
        bindings.nameRevision = scene->nameRevision();
        bindings.resolved = true;
    }

    // R9's validation: every entity on a bound path alive, with the parent it had, moved in the
    // Transform array exactly as the root (a change elsewhere), and with its name (checked when a
    // name under a parent changed). `failed` gets the ones that changed. True: the bindings hold
    // (their indices follow the shift).
    bool validateBindings(Scene* scene, Entity root, anim::ClipBindings& bindings, std::vector<Entity>& failed){
        const size_t rootIndex = transformIndex(scene, root);
        if ((rootIndex == SIZE_MAX) != (bindings.rootIndex == SIZE_MAX)) return false;
        const long long shift = rootIndex == SIZE_MAX ? 0 : (long long)rootIndex - (long long)bindings.rootIndex;
        const bool checkNames = bindings.nameRevision != scene->nameRevision();
        auto transforms = scene->getComponentArray<Transform>();
        Transform* first = transforms->findComponentFromIndex(0);
        bool valid = true;
        for (size_t i = 0; i < bindings.pathEntities.size(); i++){
            const Entity entity = bindings.pathEntities[i];
            Transform* transform = transforms->findComponent(entity);
            bool ok = transform && transform->parent == bindings.pathParents[i] &&
                      (long long)(transform - first) - (long long)bindings.pathIndices[i] == shift;
            if (ok && checkNames){
                ok = scene->getEntityName(entity) == bindings.pathNames[i];
            }
            if (!ok){
                failed.push_back(entity);
                valid = false;
            }
        }
        if (!valid) return false;
        for (size_t& index : bindings.pathIndices){
            index = (size_t)((long long)index + shift);
        }
        for (size_t& index : bindings.boundIndices){
            if (index != SIZE_MAX) index = (size_t)((long long)index + shift);
        }
        bindings.rootIndex = rootIndex;
        bindings.nameRevision = scene->nameRevision();
        return true;
    }

    // the ModelComponents on and under the root, with the file each has loaded (R10)
    void collectModels(Scene* scene, Entity root, anim::AnimationPlayerRuntime& runtime){
        runtime.modelFiles.clear();
        auto models = scene->getComponentArray<ModelComponent>();
        if (models->size() == 0) return;
        if (ModelComponent* model = scene->findComponent<ModelComponent>(root)){
            runtime.modelFiles[root] = model->loadedFilename;
        }
        const size_t rootIndex = transformIndex(scene, root);
        if (rootIndex == SIZE_MAX) return;
        const size_t last = scene->findBranchLastIndex(root);
        for (size_t i = 0; i < models->size(); i++){
            const Entity entity = models->getEntity(i);
            const size_t index = transformIndex(scene, entity);
            if (entity != root && index != SIZE_MAX && index > rootIndex && index <= last){
                runtime.modelFiles[entity] = models->getComponentFromIndex(i).loadedFilename;
            }
        }
    }

    // seconds until the state reaches its next end at its speed (R17): once / clamp_forever the
    // clip's end, loop / ping-pong the end of the current cycle; infinite when it never does
    float remainingSeconds(const AnimationPlayerComponent& player, const anim::PlayerStateRuntime& state){
        const float speed = anim::effectiveSpeed(player, state);
        if (!state.bindings.clip || speed == 0.0f) return std::numeric_limits<float>::infinity();
        const double length = state.bindings.clip->getLength();
        const double time = state.time;
        const double rate = std::abs(speed);
        ClipWrapMode wrap = anim::stateWrap(state);
        if ((wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong) && length > 0.0){
            const double boundary = speed > 0.0f ? (std::floor(time / length) + 1.0) * length
                                                 : (std::ceil(time / length) - 1.0) * length;
            return (float)(std::abs(boundary - time) / rate);
        }
        const double end = speed > 0.0f ? length : 0.0;
        return (float)(std::max(0.0, speed > 0.0f ? end - time : time - end) / rate);
    }

    void pendEvent(anim::AnimationPlayerRuntime& runtime, const anim::PlayerStateRuntime& state, const ClipEventData& data, float when){
        anim::PendingPlayerCallback callback;
        callback.event.functionName = data.function;
        callback.event.args = data.args;
        callback.event.state = state.name;
        callback.event.time = data.time;
        callback.when = when;
        callback.clip = state.clip;
        runtime.pending.push_back(std::move(callback));
    }

    void pendFinish(anim::AnimationPlayerRuntime& runtime, const anim::PlayerStateRuntime& state, float when){
        anim::PendingPlayerCallback callback;
        callback.finish = true;
        callback.event.state = state.name;
        callback.when = when;
        callback.clip = state.clip;
        runtime.pending.push_back(std::move(callback));
    }

    // Moves one enabled state's time by `span` seconds of the update starting `offset` seconds
    // into it: wrap, the stop at a cycle end for a queue, events (R16) and finishes (R18)
    void advanceState(const AnimationPlayerComponent& player, anim::AnimationPlayerRuntime& runtime,
                      anim::PlayerStateRuntime& state, float offset, float span){
        if (!state.enabled || !state.bindings.clip) return;
        const AnimationClip& clip = *state.bindings.clip;
        const double length = clip.getLength();
        const ClipWrapMode wrap = anim::stateWrap(state);
        const double delta = (double)span * anim::effectiveSpeed(player, state);
        const double from = state.time;
        double to = from + delta;
        const bool includeStart = state.startedThisUpdate;
        state.startedThisUpdate = false;
        // only the states that are not fading out fire (during a crossfade the incoming one)
        const bool fires = state.weight > 0.0f && state.targetWeight > 0.0f;
        auto when = [&](double position){
            return delta != 0.0 ? offset + (float)((double)span * std::min(1.0, std::max(0.0, (position - from) / delta))) : offset;
        };

        bool finishing = false;
        bool notifyFinish = false;
        double finishTime = 0.0;
        double eventsTo = to;

        if (wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong){
            if (length <= 0.0){
                to = 0.0;
                eventsTo = 0.0;
            }else if (state.stopAtCycleEnd && delta != 0.0){
                const double boundary = delta > 0.0 ? (std::floor(from / length) + 1.0) * length
                                                    : (std::ceil(from / length) - 1.0) * length;
                if (delta > 0.0 ? to >= boundary : to <= boundary){
                    to = boundary;
                    eventsTo = boundary;
                    finishing = true;
                    // the pose at the boundary: the end of the clip going forward in a loop
                    finishTime = (wrap == ClipWrapMode::Loop) ? (delta > 0.0 ? length : 0.0)
                                                              : anim::wrapClipTime((float)boundary, (float)length, wrap);
                }
            }
        }else if (wrap == ClipWrapMode::ClampForever){
            const double clamped = std::min(std::max(to, 0.0), length);
            const bool atEnd = delta > 0.0 ? to >= length : (delta < 0.0 ? to <= 0.0 : (length <= 0.0));
            to = clamped;
            eventsTo = clamped;
            if (atEnd && !state.finishNotified){
                state.finishNotified = true;
                notifyFinish = true;
            }
            if (atEnd && state.stopAtCycleEnd){
                finishing = true;
                finishTime = clamped;
            }
        }else{
            // once: plays to its end, then stops
            if (length <= 0.0 || (delta > 0.0 && to >= length) || (delta < 0.0 && to <= 0.0)){
                to = length <= 0.0 ? 0.0 : (delta < 0.0 ? 0.0 : length);
                eventsTo = to;
                finishing = true;
                notifyFinish = true;
                finishTime = to;
            }
        }

        if (fires && (from != eventsTo || includeStart)){
            std::vector<anim::CrossedEvent> crossed;
            anim::collectCrossedEvents(clip, wrap, (float)from, (float)eventsTo, includeStart, crossed);
            const std::vector<ClipEventData>& events = clip.getEvents();
            for (const anim::CrossedEvent& event : crossed){
                pendEvent(runtime, state, events[event.index], when(event.position));
            }
        }

        if (finishing){
            state.finishing = true;
            state.finishTime = (float)finishTime;
            state.finishWhen = when(eventsTo);
        }
        if (notifyFinish){
            pendFinish(runtime, state, when(eventsTo));
        }

        if ((wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong) && length > 0.0){
            // the stored form: [0, 2 * length) and whole periods (no precision loss over time)
            const double period = 2.0 * length;
            double periods = std::floor(to / period);
            double t = to - periods * period;
            if (t >= period || t < 0.0) t = 0.0;
            if (periods > 0.0) state.cycles += (uint64_t)periods;
            else if (periods < 0.0) state.cycles -= std::min(state.cycles, (uint64_t)(-periods));
            state.time = (float)t;
        }else{
            state.time = (float)to;
        }
    }

    // the time a state is sampled at this update, in its clip
    float sampleTimeOf(const anim::PlayerStateRuntime& state){
        if (state.finishing) return state.finishTime;
        const float length = anim::stateLength(state);
        const ClipWrapMode wrap = anim::stateWrap(state);
        if (wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong){
            return anim::wrapClipTime(state.time, length, wrap);
        }
        return std::min(std::max(state.time, 0.0f), length);
    }

}

void ActionSystem::accumulateClip(const AnimationClip& clip, anim::ClipBindings& bindings, float clipTime, float weight,
                                  std::unordered_map<Entity, anim::ChannelRest>& rest,
                                  std::vector<std::pair<Entity, ClipChannel>>* written){
    if (!(weight > 0.0f)) return;
    const std::vector<AnimationClip::Track>& tracks = clip.getTracks();
    anim::TrackSample sample;
    // the tracks of one path are neighbours: their target's lookups are made once
    Entity current = NULL_ENTITY;
    bool alive = false;
    anim::ChannelRest* currentRest = nullptr;
    TransformBlendAccum* currentAccum = nullptr;
    for (size_t i = 0; i < tracks.size() && i < bindings.targets.size(); i++){
        const AnimationClip::Track& track = tracks[i];
        const Entity target = bindings.targets[i];
        if (target == NULL_ENTITY || !track.isSampled()) continue;
        if (target != current){
            current = target;
            // bound entities are alive: destroying one under a parent bumps the hierarchy
            // revision, and bindings are validated (or resolved again) before sampling
            alive = true;
            currentRest = alive ? &restOf(target, rest) : nullptr;
            currentAccum = nullptr;
        }
        if (!alive) continue;
        anim::ChannelRest& channelRest = *currentRest;
        completeRest(scene, target, channelRest, track.channel);
        if (!anim::sampleTrack(track, clipTime, sample, &bindings.cursors[i])) continue;

        switch (track.channel){
            case ClipChannel::Position: {
                if (!channelRest.hasTransform) continue;
                Vector3 value = sample.vector;
                if (track.relative){
                    value = getRelativeTrackMatrix(target, channelRest.position) * value;
                }
                if (!currentAccum) currentAccum = &transformBlend[target];
                TransformBlendAccum& accum = *currentAccum;
                accum.positionSum = accum.positionSum + value * weight;
                accum.positionWeight += weight;
                if (!accum.hasRestPosition){
                    accum.hasRestPosition = true;
                    accum.restPosition = channelRest.position;
                }
                break;
            }
            case ClipChannel::Scale: {
                if (!channelRest.hasTransform) continue;
                Vector3 value = sample.vector;
                if (track.relative){
                    value = channelRest.scale * value;
                }
                if (!currentAccum) currentAccum = &transformBlend[target];
                TransformBlendAccum& accum = *currentAccum;
                accum.scaleSum = accum.scaleSum + value * weight;
                accum.scaleWeight += weight;
                if (!accum.hasRestScale){
                    accum.hasRestScale = true;
                    accum.restScale = channelRest.scale;
                }
                break;
            }
            case ClipChannel::Rotation: {
                if (!channelRest.hasTransform) continue;
                Quaternion value = sample.rotation;
                if (track.relative){
                    value = channelRest.rotation * value;
                }
                if (!currentAccum) currentAccum = &transformBlend[target];
                TransformBlendAccum& accum = *currentAccum;
                // as the entity tracks: aligned to the first sample's hemisphere, normalized on flush
                if (!accum.hasRotationRef){
                    accum.rotationRef = value;
                    accum.hasRotationRef = true;
                }else if (accum.rotationRef.dot(value) < 0.0f){
                    value = -value;
                }
                accum.rotationSum = accum.rotationSum + value * weight;
                accum.rotationWeight += weight;
                if (!accum.hasRestRotation){
                    accum.hasRestRotation = true;
                    accum.restRotation = channelRest.rotation;
                }
                break;
            }
            case ClipChannel::Morph: {
                if (!channelRest.hasMorph || !scene->findComponent<MeshComponent>(target)) continue;
                const size_t count = std::min(sample.weights.size(), (size_t)MAX_MORPHTARGETS);
                MorphBlendAccum& accum = morphBlend[target];
                for (size_t k = 0; k < count; k++){
                    accum.sums[k] += sample.weights[k] * weight;
                    accum.weights[k] += weight;
                    if (!accum.hasRest[k]){
                        accum.hasRest[k] = true;
                        accum.rest[k] = k < channelRest.morphWeights.size() ? channelRest.morphWeights[k] : 0.0f;
                    }
                }
                break;
            }
            case ClipChannel::Lattice: {
                LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(target);
                if (!channelRest.hasLattice || !latticecomp || !track.points) continue;
                const int pointCount = lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
                LatticeBlendAccum& accum = latticeBlend[target];
                if (!accum.active){
                    accum.sums.assign(pointCount, Vector3(0, 0, 0));
                    accum.weights.assign(pointCount, 0.0f);
                    accum.active = true;
                }else if (accum.sums.size() < (size_t)pointCount){
                    accum.sums.resize(pointCount, Vector3(0, 0, 0));
                    accum.weights.resize(pointCount, 0.0f);
                }
                if (accum.hasRest.size() < (size_t)pointCount){
                    accum.hasRest.resize(pointCount, 0);
                    accum.rest.resize(pointCount, Vector3(0, 0, 0));
                }
                for (size_t n = 0; n < track.width && n < sample.offsets.size(); n++){
                    const int point = track.points[n];
                    // points outside the bound cage are skipped (ClipValidation reports them)
                    if (point < 0 || point >= pointCount) continue;
                    accum.sums[point] += sample.offsets[n] * weight;
                    accum.weights[point] += weight;
                    if (!accum.hasRest[point]){
                        accum.hasRest[point] = 1;
                        accum.rest[point] = (size_t)point < channelRest.latticeOffsets.size() ? channelRest.latticeOffsets[point] : Vector3(0, 0, 0);
                    }
                }
                break;
            }
            case ClipChannel::Property:
                continue;
        }
        if (written){
            std::pair<Entity, ClipChannel> entry(target, track.channel);
            if (std::find(written->begin(), written->end(), entry) == written->end()){
                written->push_back(entry);
            }
        }
    }
}

bool ActionSystem::preparePlayer(Entity entity, AnimationPlayerComponent& player, float dt, bool poolChanged){
    anim::AnimationPlayerRuntime& runtime = anim::playerRuntime(player);
    const uint64_t revision = scene->hierarchyRevision();

    // models under the root: wait for their loads, rebind after a reload (R10). Collected again
    // whenever the bindings are resolved again (the hierarchy under the root changed)
    if (!runtime.modelsCollected){
        collectModels(scene, entity, runtime);
        runtime.modelsCollected = true;
        runtime.modelsRevision = revision;
    }
    bool waiting = false;
    bool reloaded = false;
    bool resolvedAny = false;
    for (auto& model : runtime.modelFiles){
        ModelComponent* component = scene->findComponent<ModelComponent>(model.first);
        if (!component){
            runtime.modelsCollected = false;
            continue;
        }
        if (component->needUpdateModel){
            waiting = true;
        }else if (component->loadedFilename != model.second){
            model.second = component->loadedFilename;
            reloaded = true;
        }
    }
    runtime.waitingForModel = waiting;
    if (waiting) return false;

    for (anim::PlayerStateRuntime& state : runtime.states){
        // the clip: loaded on first use, again after an edit (pool generation), retried after a failure
        bool fetch = !state.bindings.clip;
        if (!fetch && poolChanged && AnimationClipPool::getGeneration(state.clip) != state.bindings.clipGeneration){
            fetch = true;
        }
        if (fetch && state.loadFailed){
            state.retryTimer += dt;
            if (state.retryTimer < FAILED_RETRY_SECONDS) fetch = false;
            else state.retryTimer = 0.0f;
        }
        if (fetch){
            std::string error;
            std::shared_ptr<const AnimationClip> clip = state.clip.empty() ? nullptr : AnimationClipPool::getOrLoad(state.clip, &error);
            if (!clip){
                // an edited clip that no longer loads keeps playing what it was (retried)
                state.loadFailed = true;
                state.error = state.clip.empty() ? "no clip" : error;
                if (!state.errorLogged){
                    state.errorLogged = true;
                    Log::error("Animation Player '%s': state '%s': %s", scene->getEntityName(entity).c_str(),
                               state.name.c_str(), state.error.c_str());
                }
                if (!state.bindings.clip) continue;
            }else{
                const bool changed = clip != state.bindings.clip;
                state.loadFailed = false;
                state.errorLogged = false;
                state.error.clear();
                state.retryTimer = 0.0f;
                state.bindings.clipGeneration = AnimationClipPool::getGeneration(state.clip);
                if (changed){
                    state.bindings.clip = clip;
                    state.bindings.resolved = false;
                }
                if (state.startAtEnd){
                    state.startAtEnd = false;
                    state.time = clip->getLength();
                }
            }
        }
        if (!state.bindings.clip) continue;

        anim::ClipBindings& bindings = state.bindings;
        if (!bindings.resolved || reloaded){
            resolveBindings(scene, entity, bindings, &runtime.rest);
            resolvedAny = true;
            if (bindings.resolvedCount < sampledTrackCount(*bindings.clip)){
                bindings.nextUnresolvedRetry = runtime.clock + UNRESOLVED_RETRY_SECONDS;
            }
        }else if (bindings.hierarchyRevision != revision){
            // validate, re-resolve only what needs it (R9)
            std::vector<Entity> failed;
            if (!validateBindings(scene, entity, bindings, failed)){
                // a renamed or moved entity keeps its rest (R2); a destroyed one's goes
                for (Entity changed : failed){
                    if (!scene->isEntityCreated(changed)) runtime.rest.erase(changed);
                }
                resolveBindings(scene, entity, bindings, &runtime.rest);
                resolvedAny = true;
            }else if (bindings.resolvedCount < sampledTrackCount(*bindings.clip)){
                // unresolved paths may resolve now: at most 4 times a second (the revision is
                // left behind until then, so a later frame retries)
                if (runtime.clock >= bindings.nextUnresolvedRetry){
                    resolveBindings(scene, entity, bindings, &runtime.rest);
                    bindings.nextUnresolvedRetry = runtime.clock + UNRESOLVED_RETRY_SECONDS;
                    resolvedAny = true;
                }
            }else{
                bindings.hierarchyRevision = revision;
            }
        }
    }
    if (resolvedAny && runtime.modelsRevision != revision){
        collectModels(scene, entity, runtime);
        runtime.modelsRevision = revision;
    }
    return true;
}

void ActionSystem::advancePlayer(Entity entity, AnimationPlayerComponent& player, float dt){
    anim::AnimationPlayerRuntime& runtime = *player.runtime;
    // the player's name for warnings (looked up only when a queued state starts)
    auto who = [this, entity](){ return scene->getEntityName(entity); };
    std::vector<anim::PlayerStateRuntime>& states = runtime.states;

    // fades (real time)
    for (anim::PlayerStateRuntime& state : states){
        if (!state.enabled) continue;
        if (state.weight != state.targetWeight){
            const float step = state.fadeSpeed > 0.0f && std::isfinite(state.fadeSpeed) ? state.fadeSpeed * dt : std::abs(state.targetWeight - state.weight);
            if (state.weight < state.targetWeight) state.weight = std::min(state.targetWeight, state.weight + step);
            else state.weight = std::max(state.targetWeight, state.weight - step);
        }
        if (state.weight <= 0.0f && state.targetWeight <= 0.0f && state.stopOnFadeOut){
            anim::stopState(state);
        }
    }

    // the queue's head waits for the states playing now (R17): they stop at their next end
    std::vector<float> offsets(states.size(), 0.0f);
    if (!runtime.queue.empty()){
        const anim::QueuedState head = runtime.queue.front();
        float remaining = 0.0f;
        bool blocked = false;
        bool headPlaying = false;
        for (size_t i = 0; i < states.size(); i++){
            anim::PlayerStateRuntime& state = states[i];
            if (!state.enabled || state.targetWeight <= 0.0f) continue;
            state.stopAtCycleEnd = true;
            blocked = true;
            remaining = std::max(remaining, remainingSeconds(player, state));
            if (i == head.state) headPlaying = true;
        }
        // a fading head starts `fade` seconds before their end and crossfades
        if (blocked && head.fade > 0.0f && !headPlaying && head.state < states.size() &&
            std::isfinite(remaining) && remaining - head.fade <= dt){
            runtime.queue.pop_front();
            std::deque<anim::QueuedState> rest;
            rest.swap(runtime.queue);
            anim::playState(player, states[head.state].name, head.fade, who());
            runtime.queue.swap(rest);
            offsets[head.state] = std::max(0.0f, remaining - head.fade);
            // the others still stop at their end
            for (size_t i = 0; i < states.size(); i++){
                if (i != head.state && states[i].enabled) states[i].stopAtCycleEnd = true;
            }
        }
    }

    for (size_t i = 0; i < states.size(); i++){
        advanceState(player, runtime, states[i], offsets[i], dt - offsets[i]);
    }

    // a head without a fade (or queued while playing) starts when the last of them ended
    while (!runtime.queue.empty()){
        bool blocked = false;
        float ended = 0.0f;
        for (const anim::PlayerStateRuntime& state : states){
            if (state.finishing) ended = std::max(ended, state.finishWhen);
            else if (state.enabled && state.targetWeight > 0.0f) blocked = true;
        }
        if (blocked) break;
        const anim::QueuedState head = runtime.queue.front();
        runtime.queue.pop_front();
        if (head.state >= states.size()) continue;
        std::deque<anim::QueuedState> rest;
        rest.swap(runtime.queue);
        anim::playState(player, states[head.state].name, head.fade, who());
        runtime.queue.swap(rest);
        advanceState(player, runtime, states[head.state], ended, std::max(0.0f, dt - ended));
        break;
    }
}

void ActionSystem::samplePlayerStates(AnimationPlayerComponent& player){
    anim::AnimationPlayerRuntime& runtime = *player.runtime;
    for (anim::PlayerStateRuntime& state : runtime.states){
        if ((state.enabled || state.finishing) && state.bindings.clip && state.weight > 0.0f){
            accumulateClip(*state.bindings.clip, state.bindings, sampleTimeOf(state), state.weight, runtime.rest, nullptr);
        }
        if (state.finishing){
            // a once state ends: rewound, its last pose kept
            anim::stopState(state);
        }
    }
}

void ActionSystem::updatePlayers(double dt){
    auto players = scene->getComponentArray<AnimationPlayerComponent>();
    if (players->size() == 0) return;

    const uint64_t poolRevision = AnimationClipPool::revision();
    const bool poolChanged = poolRevision != playerPoolRevision;
    playerPoolRevision = poolRevision;
    const float step = std::isfinite(dt) ? (float)dt : 0.0f;

    // nothing below adds or removes Animation Players: the array stays put (callbacks run later)
    for (size_t i = 0; i < players->size(); i++){
        AnimationPlayerComponent& player = players->getComponentFromIndex(i);
        const Entity entity = players->getEntity(i);

        // disabled or inactive: time stops and nothing is written (R25)
        if (!player.enabled || !scene->isEntityActiveInHierarchy(entity)){
            continue;
        }

        anim::AnimationPlayerRuntime& runtime = anim::playerRuntime(player);
        runtime.clock += step;

        // play automatically on the first update in a running scene (R44)
        if (!player.started){
            player.started = true;
            if (player.playAutomatically && !runtime.states.empty()){
                const std::string name = player.defaultClip.empty() ? runtime.states[0].name : player.defaultClip;
                anim::playState(player, name, 0.0f, scene->getEntityName(entity));
            }
        }

        if (!preparePlayer(entity, player, step, poolChanged)){
            continue;
        }
        advancePlayer(entity, player, step);
        samplePlayerStates(player);

        if (!runtime.pending.empty() &&
            std::find(playersWithCallbacks.begin(), playersWithCallbacks.end(), entity) == playersWithCallbacks.end()){
            playersWithCallbacks.push_back(entity);
        }
    }
}

void ActionSystem::dispatchPlayerCallbacks(){
    if (playersWithCallbacks.empty()) return;
    std::vector<Entity> entities;
    entities.swap(playersWithCallbacks);

    for (Entity entity : entities){
        AnimationPlayerComponent* player = scene->isEntityCreated(entity) ? scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
        if (!player || !player->runtime) continue;
        // held: a callback can remove the component
        std::shared_ptr<anim::AnimationPlayerRuntime> runtime = player->runtime;
        std::vector<anim::PendingPlayerCallback> pending;
        pending.swap(runtime->pending);
        std::stable_sort(pending.begin(), pending.end(), [](const anim::PendingPlayerCallback& a, const anim::PendingPlayerCallback& b){
            return a.when < b.when;
        });

        for (const anim::PendingPlayerCallback& callback : pending){
            // callbacks may add components (arrays move) or destroy the GameObject
            player = scene->isEntityCreated(entity) ? scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
            if (!player) break;
            if (callback.finish){
                const std::string state = callback.event.state;
                player->onFinish.call(state);
                continue;
            }
            const bool subscribed = player->onEvent.hasSubscribers();
            const AnimationEvent event = callback.event;
            player->onEvent.call(event);
            // then the method of that name on the GameObject's Lua scripts (spec 3.5)
            const bool scripted = LuaBinding::callLuaScriptMethod(scene, entity, event.functionName, event.args);
            if (!subscribed && !scripted){
                const std::string key = callback.clip + "|" + event.functionName;
                if (std::find(runtime->warnedHandlers.begin(), runtime->warnedHandlers.end(), key) == runtime->warnedHandlers.end()){
                    runtime->warnedHandlers.push_back(key);
                    Log::warn("Animation Player '%s': clip event '%s' of '%s' has no handler (no onEvent subscriber, no script method)",
                              scene->getEntityName(entity).c_str(), event.functionName.c_str(), callback.clip.c_str());
                }
            }
        }
    }
}

void ActionSystem::samplePlayer(Entity entity){
    // inside update the player pass samples it anyway
    if (poseBlendScope) return;
    AnimationPlayerComponent* player = scene->isEntityCreated(entity) ? scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
    if (!player) return;
    anim::playerRuntime(*player);
    if (!preparePlayer(entity, *player, 0.0f, true)) return;

    clearPoseBlend();
    anim::AnimationPlayerRuntime& runtime = *player->runtime;
    for (anim::PlayerStateRuntime& state : runtime.states){
        if (state.enabled && state.bindings.clip && state.weight > 0.0f){
            accumulateClip(*state.bindings.clip, state.bindings, sampleTimeOf(state), state.weight, runtime.rest, nullptr);
        }
    }
    flushPoseBlend();
}

ActionSystem::PlayerPreviewResult ActionSystem::previewPlayer(Entity root, const PlayerPreview& preview, anim::ClipPreviewSession& session){
    PlayerPreviewResult result;
    if (root == NULL_ENTITY || !scene->isEntityCreated(root)){
        return result;
    }
    if (session.root != root){
        // another root: what was bound there is not this one's
        session.bindings.clear();
        session.previousTimes.clear();
    }
    session.root = root;
    result.ready = true;

    // the authored list (state names as the runtime names them); no runtime in edit mode
    const AnimationPlayerComponent* component = scene->findComponent<AnimationPlayerComponent>(root);
    std::vector<AnimationPlayerClip> entries = component ? component->clips : std::vector<AnimationPlayerClip>();
    std::vector<std::string> names = anim::playerStateNames(entries);

    std::vector<AnimationEvent> fired;
    std::vector<anim::ClipBindings> kept;
    kept.reserve(preview.states.size());
    std::unordered_map<std::string, float> times;
    std::vector<std::pair<Entity, ClipChannel>> channels;

    clearPoseBlend();
    for (size_t s = 0; s < preview.states.size(); s++){
        const PlayerPreviewState& state = preview.states[s];
        std::shared_ptr<const AnimationClip> clip = state.clip;
        ClipWrapMode wrap = state.wrap;
        std::string key;
        if (clip){
            if (wrap == ClipWrapMode::Default) wrap = clip->getWrap();
            key = "clip:" + std::to_string((uintptr_t)clip.get());
        }else{
            auto it = std::find(names.begin(), names.end(), state.state);
            if (it == names.end()){
                result.ready = false;
                continue;
            }
            const AnimationPlayerClip& entry = entries[(size_t)(it - names.begin())];
            clip = AnimationClipPool::getOrLoad(entry.clip);
            if (!clip){
                result.ready = false;
                continue;
            }
            if (wrap == ClipWrapMode::Default) wrap = entry.wrap;
            if (wrap == ClipWrapMode::Default) wrap = clip->getWrap();
            key = state.state;
        }

        // the session's bindings of this clip, resolved again after hierarchy changes
        anim::ClipBindings* bindings = nullptr;
        for (anim::ClipBindings& candidate : kept){
            if (candidate.clip == clip) bindings = &candidate;
        }
        if (!bindings){
            for (anim::ClipBindings& candidate : session.bindings){
                if (candidate.clip == clip){
                    kept.push_back(candidate);
                    bindings = &kept.back();
                    break;
                }
            }
        }
        if (!bindings){
            anim::ClipBindings created;
            created.clip = clip;
            kept.push_back(created);
            bindings = &kept.back();
        }
        if (!bindings->resolved || bindings->hierarchyRevision != scene->hierarchyRevision()){
            resolveBindings(scene, root, *bindings, nullptr);
        }
        const std::vector<AnimationClip::Track>& tracks = clip->getTracks();
        for (size_t t = 0; t < tracks.size() && t < bindings->targets.size(); t++){
            if (bindings->targets[t] == NULL_ENTITY && tracks[t].isSampled() &&
                std::find(result.unresolved.begin(), result.unresolved.end(), tracks[t].path) == result.unresolved.end()){
                result.unresolved.push_back(tracks[t].path);
            }
        }

        const float length = clip->getLength();
        const float clipTime = anim::wrapClipTime(state.time, length, wrap);
        accumulateClip(*clip, *bindings, clipTime, state.weight, session.rest, &channels);

        if (preview.fireEvents){
            auto previous = session.previousTimes.find(key);
            if (previous != session.previousTimes.end() && previous->second != state.time){
                std::vector<anim::CrossedEvent> crossed;
                anim::collectCrossedEvents(*clip, wrap, previous->second, state.time, false, crossed);
                for (const anim::CrossedEvent& event : crossed){
                    const ClipEventData& data = clip->getEvents()[event.index];
                    AnimationEvent fire;
                    fire.functionName = data.function;
                    fire.args = data.args;
                    fire.state = state.state;
                    fire.time = data.time;
                    fired.push_back(fire);
                }
            }
        }
        times[key] = state.time;
    }
    // drop the bindings (and times) of clips this call does not name
    session.bindings = std::move(kept);
    session.previousTimes = std::move(times);
    flushPoseBlend();

    result.channels = channels;
    for (const std::pair<Entity, ClipChannel>& entry : channels){
        if (std::find(result.written.begin(), result.written.end(), entry.first) == result.written.end()){
            result.written.push_back(entry.first);
        }
        if (std::find(session.written.begin(), session.written.end(), entry) == session.written.end()){
            session.written.push_back(entry);
        }
    }

    // per state in crossing order, after the pose is written
    for (const AnimationEvent& event : fired){
        AnimationPlayerComponent* player = scene->isEntityCreated(root) ? scene->findComponent<AnimationPlayerComponent>(root) : nullptr;
        if (!player) break;
        player->onEvent.call(event);
    }
    return result;
}

void ActionSystem::restorePlayerPreview(anim::ClipPreviewSession& session){
    // what previewPlayer wrote goes back to its rest
    for (const std::pair<Entity, ClipChannel>& entry : session.written){
        auto it = session.rest.find(entry.first);
        if (it == session.rest.end() || !scene->isEntityCreated(entry.first)) continue;
        const anim::ChannelRest& rest = it->second;
        std::vector<float> value;
        switch (entry.second){
            case ClipChannel::Position:
                if (!rest.hasTransform) continue;
                value = {rest.position.x, rest.position.y, rest.position.z};
                break;
            case ClipChannel::Scale:
                if (!rest.hasTransform) continue;
                value = {rest.scale.x, rest.scale.y, rest.scale.z};
                break;
            case ClipChannel::Rotation:
                if (!rest.hasTransform) continue;
                value.resize(4);
                quaternionToXYZW(rest.rotation, value.data());
                break;
            case ClipChannel::Morph:
                if (!rest.hasMorph) continue;
                value = rest.morphWeights;
                break;
            case ClipChannel::Lattice: {
                if (!rest.hasLattice) continue;
                // the whole cage as it is now (resized since: missing points rest at 0)
                LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(entry.first);
                if (!latticecomp) continue;
                const size_t pointCount = (size_t)lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
                for (size_t p = 0; p < pointCount; p++){
                    const Vector3 offset = p < rest.latticeOffsets.size() ? rest.latticeOffsets[p] : Vector3(0, 0, 0);
                    value.insert(value.end(), {offset.x, offset.y, offset.z});
                }
                break;
            }
            case ClipChannel::Property:
                continue;
        }
        anim::writeChannelValue(scene, entry.first, entry.second, {}, value);
    }
    session.reset();
}

void ActionSystem::sampleClip(const AnimationClip& clip, float time, std::vector<anim::TrackSample>& out){
    anim::sampleClipTracks(clip, time, out);
}

void ActionSystem::resetPlayers(){
    auto players = scene->getComponentArray<AnimationPlayerComponent>();
    for (size_t i = 0; i < players->size(); i++){
        AnimationPlayerComponent& player = players->getComponentFromIndex(i);
        player.runtime.reset();
        player.needsLoad = true;
        player.started = false;
        // play's scripts and C++ libraries are gone after it: their callbacks with them
        player.onEvent.clear();
        player.onFinish.clear();
    }
    playersWithCallbacks.clear();
}
