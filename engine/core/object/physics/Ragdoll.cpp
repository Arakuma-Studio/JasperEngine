// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Ragdoll.h"

#include "Rigidbody.h"
#include "Scene.h"
#include "subsystem/PhysicsSystem.h"

#include <limits>

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
    Entity nearest = NULL_ENTITY;
    float nearestDistance = std::numeric_limits<float>::max();
    for (Entity part : getPartEntities()){
        Rigidbody body(scene, part);
        const float distance = (body.getCenterOfMassPosition() - point).squaredLength();
        if (distance < nearestDistance){
            nearestDistance = distance;
            nearest = part;
        }
    }
    if (nearest != NULL_ENTITY){
        Rigidbody(scene, nearest).applyImpulse(impulse, point);
    }
}

void Ragdoll::applyForce(const Vector3& force){
    std::vector<Entity> parts = getPartEntities();
    float total = 0.0f;
    std::vector<float> masses;
    masses.reserve(parts.size());
    for (Entity part : parts){
        const float mass = Rigidbody(scene, part).getMass();
        masses.push_back(mass);
        total += mass;
    }
    if (!(total > 0.0f)){
        return;
    }
    for (size_t i = 0; i < parts.size(); i++){
        Rigidbody(scene, parts[i]).applyForce(force * (masses[i] / total));
    }
}
