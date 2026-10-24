// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Rigidbody.h"

#include "object/GameObject.h"
#include "subsystem/PhysicsSystem.h"
#include "component/RigidbodyComponent.h"
#include "util/JoltPhysicsAux.h"

#include <iostream>
#include <algorithm>

using namespace jasperengine;

JPH::EMotionType getBodyTypeToJolt(BodyType type){
    if (type == BodyType::STATIC){
        return JPH::EMotionType::Static;
    }else if (type == BodyType::KINEMATIC){
        return JPH::EMotionType::Kinematic;
    }else if (type == BodyType::DYNAMIC){
        return JPH::EMotionType::Dynamic;
    }

    return JPH::EMotionType::Static;;
}

BodyType getJoltToBodyType(JPH::EMotionType type){
    if (type == JPH::EMotionType::Static){
        return BodyType::STATIC;
    }else if (type == JPH::EMotionType::Kinematic){
        return BodyType::KINEMATIC;
    }else if (type == JPH::EMotionType::Dynamic){
        return BodyType::DYNAMIC;
    }

    return BodyType::STATIC;
}

static int appendCollider3D(RigidbodyComponent& body){
    if (!body.colliders.validIndex(body.numColliders)){
        Log::error("Cannot add more shapes in this body");
        return -1;
    }

    size_t index = body.numColliders;
    body.colliders[index] = Collider3D();
    body.numColliders++;
    body.needUpdateColliders = true;

    return static_cast<int>(index);
}

Rigidbody::Rigidbody(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

Rigidbody::~Rigidbody() = default;
Rigidbody::Rigidbody(const Rigidbody& rhs) = default;
Rigidbody& Rigidbody::operator=(const Rigidbody& rhs) = default;
Rigidbody::Rigidbody(Rigidbody&& rhs) noexcept = default;
Rigidbody& Rigidbody::operator=(Rigidbody&& rhs) noexcept = default;

void Rigidbody::checkBody(const RigidbodyComponent& body) const{
    // Scripts start (Lua init(), C++ constructors) before their scene enters the Engine,
    // which is what loads the physics bodies: make this one now rather than fail
    if (body.body.IsInvalid()){
        scene->getSystem<PhysicsSystem>()->ensureRigidbodyLoaded(entity);
    }
    if (body.body.IsInvalid()){
        Log::error("Rigidbody is not loaded");
        throw std::runtime_error("Rigidbody is not loaded");
    }
}

const JPH::BodyLockInterface& Rigidbody::getBodyLockInterface() const{
    return scene->getSystem<PhysicsSystem>()->getBodyLockInterface3D();
}

JPH::BodyInterface& Rigidbody::getBodyInterface() const{
    return scene->getSystem<PhysicsSystem>()->getBodyInterface3D();
}

const JPH::Body& Rigidbody::getJoltBody() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);

    JPH::BodyLockRead lock(getBodyLockInterface(), body.body);
    if (!lock.Succeeded())
        throw std::runtime_error("Rigidbody is locked");

    return lock.GetBody();
}

JPH::Body& Rigidbody::getJoltBodyWrite() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);

    JPH::BodyLockWrite lock(getBodyLockInterface(), body.body);
    if (!lock.Succeeded())
        throw std::runtime_error("Rigidbody is locked");

    return lock.GetBody();
}

JPH::Body* Rigidbody::getJoltBodyNoLock() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    return getBodyLockInterface().TryGetBody(body.body);
}

GameObject Rigidbody::getAttachedObject(){
    return GameObject(scene, entity);
}

void Rigidbody::load(){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.needReloadBody = true;
    body.needUpdateColliders = true;

    scene->getSystem<PhysicsSystem>()->loadRigidbody(entity);
}

int Rigidbody::createBoxCollider(float width, float height, float depth){
    return createBoxCollider(Vector3::ZERO, Quaternion::IDENTITY, width, height, depth);
}

int Rigidbody::createBoxCollider(Vector3 position, Quaternion rotation, float width, float height, float depth){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::BOX;
    shape.position = position;
    shape.rotation = rotation;
    shape.width = width;
    shape.height = height;
    shape.depth = depth;

    return index;
}

int Rigidbody::createSphereCollider(float radius){
    return createSphereCollider(Vector3::ZERO, Quaternion::IDENTITY, radius);
}

