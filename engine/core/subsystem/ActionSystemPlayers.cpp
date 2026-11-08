// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ActionSystem's Animation Players (animation clips spec 3.3, 9.R1/R2/R9/R10/R16-R21): loading
// and binding, states, fades, queue, events, the rest term in the pose blend, edit-mode preview.

#include "ActionSystem.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "ActionSystemPlayersDetail.h"
#include "audio/AudioMarkers.h"
#include "audio/TimelineAudio.h"

#include "Scene.h"
#include "Log.h"

#include "animation/AnimationPlayerRuntime.h"
#include "animation/AvatarMaskWeights.h"
#include "animation/ClipBinding.h"
#include "animation/HumanoidClip.h"
#include "animation/KeyframeSampling.h"
#include "animation/PropertyBindings.h"
#include "component/ButtonComponent.h"
#include "lattice/LatticeSolver.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "script/LuaBinding.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using playerdetail::completeRest;
using playerdetail::sampleTimeOf;

// the scene value of the channels of `entity` (R2) that have none yet
bool playerdetail::refreshHumanoidSlots(Scene* scene, Entity root, std::shared_ptr<anim::HumanoidSlots>& current){
    auto again = std::make_shared<anim::HumanoidSlots>(anim::resolveHumanoidSlots(scene, root));
    if (current){
        const pose::PoseRig* before = current->target ? current->target->rig.get() : nullptr;
        const pose::PoseRig* after = again->target ? again->target->rig.get() : nullptr;
        const std::array<Entity, pose::HUMAN_EXTRA_BONE_COUNT>* beforeExtra = current->target ? &current->target->extraEntities : nullptr;
        const std::array<Entity, pose::HUMAN_EXTRA_BONE_COUNT>* afterExtra = again->target ? &again->target->extraEntities : nullptr;
        if (before == after && again->valid == current->valid && again->entities == current->entities &&
            (beforeExtra == nullptr) == (afterExtra == nullptr) && (!beforeExtra || *beforeExtra == *afterExtra)){
            return false;
        }
    }
    current = again;
    return true;
}

void playerdetail::completeRest(Scene* scene, Entity entity, anim::ChannelRest& rest, ClipChannel channel){
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
        case ClipChannel::Humanoid:
        case ClipChannel::HumanoidPosition:
            break;
    }
}

anim::PropertyRest* playerdetail::completePropertyRest(Scene* scene, Entity entity, anim::ChannelRest& rest, const anim::PropertyBinding& binding){
    if (binding.status != anim::PropertyBindStatus::Bound || binding.key == 0) return nullptr;
    anim::PropertyRest& property = rest.properties[binding.key];
    // taken once (R2); again when the property now binds with another width (a script reload)
    if (!property.has || property.binding.width != binding.width){
        anim::readPropertyRest(scene, entity, binding, property);
    }
    property.binding = binding;
    return property.has ? &property : nullptr;
}

// the time a state is sampled at this update, in its clip
float playerdetail::sampleTimeOf(const AnimationPlayerComponent& player, const anim::PlayerStateRuntime& state){
    if (state.finishing || state.held) return state.finishTime;
    const float length = anim::stateLength(state);
    const ClipWrapMode wrap = anim::stateWrap(state);
    if (wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong){
        const float time = anim::wrapClipTime(state.time, length, wrap);
        // a loop going backwards reaches its start at the end of the clip (M2)
        if (wrap == ClipWrapMode::Loop && time == 0.0f && anim::effectiveSpeed(player, state) < 0.0f) return length;
        return time;
    }
    return std::min(std::max(state.time, 0.0f), length);
}

namespace {

    // a clip that failed to load is tried again this often (PoseAnimator's pace)
    constexpr float FAILED_RETRY_SECONDS = 1.0f;
    // unresolved paths are looked up again at most this often after hierarchy changes (R9)
    constexpr float UNRESOLVED_RETRY_SECONDS = 0.25f;

