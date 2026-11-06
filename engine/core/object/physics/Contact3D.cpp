// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Contact3D.h"

#include "Scene.h"
#include "subsystem/PhysicsSystem.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Collision/ContactListener.h"

#include <algorithm>
#include <tuple>

using namespace jasperengine;

bool ContactPairKey3D::operator<(const ContactPairKey3D& other) const{
    return std::tie(body1, body2, subShape1, subShape2) < std::tie(other.body1, other.body2, other.subShape1, other.subShape2);
}

bool ContactPairKey3D::operator==(const ContactPairKey3D& other) const{
    return body1 == other.body1 && body2 == other.body2 && subShape1 == other.subShape1 && subShape2 == other.subShape2;
}

// an empty snapshot: no Jolt pointers to read
Contact3D::Contact3D(): snapshot(std::make_shared<Contact3DSnapshot>()){
}

Contact3D::Contact3D(Scene* scene, const JPH::Body* body1, const JPH::Body* body2, const JPH::ContactManifold* contactManifold, JPH::ContactSettings* contactSettings){
    this->scene = scene;
    this->body1 = body1;
    this->body2 = body2;
    this->contactManifold = contactManifold;
    this->contactSettings = contactSettings;
    key.body1 = body1->GetID().GetIndexAndSequenceNumber();
    key.body2 = body2->GetID().GetIndexAndSequenceNumber();
    key.subShape1 = contactManifold->mSubShapeID1.GetValue();
    key.subShape2 = contactManifold->mSubShapeID2.GetValue();
}

Contact3D::Contact3D(Scene* scene, std::shared_ptr<const Contact3DSnapshot> snapshot){
    this->scene = scene;
    this->snapshot = snapshot ? std::move(snapshot) : std::make_shared<Contact3DSnapshot>();
    this->key = this->snapshot->key;
}

Contact3D::~Contact3D() = default;
Contact3D::Contact3D(const Contact3D& rhs) = default;
Contact3D& Contact3D::operator=(const Contact3D& rhs) = default;

const ContactPairKey3D& Contact3D::getKey() const{
    return key;
}

const JPH::ContactManifold* Contact3D::getJoltContactManifold() const{
    return contactManifold;
}

JPH::ContactSettings* Contact3D::getJoltContactSettings() const{
    return contactSettings;
}

size_t Contact3D::getNumContactPoints() const{
    return getContactPointCount();
}

Contact3DSnapshot Contact3D::takeSnapshot() const{
    if (snapshot){
        return *snapshot;
    }
    Contact3DSnapshot copy;
    copy.key = key;
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
    copy.approachSpeed = getApproachSpeed();
    return copy;
}

float Contact3D::getApproachSpeed() const{
    if (snapshot) return snapshot->approachSpeed;
    JPH::Vec3 centre = JPH::Vec3::sZero();
    for (const JPH::Vec3& point : contactManifold->mRelativeContactPointsOn1) centre += point;
    for (const JPH::Vec3& point : contactManifold->mRelativeContactPointsOn2) centre += point;
    const size_t points = contactManifold->mRelativeContactPointsOn1.size() + contactManifold->mRelativeContactPointsOn2.size();
    if (points > 0) centre /= (float)points;
    const JPH::RVec3 world = contactManifold->mBaseOffset + centre;
    const JPH::Vec3 relative = body1->GetPointVelocity(world) - body2->GetPointVelocity(world);
    return std::max(0.0f, relative.Dot(contactManifold->mWorldSpaceNormal));
}

Contact3DSnapshot& Contact3D::editSnapshot(){
    // the copies of this Contact3D keep what they had
    std::shared_ptr<Contact3DSnapshot> copy = std::make_shared<Contact3DSnapshot>(*snapshot);
    snapshot = copy;
    return *copy;
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

size_t Contact3D::getContactPointCount() const{
    if (snapshot) return snapshot->pointsOn1.size();
    return contactManifold->mRelativeContactPointsOn1.size();
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
    if (!snapshot){
        contactSettings->mCombinedFriction = combinedFriction;
        return;
    }
    editSnapshot().combinedFriction = combinedFriction;
    if (scene)
        scene->getSystem<PhysicsSystem>()->setContactFriction3D(key, combinedFriction);
}

float Contact3D::getCombinedRestitution() const{
    if (snapshot) return snapshot->combinedRestitution;
    return contactSettings->mCombinedRestitution;
}

void Contact3D::setCombinedRestitution(float combinedRestitution){
    if (!snapshot){
        contactSettings->mCombinedRestitution = combinedRestitution;
        return;
    }
    editSnapshot().combinedRestitution = combinedRestitution;
    if (scene)
        scene->getSystem<PhysicsSystem>()->setContactRestitution3D(key, combinedRestitution);
}

bool Contact3D::isSensor() const{
    if (snapshot) return snapshot->sensor;
    return contactSettings->mIsSensor;
}

void Contact3D::setIsSensor(bool sensor){
    if (!snapshot){
        contactSettings->mIsSensor = sensor;
        return;
    }
    editSnapshot().sensor = sensor;
    if (scene)
        scene->getSystem<PhysicsSystem>()->setContactSensor3D(key, sensor);
}
