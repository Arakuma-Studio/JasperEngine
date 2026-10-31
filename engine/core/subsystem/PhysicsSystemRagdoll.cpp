// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The ragdolls of PhysicsSystem (physics-3d 6.1.11): their parts (the Rigidbodies below the
// root), the switch between animated (kinematic parts following the bones, the Animation Player
// on) and simulated (dynamic parts moving the bones, the player paused), and self collision.

#include "PhysicsSystem.h"
#include "Scene.h"
#include "component/AnimationPlayerComponent.h"
#include "component/RagdollComponent.h"
#include "util/JoltPhysicsAux.h"

#include <algorithm>

using namespace jasperengine;

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    Entity ragdollRoot(const RagdollComponent& ragdoll, Entity entity){
        return ragdoll.root != NULL_ENTITY ? ragdoll.root : entity;
    }

    // the Animation Player paused while simulated: the given one, else the first on root, its
    // ancestors, then its descendants
    Entity ragdollAnimator(Scene* scene, const RagdollComponent& ragdoll, Entity entity){
        if (ragdoll.animator != NULL_ENTITY){
            return (scene->isEntityCreated(ragdoll.animator) && scene->findComponent<AnimationPlayerComponent>(ragdoll.animator)) ? ragdoll.animator : NULL_ENTITY;
        }
        const Entity root = ragdollRoot(ragdoll, entity);
        for (Entity current = root; current != NULL_ENTITY;){
            if (scene->findComponent<AnimationPlayerComponent>(current)){
                return current;
            }
            Transform* transform = scene->findComponent<Transform>(current);
            current = transform ? transform->parent : NULL_ENTITY;
        }
        auto transforms = scene->getComponentArray<Transform>();
        if (!transforms->hasEntity(root)){
            return NULL_ENTITY;
        }
        const size_t first = transforms->getIndex(root);
        const size_t last = scene->findBranchLastIndex(root);
        for (size_t i = first + 1; i <= last && i < transforms->size(); i++){
            const Entity candidate = transforms->getEntity(i);
            if (scene->findComponent<AnimationPlayerComponent>(candidate)){
                return candidate;
            }
        }
        return NULL_ENTITY;
    }

}

void PhysicsSystem::collectRagdollParts(Entity entity, RagdollComponent& ragdoll){
    std::vector<Entity> parts;
    const Entity root = ragdollRoot(ragdoll, entity);
    auto transforms = scene->getComponentArray<Transform>();
    if (scene->isEntityCreated(root) && transforms->hasEntity(root)){
        const size_t first = transforms->getIndex(root);
        const size_t last = scene->findBranchLastIndex(root);
        for (size_t i = first + 1; i <= last && i < transforms->size(); i++){
            const Entity candidate = transforms->getEntity(i);
            if (scene->findComponent<RigidbodyComponent>(candidate)){
                parts.push_back(candidate);
            }
        }
    }

    // parts that left keep the state they had; new ones take the ragdoll's at the next apply
    if (parts != ragdoll.parts){
        applyRagdollSelfCollision(ragdoll, true);
        ragdoll.parts = parts;
        ragdoll.appliedState = -1;
    }
    ragdoll.needCollectParts = false;
    ragdoll.collectedHierarchyRevision = scene->hierarchyRevision();
    ragdoll.collectedBodyCount = scene->getComponentArray<RigidbodyComponent>()->size();
}

void PhysicsSystem::applyRagdollSelfCollision(RagdollComponent& ragdoll, bool selfCollision){
    // off: every part pair is kept from colliding (the RAGDOLL reason of the pair filter)
    for (const auto& pair : ragdoll.ignoredPairs){
        setBodyPairIgnored3D(pair.first, pair.second, JoltPairFilter3D::RAGDOLL, false);
    }
    ragdoll.ignoredPairs.clear();
    if (!selfCollision){
        for (size_t i = 0; i < ragdoll.parts.size(); i++){
            for (size_t j = i + 1; j < ragdoll.parts.size(); j++){
                setBodyPairIgnored3D(ragdoll.parts[i], ragdoll.parts[j], JoltPairFilter3D::RAGDOLL, true);
                ragdoll.ignoredPairs.emplace_back(ragdoll.parts[i], ragdoll.parts[j]);
            }
        }
    }
    ragdoll.appliedSelfCollision = selfCollision;
}