int Rigidbody::createSphereCollider(Vector3 position, Quaternion rotation, float radius){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::SPHERE;
    shape.position = position;
    shape.rotation = rotation;
    shape.radius = radius;

    return index;
}

int Rigidbody::createCapsuleCollider(float halfHeight, float radius){
    return createCapsuleCollider(Vector3::ZERO, Quaternion::IDENTITY, halfHeight, radius);
}

int Rigidbody::createCapsuleCollider(Vector3 position, Quaternion rotation, float halfHeight, float radius){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::CAPSULE;
    shape.position = position;
    shape.rotation = rotation;
    shape.halfHeight = halfHeight;
    shape.radius = radius;

    return index;
}

int Rigidbody::createTaperedCapsuleCollider(float halfHeight, float topRadius, float bottomRadius){
    return createTaperedCapsuleCollider(Vector3::ZERO, Quaternion::IDENTITY, halfHeight, topRadius, bottomRadius);
}

int Rigidbody::createTaperedCapsuleCollider(Vector3 position, Quaternion rotation, float halfHeight, float topRadius, float bottomRadius){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::TAPERED_CAPSULE;
    shape.position = position;
    shape.rotation = rotation;
    shape.halfHeight = halfHeight;
    shape.topRadius = topRadius;
    shape.bottomRadius = bottomRadius;

    return index;
}

int Rigidbody::createCylinderCollider(float halfHeight, float radius){
    return createCylinderCollider(Vector3::ZERO, Quaternion::IDENTITY, halfHeight, radius);
}

int Rigidbody::createCylinderCollider(Vector3 position, Quaternion rotation, float halfHeight, float radius){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::CYLINDER;
    shape.position = position;
    shape.rotation = rotation;
    shape.halfHeight = halfHeight;
    shape.radius = radius;

    return index;
}

int Rigidbody::createConvexHullCollider(){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::CONVEX_HULL;
    shape.source = Collider3DSource::ENTITY_MESH;
    shape.sourceEntity = entity;

    return index;
}

int Rigidbody::createConvexHullCollider(std::vector<Vector3> vertices){
    return createConvexHullCollider(Vector3::ZERO, Quaternion::IDENTITY, vertices);
}

int Rigidbody::createConvexHullCollider(Vector3 position, Quaternion rotation, std::vector<Vector3> vertices){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::CONVEX_HULL;
    shape.position = position;
    shape.rotation = rotation;
    shape.source = Collider3DSource::RAW_VERTICES;
    shape.numVertices = vertices.size();
    for (size_t i = 0; i < shape.numVertices; i++){
        if (!shape.vertices.validIndex(i)){
            Log::warn("Convex hull vertices truncated from %zu to %zu", shape.numVertices, i);
            shape.numVertices = i;
            break;
        }
        shape.vertices[i] = vertices[i];
    }

    return index;
}

int Rigidbody::createMeshCollider(){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::MESH;
    shape.source = Collider3DSource::ENTITY_MESH;
    shape.sourceEntity = entity;

    return index;
}

int Rigidbody::createMeshCollider(std::vector<Vector3> vertices, std::vector<uint16_t> indices){
    return createMeshCollider(Vector3::ZERO, Quaternion::IDENTITY, vertices, indices);
}

int Rigidbody::createMeshCollider(Vector3 position, Quaternion rotation, std::vector<Vector3> vertices, std::vector<uint16_t> indices){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::MESH;
    shape.position = position;
    shape.rotation = rotation;
    shape.source = Collider3DSource::RAW_MESH;
    shape.numVertices = vertices.size();
    for (size_t i = 0; i < shape.numVertices; i++){
        if (!shape.vertices.validIndex(i)){
            Log::warn("Mesh shape vertices truncated from %zu to %zu", shape.numVertices, i);
            shape.numVertices = i;
            break;
        }
        shape.vertices[i] = vertices[i];
    }
    shape.numIndices = indices.size();
    for (size_t i = 0; i < shape.numIndices; i++){
        if (!shape.indices.validIndex(i)){
            Log::warn("Mesh shape indices truncated from %zu to %zu", shape.numIndices, i);
            shape.numIndices = i;
            break;
        }
        shape.indices[i] = indices[i];
    }

    return index;
}

int Rigidbody::createHeightFieldCollider(){
    return createHeightFieldCollider(0);
}

