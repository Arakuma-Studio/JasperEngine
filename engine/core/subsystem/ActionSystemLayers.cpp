// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ActionSystem's layered evaluator (anim layers spec 7.3): the Animation Players that are not
// default-layered compose their layers here. Per layer, the sampled states accumulate into the
// player's scratch (one element per slot element: value sum, weight sum, rotation reference) with
// weight `state weight * track mask`; the layer then composes bottom-up into the running value R:
// - base layer (Override): R = lerp(rest, avg, min(1, S)), exactly the R1 rest fill;
// - Override above it: R = lerp(R ?? rest, avg, λ·min(1, S)) (rotations: shortest slerp);
// - Additive: R = (R ?? rest) ⊕ λ·min(1, S)·avgΔ, Δ measured from the clip's additive_reference.
// Every composed element then joins the shared pose blend with weight 1 and no rest flags, so
// flushPoseBlend writes it unchanged.

#include "ActionSystem.h"
#include "ActionSystemPlayersDetail.h"

#include "Scene.h"
#include "Log.h"

#include "animation/AnimationPlayerRuntime.h"
#include "animation/AvatarMask.h"
#include "animation/ClipBinding.h"
#include "animation/KeyframeSampling.h"
#include "lattice/LatticeSolver.h"
#include "pool/AvatarMaskPool.h"
#include "pose/PoseAnimatorRuntime.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

using namespace jasperengine;

namespace {

    constexpr uint32_t NO_SLOT = UINT32_MAX;
    // a mask that failed to load is tried again this often (the clips' pace)
    constexpr float MASK_RETRY_SECONDS = 1.0f;

    Entity parentOf(Scene* scene, Entity entity){
        const Transform* transform = scene->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

    uint32_t slotWidth(Scene* scene, Entity target, ClipChannel channel, const AnimationClip::Track& track){
        switch (channel){
            case ClipChannel::Position:
            case ClipChannel::Rotation:
            case ClipChannel::Scale:
                return 1;
            case ClipChannel::Morph:
                return MAX_MORPHTARGETS;
            case ClipChannel::Lattice: {
                uint32_t width = 0;
                if (const LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(target)){
                    width = (uint32_t)std::max(0, lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ));
                }
                for (size_t n = 0; n < track.width && track.points; n++){
                    if (track.points[n] >= 0) width = std::max(width, (uint32_t)track.points[n] + 1);
                }
                return width;
            }
            case ClipChannel::Property:
                break;
        }
        return 0;
    }

    void setQuat(float* out, const Quaternion& q){
        out[0] = q.w; out[1] = q.x; out[2] = q.y; out[3] = q.z;
    }

    Quaternion getQuat(const float* in){
        return Quaternion(in[0], in[1], in[2], in[3]);
    }

    float dot4(const float* a, const float* b){
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    }

    // out = the normalized sum (w x y z), or `fallback` when the sum cancelled out
    void normalizeOr(const float* sum, const float* fallback, float* out){
        const float length = std::sqrt(dot4(sum, sum));
        if (!(length > 1e-12f)){
            for (int k = 0; k < 4; k++) out[k] = fallback[k];
            return;
        }
        const float inverse = 1.0f / length;
        for (int k = 0; k < 4; k++) out[k] = sum[k] * inverse;
    }

    // the rest value of element `index` of a slot (R2), in the element layout
    void restOf(const anim::PoseSlot& slot, uint32_t index, float* out){
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        const anim::ChannelRest* rest = slot.rest;
        if (!rest) return;
        switch (slot.channel){
            case ClipChannel::Position:
                out[0] = rest->position.x; out[1] = rest->position.y; out[2] = rest->position.z;
                break;
            case ClipChannel::Scale:
                out[0] = rest->scale.x; out[1] = rest->scale.y; out[2] = rest->scale.z;
                break;
            case ClipChannel::Rotation:
                setQuat(out, rest->rotation);
                break;
            case ClipChannel::Morph:
                out[0] = index < rest->morphWeights.size() ? rest->morphWeights[index] : 0.0f;
                break;
            case ClipChannel::Lattice:
                if (index < rest->latticeOffsets.size()){
                    const Vector3& offset = rest->latticeOffsets[index];
                    out[0] = offset.x; out[1] = offset.y; out[2] = offset.z;
                }
                break;
            case ClipChannel::Property:
                break;
        }
    }

