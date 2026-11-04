// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The ragdolls of PhysicsSystem (physics-3d 6.1.11): their parts (the Rigidbodies below the
// root), the switch between animated (kinematic parts following the bones, the Animation Player
// on) and simulated (dynamic parts moving the bones, the player paused), and self collision.

#include "PhysicsSystem.h"
#include "component/Transform.h"

// 3D physics only (JASPERENGINE_PHYSICS_3D, from PhysicsSystem.h's includes)
#ifdef JASPERENGINE_PHYSICS_3D
#include "Scene.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/ModelComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "object/physics/Ragdoll.h"
#include "object/physics/Rigidbody.h"
#include "component/RagdollComponent.h"
#include "util/JoltPhysicsAux.h"

#include <algorithm>
#include <limits>

using namespace jasperengine;


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
    // the hook before the bones go back to the animation (Ragdoll::onBeforeAnimatedRestore)
    if (!ragdoll.simulated && !firstApply && onRagdollBeforeAnimatedRestore3D.hasSubscribers()){
        onRagdollBeforeAnimatedRestore3D.call((int)entity);
    }

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
        pauseRagdollDrivers3D(entity, ragdoll);
        if (animator != NULL_ENTITY){
            AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(animator);
            if (ragdoll.pausedAnimator != animator){
                ragdoll.animatorWasEnabled = player.enabled;
                ragdoll.pausedAnimator = animator;
            }
            player.enabled = false;
        }
    }else{
        resumeRagdollDrivers3D(ragdoll);
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
    flushRagdollPushes3D(entity, ragdoll);
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
        // pushes given inside the last step
        flushRagdollPushes3D(entity, ragdoll);
    }
}

void PhysicsSystem::releaseRagdoll(RagdollComponent& ragdoll){
    applyRagdollSelfCollision(ragdoll, true);
    resumeRagdollDrivers3D(ragdoll);
    ragdoll.pendingPushes.clear();
    // a removed ragdoll leaves its player playing
    if (ragdoll.pausedAnimator != NULL_ENTITY && scene->isEntityCreated(ragdoll.pausedAnimator)){
        if (AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(ragdoll.pausedAnimator)){
            player->enabled = ragdoll.animatorWasEnabled;
        }
    }
    ragdoll.pausedAnimator = NULL_ENTITY;
    ragdoll.appliedState = -1;
}

void PhysicsSystem::updateRagdoll(Entity entity){
    RagdollComponent* ragdoll = scene->findComponent<RagdollComponent>(entity);
    if (!ragdoll || steppingWorld3D || !ragdoll->enabled || !scene->isEntityActiveInHierarchy(entity)){
        return;
    }
    refreshRagdoll(entity);
    applyRagdollState(entity, *ragdoll);
}

namespace {
    // the part nearest the point (Unity's "hit bone")
    Entity nearestRagdollPart(Scene* scene, const std::vector<Entity>& parts, const Vector3& point){
        Entity nearest = NULL_ENTITY;
        float nearestDistance = std::numeric_limits<float>::max();
        for (Entity part : parts){
            Rigidbody body(scene, part);
            const RigidbodyComponent& component = scene->getComponent<RigidbodyComponent>(part);
            if (component.body.IsInvalid()) continue;
            const float distance = (body.getCenterOfMassPosition() - point).squaredLength();
            if (distance < nearestDistance){
                nearestDistance = distance;
                nearest = part;
            }
        }
        return nearest;
    }

    bool ragdollStateApplied(const RagdollComponent& ragdoll){
        return ragdoll.appliedState == (ragdoll.simulated ? 1 : 0);
    }
}

void PhysicsSystem::applyRagdollImpulse3D(Entity entity, const Vector3& impulse, const Vector3& point){
    RagdollComponent* ragdoll = scene->findComponent<RagdollComponent>(entity);
    if (!ragdoll){
        return;
    }
    refreshRagdoll(entity);
    // the parts are not what the state says yet (simulated set this frame, inside a step, not
    // loaded): pushed when they are
    if (!ragdollStateApplied(*ragdoll) || steppingWorld3D){
        ragdoll->pendingPushes.push_back({impulse, point, false});
        return;
    }
    const Entity nearest = nearestRagdollPart(scene, ragdoll->parts, point);
    if (nearest != NULL_ENTITY){
        Rigidbody(scene, nearest).applyImpulse(impulse, point);
    }
}