int Rigidbody::createHeightFieldCollider(unsigned int samplesSize){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::HEIGHTFIELD;
    shape.source = Collider3DSource::ENTITY_HEIGHTFIELD;
    shape.sourceEntity = entity;
    shape.samplesSize = samplesSize;

    return index;
}

size_t Rigidbody::getNumColliders() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    return body.numColliders;
}

Collider3DType Rigidbody::getColliderType(size_t index) const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    if (index >= 0 && index < body.numColliders){
        return body.colliders[index].type;
    }else{
        Log::error("Cannot find shape %i of body", index);
        throw std::runtime_error("Cannot find shape");
    }
}

void Rigidbody::setColliderDensity(float density){
    setColliderDensity(0, density);
}

void Rigidbody::setColliderDensity(size_t index, float density){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    if (index >= 0 && index < body.numColliders){
        body.colliders[index].density = density;
        if (body.colliders[index].shape && body.colliders[index].shape->GetType() == JPH::EShapeType::Convex){
            JPH::ConvexShape* convexShape = (JPH::ConvexShape*)body.colliders[index].shape.GetPtr();
            convexShape->SetDensity(density);
        }else{
            Log::warn("The shape %i does not have density", index);
        }
    }else{
        Log::error("Cannot find shape %i of body", index);
    }
}

float Rigidbody::getColliderDensity() const{
    return getColliderDensity(0);
}

float Rigidbody::getColliderDensity(size_t index) const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    if (index >= 0 && index < body.numColliders){
        return body.colliders[index].density;
    }else{
        Log::error("Cannot find shape %i of body", index);
    }

    return 0;
}

void Rigidbody::setType(BodyType type){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    body.type = type;

    if (!body.body.IsInvalid()){
        JPH::EActivation activation = JPH::EActivation::DontActivate;

        if (type != BodyType::STATIC){
            activation = JPH::EActivation::Activate;
        }

        JPH::BodyInterface& bodyInterface = getBodyInterface();
        bodyInterface.SetMotionType(body.body, getBodyTypeToJolt(type), activation);
        if (type != BodyType::STATIC){
            bodyInterface.SetMotionQuality(body.body, PhysicsSystem::getRigidbodyMotionQualityToJolt(body.motionQuality));
        }
    }
}

BodyType Rigidbody::getType() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    return getJoltToBodyType(getBodyInterface().GetMotionType(body.body));
}

void Rigidbody::setMotionQuality(RigidbodyMotionQuality motionQuality){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    body.motionQuality = motionQuality;

    if (!body.body.IsInvalid()){
        getBodyInterface().SetMotionQuality(body.body, PhysicsSystem::getRigidbodyMotionQualityToJolt(motionQuality));
    }
}

RigidbodyMotionQuality Rigidbody::getMotionQuality() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    return body.motionQuality;
}

bool Rigidbody::canBeKinematicOrDynamic() const{
    return getJoltBody().CanBeKinematicOrDynamic();
}

void Rigidbody::activate(){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);

    getBodyInterface().ActivateBody(body.body);
}

void Rigidbody::deactivate(){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);

    getBodyInterface().DeactivateBody(body.body);
}

bool Rigidbody::isSensor() const{
    return getComponent<RigidbodyComponent>().sensor;
}

void Rigidbody::setIsSensor(bool sensor){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.sensor = sensor;

    if (!body.body.IsInvalid()){
        getJoltBodyWrite().SetIsSensor(sensor);
    }
}

bool Rigidbody::isCollideKinematicVsNonDynamic() const{
    return getJoltBody().GetCollideKinematicVsNonDynamic();
}

void Rigidbody::setCollideKinematicVsNonDynamic(bool collideKinematicVsNonDynamic){
    getJoltBodyWrite().SetCollideKinematicVsNonDynamic(collideKinematicVsNonDynamic);
}

void Rigidbody::applyAllowedDOFs(JPH::EAllowedDOFs allowedDOFs){
    if (allowedDOFs == JPH::EAllowedDOFs::None){
        Log::error("Rigidbody allowed DOFs cannot be empty (Jolt forbids it). Use a static or kinematic body, or lock movement with setGravityFactor(0).");
        return;
    }

    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.allowedDOFs = static_cast<uint8_t>(allowedDOFs);

    if (!body.body.IsInvalid()){
        JPH::Body& jbody = getJoltBodyWrite();
        JPH::MotionProperties* motionProperties = jbody.GetMotionPropertiesUnchecked();
        if (motionProperties){
            motionProperties->SetMassProperties(allowedDOFs, jbody.GetShape()->GetMassProperties());
        }
    }
}