    // the first touch of an element in this layer resets its sums
    anim::PoseElement& touch(anim::PlayerPoseScratch& scratch, uint32_t element){
        anim::PoseElement& e = scratch.elements[element];
        if (!e.touched){
            e.touched = true;
            e.weight = 0.0f;
            e.sum[0] = e.sum[1] = e.sum[2] = e.sum[3] = 0.0f;
            e.hasRef = false;
            scratch.touched.push_back(element);
        }
        return e;
    }

    void addVector(anim::PoseElement& e, const Vector3& v, float w){
        e.sum[0] += v.x * w;
        e.sum[1] += v.y * w;
        e.sum[2] += v.z * w;
        e.weight += w;
    }

    void addScalar(anim::PoseElement& e, float v, float w){
        e.sum[0] += v * w;
        e.weight += w;
    }

    // as the entity tracks: aligned to the layer's first sample's hemisphere
    void addRotation(anim::PoseElement& e, const Quaternion& q, float w){
        float v[4] = {q.w, q.x, q.y, q.z};
        if (!e.hasRef){
            e.hasRef = true;
            for (int k = 0; k < 4; k++) e.ref[k] = v[k];
        }else if (dot4(e.ref, v) < 0.0f){
            for (int k = 0; k < 4; k++) v[k] = -v[k];
        }
        for (int k = 0; k < 4; k++) e.sum[k] += v[k] * w;
        e.weight += w;
    }

    // additive rotations: aligned to the identity's hemisphere
    void addRotationDelta(anim::PoseElement& e, Quaternion d, float w){
        if (d.w < 0.0f) d = -d;
        e.sum[0] += d.w * w;
        e.sum[1] += d.x * w;
        e.sum[2] += d.y * w;
        e.sum[3] += d.z * w;
        e.weight += w;
    }

    float ratio(float value, float reference, bool& zero){
        if (reference == 0.0f){
            zero = true;
            return 1.0f;
        }
        return value / reference;
    }

}

void playerdetail::buildPoseSlots(Scene* scene, anim::PlayerPoseScratch& scratch, const std::vector<PoseSlotSource>& sources, anim::RestMap& rest){
    scratch.slots.clear();
    scratch.elementSlot.clear();
    scratch.touched.clear();
    scratch.written.clear();
    std::unordered_map<Entity, std::array<uint32_t, 6>> byTarget;
    uint32_t elements = 0;
    for (const PoseSlotSource& source : sources){
        std::vector<uint32_t>& trackSlots = *source.slots;
        const anim::ClipBindings& bindings = *source.bindings;
        if (!bindings.clip){
            trackSlots.clear();
            continue;
        }
        const std::vector<AnimationClip::Track>& tracks = bindings.clip->getTracks();
        trackSlots.assign(tracks.size(), NO_SLOT);
        for (size_t i = 0; i < tracks.size() && i < bindings.targets.size(); i++){
            const Entity target = bindings.targets[i];
            const AnimationClip::Track& track = tracks[i];
            // property tracks are not sampled yet (anim-properties adds their slots here)
            if (target == NULL_ENTITY || !track.isSampled() || track.channel == ClipChannel::Property) continue;
            auto found = byTarget.find(target);
            if (found == byTarget.end()){
                std::array<uint32_t, 6> none;
                none.fill(NO_SLOT);
                found = byTarget.emplace(target, none).first;
            }
            uint32_t& slotIndex = found->second[(size_t)track.channel];
            const uint32_t width = slotWidth(scene, target, track.channel, track);
            if (slotIndex == NO_SLOT){
                if (width == 0) continue;
                anim::PoseSlot slot;
                slot.target = target;
                slot.channel = track.channel;
                slot.first = elements;
                slot.width = width;
                slot.rest = &rest[target];
                slotIndex = (uint32_t)scratch.slots.size();
                scratch.slots.push_back(slot);
                elements += width;
            }else if (width > scratch.slots[slotIndex].width){
                // a later track of the same lattice lists higher points: the slot grows (moved to
                // the end of the element table)
                anim::PoseSlot& slot = scratch.slots[slotIndex];
                slot.first = elements;
                slot.width = width;
                elements += width;
            }
            trackSlots[i] = slotIndex;
        }
    }
    scratch.elements.assign(elements, anim::PoseElement());
    scratch.elementSlot.assign(elements, NO_SLOT);
    for (uint32_t s = 0; s < scratch.slots.size(); s++){
        for (uint32_t k = 0; k < scratch.slots[s].width; k++) scratch.elementSlot[scratch.slots[s].first + k] = s;
    }
    scratch.touched.reserve(elements);
    scratch.written.reserve(elements);
    scratch.dirty = false;
}

