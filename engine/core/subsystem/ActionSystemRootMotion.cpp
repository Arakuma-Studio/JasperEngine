// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation Player's root motion (docs/specs/S94-rendering-anim-gaps.md F34): Unity's Root
// Transform options of a clip (its root_motion block) and Apply Root Motion, on the math the
// Animator uses (animation/RootMotion.h).
// - The root bone: the block's bone, else the humanoid hips, else the shallowest bone the clip
//   positions. Clips that play through the humanoid path have none (their root motion is the
//   Animator's): said once.
// - Each state on the base layer adds its step's motion by weight. The written pose loses the
//   root motion of the heaviest of them (the one whose clip settings decide), after the blend.
// - Nothing runs for a player whose clips have no root_motion block while Apply Root Motion is off.

#include "ActionSystem.h"
#include "ActionSystemPlayersDetail.h"

#include "Log.h"
#include "Scene.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorPose.h"
#include "animation/ClipBinding.h"
#include "animation/KeyframeSampling.h"
#include "animation/RootMotion.h"
#include "component/AnimationPlayerComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/Transform.h"
#include "subsystem/PhysicsSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{

    using RootLink = anim::PlayerStateRuntime::RootLink;
    using StateRootMotion = anim::PlayerStateRuntime::RootMotion;

    constexpr double kRootMotionMaxLaps = 1000.0;
    // a frame's root motion above this is a time jump, not motion (the Animator's rule)
    constexpr float kPlayerMaxRootMotionPerFrame = 100.0f;

    // the links from under `root` down to `entity` (entity last); empty when it is not under root
    std::vector<RootLink> rootChainOf(Scene* scene, Entity root, Entity entity, const anim::ClipBindings& bindings,
                                      anim::RestMap& rest){
        std::vector<Entity> entities;
        for (Entity e = entity; e != root; ){
            const Transform* transform = scene->isEntityCreated(e) ? scene->findComponent<Transform>(e) : nullptr;
            if (!transform || entities.size() > 256) return {};
            entities.push_back(e);
            e = transform->parent;
            if (e == NULL_ENTITY) return {};
        }
        std::vector<RootLink> chain(entities.size());
        const std::vector<AnimationClip::Track>& tracks = bindings.clip->getTracks();
        for (size_t i = 0; i < entities.size(); i++){
            const Entity e = entities[entities.size() - 1 - i];
            RootLink& link = chain[i];
            const auto found = rest.find(e);
            if (found != rest.end() && found->second.hasTransform){
                link.position = found->second.position;
                link.rotation = found->second.rotation;
                link.scale = found->second.scale;
            }else{
                const Transform& transform = scene->getComponent<Transform>(e);
                link.position = transform.position;
                link.rotation = transform.rotation;
                link.scale = transform.scale;
            }
            for (size_t t = 0; t < tracks.size() && t < bindings.targets.size(); t++){
                if (bindings.targets[t] != e || !tracks[t].isSampled()) continue;
                if (tracks[t].channel == ClipChannel::Position) link.positionTrack = (int)t;
                else if (tracks[t].channel == ClipChannel::Rotation) link.rotationTrack = (int)t;
                else if (tracks[t].channel == ClipChannel::Scale) link.scaleTrack = (int)t;
            }
        }
        return chain;
    }

    // the chain's end in the root's space at clipTime (tracks < 0: the rest)
    void rootChainPose(const std::vector<RootLink>& chain, const AnimationClip* clip, float clipTime, Vector3& position,
                       Quaternion& rotation, Vector3* parentPosition = nullptr){
        anim::Affine world;
        rotation = Quaternion();
        anim::TrackSample sample;
        for (size_t i = 0; i < chain.size(); i++){
            const RootLink& link = chain[i];
            Vector3 p = link.position;
            Quaternion q = link.rotation;
            Vector3 s = link.scale;
            if (clip){
                const std::vector<AnimationClip::Track>& tracks = clip->getTracks();
                if (link.positionTrack >= 0 && anim::sampleTrack(tracks[(size_t)link.positionTrack], clipTime, sample)){
                    p = tracks[(size_t)link.positionTrack].relative ? link.position + sample.vector : sample.vector;
                }
                if (link.rotationTrack >= 0 && anim::sampleTrack(tracks[(size_t)link.rotationTrack], clipTime, sample)){
                    q = tracks[(size_t)link.rotationTrack].relative ? link.rotation * sample.rotation : sample.rotation;
                }
                if (link.scaleTrack >= 0 && anim::sampleTrack(tracks[(size_t)link.scaleTrack], clipTime, sample)){
                    s = tracks[(size_t)link.scaleTrack].relative ? link.scale * sample.vector : sample.vector;
                }
            }
            if (parentPosition && i + 1 == chain.size()) *parentPosition = world.translation();
            world = world * anim::Affine::compose(p, q, s);
            rotation = rotation * q;
        }
        position = world.translation();
    }

    // the basis of a state's clip at clipTime (clip null: the rest pose)
    anim::RootBasisSample playerRootBasis(const StateRootMotion& rm, const AnimationClip* clip, float clipTime){
        Vector3 p;
        Quaternion q;
        rootChainPose(rm.boneChain, clip, clipTime, p, q);
        if (!rm.body) return anim::rootBasis(rm.settings, p, q, nullptr);
        anim::HumanBodyPoints body;
        for (size_t s = 0; s < (size_t)pose::HUMAN_BONE_COUNT; s++){
            if (rm.bodyChains[s].empty()) continue;
            Vector3 at;
            Quaternion unused;
            rootChainPose(rm.bodyChains[s], clip, clipTime, at, unused);
            body.set(pose::boneAt((int)s), at);
        }
        return anim::rootBasis(rm.settings, p, q, &body);
    }

    void playerRootPoint(const StateRootMotion& rm, const AnimationClip& clip, float clipTime, Vector3& p, float& h){
        anim::rootMotionPoint(rm.settings, rm.frame, playerRootBasis(rm, &clip, clipTime), p, h);
    }

    // the root's motion over a step on the stored time line [from, to] (the Animator's rule)
    void playerRootDelta(const StateRootMotion& rm, const AnimationClip& clip, ClipWrapMode wrap, double from, double to,
                         Vector3& delta, float& yaw){
        delta = Vector3(0, 0, 0);
        yaw = 0.0f;
        const double length = clip.getLength();
        if (!(length > 0.0) || from == to) return;
        Vector3 p0, p1;
        float h0 = 0.0f, h1 = 0.0f;
        if (wrap != ClipWrapMode::Loop){
            // clamped by value: MSVC 19.52 at /O2 /Ob2 read the reference std::min(std::max(to, 0.0),
            // length) returns, inside a conditional, through a truncated address (an access violation)
            const auto clipTimeOf = [&](double time){
                if (wrap == ClipWrapMode::PingPong) return anim::wrapClipTime((float)time, (float)length, wrap);
                return (float)(time < 0.0 ? 0.0 : (time > length ? length : time));
            };
            const float a = clipTimeOf(from);
            const float b = clipTimeOf(to);
            playerRootPoint(rm, clip, a, p0, h0);
            playerRootPoint(rm, clip, b, p1, h1);
            delta = anim::rootRotateYaw(anim::rootRotateYaw(p1 - p0, -h0), rm.frame.turn);
            yaw = h1 - h0;
            return;
        }
        const double k0 = std::floor(from / length);
        const double k1 = std::floor(to / length);
        const float t0 = (float)(from - k0 * length);
        const float t1 = (float)(to - k1 * length);
        const int n = (int)std::min(std::max(k1 - k0, -kRootMotionMaxLaps), kRootMotionMaxLaps);
        const Vector3 lap = rm.lapEnd - rm.lapStart;
        const float turn = rm.lapEndHeading;
        Vector3 sum(0, 0, 0);
        if (n > 0){
            for (int m = 0; m < n; m++) sum = sum + anim::rootRotateYaw(lap, (float)m * turn);
        }else if (n < 0){
            for (int m = n; m < 0; m++) sum = sum - anim::rootRotateYaw(lap, (float)m * turn);
        }
        playerRootPoint(rm, clip, t0, p0, h0);
        playerRootPoint(rm, clip, t1, p1, h1);
        const Vector3 inner = sum + anim::rootRotateYaw(p1 - rm.lapStart, (float)n * turn) - (p0 - rm.lapStart);
        delta = anim::rootRotateYaw(anim::rootRotateYaw(inner, -h0), rm.frame.turn);
        yaw = (float)n * turn + h1 - h0;
    }

    // the root bone of a state's clip: the block's bone, the humanoid hips, the shallowest bone it positions
    Entity playerRootBone(Scene* scene, Entity root, const anim::ClipBindings& bindings, const StateRootMotion& rm,
                          const anim::HumanoidSlots* slots){
        if (!rm.settings.bone.empty()) return anim::resolvePath(scene, root, rm.settings.bone);
        if (slots && slots->valid && slots->entities[(size_t)pose::HumanBone::Hips] != NULL_ENTITY){
            return slots->entities[(size_t)pose::HumanBone::Hips];
        }
        Entity best = NULL_ENTITY;
        int bestDepth = 1 << 30;
        const std::vector<AnimationClip::Track>& tracks = bindings.clip->getTracks();
        for (size_t t = 0; t < tracks.size() && t < bindings.targets.size(); t++){
            const Entity e = bindings.targets[t];
            if (e == NULL_ENTITY || e == root || tracks[t].channel != ClipChannel::Position || !tracks[t].isSampled()) continue;
            int depth = 0;
            for (Entity p = scene->getComponent<Transform>(e).parent; p != NULL_ENTITY && p != root && depth < 256;
                 p = scene->getComponent<Transform>(p).parent) depth++;
            if (depth < bestDepth){
                bestDepth = depth;
                best = e;
            }
        }
        return best;
    }

    // `entity` is `ancestor` or under it
    bool rootUnder(Scene* scene, Entity ancestor, Entity entity){
        int guard = 0;
        for (Entity e = entity; e != NULL_ENTITY && guard < 256; e = scene->getComponent<Transform>(e).parent, guard++){
            if (e == ancestor) return true;
        }
        return false;
    }

    // binds a state's root motion to its clip (and bindings); bone NULL_ENTITY when the clip moves no root
    void bindPlayerRootMotion(Scene* scene, Entity root, const std::string& who, anim::AnimationPlayerRuntime& runtime,
                              anim::PlayerStateRuntime& state, bool apply){
        StateRootMotion& rm = state.rootMotion;
        rm.clip = state.bindings.clip.get();
        rm.serial = state.bindings.serial;
        rm.settings = anim::readClipRootMotion(state.bindings.clip->getUnknownKeys());
        rm.bone = NULL_ENTITY;
        rm.boneChain.clear();
        rm.body = false;
        for (std::vector<RootLink>& chain : rm.bodyChains) chain.clear();
        if (!rm.settings.present && !apply) return;
        if (state.bindings.humanoidPath()){
            Log::warn("Animation Player '%s': '%s' plays as a humanoid clip: its root motion needs an Animator", who.c_str(), state.name.c_str());
            return;
        }
        const bool needsBody = anim::rootMotionNeedsBody(rm.settings);
        if (!runtime.humanoid && (needsBody || rm.settings.bone.empty()) && playerdetail::refreshHumanoidSlots(scene, root, runtime.humanoid)){
            runtime.humanoidRevision++;     // what the humanoid masks were stamped with changed
        }
        const anim::HumanoidSlots* slots = runtime.humanoid.get();
        rm.bone = playerRootBone(scene, root, state.bindings, rm, slots);
        if (rm.bone != NULL_ENTITY) rm.boneChain = rootChainOf(scene, root, rm.bone, state.bindings, runtime.rest);
        if (rm.boneChain.empty()){
            rm.bone = NULL_ENTITY;
            return;
        }
        if (needsBody){
            bool usable = slots && slots->valid;
            anim::HumanBodyPoints probe;
            for (size_t s = 0; usable && s < (size_t)pose::HUMAN_BONE_COUNT; s++){
                const Entity e = slots->entities[s];
                if (e == NULL_ENTITY) continue;
                rm.bodyChains[s] = rootChainOf(scene, root, e, state.bindings, runtime.rest);
                // the body moves with the root bone: every slot under it
                if (rm.bodyChains[s].empty() || !rootUnder(scene, rm.bone, e)){
                    usable = false;
                    break;
                }
                probe.set(pose::boneAt((int)s), Vector3(0, 0, 0));
            }
            rm.body = usable && probe.usable();
            if (!rm.body){
                for (std::vector<RootLink>& chain : rm.bodyChains) chain.clear();
                Log::warn("Animation Player '%s': '%s': root motion Based Upon Body Orientation / Center Of Mass / Feet needs a humanoid "
                          "rig under its root bone; it plays as Original", who.c_str(), state.name.c_str());
            }
        }
        const anim::RootBasisSample start = playerRootBasis(rm, rm.clip, 0.0f);
        if (rm.body){
            anim::HumanBodyPoints first, rest;
            for (size_t s = 0; s < (size_t)pose::HUMAN_BONE_COUNT; s++){
                if (rm.bodyChains[s].empty()) continue;
                Vector3 at;
                Quaternion unused;
                rootChainPose(rm.bodyChains[s], rm.clip, 0.0f, at, unused);
                first.set(pose::boneAt((int)s), at);
                rootChainPose(rm.bodyChains[s], nullptr, 0.0f, at, unused);
                rest.set(pose::boneAt((int)s), at);
            }
            rm.frame = anim::rootMotionFrame(rm.settings, start, &first, &rest);
        }else{
            rm.frame = anim::rootMotionFrame(rm.settings, start, nullptr, nullptr);
        }
        float heading = 0.0f;
        playerRootPoint(rm, *rm.clip, 0.0f, rm.lapStart, heading);
        playerRootPoint(rm, *rm.clip, rm.clip->getLength(), rm.lapEnd, rm.lapEndHeading);
    }

    // the entity's local transform composed up to (not including) `root`
    anim::Affine playerParentChain(Scene* scene, Entity root, Entity entity, Quaternion& rotation){
        std::vector<Entity> parents;
        for (Entity p = scene->getComponent<Transform>(entity).parent; p != NULL_ENTITY && p != root && parents.size() < 256;
             p = scene->getComponent<Transform>(p).parent) parents.push_back(p);
        anim::Affine world;
        rotation = Quaternion();
        for (auto it = parents.rbegin(); it != parents.rend(); ++it){
            const Transform& transform = scene->getComponent<Transform>(*it);
            world = world * anim::Affine::compose(transform.position, transform.rotation, transform.scale);
            rotation = rotation * transform.rotation;
        }
        return world;
    }

}