void Rigidbody::setAllowedDOFsAll(){
    applyAllowedDOFs(JPH::EAllowedDOFs::All);
}

void Rigidbody::setAllowedDOFs2DPlane(){
    applyAllowedDOFs(JPH::EAllowedDOFs::Plane2D);
}

void Rigidbody::setAllowedDOFs(bool translationX, bool translationY, bool translationZ, bool rotationX, bool rotationY, bool rotationZ){
    JPH::EAllowedDOFs allowedDOFs = JPH::EAllowedDOFs::None;

    if (translationX){
        allowedDOFs |= JPH::EAllowedDOFs::TranslationX;
    }
    if (translationY){
        allowedDOFs |= JPH::EAllowedDOFs::TranslationY;
    }
    if (translationZ){
        allowedDOFs |= JPH::EAllowedDOFs::TranslationZ;
    }
    if (rotationX){
        allowedDOFs |= JPH::EAllowedDOFs::RotationX;
    }
    if (rotationY){
        allowedDOFs |= JPH::EAllowedDOFs::RotationY;
    }
    if (rotationZ){
        allowedDOFs |= JPH::EAllowedDOFs::RotationZ;
    }

    applyAllowedDOFs(allowedDOFs);
}

float Rigidbody::getMass() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    return getBodyInterface().GetShape(body.body)->GetMassProperties().mMass;
}

void Rigidbody::setMass(float mass){
    JPH::Body& jbody = getJoltBodyWrite();
    JPH::MassProperties massProperties = jbody.GetShape()->GetMassProperties();
    massProperties.mMass = mass;
    jbody.GetMotionProperties()->SetMassProperties(jbody.GetMotionProperties()->GetAllowedDOFs(), massProperties);
}

void Rigidbody::setOverrideMassAndInertia(Vector3 solidBoxSize, float solidBoxDensity){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    if (!body.body.IsInvalid()){
        body.solidBoxSize = solidBoxSize;
        body.solidBoxDensity = solidBoxDensity;
        body.overrideMassProperties = true;
    }else{
        Log::error("Cannot override mass and inertia of loaded body");
    }
}

float Rigidbody::getGravityFactor() const{
    return getComponent<RigidbodyComponent>().gravityFactor;
}

void Rigidbody::setGravityFactor(float gravityFactor){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.gravityFactor = gravityFactor;

    if (!body.body.IsInvalid()){
        JPH::MotionProperties* motionProperties = getJoltBodyWrite().GetMotionPropertiesUnchecked();
        if (motionProperties){
            motionProperties->SetGravityFactor(gravityFactor);
        }
    }
}

// Kept in the component, so a reloaded body keeps them, and combined with the GameObject
// layer into the Jolt ObjectLayer
void Rigidbody::setBitsFilter(uint16_t category, uint16_t mask){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    body.categoryBits = category;
    body.maskBits = mask;
    scene->getSystem<PhysicsSystem>()->refreshRigidbodyFilter3D(entity);
}

uint16_t Rigidbody::getCategoryBitsFilter() const{
    return getComponent<RigidbodyComponent>().categoryBits;
}

void Rigidbody::setCategoryBitsFilter(uint16_t category){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    body.categoryBits = category;
    scene->getSystem<PhysicsSystem>()->refreshRigidbodyFilter3D(entity);
}

uint16_t Rigidbody::getMaskBitsFilter() const{
    return getComponent<RigidbodyComponent>().maskBits;
}

void Rigidbody::setMaskBitsFilter(uint16_t mask){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    body.maskBits = mask;
    scene->getSystem<PhysicsSystem>()->refreshRigidbodyFilter3D(entity);
}

uint32_t Rigidbody::getCollisionGroupID() const{
    return getJoltBody().GetCollisionGroup().GetGroupID();
}

void Rigidbody::setCollisionGroupID(uint32_t group){
    JPH::Ref<JPH::GroupFilterTable> group_filter = new JPH::GroupFilterTable(2);
    group_filter->DisableCollision(0, 0);

    getJoltBodyWrite().SetCollisionGroup(JPH::CollisionGroup(group_filter, group, 0));
}