    // the entity's place in the Transform array (SIZE_MAX without a Transform): one lookup.
    // The array's storage is paged (S07): an index, never a pointer difference
    size_t transformIndex(Scene* scene, Entity entity){
        auto transforms = scene->getComponentArray<Transform>();
        if (entity == NULL_ENTITY || !transforms->hasEntity(entity)) return SIZE_MAX;
        return transforms->getIndex(entity);
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

    // the path tracks that can resolve (humanoid tracks are realised on the rig, not by path, and
    // so are the humanoid-mapped tracks of a Retarget entry's generic clip)
    size_t sampledTrackCount(const anim::ClipBindings& bindings){
        const std::vector<AnimationClip::Track>& tracks = bindings.clip->getTracks();
        const bool retargeted = bindings.retarget && !bindings.clip->isHumanoid();
        if (retargeted && !bindings.source) return 0;
        const std::vector<uint8_t>* byPath = retargeted && bindings.humanoid ? &bindings.humanoid->reader.byPath : nullptr;
        size_t count = 0;
        for (size_t i = 0; i < tracks.size(); i++){
            if (!tracks[i].isSampled() || isHumanoidChannel(tracks[i].channel)) continue;
            if (byPath && (i >= byPath->size() || !(*byPath)[i])) continue;
            count++;
        }
        return count;
    }

    // the entities under `root` (itself excluded) by name, -> NULL_ENTITY for names used twice
    // ClipBindings::serial: one number per resolve, process-wide (main thread)
    uint64_t bindSerial = 0;

    // A property track on its path's entity (anim-properties spec 8, Binding); the reasons it
    // does not bind, and a few runtime caveats, are logged once per change
    anim::PropertyBinding bindPropertyTrack(Scene* scene, Entity root, Entity target, const AnimationClip::Track& track,
                                            const anim::PropertyBinding* previous){
        anim::PropertyRef ref;
        anim::parsePropertyRef(track.component, track.property, ref);
        // a rebind keeps the script the track had (two scripts of a class: never the other one)
        anim::PropertyBinding binding = anim::bindProperty(scene, target, ref, track.width, previous ? previous->scriptIndex : -1);
        const bool changed = !previous || previous->status != binding.status || previous->message != binding.message;
        if (!changed) return binding;
        const std::string who = scene->getEntityName(root);
        const std::string name = (track.path.empty() ? std::string("") : track.path + " : ") + track.component + "." + track.property;
        if (binding.status != anim::PropertyBindStatus::Bound){
            Log::warn("Animation Player '%s': property track '%s' does not bind: %s", who.c_str(), name.c_str(), binding.message.c_str());
            return binding;
        }
        if (track.component == "ui" && ref.base == "color" && scene->findComponent<ButtonComponent>(target)){
            Log::warn("Animation Player '%s': '%s' is a Button: it writes ui.color from its state colours (animate button.colorNormal)",
                      who.c_str(), name.c_str());
        }
        if (track.component == "gameobject" && target == root){
            Log::warn("Animation Player '%s': the clip animates its own active state: the player stops when it turns it off", who.c_str());
        }
        if (track.component == "sprite" && ref.base == "frame"){
            const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(target);
            int highest = -1;
            for (size_t k = 0; k < track.keyCount; k++) highest = std::max(highest, (int)std::lround(track.weights[k]));
            if (sprite && sprite->numFramesRect == 0){
                Log::warn("Animation Player '%s': '%s': the sprite has no frames (the track does nothing)", who.c_str(), name.c_str());
            }else if (sprite && highest >= (int)sprite->numFramesRect){
                Log::warn("Animation Player '%s': '%s': frame %d is outside the sprite's %u frames (skipped)", who.c_str(), name.c_str(),
                          highest, sprite->numFramesRect);
            }
        }
        return binding;
    }

    // a bound script field's script is not the instance it bound
    bool scriptBindingsStale(Scene* scene, const anim::ClipBindings& bindings){
        for (size_t i = 0; i < bindings.properties.size() && i < bindings.targets.size(); i++){
            const anim::PropertyBinding& binding = bindings.properties[i];
            if (binding.scriptIndex >= 0 && bindings.targets[i] != NULL_ENTITY &&
                anim::scriptBindingStale(scene, bindings.targets[i], binding)) return true;
        }
        return false;
    }

    // Every track of the bindings' clip resolved again from `root`; the rest of every newly bound
    // channel is taken now, before anything writes it. A humanoid clip (anim-retarget 7.4), or a
    // generic clip of a Retarget entry (through `bindings.source`: the one retarget path the
    // Animator plays too), binds its humanoid motion to the root's rig `humanoid` (none: it stays
    // unresolved) and tries its other tracks that do not resolve by path again by unique bone
    // name under the root (7.6); a retargeted clip's humanoid-mapped tracks never bind by path. A
    // Retarget entry whose source failed binds nothing (`retargetError`).
    void resolveBindings(Scene* scene, Entity root, anim::ClipBindings& bindings, anim::RestMap* rest,
                         const anim::HumanoidSlots* humanoid = nullptr){
        bindings.serial = ++bindSerial;
        const std::vector<AnimationClip::Track>& tracks = bindings.clip->getTracks();
        const size_t count = tracks.size();
        const bool humanoidClip = bindings.humanoidPath();
        const bool retargeted = bindings.retarget && !bindings.clip->isHumanoid();
        // the humanoid binding first: it says which tracks of a retargeted clip play by path
        std::shared_ptr<anim::HumanoidBinding> humanoidBinding;
        if (humanoidClip && humanoid && humanoid->target && (!retargeted || bindings.source)){
            auto binding = std::make_shared<anim::HumanoidBinding>();
            if (anim::bindHumanoidClip(*bindings.clip, humanoid->target, *binding, retargeted ? bindings.source : nullptr)){
                humanoidBinding = binding;
            }
        }
        anim::RetargetBinding probe;
        const std::vector<uint8_t>* byPath = nullptr;
        if (retargeted && bindings.source){
            if (humanoidBinding){
                byPath = &humanoidBinding->reader.byPath;
            }else if (probe.bind(*bindings.clip, bindings.source)){
                byPath = &probe.byPath;
            }
        }
        bindings.targets.assign(count, NULL_ENTITY);
        bindings.cursors.assign(count, anim::KeyCursor());
        bindings.rests.assign(rest ? count : 0, nullptr);
        bindings.byName.clear();
        bindings.resolvedCount = 0;
        bindings.unboundProperties = 0;
        bindings.scriptProperties = 0;
        std::vector<anim::PropertyBinding> previous;
        previous.swap(bindings.properties);
        if (bindings.clip->hasPropertyTracks()) bindings.properties.assign(count, anim::PropertyBinding());
        std::unordered_map<std::string, Entity> cache;
        std::unordered_map<std::string, Entity> names;
        bool namesCollected = false;
        for (size_t i = 0; i < count; i++){
            if (!tracks[i].isSampled() || isHumanoidChannel(tracks[i].channel)) continue;
            // a Retarget entry: nothing without its source, never the humanoid-mapped tracks
            if (retargeted && (!bindings.source || (byPath && (i >= byPath->size() || !(*byPath)[i])))) continue;
            Entity target = resolveCached(scene, root, tracks[i].path, cache);
            if (target == NULL_ENTITY && humanoidClip && !tracks[i].path.empty()){
                // a prop or an unmapped bone of another rig: its unique name under the root
                std::vector<anim::PathSegment> segments;
                if (anim::parsePath(tracks[i].path, segments) && !segments.empty() && segments.back().index == 0){
                    if (!namesCollected){
                        anim::uniqueNamesUnder(scene, root, names);
                        namesCollected = true;
                    }
                    auto found = names.find(segments.back().name);
                    if (found != names.end() && found->second != NULL_ENTITY){
                        target = found->second;
                        cache[tracks[i].path] = target;
                        if (bindings.byName.empty()) bindings.byName.assign(count, false);
                        bindings.byName[i] = true;
                    }
                }
            }
            if (target == NULL_ENTITY) continue;
            if (tracks[i].channel == ClipChannel::Property){
                anim::PropertyBinding& binding = bindings.properties[i];
                binding = bindPropertyTrack(scene, root, target, tracks[i], i < previous.size() ? &previous[i] : nullptr);
                if (binding.status != anim::PropertyBindStatus::Bound){
                    bindings.unboundProperties++;
                    continue;
                }
                bindings.targets[i] = target;
                bindings.resolvedCount++;
                if (binding.scriptIndex >= 0) bindings.scriptProperties++;
                if (rest){
                    bindings.rests[i] = &(*rest)[target];
                    playerdetail::completePropertyRest(scene, target, *bindings.rests[i], binding);
                }
                continue;
            }
            bindings.targets[i] = target;
            bindings.resolvedCount++;
            if (rest){
                bindings.rests[i] = &(*rest)[target];
                completeRest(scene, target, *bindings.rests[i], tracks[i].channel);
            }
        }
        bindings.humanoid = humanoidBinding;
        if (humanoidBinding && rest){
            for (const anim::HumanoidOutput& output : humanoidBinding->outputs){
                completeRest(scene, output.entity, (*rest)[output.entity], output.channel);
            }
        }
        // every entity a resolved path went through (the targets and their ancestors), with the
        // "#N" its segment selected it by
        bindings.pathEntities.clear();
        bindings.pathNames.clear();
        bindings.pathParents.clear();
        bindings.pathIndices.clear();
        bindings.pathSiblingIndices.clear();
        std::vector<std::pair<Entity, int>> entities;
        for (const auto& entry : cache){
            if (entry.second == NULL_ENTITY) continue;
            const size_t slash = entry.first.rfind('/');
            std::vector<anim::PathSegment> segment;
            anim::parsePath(slash == std::string::npos ? entry.first : entry.first.substr(slash + 1), segment);
            entities.push_back({entry.second, segment.empty() ? 0 : segment.back().index});
        }
        std::sort(entities.begin(), entities.end());
        entities.erase(std::unique(entities.begin(), entities.end(),
                                   [](const std::pair<Entity, int>& a, const std::pair<Entity, int>& b){ return a.first == b.first; }),
                       entities.end());
        for (const std::pair<Entity, int>& entry : entities){
            bindings.pathEntities.push_back(entry.first);
            bindings.pathNames.push_back(scene->getEntityName(entry.first));
            bindings.pathParents.push_back(parentOf(scene, entry.first));
            bindings.pathIndices.push_back(transformIndex(scene, entry.first));
            bindings.pathSiblingIndices.push_back(entry.second);
        }
        // rest entries of entities gone since (M4: a reused id must not inherit them)
        if (rest){
            for (auto it = rest->begin(); it != rest->end();){
                if (!scene->isEntityCreated(it->first)) it = rest->erase(it);
                else ++it;
            }
        }
        bindings.rootIndex = transformIndex(scene, root);
        bindings.hierarchyRevision = scene->hierarchyRevision();
        bindings.resolved = true;
    }

    // R9's validation: every entity on a bound path is alive, has its parent and its name, and is
    // still the sibling its "#N" selects. Fast path: every one moved in the Transform array exactly
    // as the root did (a change elsewhere) with no rename or destroy since the last check: nothing
    // under the root moved, so nothing resolves differently. Otherwise the exact check: names, and
    // the earlier same-named siblings counted again (children of a parent lie in its branch, so
    // they are the entries between the parent and the entity with that parent): a sibling that
    // appeared, went or was renamed before it fails, a spawn or destroy elsewhere under the rig
    // does not. `failed` gets the entities that changed; true: the bindings hold.
    // `namesChanged`: a name under the root may have changed (a rename under its top ancestor, a
    // destroyed id that can come back, or updates this player missed)
    bool validateBindings(Scene* scene, Entity root, anim::ClipBindings& bindings, bool namesChanged,
                          const std::unordered_set<Entity>& destroyed, std::vector<Entity>& failed){
        auto transforms = scene->getComponentArray<Transform>();
        // settled: a slot is then its index
        transforms->settle();
        const size_t rootIndex = transformIndex(scene, root);
        auto indexOf = [&transforms](Entity entity){ return transforms->orderKey(entity); };
        if (!namesChanged && !destroyed.empty()){
            for (Entity entity : bindings.pathEntities){
                if (destroyed.count(entity)){
                    namesChanged = true;
                    break;
                }
            }
        }
        bool uniform = !namesChanged && (rootIndex == SIZE_MAX) == (bindings.rootIndex == SIZE_MAX);
        const long long shift = (rootIndex == SIZE_MAX || bindings.rootIndex == SIZE_MAX) ? 0 :
                                (long long)rootIndex - (long long)bindings.rootIndex;
        std::vector<Transform*> found(bindings.pathEntities.size(), nullptr);
        for (size_t i = 0; i < bindings.pathEntities.size(); i++){
            Transform* transform = transforms->findComponent(bindings.pathEntities[i]);
            found[i] = transform;
            if (!transform || transform->parent != bindings.pathParents[i]){
                failed.push_back(bindings.pathEntities[i]);
                continue;
            }
            if (uniform && (long long)indexOf(bindings.pathEntities[i]) - (long long)bindings.pathIndices[i] != shift) uniform = false;
        }
        if (!failed.empty()) return false;

        if (!uniform){
            for (size_t i = 0; i < bindings.pathEntities.size(); i++){
                const Entity entity = bindings.pathEntities[i];
                const Entity parent = bindings.pathParents[i];
                const std::string& name = bindings.pathNames[i];
                bool ok = scene->getEntityNameRef(entity) == name;
                Transform* parentTransform = ok ? transforms->findComponent(parent) : nullptr;
                if (ok && !parentTransform) ok = false;
                if (ok){
                    const size_t index = indexOf(entity);
                    int earlier = 0;
                    for (size_t j = indexOf(parent) + 1; j < index && earlier <= bindings.pathSiblingIndices[i]; j++){
                        if (transforms->getComponentFromIndex(j).parent == parent &&
                            scene->getEntityNameRef(transforms->getEntity(j)) == name){
                            earlier++;
                        }
                    }
                    ok = earlier == bindings.pathSiblingIndices[i];
                }
                if (!ok) failed.push_back(entity);
            }
            if (!failed.empty()) return false;
        }

        for (size_t i = 0; i < bindings.pathEntities.size(); i++){
            bindings.pathIndices[i] = indexOf(bindings.pathEntities[i]);
        }
        bindings.rootIndex = rootIndex;
        return true;
    }

    // the top of the Transform hierarchy above `entity` (itself when parentless)
    Entity topAncestor(Scene* scene, Entity entity){
        for (int depth = 0; depth < 1024; depth++){
            const Entity parent = parentOf(scene, entity);
            if (parent == NULL_ENTITY) break;
            entity = parent;
        }
        return entity;
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

    // the period after which a queue behind a loop / ping-pong state starts (R17: the end of the
    // current cycle: a loop's length, a ping-pong's way there and back)
    double queueCycle(ClipWrapMode wrap, double length){
        return wrap == ClipWrapMode::PingPong ? 2.0 * length : length;
    }

    // seconds until the state reaches its next end at its speed (R17): once / clamp_forever the
    // clip's end, loop / ping-pong the end of the current cycle; infinite when it never does
    float remainingSeconds(const AnimationPlayerComponent& player, const anim::PlayerStateRuntime& state){
        const float speed = anim::effectiveSpeed(player, state);
        if (!state.bindings.clip || speed == 0.0f) return std::numeric_limits<float>::infinity();
        const double length = state.bindings.clip->getLength();
        if (length <= 0.0) return 0.0f;
        const double time = state.time;
        const double rate = std::abs(speed);
        ClipWrapMode wrap = anim::stateWrap(state);
        if (wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong){
            const double cycle = queueCycle(wrap, length);
            const double boundary = speed > 0.0f ? (std::floor(time / cycle) + 1.0) * cycle
                                                 : (std::ceil(time / cycle) - 1.0) * cycle;
            return (float)(std::abs(boundary - time) / rate);
        }
        const double end = speed > 0.0f ? length : 0.0;
        return (float)(std::max(0.0, speed > 0.0f ? end - time : time - end) / rate);
    }

    // the root's humanoid rig, shared by humanoid masks and humanoid clips (anim-layers 6.2 rule 2):
    // resolved when first needed, again after a model reload or a pool change (a sidecar edit
    // invalidates the model's clips) when the rig changed, and when a bone entity went
    void refreshHumanoidRig(Scene* scene, Entity entity, anim::AnimationPlayerRuntime& runtime, bool needed, bool reloaded,
                            bool poolChanged){
        if (reloaded && runtime.humanoid){
            runtime.humanoid.reset();
            runtime.humanoidWarned = false;
        }
        const bool hierarchyChanged = runtime.humanoidHierarchy != scene->hierarchyRevision();
        runtime.humanoidHierarchy = scene->hierarchyRevision();
        if (runtime.humanoid && hierarchyChanged){
            bool alive = true;
            for (Entity bone : runtime.humanoid->entities){
                if (bone != NULL_ENTITY && !scene->isEntityCreated(bone)) alive = false;
            }
            if (runtime.humanoid->target){
                for (Entity bone : runtime.humanoid->target->extraEntities){
                    if (bone != NULL_ENTITY && !scene->isEntityCreated(bone)) alive = false;
                }
            }
            if (!alive) runtime.humanoid.reset();
        }
        if (!needed) return;
        if (!runtime.humanoid){
            runtime.humanoid = std::make_shared<anim::HumanoidSlots>(anim::resolveHumanoidSlots(scene, entity));
            runtime.humanoidRevision++;
        }else if (poolChanged){
            if (playerdetail::refreshHumanoidSlots(scene, entity, runtime.humanoid)) runtime.humanoidRevision++;
        }
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
        // a layer at weight 0 fires nothing (anim layers spec 5.8); the base layer always fires
        const float layerWeight = runtime.layerWeight(runtime.stateLayer(state));
        if (!state.enabled || !state.bindings.clip) return;
        const AnimationClip& clip = *state.bindings.clip;
        const double length = clip.getLength();
        const ClipWrapMode wrap = anim::stateWrap(state);
        const double delta = (double)span * anim::effectiveSpeed(player, state);
        const double from = state.time;
        double to = from + delta;
        const bool includeStart = state.startedThisUpdate;
        // only the states that are not fading out fire (during a crossfade the incoming one); a
        // state that starts without weight keeps its start events until it fires or moves on
        const bool fires = state.weight > 0.0f && state.targetWeight > 0.0f && layerWeight > 0.0f;
        if (fires || delta != 0.0) state.startedThisUpdate = false;
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
                // a queue does not wait for a clip without length (M7)
                if (state.stopAtCycleEnd){
                    finishing = true;
                    finishTime = 0.0;
                }
            }else if (state.stopAtCycleEnd && delta != 0.0){
                const double cycle = queueCycle(wrap, length);
                const double boundary = delta > 0.0 ? (std::floor(from / cycle) + 1.0) * cycle
                                                    : (std::ceil(from / cycle) - 1.0) * cycle;
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
            // audio cues by the same crossing (S40 R4); a clip without cues pays this test
            if (!clip.getAudioCues().empty()){
                crossed.clear();
                anim::collectCrossedCues(clip, wrap, (float)from, (float)eventsTo, includeStart, crossed);
                for (const anim::CrossedEvent& cue : crossed){
                    anim::PendingPlayerCue pending;
                    pending.state = (size_t)(&state - runtime.states.data());
                    pending.cue = cue.index;
                    pending.position = cue.position;
                    pending.when = when(cue.position);
                    runtime.pendingCues.push_back(pending);
                }
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


}

void ActionSystem::accumulateClip(const AnimationClip& clip, anim::ClipBindings& bindings, float clipTime, float weight,
                                  std::unordered_map<Entity, anim::ChannelRest>& rest,
                                  std::vector<std::pair<Entity, ClipChannel>>* written,
                                  std::vector<std::pair<Entity, uint32_t>>* writtenProperties){
    if (!(weight > 0.0f)) return;
    const std::vector<AnimationClip::Track>& tracks = clip.getTracks();
    anim::TrackSample sample;
    // the tracks of one path are neighbours: their target's lookups are made once
    Entity current = NULL_ENTITY;
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
            currentRest = i < bindings.rests.size() && bindings.rests[i] ? bindings.rests[i] : &rest[target];
            currentAccum = nullptr;
        }
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
            case ClipChannel::Property: {
                // anim-properties spec 8: per component in key space (a swizzle track its own
                // component, a whole track the others), a discrete property's winner
                if (i >= bindings.properties.size()) continue;
                const anim::PropertyBinding& binding = bindings.properties[i];
                anim::PropertyRest* propertyRest = nullptr;
                auto found = channelRest.properties.find(binding.key);
                if (found != channelRest.properties.end() && found->second.has && found->second.binding.width == binding.width &&
                    found->second.binding.kind == binding.kind){
                    propertyRest = &found->second;
                }else{
                    propertyRest = playerdetail::completePropertyRest(scene, target, channelRest, binding);
                }
                if (!propertyRest) continue;
                // a discrete binding of keys that interpolate (a script's Int field keyed with numbers) steps
                if (binding.discrete && !track.discrete) anim::sampleTrackStepped(track, clipTime, sample);
                PropertyBlendAccum& accum = propertyAccum(target, binding.key, propertyRest);
                if (binding.discrete){
                    accum.total += weight;
                    if (!sample.weights.empty() && weight >= accum.bestWeight){
                        accum.bestWeight = weight;
                        accum.bestValue = sample.weights[0];
                    }
                }else{
                    for (size_t j = 0; j < sample.weights.size(); j++){
                        const size_t k = binding.swizzle >= 0 ? (size_t)binding.swizzle : j;
                        if (k >= 4 || k >= binding.width) break;
                        if (track.overridden & (1u << k)) continue;
                        accum.sums[k] += sample.weights[j] * weight;
                        accum.weights[k] += weight;
                    }
                    // RGB keys ("#RRGGBB", 3 numbers) of a Color4 key an opaque alpha (spec 5.4)
                    if (anim::keysOpaqueAlpha(binding, sample.weights.size()) && !(track.overridden & 8u)){
                        accum.sums[3] += weight;
                        accum.weights[3] += weight;
                    }
                }
                if (writtenProperties){
                    std::pair<Entity, uint32_t> entry(target, binding.key);
                    if (std::find(writtenProperties->begin(), writtenProperties->end(), entry) == writtenProperties->end()){
                        writtenProperties->push_back(entry);
                    }
                }
                continue;
            }
            case ClipChannel::Humanoid:
            case ClipChannel::HumanoidPosition:
                continue;
        }
        if (written){
            std::pair<Entity, ClipChannel> entry(target, track.channel);
            if (std::find(written->begin(), written->end(), entry) == written->end()){
                written->push_back(entry);
            }
        }
    }

    // a humanoid clip's realisation on the root's rig (anim-retarget 7.3): ordinary rotation and
    // position contributions of the bone entities
    if (bindings.humanoid && !bindings.humanoid->outputs.empty()){
        anim::HumanoidBinding& humanoid = *bindings.humanoid;
        anim::sampleHumanoidBinding(clip, humanoid, clipTime, &bindings.cursors);
        for (const anim::HumanoidOutput& output : humanoid.outputs){
            if (!anim::isLive(humanoid, output)) continue;
            anim::ChannelRest& channelRest = rest[output.entity];
            completeRest(scene, output.entity, channelRest, output.channel);
            if (!channelRest.hasTransform) continue;
            TransformBlendAccum& accum = transformBlend[output.entity];
            if (output.channel == ClipChannel::Position){
                accum.positionSum = accum.positionSum + output.position * weight;
                accum.positionWeight += weight;
                if (!accum.hasRestPosition){
                    accum.hasRestPosition = true;
                    accum.restPosition = channelRest.position;
                }
            }else{
                Quaternion value = output.rotation;
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
            }
            if (written){
                std::pair<Entity, ClipChannel> entry(output.entity, output.channel);
                if (std::find(written->begin(), written->end(), entry) == written->end()) written->push_back(entry);
            }
        }
    }
}

bool ActionSystem::preparePlayer(Entity entity, AnimationPlayerComponent& player, float dt, bool poolChanged, bool animInCall){
    anim::AnimationPlayerRuntime& runtime = anim::playerRuntime(player);
    const uint64_t revision = scene->hierarchyRevision();

    // duplicate state names play under their " 2" alias (spec 3.2, R40): said once per player
    // and clip list
    if (runtime.duplicateStateNames != runtime.warnedDuplicateStateNames){
        runtime.warnedDuplicateStateNames = runtime.duplicateStateNames;
        if (!runtime.duplicateStateNames.empty()){
            Log::warn("Animation Player '%s': duplicate state names: %s (rename the clip entries to use their own names)",
                      scene->getEntityName(entity).c_str(), runtime.duplicateStateNames.c_str());
        }
    }

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
    bool clipsLoading = false;
    bool namesKnown = false;
    bool namesChanged = true;
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

    // the humanoid rig of the root, when a humanoid clip (or a humanoid mask) needs it
    bool humanoidNeeded = false;
    for (const anim::PlayerStateRuntime& state : runtime.states){
        if (state.retarget || (state.bindings.clip && state.bindings.clip->isHumanoid())) humanoidNeeded = true;
    }
    for (const anim::LayerRuntime& layer : runtime.layers){
        if (layer.mask && layer.mask->usesHumanoid()) humanoidNeeded = true;
    }
    refreshHumanoidRig(scene, entity, runtime, humanoidNeeded, reloaded, poolChanged);

    for (anim::PlayerStateRuntime& state : runtime.states){
        // the clip: loaded on first use, again after an edit (pool generation), retried after a failure
        bool fetch = !state.bindings.clip;
        if (!fetch && poolChanged && AnimationClipPool::getGeneration(state.poolKey) != state.bindings.clipGeneration){
            fetch = true;
        }
        if (fetch && state.loadFailed){
            state.retryTimer += dt;
            if (state.retryTimer < FAILED_RETRY_SECONDS) fetch = false;
            else state.retryTimer = 0.0f;
        }
        if (fetch){
            std::string error;
            bool loading = false;
            std::shared_ptr<const AnimationClip> clip = state.clip.empty() ? nullptr : AnimationClipPool::pollOrLoad(state.poolKey, loading, &error, animInCall);
            if (loading){
                // a model sub-clip building on the thread pool (async loading): the player waits
                // for it as for its models (time stops, calls are kept); a bound clip plays on meanwhile
                if (!state.bindings.clip){
                    clipsLoading = true;
                    continue;
                }
            }else if (!clip){
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
                state.bindings.clipGeneration = AnimationClipPool::getGeneration(state.poolKey);
                if (changed || state.bindings.retarget != (state.retarget && !clip->isHumanoid())){
                    state.bindings.clip = clip;
                    state.bindings.resolved = false;
                    anim::prepareRetarget(state.bindings, state.clip, state.retarget);
                    if (!state.bindings.retargetError.empty() && !state.errorLogged){
                        state.errorLogged = true;
                        Log::error("Animation Player '%s': state '%s': %s", scene->getEntityName(entity).c_str(),
                                   state.name.c_str(), state.bindings.retargetError.c_str());
                    }
                    state.error = state.bindings.retargetError;
                }
                if (state.startAtEnd){
                    state.startAtEnd = false;
                    state.time = clip->getLength();
                }
            }
        }
        if (!state.bindings.clip) continue;
        // a Retarget entry follows its source model's Avatar (a sidecar edit invalidates the model)
        if (poolChanged && state.bindings.retarget){
            std::string error;
            std::shared_ptr<const anim::RetargetSource> again = anim::clipRetargetSource(*state.bindings.clip, state.clip, &error);
            if (again != state.bindings.source){
                state.bindings.source = again;
                state.bindings.retargetError = again ? std::string() : error;
                state.bindings.resolved = false;
            }
        }

        anim::ClipBindings& bindings = state.bindings;
        // a humanoid clip (or a Retarget entry) binds on the root's rig: again when the rig changed
        if (bindings.humanoidPath() && !runtime.humanoid){
            refreshHumanoidRig(scene, entity, runtime, true, false, false);
        }
        const anim::HumanoidSlots* humanoid = bindings.humanoidPath() ? runtime.humanoid.get() : nullptr;
        if (bindings.resolved && humanoid && state.humanoidRevision != runtime.humanoidRevision){
            bindings.resolved = false;
        }
        if (!bindings.resolved || reloaded){
            resolveBindings(scene, entity, bindings, &runtime.rest, humanoid);
            state.humanoidRevision = runtime.humanoidRevision;
            resolvedAny = true;
            // H5: a humanoid clip on a root without a humanoid rig (once per state and clip)
            if (humanoid && !bindings.humanoid && (!bindings.retarget || bindings.source) && state.humanoidWarned != bindings.clip.get()){
                state.humanoidWarned = bindings.clip.get();
                Log::warn("Animation Player '%s': state '%s' plays a humanoid clip, but the root has no humanoid rig (%s): "
                          "map its model's bones (Configure Avatar...); its other tracks still play",
                          scene->getEntityName(entity).c_str(), state.name.c_str(),
                          humanoid->error.empty() ? "not a humanoid" : humanoid->error.c_str());
            }
            if (bindings.resolvedCount < sampledTrackCount(bindings)){
                bindings.nextUnresolvedRetry = runtime.clock + UNRESOLVED_RETRY_SECONDS;
            }
        }else if (bindings.hierarchyRevision != revision){
            // validate, re-resolve only what needs it (R9). Names are compared when something
            // was renamed under this rig's top, a path entity's id was destroyed, or this player
            // missed updates (the log only covers the last one)
            if (!namesKnown){
                namesKnown = true;
                namesChanged = hierarchyLogOverflow || !renamedTops.empty();
                if (!hierarchyLogOverflow && !renamedTops.empty()){
                    namesChanged = renamedTops.count(topAncestor(scene, entity)) != 0;
                }
            }
            const bool missed = bindings.checkedFrame + 1 != playerFrame;
            std::vector<Entity> failed;
            if (!validateBindings(scene, entity, bindings, namesChanged || missed, destroyedEntities, failed)){
                // a renamed or moved entity keeps its rest (R2); a destroyed one's goes
                for (Entity changed : failed){
                    if (!scene->isEntityCreated(changed)) runtime.rest.erase(changed);
                }
                resolveBindings(scene, entity, bindings, &runtime.rest, humanoid);
                state.humanoidRevision = runtime.humanoidRevision;
                resolvedAny = true;
            }else if (bindings.resolvedCount < sampledTrackCount(bindings)){
                // unresolved paths may resolve now: at most 4 times a second (the revision is
                // left behind until then, so a later frame retries)
                if (runtime.clock >= bindings.nextUnresolvedRetry){
                    resolveBindings(scene, entity, bindings, &runtime.rest, humanoid);
                    state.humanoidRevision = runtime.humanoidRevision;
                    bindings.nextUnresolvedRetry = runtime.clock + UNRESOLVED_RETRY_SECONDS;
                    resolvedAny = true;
                }
            }else{
                bindings.hierarchyRevision = revision;
            }
        }else if (bindings.unboundProperties > 0 && runtime.clock >= bindings.nextUnresolvedRetry){
            // a property's component, script or field may be there now (added at runtime): looked
            // for again at most 4 times a second
            resolveBindings(scene, entity, bindings, &runtime.rest, humanoid);
            bindings.nextUnresolvedRetry = runtime.clock + UNRESOLVED_RETRY_SECONDS;
            resolvedAny = true;
        }
        // the bind stamp of script fields (spec P4), every update whatever the checks above did
        // (a frame that changed the hierarchy too): a reloaded, removed or reordered script binds
        // again (its declared type may have changed)
        if (bindings.resolved && bindings.scriptProperties > 0 && scriptBindingsStale(scene, bindings)){
            resolveBindings(scene, entity, bindings, &runtime.rest, humanoid);
            resolvedAny = true;
        }
        bindings.checkedFrame = playerFrame;
    }
    if (resolvedAny && runtime.modelsRevision != revision){
        collectModels(scene, entity, runtime);
        runtime.modelsRevision = revision;
    }
    // layers, masks, mixing transforms (anim layers spec 7.3): what the evaluator needs
    if (!anim::isDefaultLayering(player)){
        // the humanoid rig was refreshed above (a reload included)
        preparePlayerLayers(entity, player, dt, false);
    }
    return !clipsLoading;
}

namespace {

