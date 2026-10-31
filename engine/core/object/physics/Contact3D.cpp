// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Contact3D.h"

#include "Scene.h"
#include "subsystem/PhysicsSystem.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Collision/ContactListener.h"

#include <tuple>

using namespace jasperengine;

namespace {

    Vector3 toVector3(JPH::Vec3Arg v){
        return Vector3(v.GetX(), v.GetY(), v.GetZ());
    }

}

bool ContactPairKey3D::operator<(const ContactPairKey3D& other) const{
    return std::tie(body1, body2, subShape1, subShape2) < std::tie(other.body1, other.body2, other.subShape1, other.subShape2);
}

bool ContactPairKey3D::operator==(const ContactPairKey3D& other) const{
    return body1 == other.body1 && body2 == other.body2 && subShape1 == other.subShape1 && subShape2 == other.subShape2;
}

Contact3D::Contact3D() = default;

Contact3D::Contact3D(Scene* scene, const JPH::Body* body1, const JPH::Body* body2, const JPH::ContactManifold* contactManifold, const JPH::ContactSettings* contactSettings){
    this->scene = scene;
    key.body1 = body1->GetID().GetIndexAndSequenceNumber();
    key.body2 = body2->GetID().GetIndexAndSequenceNumber();
    key.subShape1 = contactManifold->mSubShapeID1.GetValue();
    key.subShape2 = contactManifold->mSubShapeID2.GetValue();

    baseOffset = Vector3(contactManifold->mBaseOffset.GetX(), contactManifold->mBaseOffset.GetY(), contactManifold->mBaseOffset.GetZ());
    worldSpaceNormal = toVector3(contactManifold->mWorldSpaceNormal);
    penetrationDepth = contactManifold->mPenetrationDepth;
    colliderIndex1 = body1->GetShape()->GetSubShapeUserData(contactManifold->mSubShapeID1);
    colliderIndex2 = body2->GetShape()->GetSubShapeUserData(contactManifold->mSubShapeID2);
    for (JPH::Vec3 point : contactManifold->mRelativeContactPointsOn1){
        pointsOn1.push_back(toVector3(point));
    }
    for (JPH::Vec3 point : contactManifold->mRelativeContactPointsOn2){
        pointsOn2.push_back(toVector3(point));
    }

    combinedFriction = contactSettings->mCombinedFriction;
    combinedRestitution = contactSettings->mCombinedRestitution;
    sensor = contactSettings->mIsSensor;
}

Contact3D::~Contact3D() = default;
Contact3D::Contact3D(const Contact3D& rhs) = default;
Contact3D& Contact3D::operator=(const Contact3D& rhs) = default;

const ContactPairKey3D& Contact3D::getKey() const{
    return key;
}

Vector3 Contact3D::getBaseOffset() const{
    return baseOffset;
}

Vector3 Contact3D::getWorldSpaceNormal() const{
    return worldSpaceNormal;
}

float Contact3D::getPenetrationDepth() const{
    return penetrationDepth;
}

size_t Contact3D::getColliderIndex1() const{
    return colliderIndex1;
}

size_t Contact3D::getColliderIndex2() const{
    return colliderIndex2;
}

size_t Contact3D::getContactPointCount() const{
    return pointsOn1.size();
}

Vector3 Contact3D::getRelativeContactPointsOnA(size_t index) const{
    return index < pointsOn1.size() ? pointsOn1[index] : Vector3(0, 0, 0);
}

Vector3 Contact3D::getRelativeContactPointsOnB(size_t index) const{
    return index < pointsOn2.size() ? pointsOn2[index] : Vector3(0, 0, 0);
}

float Contact3D::getCombinedFriction() const{
    return combinedFriction;
}

void Contact3D::setCombinedFriction(float combinedFriction){
    this->combinedFriction = combinedFriction;
    if (scene)
        scene->getSystem<PhysicsSystem>()->setContactFriction3D(key, combinedFriction);
}

float Contact3D::getCombinedRestitution() const{
    return combinedRestitution;
}

void Contact3D::setCombinedRestitution(float combinedRestitution){
    this->combinedRestitution = combinedRestitution;
    if (scene)
        scene->getSystem<PhysicsSystem>()->setContactRestitution3D(key, combinedRestitution);
}

bool Contact3D::isSensor() const{
    return sensor;
}

void Contact3D::setIsSensor(bool sensor){
    this->sensor = sensor;
    if (scene)
        scene->getSystem<PhysicsSystem>()->setContactSensor3D(key, sensor);
}