bool Rigidbody::isAllowSleeping() const{
    return getJoltBody().GetAllowSleeping();
}

void Rigidbody::setAllowSleeping(bool allowSleeping){
    getJoltBodyWrite().SetAllowSleeping(allowSleeping);
}

// Kept in the component too, so a body made again (a collider or scale change) keeps them
float Rigidbody::getFriction() const{
    return getComponent<RigidbodyComponent>().friction;
}

void Rigidbody::setFriction(float friction){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    if (friction < 0 || friction > 1){
        Log::warn("Friction should be 0 and 1, 0 = no friction");
    }

    body.friction = friction;
    if (!body.body.IsInvalid()){
        getBodyInterface().SetFriction(body.body, friction);
    }
}

float Rigidbody::getRestitution() const{
    return getComponent<RigidbodyComponent>().restitution;
}

void Rigidbody::setRestitution(float restitution){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    body.restitution = restitution;
    if (!body.body.IsInvalid()){
        getBodyInterface().SetRestitution(body.body, restitution);
    }
}

float Rigidbody::getBuoyancy() const{
    return getComponent<RigidbodyComponent>().buoyancy;
}

void Rigidbody::setBuoyancy(float buoyancy){
    getComponent<RigidbodyComponent>().buoyancy = buoyancy;
}

float Rigidbody::getWaterDrag() const{
    return getComponent<RigidbodyComponent>().waterDrag;
}

void Rigidbody::setWaterDrag(float waterDrag){
    getComponent<RigidbodyComponent>().waterDrag = waterDrag;
}

float Rigidbody::getWaterAngularDrag() const{
    return getComponent<RigidbodyComponent>().waterAngularDrag;
}

void Rigidbody::setWaterAngularDrag(float waterAngularDrag){
    getComponent<RigidbodyComponent>().waterAngularDrag = waterAngularDrag;
}

Vector3 Rigidbody::getPosition() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    JPH::RVec3 position = getBodyInterface().GetPosition(body.body);

    return Vector3(position.GetX(), position.GetY(), position.GetZ());
}

void Rigidbody::setPosition(Vector3 position){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    getBodyInterface().SetPosition(body.body, JPH::Vec3(position.x, position.y, position.z), JPH::EActivation::Activate);

    scene->getSystem<PhysicsSystem>()->updateTransformFromRigidbody(entity, position, getRotation());
}

Quaternion Rigidbody::getRotation() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    JPH::Quat rotation = getBodyInterface().GetRotation(body.body);

    return Quaternion(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
}

void Rigidbody::setRotation(Quaternion rotation){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    std::shared_ptr<PhysicsSystem> physicsSystem = scene->getSystem<PhysicsSystem>();

    checkBody(body);
    getBodyInterface().SetRotation(body.body, physicsSystem->toValidatedJoltRotation(rotation, entity, -1), JPH::EActivation::Activate);

    physicsSystem->updateTransformFromRigidbody(entity, getPosition(), getRotation());
}

Vector3 Rigidbody::getLinearVelocity() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    JPH::Vec3 vec = getBodyInterface().GetLinearVelocity(body.body);

    return Vector3(vec.GetX(), vec.GetY(), vec.GetZ());
}

void Rigidbody::setLinearVelocity(Vector3 linearVelocity){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    getBodyInterface().SetLinearVelocity(body.body, JPH::Vec3(linearVelocity.x, linearVelocity.y, linearVelocity.z));
}

void Rigidbody::setLinearVelocityClamped(Vector3 linearVelocity){
    getJoltBodyWrite().SetLinearVelocityClamped(JPH::Vec3(linearVelocity.x, linearVelocity.y, linearVelocity.z));
}

Vector3 Rigidbody::getAngularVelocity() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    JPH::Vec3 vec = getBodyInterface().GetAngularVelocity(body.body);

    return Vector3(vec.GetX(), vec.GetY(), vec.GetZ());
}

void Rigidbody::setAngularVelocity(Vector3 angularVelocity){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    getBodyInterface().SetAngularVelocity(body.body, JPH::Vec3(angularVelocity.x, angularVelocity.y, angularVelocity.z));
}

void Rigidbody::setAngularVelocityClamped(Vector3 angularVelocity){
    getJoltBodyWrite().SetAngularVelocityClamped(JPH::Vec3(angularVelocity.x, angularVelocity.y, angularVelocity.z));
}