void PhysicsSystem::applyRagdollState(Entity entity, RagdollComponent& ragdoll){
    const int state = ragdoll.simulated ? 1 : 0;
    if (ragdoll.appliedSelfCollision != ragdoll.selfCollision || (!ragdoll.selfCollision && ragdoll.ignoredPairs.empty() && ragdoll.parts.size() > 1)){
        applyRagdollSelfCollision(ragdoll, ragdoll.selfCollision);
    }
    if (ragdoll.appliedState == state){
        return;
    }
    const bool firstApply = ragdoll.appliedState < 0;

    for (Entity part : ragdoll.parts){
        RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(part);
        if (!body){
            continue;
        }
        if (ragdoll.simulated){
            // the bodies take over the bones, with the motion the animation gave them
            setRigidbodyType(part, *body, BodyType::DYNAMIC);
            if (!ragdoll.inheritVelocity && !body->body.IsInvalid()){
                getBodyInterface3D().SetLinearAndAngularVelocity(body->body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            }
        }else{
            // the bones take over the bodies again (they snap to the animated pose, Unity)
            setRigidbodyType(part, *body, BodyType::KINEMATIC);
            if (!body->body.IsInvalid()){
                getBodyInterface3D().SetLinearAndAngularVelocity(body->body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            }
        }
    }

    const Entity animator = ragdollAnimator(scene, ragdoll, entity);
    if (ragdoll.simulated){
        if (animator != NULL_ENTITY){
            AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(animator);
            if (ragdoll.pausedAnimator != animator){
                ragdoll.animatorWasEnabled = player.enabled;
                ragdoll.pausedAnimator = animator;
            }
            player.enabled = false;
        }
    }else{
        if (ragdoll.pausedAnimator != NULL_ENTITY && scene->isEntityCreated(ragdoll.pausedAnimator)){
            if (AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(ragdoll.pausedAnimator)){
                player->enabled = ragdoll.animatorWasEnabled;
            }
        }else if (!firstApply && animator != NULL_ENTITY){
            scene->getComponent<AnimationPlayerComponent>(animator).enabled = true;
        }
        ragdoll.pausedAnimator = NULL_ENTITY;
    }
    ragdoll.appliedState = state;
}

void PhysicsSystem::refreshRagdoll(Entity entity){
    RagdollComponent* ragdoll = scene->findComponent<RagdollComponent>(entity);
    if (!ragdoll){
        return;
    }
    if (ragdoll->needCollectParts || ragdoll->collectedHierarchyRevision != scene->hierarchyRevision() ||
            ragdoll->collectedBodyCount != scene->getComponentArray<RigidbodyComponent>()->size()){
        collectRagdollParts(entity, *ragdoll);
    }
}

void PhysicsSystem::updateRagdolls(){
    auto ragdolls = scene->getComponentArray<RagdollComponent>();
    for (size_t i = 0; i < ragdolls->size(); i++){
        RagdollComponent& ragdoll = ragdolls->getComponentFromIndex(i);
        const Entity entity = ragdolls->getEntity(i);
        if (!ragdoll.enabled || !scene->isEntityActiveInHierarchy(entity)){
            continue;
        }
        refreshRagdoll(entity);
        applyRagdollState(entity, ragdoll);
    }
}

void PhysicsSystem::releaseRagdoll(RagdollComponent& ragdoll){
    applyRagdollSelfCollision(ragdoll, true);
    // a removed ragdoll leaves its player playing
    if (ragdoll.pausedAnimator != NULL_ENTITY && scene->isEntityCreated(ragdoll.pausedAnimator)){
        if (AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(ragdoll.pausedAnimator)){
            player->enabled = ragdoll.animatorWasEnabled;
        }
    }
    ragdoll.pausedAnimator = NULL_ENTITY;
    ragdoll.appliedState = -1;
}

#endif