void ActionSystem::stepPlayerRootMotion(Entity entity, AnimationPlayerComponent& player){
    anim::AnimationPlayerRuntime& runtime = *player.runtime;
    runtime.rootDelta = Vector3(0, 0, 0);
    runtime.rootYaw = 0.0f;
    runtime.rootState = -1;
    // a player saved before S94 kept every clip's drift with Apply Root Motion off
    if (!player.applyRootMotion && player.legacyRootDrift) return;
    Vector3 sum(0, 0, 0);
    float yawSum = 0.0f;
    float weightSum = 0.0f;
    float heaviest = 0.0f;
    for (size_t i = 0; i < runtime.states.size(); i++){
        anim::PlayerStateRuntime& state = runtime.states[i];
        if (!state.bindings.clip || !state.bindings.resolved) continue;
        StateRootMotion& rm = state.rootMotion;
        if (rm.clip != state.bindings.clip.get() || rm.serial != state.bindings.serial){
            bindPlayerRootMotion(scene, entity, scene->getEntityName(entity), runtime, state, player.applyRootMotion);
        }else if (!rm.settings.present && !player.applyRootMotion){
            continue;
        }
        if (rm.bone == NULL_ENTITY || runtime.stateLayer(state) != 0 || !(state.weight > 0.0f)) continue;
        if (!(state.enabled || state.finishing || state.held)) continue;
        if (state.weight > heaviest){
            heaviest = state.weight;
            runtime.rootState = (int)i;
        }
        if (!rm.stepped) continue;
        Vector3 delta;
        float yaw = 0.0f;
        playerRootDelta(rm, *state.bindings.clip, anim::stateWrap(state), rm.stepFrom, rm.stepTo, delta, yaw);
        sum = sum + delta * state.weight;
        yawSum += yaw * state.weight;
        weightSum += state.weight;
    }
    if (weightSum > 0.0f){
        runtime.rootDelta = sum / weightSum;
        runtime.rootYaw = yawSum / weightSum;
    }
    if (runtime.rootState >= 0) playersWithRootMotion.push_back(entity);
}