void ActionSystem::preparePlayerLayers(Entity entity, AnimationPlayerComponent& player, float dt, bool modelsReloaded){
    anim::AnimationPlayerRuntime& runtime = *player.runtime;

    // the layers' masks: loaded on first use, again after an edit (pool generation), retried
    // every second after a failure (logged once); a layer without its mask contributes nothing
    bool humanoidNeeded = false;
    for (anim::LayerRuntime& layer : runtime.layers){
        if (layer.maskPath.empty()){
            layer.mask.reset();
            layer.maskLoading = false;
            layer.maskFailed = false;
            continue;
        }
        bool fetch = !layer.mask;
        if (!fetch && playerMaskPoolChanged && AvatarMaskPool::getGeneration(layer.maskPath) != layer.maskGeneration) fetch = true;
        if (fetch && layer.maskFailed){
            layer.maskRetry += dt;
            if (layer.maskRetry < MASK_RETRY_SECONDS) fetch = false;
            else layer.maskRetry = 0.0f;
        }
        if (fetch){
            std::string error;
            std::shared_ptr<const anim::AvatarMask> mask = AvatarMaskPool::getOrLoad(layer.maskPath, &error);
            layer.maskLoading = false;
            if (!mask){
                layer.mask.reset();
                layer.maskFailed = true;
                layer.maskError = error;
                if (!layer.maskErrorLogged){
                    layer.maskErrorLogged = true;
                    Log::error("Animation Player '%s': layer '%s': mask '%s': %s (the layer contributes nothing)", scene->getEntityName(entity).c_str(),
                               layer.name.c_str(), layer.maskPath.c_str(), error.c_str());
                }
            }else{
                if (mask != layer.mask) layer.maskEntitiesValid = false;
                layer.mask = mask;
                layer.maskGeneration = AvatarMaskPool::getGeneration(layer.maskPath);
                layer.maskFailed = false;
                layer.maskErrorLogged = false;
                layer.maskError.clear();
                layer.maskRetry = 0.0f;
            }
        }
        if (layer.mask && layer.mask->hasHumanoid()) humanoidNeeded = true;
    }

    // the humanoid rig of the root (spec 6.2 rule 2): when a mask needs it, again after a reload
    if (modelsReloaded && runtime.humanoid){
        runtime.humanoid.reset();
        runtime.humanoidWarned = false;
    }
    if (humanoidNeeded && !runtime.humanoid){
        runtime.humanoid = std::make_shared<anim::HumanoidSlots>(anim::resolveHumanoidSlots(scene, entity));
        runtime.humanoidRevision++;
        if (!runtime.humanoid->valid && !runtime.humanoidWarned){
            runtime.humanoidWarned = true;
            Log::warn("Animation Player '%s': a layer's humanoid mask needs a humanoid rig on the root (%s): its transforms and default apply",
                      scene->getEntityName(entity).c_str(), runtime.humanoid->error.c_str());
        }
    }

    // the slot table, when any state's bindings changed since it was built
    std::vector<anim::PlayerStateRuntime>& states = runtime.states;
    bool rebuild = runtime.scratch.dirty || runtime.scratch.builtFor.size() != states.size();
    for (size_t i = 0; !rebuild && i < states.size(); i++){
        if (runtime.scratch.builtFor[i] != (states[i].bindings.clip ? states[i].bindings.serial : 0)) rebuild = true;
    }
    if (!rebuild){
        // a lattice cage resized since: its slot is rebuilt
        for (const anim::PoseSlot& slot : runtime.scratch.slots){
            if (slot.channel != ClipChannel::Lattice) continue;
            const LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(slot.target);
            if (latticecomp && (uint32_t)lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ) > slot.width){
                rebuild = true;
                break;
            }
        }
    }
    if (rebuild){
        std::vector<playerdetail::PoseSlotSource> sources;
        sources.reserve(states.size());
        runtime.scratch.builtFor.assign(states.size(), 0);
        for (size_t i = 0; i < states.size(); i++){
            sources.push_back({&states[i].bindings, &states[i].trackSlots});
            runtime.scratch.builtFor[i] = states[i].bindings.clip ? states[i].bindings.serial : 0;
        }
        playerdetail::buildPoseSlots(scene, runtime.scratch, sources, runtime.rest);
        runtime.scratchSerial++;
        for (anim::LayerRuntime& layer : runtime.layers) layer.maskEntitiesValid = false;
    }

    // the masks' transform entries on the root (with the bindings)
    for (anim::LayerRuntime& layer : runtime.layers){
        if (layer.mask && !layer.maskEntitiesValid){
            layer.maskEntities = layer.mask->bindTransforms(scene, entity);
            layer.maskEntitiesValid = true;
            layer.maskEntitiesSerial++;
        }
    }

    // per state: the track mask weights (layer mask * mixing transforms) and the additive
    // reference, when what they were computed for changed
    for (anim::PlayerStateRuntime& state : states){
        if (!state.bindings.clip) continue;
        const int layerIndex = runtime.stateLayer(state);
        anim::LayerRuntime& layer = runtime.layers[(size_t)layerIndex];
        const anim::AvatarMask* mask = layer.mask.get();
        anim::PlayerStateRuntime::MaskStamp stamp;
        stamp.mask = mask;
        stamp.maskEntities = mask ? layer.maskEntitiesSerial : 0;
        stamp.layer = layerIndex;
        stamp.bindings = runtime.scratchSerial;
        stamp.mixing = state.mixingRevision;
        stamp.humanoid = runtime.humanoidRevision;
        stamp.valid = true;
        const anim::PlayerStateRuntime::MaskStamp& old = state.maskStamp;
        if (!(old.valid && old.mask == stamp.mask && old.maskEntities == stamp.maskEntities && old.layer == stamp.layer &&
              old.bindings == stamp.bindings && old.mixing == stamp.mixing && old.humanoid == stamp.humanoid)){
            state.maskStamp = stamp;
            if (!mask && state.mixing.empty()){
                state.trackMask.clear();
            }else{
                const std::vector<AnimationClip::Track>& tracks = state.bindings.clip->getTracks();
                state.trackMask.assign(tracks.size(), 1.0f);
                std::vector<std::pair<Entity, bool>> mixing;
                for (const anim::MixingTransform& transform : state.mixing){
                    const Entity target = transform.path.empty() ? entity : anim::resolvePath(scene, entity, transform.path);
                    if (target != NULL_ENTITY) mixing.push_back({target, transform.recursive});
                }
                const anim::HumanoidSlots* humanoid = runtime.humanoid.get();
                for (size_t i = 0; i < tracks.size() && i < state.bindings.targets.size(); i++){
                    const Entity target = state.bindings.targets[i];
                    if (target == NULL_ENTITY) continue;
                    float weight = mask ? mask->weightOf(scene, entity, target, layer.maskEntities, humanoid).weight : 1.0f;
                    if (!state.mixing.empty()){
                        bool covered = false;
                        Entity current = target;
                        for (int depth = 0; depth < 1024 && current != NULL_ENTITY && !covered; depth++){
                            for (const std::pair<Entity, bool>& entry : mixing){
                                if (entry.first == current && (depth == 0 || entry.second)) covered = true;
                            }
                            if (current == entity) break;
                            current = parentOf(scene, current);
                        }
                        if (!covered) weight = 0.0f;
                    }
                    state.trackMask[i] = weight;
                }
            }
        }
        if (layer.blend == AnimationLayerBlend::Additive && layerIndex > 0){
            if (state.referenceSerial != state.bindings.serial || state.referenceClip != state.bindings.clip.get()){
                anim::sampleClipTracks(*state.bindings.clip, state.bindings.clip->getAdditiveReference(), state.reference);
                state.referenceSerial = state.bindings.serial;
                state.referenceClip = state.bindings.clip.get();
                state.referenceWarned = false;
            }
        }
    }
}

