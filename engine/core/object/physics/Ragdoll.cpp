// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Ragdoll.h"

#include "Rigidbody.h"
#include "Scene.h"
#include "subsystem/PhysicsSystem.h"


using namespace jasperengine;

Ragdoll::Ragdoll(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

Ragdoll::~Ragdoll() = default;
Ragdoll::Ragdoll(const Ragdoll& rhs) = default;
Ragdoll& Ragdoll::operator=(const Ragdoll& rhs) = default;
Ragdoll::Ragdoll(Ragdoll&& rhs) noexcept = default;
Ragdoll& Ragdoll::operator=(Ragdoll&& rhs) noexcept = default;

RagdollComponent& Ragdoll::ragdoll() const{
    return getComponent<RagdollComponent>();
}

void Ragdoll::setEnabled(bool enabled){
    ragdoll().enabled = enabled;
}

bool Ragdoll::isEnabled() const{
    return ragdoll().enabled;
}

bool Ragdoll::isSimulated() const{
    return ragdoll().simulated;
}

void Ragdoll::setSimulated(bool simulated){
    ragdoll().simulated = simulated;
    // now, so an impulse right after reaches dynamic parts (inside a step: at the next one)
    scene->getSystem<PhysicsSystem>()->updateRagdoll(entity);
}

Entity Ragdoll::getRoot() const{
    return ragdoll().root;
}

void Ragdoll::setRoot(Entity root){
    ragdoll().root = root;
    ragdoll().needCollectParts = true;
}

Entity Ragdoll::getAnimator() const{
    return ragdoll().animator;
}

void Ragdoll::setAnimator(Entity animator){
    ragdoll().animator = animator;
}

bool Ragdoll::isInheritVelocity() const{
    return ragdoll().inheritVelocity;
}

void Ragdoll::setInheritVelocity(bool inheritVelocity){
    ragdoll().inheritVelocity = inheritVelocity;
}

bool Ragdoll::isSelfCollision() const{
    return ragdoll().selfCollision;
}

void Ragdoll::setSelfCollision(bool selfCollision){
    ragdoll().selfCollision = selfCollision;
}

float Ragdoll::getTotalMass() const{
    return ragdoll().totalMass;
}

void Ragdoll::setTotalMass(float totalMass){
    ragdoll().totalMass = totalMass;
}

std::vector<Entity> Ragdoll::getPartEntities() const{
    // collected now when the ragdoll has not stepped yet (a script's init())
    scene->getSystem<PhysicsSystem>()->refreshRagdoll(entity);
    return ragdoll().parts;
}

size_t Ragdoll::getPartCount() const{
    return getPartEntities().size();
}

std::vector<Rigidbody> Ragdoll::getParts() const{
    std::vector<Rigidbody> parts;
    for (Entity part : getPartEntities()){
        parts.emplace_back(scene, part);
    }
    return parts;
}

void Ragdoll::applyImpulse(const Vector3& impulse, const Vector3& point){
    scene->getSystem<PhysicsSystem>()->applyRagdollImpulse3D(entity, impulse, point);
}

void Ragdoll::applyForce(const Vector3& force){
    scene->getSystem<PhysicsSystem>()->applyRagdollForce3D(entity, force);
}

FunctionSubscribe<void(int)>& Ragdoll::onBeforeAnimatedRestore() const{
    return scene->getSystem<PhysicsSystem>()->onRagdollBeforeAnimatedRestore3D;
}