void ActionSystem::applyPlayersRootMotion(double dt){
    for (const Entity entity : playersWithRootMotion){
        AnimationPlayerComponent* player = scene->isEntityCreated(entity) ? scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
        if (!player || !player->runtime) continue;
        anim::AnimationPlayerRuntime& runtime = *player->runtime;
        if (runtime.rootState < 0 || (size_t)runtime.rootState >= runtime.states.size()) continue;
        const StateRootMotion& rm = runtime.states[(size_t)runtime.rootState].rootMotion;
        Transform* bone = rm.bone != NULL_ENTITY && scene->isEntityCreated(rm.bone) ? scene->findComponent<Transform>(rm.bone) : nullptr;
        if (!bone || rm.boneChain.empty()) continue;

        // the root out of the written pose: untracked channels from their rest (so the
        // correction never applies twice), into the root's space and back
        const RootLink& link = rm.boneChain.back();
        const Vector3 local = link.positionTrack >= 0 ? bone->position : link.position;
        const Quaternion localRotation = link.rotationTrack >= 0 ? bone->rotation : link.rotation;
        Quaternion parentRotation;
        const anim::Affine parent = playerParentChain(scene, entity, rm.bone, parentRotation);
        Vector3 p = parent.transformPoint(local);
        Quaternion q = parentRotation * localRotation;
        bone->position = local;
        bone->rotation = localRotation;
        anim::RootBasisSample now;
        if (rm.body){
            anim::HumanBodyPoints body;
            const std::shared_ptr<anim::HumanoidSlots>& slots = runtime.humanoid;
            for (size_t s = 0; slots && s < (size_t)pose::HUMAN_BONE_COUNT; s++){
                const Entity e = slots->entities[s];
                if (e == NULL_ENTITY || rm.bodyChains[s].empty() || !scene->isEntityCreated(e)) continue;
                Quaternion unused;
                const anim::Affine chain = playerParentChain(scene, entity, e, unused);
                body.set(pose::boneAt((int)s), chain.transformPoint(scene->getComponent<Transform>(e).position));
            }
            now = anim::rootBasis(rm.settings, p, q, &body);
        }else{
            now = anim::rootBasis(rm.settings, p, q, nullptr);
        }
        anim::removeRootMotion(rm.settings, rm.frame, now, p, q);
        bone->position = parent.inverse().transformPoint(p);
        bone->rotation = (parentRotation.inverse() * q).normalized();
        bone->needUpdate = true;

        // Apply Root Motion: the delta moves the GameObject (or its Character Controller)
        if (!player->applyRootMotion) continue;
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) continue;
        const Vector3 move = transform->rotation * (transform->scale * runtime.rootDelta);
        if (move.length() > kPlayerMaxRootMotionPerFrame) continue;
        const Quaternion turn = anim::rootYawRotation(runtime.rootYaw);
#ifdef JASPERENGINE_PHYSICS_3D
        if (scene->findComponent<CharacterControllerComponent>(entity) && dt > 0.0){
            const Vector3 worldDelta = transform->worldRotation * (transform->worldScale * runtime.rootDelta);
            scene->getSystem<PhysicsSystem>()->setRootMotion(entity, worldDelta * (float)(1.0 / dt), runtime.rootYaw / (float)dt);
            continue;
        }
#else
        (void)dt;
#endif
        transform->position = transform->position + move;
        transform->rotation = (transform->rotation * turn).normalized();
        transform->needUpdate = true;
    }
    playersWithRootMotion.clear();
}