    // one fade step of `seconds` (real time); a state faded out with stopOnFadeOut stops
    void fadeState(anim::PlayerStateRuntime& state, float seconds){
        if (!state.enabled && !state.held) return;
        if (state.weight != state.targetWeight){
            const float step = state.fadeSpeed > 0.0f && std::isfinite(state.fadeSpeed) ? state.fadeSpeed * seconds : std::abs(state.targetWeight - state.weight);
            if (state.weight < state.targetWeight) state.weight = std::min(state.targetWeight, state.weight + step);
            else state.weight = std::max(state.targetWeight, state.weight - step);
        }
        // a held state goes when nothing is left of it (R1b)
        if (state.weight <= 0.0f && state.targetWeight <= 0.0f && (state.stopOnFadeOut || state.held)){
            anim::stopState(state);
        }
    }

    struct FadeTarget{
        float targetWeight;
        float fadeSpeed;
    };

}

// Starts the queue's head `offset` seconds into the update: a crossfade (fade > 0) or an
// instant play. The playing state queued again restarts from its start (no copy state in v1, so
// no crossfade with itself: the others fade). The fades it starts take their first step for the
// rest of the update now, so its events at 0 fire and the others fade from this frame on (I1, I2).
void ActionSystem::startQueuedState(Entity entity, AnimationPlayerComponent& player, const anim::QueuedState& head, float offset, float dt){
    anim::AnimationPlayerRuntime& runtime = *player.runtime;
    std::vector<anim::PlayerStateRuntime>& states = runtime.states;
    anim::PlayerStateRuntime& target = states[head.state];
    const bool restart = target.enabled || target.finishing;
    const float weight = target.weight;

    std::vector<FadeTarget> before;
    before.reserve(states.size());
    for (const anim::PlayerStateRuntime& state : states) before.push_back({state.targetWeight, state.fadeSpeed});

    // the rest of the layer's queue stays queued
    std::deque<anim::QueuedState>& queue = runtime.layers[(size_t)runtime.stateLayer(target)].queue;
    std::deque<anim::QueuedState> rest;
    rest.swap(queue);
    anim::playState(player, target.name, head.fade, scene->getEntityName(entity));
    runtime.layers[(size_t)runtime.stateLayer(target)].queue.swap(rest);
    if (restart && head.fade > 0.0f){
        anim::startState(player, target);
        target.weight = std::max(weight, 1e-6f);
        target.targetWeight = 1.0f;
        target.fadeSpeed = head.fade > 0.0f ? std::abs(1.0f - target.weight) / head.fade : 0.0f;
        if (target.fadeSpeed == 0.0f) target.weight = 1.0f;
    }
    const float span = std::max(0.0f, dt - offset);
    for (size_t i = 0; i < states.size(); i++){
        if (i == head.state || states[i].targetWeight != before[i].targetWeight || states[i].fadeSpeed != before[i].fadeSpeed){
            fadeState(states[i], span);
        }
    }
}

void ActionSystem::advancePlayer(Entity entity, AnimationPlayerComponent& player, float dt){
    anim::AnimationPlayerRuntime& runtime = *player.runtime;
    std::vector<anim::PlayerStateRuntime>& states = runtime.states;

    // layer weight fades (linear, the player's time: anim layers spec 5.9), before the states'
    for (size_t l = 1; l < runtime.layers.size(); l++){
        anim::LayerRuntime& layer = runtime.layers[l];
        if (layer.weight == layer.targetWeight) continue;
        const float step = layer.fadeSpeed > 0.0f && std::isfinite(layer.fadeSpeed) ? layer.fadeSpeed * dt : std::abs(layer.targetWeight - layer.weight);
        if (layer.weight < layer.targetWeight) layer.weight = std::min(layer.targetWeight, layer.weight + step);
        else layer.weight = std::max(layer.targetWeight, layer.weight - step);
    }

    // fades (real time), held states included
    for (anim::PlayerStateRuntime& state : states){
        fadeState(state, dt);
    }

    // each layer's queue head waits for the states playing now on that layer (R17 per layer):
    // they stop at their next end
    std::vector<float>& offsets = playerStateOffsets;
    offsets.assign(states.size(), 0.0f);
    for (size_t l = 0; l < runtime.layers.size(); l++){
        if (runtime.layers[l].queue.empty()) continue;
        const anim::QueuedState head = runtime.layers[l].queue.front();
        float remaining = 0.0f;
        bool blocked = false;
        bool headPlaying = false;
        for (size_t i = 0; i < states.size(); i++){
            anim::PlayerStateRuntime& state = states[i];
            if (!state.enabled || state.targetWeight <= 0.0f || (size_t)runtime.stateLayer(state) != l) continue;
            state.stopAtCycleEnd = true;
            blocked = true;
            remaining = std::max(remaining, remainingSeconds(player, state));
            if (i == head.state) headPlaying = true;
        }
        // a fading head starts `fade` seconds before their end and crossfades
        if (blocked && head.fade > 0.0f && !headPlaying && head.state < states.size() &&
            std::isfinite(remaining) && remaining - head.fade <= dt){
            runtime.layers[l].queue.pop_front();
            const float offset = std::max(0.0f, remaining - head.fade);
            startQueuedState(entity, player, head, offset, dt);
            offsets[head.state] = offset;
            // the others still stop at their end
            for (size_t i = 0; i < states.size(); i++){
                if (i != head.state && states[i].enabled && (size_t)runtime.stateLayer(states[i]) == l) states[i].stopAtCycleEnd = true;
            }
        }
    }

    for (size_t i = 0; i < states.size(); i++){
        advanceState(player, runtime, states[i], offsets[i], dt - offsets[i]);
    }

    // a head without a fade (or the playing state queued again) starts when the last of its
    // layer's states ended
    for (size_t l = 0; l < runtime.layers.size(); l++){
        while (!runtime.layers[l].queue.empty()){
            bool blocked = false;
            float ended = 0.0f;
            for (const anim::PlayerStateRuntime& state : states){
                if ((size_t)runtime.stateLayer(state) != l) continue;
                if (state.finishing) ended = std::max(ended, state.finishWhen);
                else if (state.enabled && state.targetWeight > 0.0f) blocked = true;
            }
            if (blocked) break;
            const anim::QueuedState head = runtime.layers[l].queue.front();
            runtime.layers[l].queue.pop_front();
            if (head.state >= states.size()) continue;
            startQueuedState(entity, player, head, ended, dt);
            advanceState(player, runtime, states[head.state], ended, std::max(0.0f, dt - ended));
            break;
        }
    }
}

void ActionSystem::samplePlayerStates(Entity entity, AnimationPlayerComponent& player){
    anim::AnimationPlayerRuntime& runtime = *player.runtime;
    // the v1 fast path for a player without layers, masks or mixing transforms (spec 5.5, L1);
    // every other player goes through the layered evaluator
    const bool layered = !anim::isDefaultLayering(player);
    for (anim::PlayerStateRuntime& state : runtime.states){
        if (state.bindings.humanoid){
            state.bindings.humanoid->mirror = anim::stateMirror(state);
            state.bindings.humanoid->footIK = anim::stateFootIK(state);
        }
    }
    if (layered) evaluateLayeredPlayer(entity, player, true);
    for (anim::PlayerStateRuntime& state : runtime.states){
        if (!layered && (state.enabled || state.finishing || state.held) && state.bindings.clip && state.weight > 0.0f){
            accumulateClip(*state.bindings.clip, state.bindings, sampleTimeOf(player, state), state.weight, runtime.rest, nullptr);
        }
        if (state.finishing){
            // R1b: an ended state holds its last pose at its weight (not playing, not advancing)
            // until a fade takes it to 0
            state.finishing = false;
            state.enabled = false;
            state.playing = false;
            state.held = state.weight > 0.0f;
            state.stopAtCycleEnd = false;
            state.startedThisUpdate = false;
            if (!state.held){
                anim::stopState(state);
            }else{
                state.time = state.finishTime;
            }
        }
    }
}

void ActionSystem::updatePlayers(double dt){
    auto players = scene->getComponentArray<AnimationPlayerComponent>();
    if (players->size() == 0) return;

    // what changed in the hierarchy since the last update (the bind validation's names rule)
    playerFrame++;
    {
        std::vector<Entity> renamed;
        std::vector<Entity> destroyed;
        bool overflow = false;
        scene->takeHierarchyLog(renamed, destroyed, overflow);
        hierarchyLogOverflow = overflow;
        renamedTops.clear();
        destroyedEntities.clear();
        for (Entity entity : renamed){
            if (scene->isEntityCreated(entity)) renamedTops.insert(topAncestor(scene, entity));
        }
        destroyedEntities.insert(destroyed.begin(), destroyed.end());
    }

    const uint64_t poolRevision = AnimationClipPool::revision();
    const bool poolChanged = poolRevision != playerPoolRevision;
    playerPoolRevision = poolRevision;
    const uint64_t maskPoolRevision = AnimatorAssetPool::revision();
    playerMaskPoolChanged = maskPoolRevision != playerMaskPoolRevision;
    playerMaskPoolRevision = maskPoolRevision;
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
        samplePlayerStates(entity, player);
        if (!runtime.pendingCues.empty() || !runtime.cueVoices.empty()) firePlayerCues(entity, player, step);

        if (!runtime.pending.empty() &&
            std::find(playersWithCallbacks.begin(), playersWithCallbacks.end(), entity) == playersWithCallbacks.end()){
            playersWithCallbacks.push_back(entity);
        }
    }
}