void ActionSystem::evaluateLayeredPlayer(Entity entity, AnimationPlayerComponent& player, bool includeFinishing){
    (void)entity;
    anim::AnimationPlayerRuntime& runtime = *player.runtime;
    anim::PlayerPoseScratch& scratch = runtime.scratch;
    if (scratch.dirty) return;      // not prepared yet (a sample before the first update)

    scratch.layerInputs.resize(runtime.layers.size());
    for (size_t l = 0; l < runtime.layers.size(); l++){
        const anim::LayerRuntime& layer = runtime.layers[l];
        anim::PoseLayerInput& input = scratch.layerInputs[l];
        input.weight = l == 0 ? 1.0f : layer.weight;
        input.blend = l == 0 ? AnimationLayerBlend::Override : layer.blend;
        // L2: a layer at weight 0 costs nothing; spec 5.7: a mask still loading or failed
        input.skip = (l > 0 && !(layer.weight > 0.0f)) || (!layer.maskPath.empty() && !layer.mask);
    }
    scratch.stateInputs.clear();
    for (anim::PlayerStateRuntime& state : runtime.states){
        if (!((state.enabled || (includeFinishing && state.finishing) || state.held) && state.bindings.clip && state.weight > 0.0f)) continue;
        if (state.trackSlots.size() != state.bindings.clip->getTracks().size()) continue;
        anim::PoseStateInput input;
        input.clip = state.bindings.clip.get();
        input.bindings = &state.bindings;
        input.time = playerdetail::sampleTimeOf(player, state);
        input.weight = state.weight;
        input.layer = runtime.stateLayer(state);
        input.trackMask = state.trackMask.empty() ? nullptr : state.trackMask.data();
        input.trackSlots = state.trackSlots.data();
        if (input.layer > 0 && scratch.layerInputs[(size_t)input.layer].blend == AnimationLayerBlend::Additive){
            if (state.reference.size() != state.bindings.clip->getTracks().size()) continue;
            input.reference = state.reference.data();
            input.referenceWarned = &state.referenceWarned;
        }
        scratch.stateInputs.push_back(input);
    }
    composePlayerLayers(scratch, nullptr);
}

