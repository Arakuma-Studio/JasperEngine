// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CharacterController2D.h"

#include "subsystem/PhysicsSystem.h"

using namespace jasperengine;

CharacterController2D::CharacterController2D(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

CharacterController2D::~CharacterController2D() = default;
CharacterController2D::CharacterController2D(const CharacterController2D& rhs) = default;
CharacterController2D& CharacterController2D::operator=(const CharacterController2D& rhs) = default;
CharacterController2D::CharacterController2D(CharacterController2D&& rhs) noexcept = default;
CharacterController2D& CharacterController2D::operator=(CharacterController2D&& rhs) noexcept = default;

CharacterController2DComponent& CharacterController2D::component() const{
    return getComponent<CharacterController2DComponent>();
}

bool CharacterController2D::isValid() const{
    return scene && scene->findComponent<CharacterController2DComponent>(entity) != nullptr;
}

int CharacterController2D::move(const Vector2& motion){
    component();
    return scene->getSystem<PhysicsSystem>()->moveCharacter2D(entity, motion);
}

bool CharacterController2D::simpleMove(const Vector2& speed){
    component();
    return scene->getSystem<PhysicsSystem>()->simpleMoveCharacter2D(entity, speed);
}

void CharacterController2D::setEnabled(bool enabled){ component().enabled = enabled; }
bool CharacterController2D::isEnabled() const{ return component().enabled; }

void CharacterController2D::setCenter(const Vector2& center){ component().center = center; }
Vector2 CharacterController2D::getCenter() const{ return component().center; }
void CharacterController2D::setHeight(float height){ component().height = height; }
float CharacterController2D::getHeight() const{ return component().height; }
void CharacterController2D::setRadius(float radius){ component().radius = radius; }
float CharacterController2D::getRadius() const{ return component().radius; }
void CharacterController2D::setSlopeLimit(float degrees){ component().slopeLimit = degrees; }
float CharacterController2D::getSlopeLimit() const{ return component().slopeLimit; }
void CharacterController2D::setStepOffset(float stepOffset){ component().stepOffset = stepOffset; }
float CharacterController2D::getStepOffset() const{ return component().stepOffset; }
void CharacterController2D::setSkinWidth(float skinWidth){ component().skinWidth = skinWidth; }
float CharacterController2D::getSkinWidth() const{ return component().skinWidth; }
void CharacterController2D::setMinMoveDistance(float distance){ component().minMoveDistance = distance; }
float CharacterController2D::getMinMoveDistance() const{ return component().minMoveDistance; }
void CharacterController2D::setPushStrength(float strength){ component().pushStrength = strength; }
float CharacterController2D::getPushStrength() const{ return component().pushStrength; }
void CharacterController2D::setPushBodies(bool push){ component().pushBodies = push; }
bool CharacterController2D::isPushBodies() const{ return component().pushBodies; }
void CharacterController2D::setMovingPlatforms(bool moving){ component().movingPlatforms = moving; }
bool CharacterController2D::isMovingPlatforms() const{ return component().movingPlatforms; }

void CharacterController2D::setBitsFilter(uint16_t categoryBits, uint16_t maskBits){
    CharacterController2DComponent& c = component();
    c.categoryBits = categoryBits;
    c.maskBits = maskBits;
}
uint16_t CharacterController2D::getCategoryBits() const{ return component().categoryBits; }
uint16_t CharacterController2D::getMaskBits() const{ return component().maskBits; }

bool CharacterController2D::isGrounded() const{ return component().isGrounded; }
bool CharacterController2D::isOnSteepSlope() const{ return component().isOnSteepSlope; }
int CharacterController2D::getCollisionFlags() const{ return component().collisionFlags; }
Vector2 CharacterController2D::getVelocity() const{ return component().velocity; }
Vector2 CharacterController2D::getGroundNormal() const{ return component().groundNormal; }
Entity CharacterController2D::getGroundEntity() const{ return component().groundEntity; }
Vector2 CharacterController2D::getGroundVelocity() const{ return component().groundVelocity; }