void PhysicsSystem::applyRagdollForce3D(Entity entity, const Vector3& force){
    RagdollComponent* ragdoll = scene->findComponent<RagdollComponent>(entity);
    if (!ragdoll){
        return;
    }
    refreshRagdoll(entity);
    if (!ragdollStateApplied(*ragdoll) || steppingWorld3D){
        ragdoll->pendingPushes.push_back({force, Vector3::ZERO, true});
        return;
    }
    float total = 0.0f;
    std::vector<float> masses;
    masses.reserve(ragdoll->parts.size());
    for (Entity part : ragdoll->parts){
        const float mass = scene->getComponent<RigidbodyComponent>(part).body.IsInvalid() ? 0.0f : Rigidbody(scene, part).getMass();
        masses.push_back(mass);
        total += mass;
    }
    if (!(total > 0.0f)){
        return;
    }
    for (size_t i = 0; i < ragdoll->parts.size(); i++){
        if (masses[i] > 0.0f){
            Rigidbody(scene, ragdoll->parts[i]).applyForce(force * (masses[i] / total));
        }
    }
}

void PhysicsSystem::flushRagdollPushes3D(Entity entity, RagdollComponent& ragdoll){
    if (ragdoll.pendingPushes.empty()){
        return;
    }
    std::vector<RagdollComponent::Push> pushes;
    pushes.swap(ragdoll.pendingPushes);
    for (const RagdollComponent::Push& push : pushes){
        if (push.force){
            applyRagdollForce3D(entity, push.value);
        }else{
            applyRagdollImpulse3D(entity, push.value, push.point);
        }
    }
}

// The other things that move the bones: Model entity animations (Model:playAnimation, running
// Animation actions), Pose Animators, and Vertex Animators of the character (they would keep
// deforming the mesh while the bodies hold the bones). Looked for on root, its ancestors and its
// subtree; only what is running is paused, and resumed when animated again.
void PhysicsSystem::pauseRagdollDrivers3D(Entity entity, RagdollComponent& ragdoll){
    if (!ragdoll.pausedDrivers.empty()){
        return;
    }
    const Entity root = ragdollRoot(ragdoll, entity);
    std::vector<Entity> candidates;
    for (Entity current = root; current != NULL_ENTITY;){
        candidates.push_back(current);
        Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    auto transforms = scene->getComponentArray<Transform>();
    if (transforms->hasEntity(root)){
        const size_t first = transforms->getIndex(root);
        const size_t last = scene->findBranchLastIndex(root);
        for (size_t i = first + 1; i <= last && i < transforms->size(); i++){
            candidates.push_back(transforms->getEntity(i));
        }
    }
    for (Entity candidate : candidates){
        if (ModelComponent* model = scene->findComponent<ModelComponent>(candidate)){
            for (Entity animation : model->animations){
                if (animation == NULL_ENTITY || !scene->isEntityCreated(animation)) continue;
                ActionComponent* action = scene->findComponent<ActionComponent>(animation);
                if (action && (action->state == ActionState::Running || action->startTrigger)){
                    action->startTrigger = false;
                    action->pauseTrigger = true;
                    ragdoll.pausedDrivers.emplace_back(animation, RagdollComponent::Driver::ACTION);
                }
            }
        }
        if (PoseAnimatorComponent* pose = scene->findComponent<PoseAnimatorComponent>(candidate)){
            if (pose->playing || (pose->autoplay && !pose->started)){
                pose->playing = false;
                pose->started = true;
                ragdoll.pausedDrivers.emplace_back(candidate, RagdollComponent::Driver::POSE);
            }
        }
        if (VertexAnimatorComponent* vertex = scene->findComponent<VertexAnimatorComponent>(candidate)){
            if (vertex->enabled){
                vertex->enabled = false;
                ragdoll.pausedDrivers.emplace_back(candidate, RagdollComponent::Driver::VERTEX);
            }
        }
    }
}

void PhysicsSystem::resumeRagdollDrivers3D(RagdollComponent& ragdoll){
    for (const auto& [driver, kind] : ragdoll.pausedDrivers){
        if (!scene->isEntityCreated(driver)) continue;
        switch (kind){
            case RagdollComponent::Driver::ACTION:
                if (ActionComponent* action = scene->findComponent<ActionComponent>(driver)){
                    action->pauseTrigger = false;
                    action->startTrigger = true;
                }
                break;
            case RagdollComponent::Driver::POSE:
                if (PoseAnimatorComponent* pose = scene->findComponent<PoseAnimatorComponent>(driver)){
                    pose->playing = true;
                }
                break;
            case RagdollComponent::Driver::VERTEX:
                if (VertexAnimatorComponent* vertex = scene->findComponent<VertexAnimatorComponent>(driver)){
                    vertex->enabled = true;
                }
                break;
        }
    }
    ragdoll.pausedDrivers.clear();
}

#endif