Vector3 Rigidbody::getPointVelocityCOM(Vector3 pointRelativeToCOM) const{
    JPH::Vec3 vec = getJoltBody().GetPointVelocityCOM(JPH::Vec3(pointRelativeToCOM.x, pointRelativeToCOM.y, pointRelativeToCOM.z));

    return Vector3(vec.GetX(), vec.GetY(), vec.GetZ());
}

Vector3 Rigidbody::getPointVelocity(Vector3 point) const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    JPH::Vec3 vec = getBodyInterface().GetPointVelocity(body.body, JPH::Vec3(point.x, point.y, point.z));

    return Vector3(vec.GetX(), vec.GetY(), vec.GetZ());
}

Vector3 Rigidbody::getAccumulatedForce() const{
    JPH::Vec3 vec = getJoltBody().GetAccumulatedForce();

    return Vector3(vec.GetX(), vec.GetY(), vec.GetZ());
}

Vector3 Rigidbody::getAccumulatedTorque() const{
    JPH::Vec3 vec = getJoltBody().GetAccumulatedTorque();

    return Vector3(vec.GetX(), vec.GetY(), vec.GetZ());
}

Matrix4 Rigidbody::getInverseInertia() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    JPH::Mat44 mat = getBodyInterface().GetInverseInertia(body.body);

    Matrix4 retMat;
    for (int i = 0; i < 4; i++){
        retMat.setColumn(i, Vector4(mat.GetColumn4(i).GetX(), mat.GetColumn4(i).GetY(), mat.GetColumn4(i).GetZ(), mat.GetColumn4(i).GetW()));
    }

    return retMat;
}

void Rigidbody::applyForce(const Vector3& force){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    getBodyInterface().AddForce(body.body, JPH::Vec3(force.x, force.y, force.z));
}

void Rigidbody::applyForce(const Vector3& force, const Vector3& point){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    getBodyInterface().AddForce(body.body, JPH::Vec3(force.x, force.y, force.z), JPH::Vec3(point.x, point.y, point.z));
}

void Rigidbody::applyTorque(const Vector3& torque){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    getBodyInterface().AddTorque(body.body, JPH::Vec3(torque.x, torque.y, torque.z));
}

void Rigidbody::applyImpulse(const Vector3& impulse){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    getBodyInterface().AddImpulse(body.body, JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

void Rigidbody::applyImpulse(const Vector3& impulse, const Vector3& point){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    getBodyInterface().AddImpulse(body.body, JPH::Vec3(impulse.x, impulse.y, impulse.z), JPH::Vec3(point.x, point.y, point.z));
}

void Rigidbody::applyAngularImpulse(const Vector3& angularImpulse){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    getBodyInterface().AddAngularImpulse(body.body, JPH::Vec3(angularImpulse.x, angularImpulse.y, angularImpulse.z));
}

bool Rigidbody::applyBuoyancyImpulse(const Vector3& surfacePosition, const Vector3& surfaceNormal, const float buoyancy, const float linearDrag, const float angularDrag, const Vector3& fluidVelocity, const Vector3& gravity, const float deltaTime){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    JPH::Vec3 surPos = JPH::Vec3(surfacePosition.x, surfacePosition.y, surfacePosition.z);
    JPH::Vec3 surNor = JPH::Vec3(surfaceNormal.x, surfaceNormal.y, surfaceNormal.z);
    JPH::Vec3 fluVel = JPH::Vec3(fluidVelocity.x, fluidVelocity.y, fluidVelocity.z);
    JPH::Vec3 grav = JPH::Vec3(gravity.x, gravity.y, gravity.z);

    return getJoltBodyWrite().ApplyBuoyancyImpulse(surPos, surNor, buoyancy, linearDrag, angularDrag, fluVel, grav, deltaTime);
}

Vector3 Rigidbody::getCenterOfMassPosition(){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    JPH::Vec3 vec = getBodyInterface().GetCenterOfMassPosition(body.body);

    return Vector3(vec.GetX(), vec.GetY(), vec.GetZ());
}

void Rigidbody::setEnabled(bool enabled){
    getComponent<RigidbodyComponent>().enabled = enabled;
}

bool Rigidbody::isEnabled() const{
    return getComponent<RigidbodyComponent>().enabled;
}