void ActionSystem::composePlayerLayers(anim::PlayerPoseScratch& scratch, std::vector<std::pair<Entity, ClipChannel>>* written){
    anim::TrackSample sample;
    for (size_t l = 0; l < scratch.layerInputs.size(); l++){
        const anim::PoseLayerInput& layer = scratch.layerInputs[l];
        if (layer.skip) continue;
        const bool additive = l > 0 && layer.blend == AnimationLayerBlend::Additive;

        // 1. the layer's states into its sums
        for (const anim::PoseStateInput& state : scratch.stateInputs){
            if ((size_t)state.layer != l) continue;
            const std::vector<AnimationClip::Track>& tracks = state.clip->getTracks();
            anim::ClipBindings& bindings = *state.bindings;
            bool zeroScale = false;
            for (size_t i = 0; i < tracks.size() && i < bindings.targets.size(); i++){
                const uint32_t slotIndex = state.trackSlots[i];
                if (slotIndex == NO_SLOT) continue;
                const AnimationClip::Track& track = tracks[i];
                const anim::PoseSlot& slot = scratch.slots[slotIndex];
                const Entity target = slot.target;
                anim::ChannelRest& rest = *slot.rest;
                const float mask = state.trackMask ? state.trackMask[i] : 1.0f;
                const float w = state.weight * mask;
                if (!(w > 0.0f)){
                    // masked out on the base layer: the bone stays at its rest (spec 5.2)
                    if (l == 0 && slot.width == 1){
                        playerdetail::completeRest(scene, target, rest, track.channel);
                        if (rest.hasTransform) touch(scratch, slot.first);
                    }
                    continue;
                }
                if (!rest.hasTransform || track.channel == ClipChannel::Morph || track.channel == ClipChannel::Lattice){
                    playerdetail::completeRest(scene, target, rest, track.channel);
                }
                if (!anim::sampleTrack(track, state.time, sample, &bindings.cursors[i])) continue;
                const anim::TrackSample* reference = (additive && state.reference) ? &state.reference[i] : nullptr;
                if (additive && (!reference || !reference->valid)) continue;

                switch (track.channel){
                    case ClipChannel::Position: {
                        if (!rest.hasTransform) continue;
                        if (additive){
                            // a relative track's values go through its rest matrix first (an
                            // anchored UI element's is not a pure translation)
                            if (track.relative){
                                const Matrix4 matrix = getRelativeTrackMatrix(target, rest.position);
                                addVector(touch(scratch, slot.first), matrix * sample.vector - matrix * reference->vector, w);
                            }else{
                                addVector(touch(scratch, slot.first), sample.vector - reference->vector, w);
                            }
                        }else{
                            Vector3 value = sample.vector;
                            if (track.relative) value = getRelativeTrackMatrix(target, rest.position) * value;
                            addVector(touch(scratch, slot.first), value, w);
                        }
                        break;
                    }
                    case ClipChannel::Scale: {
                        if (!rest.hasTransform) continue;
                        if (additive){
                            const Vector3 d(ratio(sample.vector.x, reference->vector.x, zeroScale),
                                            ratio(sample.vector.y, reference->vector.y, zeroScale),
                                            ratio(sample.vector.z, reference->vector.z, zeroScale));
                            addVector(touch(scratch, slot.first), d, w);
                        }else{
                            Vector3 value = sample.vector;
                            if (track.relative) value = rest.scale * value;
                            addVector(touch(scratch, slot.first), value, w);
                        }
                        break;
                    }
                    case ClipChannel::Rotation: {
                        if (!rest.hasTransform) continue;
                        if (additive){
                            addRotationDelta(touch(scratch, slot.first), reference->rotation.inverse() * sample.rotation, w);
                        }else{
                            Quaternion value = sample.rotation;
                            if (track.relative) value = rest.rotation * value;
                            addRotation(touch(scratch, slot.first), value, w);
                        }
                        break;
                    }
                    case ClipChannel::Morph: {
                        if (!rest.hasMorph || !scene->findComponent<MeshComponent>(target)) continue;
                        const size_t count = std::min(sample.weights.size(), (size_t)std::min<uint32_t>(slot.width, MAX_MORPHTARGETS));
                        for (size_t k = 0; k < count; k++){
                            float value = sample.weights[k];
                            if (additive) value -= k < reference->weights.size() ? reference->weights[k] : 0.0f;
                            addScalar(touch(scratch, slot.first + (uint32_t)k), value, w);
                        }
                        break;
                    }
                    case ClipChannel::Lattice: {
                        const LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(target);
                        if (!rest.hasLattice || !latticecomp || !track.points) continue;
                        const int pointCount = lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
                        for (size_t n = 0; n < track.width && n < sample.offsets.size(); n++){
                            const int point = track.points[n];
                            // points outside the bound cage are skipped (ClipValidation reports them)
                            if (point < 0 || point >= pointCount || (uint32_t)point >= slot.width) continue;
                            Vector3 value = sample.offsets[n];
                            if (additive) value = value - (n < reference->offsets.size() ? reference->offsets[n] : Vector3(0, 0, 0));
                            addVector(touch(scratch, slot.first + (uint32_t)point), value, w);
                        }
                        break;
                    }
                    case ClipChannel::Property:
                        continue;
                }
            }
            if (zeroScale && state.referenceWarned && !*state.referenceWarned){
                *state.referenceWarned = true;
                Log::warn("Animation Player: additive clip with a scale of 0 at its additive_reference: those components add nothing");
            }
        }

        // 2. compose the layer's elements into R (raw floats: no Quaternion temporaries here)
        const float lambda = l == 0 ? 1.0f : layer.weight;
        for (uint32_t index : scratch.touched){
            anim::PoseElement& e = scratch.elements[index];
            e.touched = false;
            const anim::PoseSlot& slot = scratch.slots[scratch.elementSlot[index]];
            const float S = e.weight;
            const bool rotation = slot.channel == ClipChannel::Rotation;
            const bool hadResult = e.hasResult;
            float rest[4];
            const bool needRest = l == 0 ? S < 1.0f : !e.hasResult;
            if (needRest || rotation) restOf(slot, index - slot.first, rest);

            if (l == 0){
                // R = lerp(rest, avg, min(1, S)): the rest fills what the states leave (R1)
                if (rotation){
                    if (S >= 1.0f){
                        normalizeOr(e.sum, rest, e.result);
                    }else{
                        const float sign = (e.hasRef && dot4(e.ref, rest) < 0.0f) ? -1.0f : 1.0f;
                        float sum[4];
                        for (int k = 0; k < 4; k++) sum[k] = e.sum[k] + sign * rest[k] * (1.0f - S);
                        normalizeOr(sum, rest, e.result);
                    }
                }else if (S >= 1.0f){
                    const float inverse = 1.0f / S;
                    for (int k = 0; k < 4; k++) e.result[k] = e.sum[k] * inverse;
                }else{
                    for (int k = 0; k < 4; k++) e.result[k] = e.sum[k] + rest[k] * (1.0f - S);
                }
                e.hasResult = true;
            }else{
                if (!(S > 0.0f)) continue;
                const float alpha = lambda * std::min(1.0f, S);
                float base[4];
                for (int k = 0; k < 4; k++) base[k] = e.hasResult ? e.result[k] : rest[k];
                const float inverse = 1.0f / S;
                if (!additive){
                    if (rotation){
                        float avg[4];
                        normalizeOr(e.sum, base, avg);
                        // a full override (the common case) needs no slerp
                        if (alpha >= 1.0f){
                            for (int k = 0; k < 4; k++) e.result[k] = avg[k];
                        }else{
                            setQuat(e.result, pose::slerpShortest(getQuat(base), getQuat(avg), alpha));
                        }
                    }else{
                        for (int k = 0; k < 4; k++) e.result[k] = base[k] + (e.sum[k] * inverse - base[k]) * alpha;
                    }
                }else if (rotation){
                    static const float identity[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                    float delta[4];
                    normalizeOr(e.sum, identity, delta);
                    const Quaternion scaled = alpha >= 1.0f ? getQuat(delta) : pose::slerpShortest(Quaternion(), getQuat(delta), alpha);
                    // base * scaled (w x y z)
                    const float bw = base[0], bx = base[1], by = base[2], bz = base[3];
                    const float product[4] = {
                        bw * scaled.w - bx * scaled.x - by * scaled.y - bz * scaled.z,
                        bw * scaled.x + bx * scaled.w + by * scaled.z - bz * scaled.y,
                        bw * scaled.y - bx * scaled.z + by * scaled.w + bz * scaled.x,
                        bw * scaled.z + bx * scaled.y - by * scaled.x + bz * scaled.w};
                    normalizeOr(product, base, e.result);
                }else if (slot.channel == ClipChannel::Scale){
                    for (int k = 0; k < 3; k++) e.result[k] = base[k] * (1.0f + alpha * (e.sum[k] * inverse - 1.0f));
                    e.result[3] = 0.0f;
                }else{
                    for (int k = 0; k < 4; k++) e.result[k] = base[k] + alpha * (e.sum[k] * inverse);
                }
                e.hasResult = true;
            }
            if (!hadResult) scratch.written.push_back(index);
        }
        scratch.touched.clear();
    }

    // 3. every composed element into the shared pose blend with weight 1 (no rest flags: the
    // rest fill is applied already); flushPoseBlend writes it unchanged
    Entity currentTarget = NULL_ENTITY;
    TransformBlendAccum* currentAccum = nullptr;
    for (uint32_t index : scratch.written){
        anim::PoseElement& e = scratch.elements[index];
        e.hasResult = false;
        const anim::PoseSlot& slot = scratch.slots[scratch.elementSlot[index]];
        const Entity target = slot.target;
        switch (slot.channel){
            case ClipChannel::Position:
            case ClipChannel::Scale:
            case ClipChannel::Rotation: {
                if (target != currentTarget || !currentAccum){
                    currentTarget = target;
                    currentAccum = &transformBlend[target];
                }
                TransformBlendAccum& accum = *currentAccum;
                if (slot.channel == ClipChannel::Position){
                    accum.positionSum.x += e.result[0];
                    accum.positionSum.y += e.result[1];
                    accum.positionSum.z += e.result[2];
                    accum.positionWeight += 1.0f;
                }else if (slot.channel == ClipChannel::Scale){
                    accum.scaleSum.x += e.result[0];
                    accum.scaleSum.y += e.result[1];
                    accum.scaleSum.z += e.result[2];
                    accum.scaleWeight += 1.0f;
                }else{
                    float sign = 1.0f;
                    if (!accum.hasRotationRef){
                        accum.rotationRef.w = e.result[0];
                        accum.rotationRef.x = e.result[1];
                        accum.rotationRef.y = e.result[2];
                        accum.rotationRef.z = e.result[3];
                        accum.hasRotationRef = true;
                    }else if (accum.rotationRef.w * e.result[0] + accum.rotationRef.x * e.result[1] +
                              accum.rotationRef.y * e.result[2] + accum.rotationRef.z * e.result[3] < 0.0f){
                        sign = -1.0f;
                    }
                    accum.rotationSum.w += sign * e.result[0];
                    accum.rotationSum.x += sign * e.result[1];
                    accum.rotationSum.y += sign * e.result[2];
                    accum.rotationSum.z += sign * e.result[3];
                    accum.rotationWeight += 1.0f;
                }
                break;
            }
            case ClipChannel::Morph: {
                const uint32_t k = index - slot.first;
                if (k >= (uint32_t)MAX_MORPHTARGETS) break;
                MorphBlendAccum& accum = morphBlend[target];
                accum.sums[k] += e.result[0];
                accum.weights[k] += 1.0f;
                break;
            }
            case ClipChannel::Lattice: {
                LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(target);
                if (!latticecomp) break;
                const int pointCount = lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
                const uint32_t point = index - slot.first;
                if ((int)point >= pointCount) break;
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
                accum.sums[point] += Vector3(e.result[0], e.result[1], e.result[2]);
                accum.weights[point] += 1.0f;
                break;
            }
            case ClipChannel::Property:
                break;
        }
        if (written){
            std::pair<Entity, ClipChannel> entry(target, slot.channel);
            if (std::find(written->begin(), written->end(), entry) == written->end()) written->push_back(entry);
        }
    }
    scratch.written.clear();
}

void ActionSystem::previewLayers(Entity root, const PlayerPreview& preview, anim::ClipPreviewSession& session,
                                 std::vector<anim::ClipBindings>& bindings, const std::vector<LayeredPreviewState>& states,
                                 std::vector<std::pair<Entity, ClipChannel>>& written){
    anim::PlayerPoseScratch& scratch = session.scratch;
    const size_t layerCount = std::max<size_t>(1, preview.layers.size());

    // the layers' masks (the pool caches them; one that does not load contributes nothing)
    std::vector<std::shared_ptr<const anim::AvatarMask>> masks(layerCount);
    bool humanoidNeeded = false;
    scratch.layerInputs.assign(layerCount, anim::PoseLayerInput());
    for (size_t l = 0; l < layerCount; l++){
        anim::PoseLayerInput& input = scratch.layerInputs[l];
        if (l < preview.layers.size()){
            const PlayerPreviewLayer& layer = preview.layers[l];
            input.weight = l == 0 ? 1.0f : std::min(std::max(std::isfinite(layer.weight) ? layer.weight : 0.0f, 0.0f), 1.0f);
            input.blend = l == 0 ? AnimationLayerBlend::Override : layer.blend;
            input.skip = l > 0 && !(input.weight > 0.0f);
            if (!layer.mask.empty()){
                masks[l] = AvatarMaskPool::getOrLoad(layer.mask);
                if (!masks[l]) input.skip = true;
                else if (masks[l]->hasHumanoid()) humanoidNeeded = true;
            }
        }
    }
    if (humanoidNeeded && (!session.humanoid || session.humanoidRevision != scene->hierarchyRevision())){
        session.humanoid = std::make_shared<anim::HumanoidSlots>(anim::resolveHumanoidSlots(scene, root));
        session.humanoidRevision = scene->hierarchyRevision();
    }

    // one slot table over every previewed clip's bindings (an editor preview: rebuilt per call)
    session.trackSlots.assign(states.size(), std::vector<uint32_t>());
    session.trackMasks.assign(states.size(), std::vector<float>());
    session.references.resize(states.size());
    std::vector<playerdetail::PoseSlotSource> sources;
    for (size_t i = 0; i < states.size(); i++){
        sources.push_back({&bindings[states[i].bindings], &session.trackSlots[i]});
    }
    playerdetail::buildPoseSlots(scene, scratch, sources, session.rest);

    std::vector<std::vector<Entity>> maskEntities(layerCount);
    for (size_t l = 0; l < layerCount; l++){
        if (masks[l]) maskEntities[l] = masks[l]->bindTransforms(scene, root);
    }
    scratch.stateInputs.clear();
    for (size_t i = 0; i < states.size(); i++){
        const LayeredPreviewState& state = states[i];
        anim::ClipBindings& binding = bindings[state.bindings];
        if (!binding.clip || !(state.weight > 0.0f)) continue;
        const size_t layer = state.layer >= 0 && (size_t)state.layer < layerCount ? (size_t)state.layer : 0;
        const std::vector<AnimationClip::Track>& tracks = binding.clip->getTracks();
        if (masks[layer]){
            session.trackMasks[i].assign(tracks.size(), 1.0f);
            for (size_t t = 0; t < tracks.size() && t < binding.targets.size(); t++){
                if (binding.targets[t] == NULL_ENTITY) continue;
                session.trackMasks[i][t] = masks[layer]->weightOf(scene, root, binding.targets[t], maskEntities[layer], session.humanoid.get()).weight;
            }
        }
        anim::PoseStateInput input;
        input.clip = binding.clip.get();
        input.bindings = &binding;
        input.time = state.time;
        input.weight = state.weight;
        input.layer = (int)layer;
        input.trackMask = session.trackMasks[i].empty() ? nullptr : session.trackMasks[i].data();
        input.trackSlots = session.trackSlots[i].data();
        if (layer > 0 && scratch.layerInputs[layer].blend == AnimationLayerBlend::Additive){
            anim::sampleClipTracks(*binding.clip, binding.clip->getAdditiveReference(), session.references[i]);
            input.reference = session.references[i].data();
        }
        if (session.trackSlots[i].size() != tracks.size()) continue;
        scratch.stateInputs.push_back(input);
    }
    composePlayerLayers(scratch, &written);
}