void ActionSystem::firePlayerCues(Entity entity, AnimationPlayerComponent& player, float dt){
    anim::AnimationPlayerRuntime& runtime = *player.runtime;
    // (kept, not swapped: a crowd crossing cues every frame allocates nothing; firing adds no cue)
    std::vector<anim::PendingPlayerCue>& pending = runtime.pendingCues;
#if JASPERENGINE_SOUND_LAYER
    if (pending.size() > 1){
        std::stable_sort(pending.begin(), pending.end(), [](const anim::PendingPlayerCue& a, const anim::PendingPlayerCue& b){
            return a.when < b.when;
        });
    }
    for (const anim::PendingPlayerCue& crossed : pending){
        if (crossed.state >= runtime.states.size()) continue;
        const anim::PlayerStateRuntime& state = runtime.states[crossed.state];
        if (!state.bindings.clip || crossed.cue >= state.bindings.clip->getAudioCues().size()) continue;
        const ClipAudioCue& cue = state.bindings.clip->getAudioCues()[crossed.cue];
        TimelineCueInfo info;
        info.clip = state.clip;
        info.cue = (int)crossed.cue;
        info.scene = scene;
        info.player = entity;
        info.state = state.name;
        info.layer = runtime.stateLayer(state);
        info.position = crossed.position;
        const float speed = anim::effectiveSpeed(player, state);
        const TimelineVoice voice = TimelineAudio::fireCue(scene, entity, cue, TimelineAudio::delayFramesAt(crossed.when, dt), speed, info);
        if (!voice.empty()) runtime.cueVoices.push_back({state.name, voice.id, voice.generation, voice.source});
    }
    pending.clear();

    // state_exit (R7): its state stopped (stop(), its end), was left or faded out
    for (size_t i = 0; i < runtime.cueVoices.size();){
        const anim::PlayerCueVoice& voice = runtime.cueVoices[i];
        const anim::PlayerStateRuntime* state = nullptr;
        for (const anim::PlayerStateRuntime& candidate : runtime.states){
            if (candidate.name == voice.state) state = &candidate;
        }
        const bool exited = !state || !state->enabled || (state->targetWeight <= 0.0f && state->weight <= 0.0f);
        if (!exited){
            i++;
            continue;
        }
        TimelineVoice stopped;
        stopped.id = voice.id;
        stopped.generation = voice.generation;
        stopped.source = voice.source;
        TimelineAudio::stop(scene, stopped);
        runtime.cueVoices.erase(runtime.cueVoices.begin() + (std::ptrdiff_t)i);
    }
#else
    (void)entity;
    (void)dt;
    if (!pending.empty() && !warnedNoSoundLayer){
        warnedNoSoundLayer = true;
        Log::error("Animation Player: clip audio cues need the soundlayer module, which this build leaves out");
    }
    pending.clear();
    runtime.cueVoices.clear();
#endif
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
#if JASPERENGINE_AUDIO_CAPTURE
            if (AudioMarkers::isEnabled()) AudioMarkers::animationEvent(scene, entity, event.functionName, event.state, event.time);
#endif
            player->onEvent.call(event);
            // then the method of that name on the GameObject's Lua scripts (spec 3.5)
            const bool scripted = LuaBinding::callLuaScriptMethod(scene, entity, event.functionName, event.args);
            if (!subscribed && !scripted){
                const std::string key = callback.clip + "|" + event.functionName;
                if (warnedEventHandlers.insert(key).second){
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
    playerMaskPoolChanged = true;   // generations decide
    // the caller reads the pose when this returns
    if (!preparePlayer(entity, *player, 0.0f, true, true)) return;

    clearPoseBlend();
    anim::AnimationPlayerRuntime& runtime = *player->runtime;
    for (anim::PlayerStateRuntime& state : runtime.states){
        if (state.bindings.humanoid){
            state.bindings.humanoid->mirror = anim::stateMirror(state);
            state.bindings.humanoid->footIK = anim::stateFootIK(state);
        }
    }
    if (!anim::isDefaultLayering(*player)){
        evaluateLayeredPlayer(entity, *player, false);
    }else{
        for (anim::PlayerStateRuntime& state : runtime.states){
            if ((state.enabled || state.held) && state.bindings.clip && state.weight > 0.0f){
                accumulateClip(*state.bindings.clip, state.bindings, sampleTimeOf(*player, state), state.weight, runtime.rest, nullptr);
            }
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
    // the layered preview (anim layers spec 7.5): each state's bindings, time, weight and layer
    std::vector<LayeredPreviewState> layeredStates;
    std::unordered_map<std::string, float> times;
    std::vector<std::pair<Entity, ClipChannel>> channels;
    std::vector<std::pair<Entity, uint32_t>> properties;

    clearPoseBlend();
    for (size_t s = 0; s < preview.states.size(); s++){
        const PlayerPreviewState& state = preview.states[s];
        std::shared_ptr<const AnimationClip> clip = state.clip;
        ClipWrapMode wrap = state.wrap;
        std::string key;
        bool mirror = state.mirror > 0;
        bool footIK = state.footIK > 0;
        bool retarget = state.retarget > 0;
        std::string reference = state.reference;
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
            if (state.retarget < 0) retarget = entry.retarget;
            reference = entry.clip;
            if (state.mirror < 0) mirror = entry.mirror;
            if (state.footIK < 0) footIK = entry.footIK;
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
        const bool retargeted = retarget && !clip->isHumanoid();
        for (anim::ClipBindings& candidate : kept){
            if (candidate.clip == clip && candidate.retarget == retargeted) bindings = &candidate;
        }
        if (!bindings){
            for (anim::ClipBindings& candidate : session.bindings){
                if (candidate.clip == clip && candidate.retarget == retargeted){
                    kept.push_back(candidate);
                    bindings = &kept.back();
                    break;
                }
            }
        }
        if (!bindings){
            anim::ClipBindings created;
            created.clip = clip;
            anim::prepareRetarget(created, reference, retargeted);
            kept.push_back(created);
            bindings = &kept.back();
        }
        if (!bindings->retargetError.empty() && result.humanoidError.empty()) result.humanoidError = bindings->retargetError;
        // a humanoid clip (or a retargeted one) realises on the root's rig (looked up once per hierarchy revision)
        const anim::HumanoidSlots* humanoid = nullptr;
        if (bindings->humanoidPath()){
            if (!session.humanoid || session.humanoidRevision != scene->hierarchyRevision() ||
                session.humanoidPool != AnimationClipPool::revision()){
                // kept when the rig is the same (a clip insert changes the pool revision too)
                playerdetail::refreshHumanoidSlots(scene, root, session.humanoid);
                session.humanoidRevision = scene->hierarchyRevision();
                session.humanoidPool = AnimationClipPool::revision();
            }
            humanoid = session.humanoid.get();
        }
        if (!bindings->resolved || bindings->hierarchyRevision != scene->hierarchyRevision() ||
            (humanoid && (bindings->humanoid ? bindings->humanoid->target : nullptr) != humanoid->target)){
            resolveBindings(scene, root, *bindings, nullptr, humanoid);
        }
        if (bindings->humanoid){
            bindings->humanoid->mirror = mirror;
            bindings->humanoid->footIK = footIK;
        }else if (humanoid && result.humanoidError.empty() && (!bindings->retarget || bindings->source)){
            result.humanoidError = "the root has no humanoid rig (" + (humanoid->error.empty() ? std::string("not a humanoid") : humanoid->error) + ")";
        }
        const std::vector<AnimationClip::Track>& tracks = clip->getTracks();
        for (size_t t = 0; t < tracks.size() && t < bindings->targets.size(); t++){
            if (bindings->targets[t] == NULL_ENTITY && tracks[t].isSampled() && !isHumanoidChannel(tracks[t].channel) &&
                std::find(result.unresolved.begin(), result.unresolved.end(), tracks[t].path) == result.unresolved.end()){
                result.unresolved.push_back(tracks[t].path);
            }
        }

        const float length = clip->getLength();
        const float clipTime = anim::wrapClipTime(state.time, length, wrap);
        if (preview.layered){
            layeredStates.push_back({(size_t)(bindings - kept.data()), clipTime, state.weight, state.layer});
        }else{
            accumulateClip(*clip, *bindings, clipTime, state.weight, session.rest, &channels, &properties);
        }

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
    if (preview.layered && !layeredStates.empty()){
        previewLayers(root, preview, session, kept, layeredStates, channels, properties);
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
    result.properties = properties;
    for (const std::pair<Entity, uint32_t>& entry : properties){
        if (std::find(result.written.begin(), result.written.end(), entry.first) == result.written.end()){
            result.written.push_back(entry.first);
        }
        if (std::find(session.writtenProperties.begin(), session.writtenProperties.end(), entry) == session.writtenProperties.end()){
            session.writtenProperties.push_back(entry);
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
            case ClipChannel::Humanoid:
            case ClipChannel::HumanoidPosition:
                continue;
        }
        anim::writeChannelValue(scene, entry.first, entry.second, {}, value);
    }
    // properties: the rest value, then what the write changed besides it (a sprite's texture
    // rect, an anchor preset); GameObject active last, parents first (it runs no game code in
    // edit mode, but it is what the flush does)
    // the stored rests, exactly; the ones that run callbacks last, in hierarchy order
    std::vector<std::pair<size_t, std::pair<Entity, const anim::PropertyRest*>>> deferred;
    for (const std::pair<Entity, uint32_t>& entry : session.writtenProperties){
        auto it = session.rest.find(entry.first);
        if (it == session.rest.end() || !scene->isEntityCreated(entry.first)) continue;
        auto property = it->second.properties.find(entry.second);
        if (property == it->second.properties.end() || !property->second.has) continue;
        const anim::PropertyRest& rest = property->second;
        if (rest.binding.deferred){
            deferred.push_back({hierarchyIndex(scene, entry.first), {entry.first, &rest}});
            continue;
        }
        anim::restorePropertyRest(scene, entry.first, rest);
    }
    std::stable_sort(deferred.begin(), deferred.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
    for (const auto& entry : deferred){
        if (!scene->isEntityCreated(entry.second.first)) continue;
        anim::restorePropertyRest(scene, entry.second.first, *entry.second.second);
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
        player.onStateEnter.clear();
        player.onStateExit.clear();
    }
    playersWithCallbacks.clear();
    warnedEventHandlers.clear();
}
