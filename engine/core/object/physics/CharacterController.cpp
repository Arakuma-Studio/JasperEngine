// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CharacterController.h"

#include "subsystem/PhysicsSystem.h"

#include <stdexcept>

using namespace jasperengine;

CharacterController::CharacterController(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

CharacterController::~CharacterController() = default;
CharacterController::CharacterController(const CharacterController& rhs) = default;
CharacterController& CharacterController::operator=(const CharacterController& rhs) = default;
CharacterController::CharacterController(CharacterController&& rhs) noexcept = default;
CharacterController& CharacterController::operator=(CharacterController&& rhs) noexcept = default;

CharacterControllerComponent& CharacterController::component() const{
    return getComponent<CharacterControllerComponent>();
}

bool CharacterController::isValid() const{
    return scene && scene->findComponent<CharacterControllerComponent>(entity) != nullptr;
}

int CharacterController::move(const Vector3& motion){
    component();
    return scene->getSystem<PhysicsSystem>()->moveCharacter(entity, motion);
}

bool CharacterController::simpleMove(const Vector3& speed){
    component();
    return scene->getSystem<PhysicsSystem>()->simpleMoveCharacter(entity, speed);
}

bool CharacterController::trySetSize(float height, float radius, const Vector3& center){
    component();
    return scene->getSystem<PhysicsSystem>()->trySetCharacterSize(entity, height, radius, center);
}

void CharacterController::setEnabled(bool enabled){ component().enabled = enabled; }
bool CharacterController::isEnabled() const{ return component().enabled; }

// the size goes through trySetSize: a capsule that does not fit there is refused
void CharacterController::setCenter(const Vector3& center){ const CharacterControllerComponent& c = component(); trySetSize(c.height, c.radius, center); }
Vector3 CharacterController::getCenter() const{ return component().center; }
void CharacterController::setHeight(float height){ const CharacterControllerComponent& c = component(); trySetSize(height, c.radius, c.center); }
float CharacterController::getHeight() const{ return component().height; }
void CharacterController::setRadius(float radius){ const CharacterControllerComponent& c = component(); trySetSize(c.height, radius, c.center); }
float CharacterController::getRadius() const{ return component().radius; }

void CharacterController::setSlopeLimit(float degrees){ component().slopeLimit = degrees; }
float CharacterController::getSlopeLimit() const{ return component().slopeLimit; }
void CharacterController::setStepOffset(float stepOffset){ component().stepOffset = stepOffset; }
float CharacterController::getStepOffset() const{ return component().stepOffset; }
void CharacterController::setSkinWidth(float skinWidth){ component().skinWidth = skinWidth; }
float CharacterController::getSkinWidth() const{ return component().skinWidth; }
void CharacterController::setMinMoveDistance(float distance){ component().minMoveDistance = distance; }
float CharacterController::getMinMoveDistance() const{ return component().minMoveDistance; }
void CharacterController::setMass(float mass){ component().mass = mass; }
float CharacterController::getMass() const{ return component().mass; }
void CharacterController::setPushStrength(float strength){ component().pushStrength = strength; }
float CharacterController::getPushStrength() const{ return component().pushStrength; }
void CharacterController::setPushBodies(bool push){ component().pushBodies = push; }
bool CharacterController::isPushBodies() const{ return component().pushBodies; }
void CharacterController::setMovingPlatforms(bool moving){ component().movingPlatforms = moving; }
bool CharacterController::isMovingPlatforms() const{ return component().movingPlatforms; }
void CharacterController::setInheritPlatformYaw(bool inherit){ component().inheritPlatformYaw = inherit; }
bool CharacterController::isInheritPlatformYaw() const{ return component().inheritPlatformYaw; }
void CharacterController::setDetectCollisions(bool detect){ component().detectCollisions = detect; }
bool CharacterController::isDetectCollisions() const{ return component().detectCollisions; }
void CharacterController::setCharacterCollisions(bool collide){ component().characterCollisions = collide; }
bool CharacterController::isCharacterCollisions() const{ return component().characterCollisions; }
void CharacterController::setEnhancedEdgeRemoval(bool enhanced){ component().enhancedEdgeRemoval = enhanced; }
bool CharacterController::isEnhancedEdgeRemoval() const{ return component().enhancedEdgeRemoval; }

void CharacterController::setBitsFilter(uint16_t categoryBits, uint16_t maskBits){
    CharacterControllerComponent& c = component();
    c.categoryBits = categoryBits;
    c.maskBits = maskBits;
}
uint16_t CharacterController::getCategoryBits() const{ return component().categoryBits; }
uint16_t CharacterController::getMaskBits() const{ return component().maskBits; }

bool CharacterController::isGrounded() const{ return component().isGrounded; }
bool CharacterController::isOnSteepSlope() const{ return component().isOnSteepSlope; }
int CharacterController::getCollisionFlags() const{ return component().collisionFlags; }
Vector3 CharacterController::getVelocity() const{ return component().velocity; }
Vector3 CharacterController::getGroundNormal() const{ return component().groundNormal; }
Entity CharacterController::getGroundEntity() const{ return component().groundEntity; }
Vector3 CharacterController::getGroundVelocity() const{ return component().groundVelocity; }
