// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Contact3D.h"

#include <algorithm>

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Collision/ContactListener.h"
#include "subsystem/PhysicsSystem.h"
#include "Scene.h"

using namespace jasperengine;

Contact3D::Contact3D(Scene* scene, const JPH::Body* body1, const JPH::Body* body2, const JPH::ContactManifold* contactManifold, JPH::ContactSettings* contactSettings){
    this->scene = scene;
    this->body1 = body1;
    this->body2 = body2;
    this->contactManifold = contactManifold;
    this->contactSettings = contactSettings;
}

Contact3D::Contact3D(Scene* scene, std::shared_ptr<const Contact3DSnapshot> snapshot){
    this->scene = scene;
    this->body1 = nullptr;
    this->body2 = nullptr;
    this->contactManifold = nullptr;
    this->contactSettings = nullptr;
    this->snapshot = std::move(snapshot);
}

Contact3D::~Contact3D() = default;
Contact3D::Contact3D(const Contact3D& rhs) = default;
Contact3D& Contact3D::operator=(const Contact3D& rhs) = default;

const JPH::ContactManifold* Contact3D::getJoltContactManifold() const{
    return contactManifold;
}

JPH::ContactSettings* Contact3D::getJoltContactSettings() const{
    return contactSettings;
}

size_t Contact3D::getNumContactPoints() const{
    if (snapshot) return snapshot->pointsOn1.size();
    return contactManifold->mRelativeContactPointsOn1.size();
}

Contact3DSnapshot Contact3D::takeSnapshot() const{
    if (snapshot){
        return *snapshot;
    }
    Contact3DSnapshot copy;
    copy.baseOffset = getBaseOffset();
    copy.worldSpaceNormal = getWorldSpaceNormal();
    copy.penetrationDepth = getPenetrationDepth();
    copy.colliderIndex1 = getColliderIndex1();
    copy.colliderIndex2 = getColliderIndex2();
    const size_t points1 = std::min<size_t>(4, contactManifold->mRelativeContactPointsOn1.size());
    const size_t points2 = std::min<size_t>(4, contactManifold->mRelativeContactPointsOn2.size());
    for (size_t i = 0; i < points1; i++) copy.pointsOn1.push_back(getRelativeContactPointsOnA(i));
    for (size_t i = 0; i < points2; i++) copy.pointsOn2.push_back(getRelativeContactPointsOnB(i));
    copy.combinedFriction = getCombinedFriction();
    copy.combinedRestitution = getCombinedRestitution();
    copy.sensor = isSensor();
    return copy;
}

void Contact3D::warnSnapshotSetter() const{
    if (scene){
        scene->getSystem<PhysicsSystem>()->warnContactSnapshotSetter();
    }
}

Vector3 Contact3D::getBaseOffset() const{
    if (snapshot) return snapshot->baseOffset;
    return Vector3(contactManifold->mBaseOffset.GetX(), contactManifold->mBaseOffset.GetY(), contactManifold->mBaseOffset.GetZ());
}

Vector3 Contact3D::getWorldSpaceNormal() const{
    if (snapshot) return snapshot->worldSpaceNormal;
    return Vector3(contactManifold->mWorldSpaceNormal.GetX(), contactManifold->mWorldSpaceNormal.GetY(), contactManifold->mWorldSpaceNormal.GetZ());
}

float Contact3D::getPenetrationDepth() const{
    if (snapshot) return snapshot->penetrationDepth;
    return contactManifold->mPenetrationDepth;
}

size_t Contact3D::getColliderIndex1() const{
    if (snapshot) return snapshot->colliderIndex1;
    return body1->GetShape()->GetSubShapeUserData(contactManifold->mSubShapeID1);
}

size_t Contact3D::getColliderIndex2() const{
    if (snapshot) return snapshot->colliderIndex2;
    return body2->GetShape()->GetSubShapeUserData(contactManifold->mSubShapeID2);
}

Vector3 Contact3D::getRelativeContactPointsOnA(size_t index) const{
    if (snapshot) return index < snapshot->pointsOn1.size() ? snapshot->pointsOn1[index] : Vector3(0, 0, 0);
    if (index >= contactManifold->mRelativeContactPointsOn1.size()) return Vector3(0, 0, 0);
    JPH::Vec3 point = contactManifold->mRelativeContactPointsOn1[index];
    return Vector3(point.GetX(), point.GetY(), point.GetZ());
}

Vector3 Contact3D::getRelativeContactPointsOnB(size_t index) const{
    if (snapshot) return index < snapshot->pointsOn2.size() ? snapshot->pointsOn2[index] : Vector3(0, 0, 0);
    if (index >= contactManifold->mRelativeContactPointsOn2.size()) return Vector3(0, 0, 0);
    JPH::Vec3 point = contactManifold->mRelativeContactPointsOn2[index];
    return Vector3(point.GetX(), point.GetY(), point.GetZ());
}

float Contact3D::getCombinedFriction() const{
    if (snapshot) return snapshot->combinedFriction;
    return contactSettings->mCombinedFriction;
}

void Contact3D::setCombinedFriction(float combinedFriction){
    if (snapshot){ warnSnapshotSetter(); return; }
    contactSettings->mCombinedFriction = combinedFriction;
}

float Contact3D::getCombinedRestitution() const{
    if (snapshot) return snapshot->combinedRestitution;
    return contactSettings->mCombinedRestitution;
}

void Contact3D::setCombinedRestitution(float combinedRestitution){
    if (snapshot){ warnSnapshotSetter(); return; }
    contactSettings->mCombinedRestitution = combinedRestitution;
}

bool Contact3D::isSensor() const{
    if (snapshot) return snapshot->sensor;
    return contactSettings->mIsSensor;
}

void Contact3D::setIsSensor(bool sensor){
    if (snapshot){ warnSnapshotSetter(); return; }
    contactSettings->mIsSensor = sensor;
}
