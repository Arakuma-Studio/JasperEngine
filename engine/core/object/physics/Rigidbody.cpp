// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Rigidbody.h"

#include "object/GameObject.h"
#include "subsystem/PhysicsSystem.h"
#include "component/RigidbodyComponent.h"
#include "util/JoltPhysicsAux.h"
#include "pool/PhysicsMaterialPool.h"
#include "physics/AttachedColliders.h"
#include "Engine.h"
#include <cmath>

#include <iostream>
#include <algorithm>
#include <stdexcept>

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
    // any number of colliders (no export-only cap)
    const size_t index = body.colliders.size();
    body.colliders.push_back(Collider3D());
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

bool Rigidbody::isValid() const{
    return scene && scene->findComponent<RigidbodyComponent>(entity) != nullptr;
}

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
    markChanged<RigidbodyComponent>();

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
    shape.vertices = std::move(vertices);

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

int Rigidbody::createMeshCollider(std::vector<Vector3> vertices, std::vector<uint32_t> indices){
    return createMeshCollider(Vector3::ZERO, Quaternion::IDENTITY, std::move(vertices), std::move(indices));
}

int Rigidbody::createMeshCollider(Vector3 position, Quaternion rotation, std::vector<Vector3> vertices, std::vector<uint32_t> indices){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    int index = appendCollider3D(body);
    if (index < 0) return index;

    Collider3D& shape = body.colliders[index];
    shape.type = Collider3DType::MESH;
    shape.position = position;
    shape.rotation = rotation;
    shape.source = Collider3DSource::RAW_MESH;
    shape.vertices = std::move(vertices);
    shape.indices = std::move(indices);

    return index;
}

int Rigidbody::createMeshCollider(std::vector<Vector3> vertices, std::vector<uint16_t> indices){
    return createMeshCollider(Vector3::ZERO, Quaternion::IDENTITY, std::move(vertices), std::vector<uint32_t>(indices.begin(), indices.end()));
}

int Rigidbody::createMeshCollider(Vector3 position, Quaternion rotation, std::vector<Vector3> vertices, std::vector<uint16_t> indices){
    return createMeshCollider(position, rotation, std::move(vertices), std::vector<uint32_t>(indices.begin(), indices.end()));
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
    return getComponent<RigidbodyComponent>().colliders.size();
}

size_t Rigidbody::getColliderCount() const{
    return getNumColliders();
}

Collider3DType Rigidbody::getColliderType(size_t index) const{
    return colliderAt(index).type;
}

void Rigidbody::setColliderDensity(float density){
    setColliderDensity(0, density);
}

void Rigidbody::setColliderDensity(size_t index, float density){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    if (index < body.colliders.size()){
        body.colliders[index].density = density;
        if (body.colliders[index].shape && body.colliders[index].shape->GetType() == JPH::EShapeType::Convex){
            JPH::ConvexShape* convexShape = (JPH::ConvexShape*)body.colliders[index].shape.GetPtr();
            convexShape->SetDensity(density);
            // the body's mass follows (its shape is made again from the leaves)
            collidersChanged();
        }else if (body.colliders[index].shape){
            Log::warn("The shape %zu does not have density", index);
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

float Rigidbody::getColliderDensity() const{
    return getColliderDensity(0);
}

float Rigidbody::getColliderDensity(size_t index) const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    if (index < body.colliders.size()){
        return body.colliders[index].density;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return 0;
}

bool Rigidbody::setType(BodyType type){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    return scene->getSystem<PhysicsSystem>()->setRigidbodyType(entity, body, type);
}

bool Rigidbody::isKinematic() const{
    return getComponent<RigidbodyComponent>().type == BodyType::KINEMATIC;
}

void Rigidbody::setKinematic(bool kinematic){
    setType(kinematic ? BodyType::KINEMATIC : BodyType::DYNAMIC);
}

BodyType Rigidbody::getType() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    // a switch asked for inside a step reads back at once
    if (body.hasPendingType){
        return body.pendingType;
    }
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
    return getComponent<RigidbodyComponent>().collideKinematicVsNonDynamic;
}

void Rigidbody::setCollideKinematicVsNonDynamic(bool collideKinematicVsNonDynamic){
    getComponent<RigidbodyComponent>().collideKinematicVsNonDynamic = collideKinematicVsNonDynamic;
    parametersChanged();
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
    // the DOFs are set; the mass, centre of mass and inertia the fields ask for again
    massChanged();
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

void Rigidbody::getAllowedDOFs(bool& translationX, bool& translationY, bool& translationZ, bool& rotationX, bool& rotationY, bool& rotationZ) const{
    const uint8_t flags = getComponent<RigidbodyComponent>().allowedDOFs;
    translationX = (flags & RigidbodyAllowedDOF_TranslationX) != 0;
    translationY = (flags & RigidbodyAllowedDOF_TranslationY) != 0;
    translationZ = (flags & RigidbodyAllowedDOF_TranslationZ) != 0;
    rotationX = (flags & RigidbodyAllowedDOF_RotationX) != 0;
    rotationY = (flags & RigidbodyAllowedDOF_RotationY) != 0;
    rotationZ = (flags & RigidbodyAllowedDOF_RotationZ) != 0;
}

float Rigidbody::getMass() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    if (body.overrideMassProperties){
        JPH::MassProperties box;
        box.SetMassAndInertiaOfSolidBox(JPH::Vec3(body.solidBoxSize.x, body.solidBoxSize.y, body.solidBoxSize.z), body.solidBoxDensity);
        return box.mMass;
    }
    if (!body.automaticMass){
        return body.mass;
    }
    PhysicsSystem::MassInfo3D info;
    if (scene->getSystem<PhysicsSystem>()->computeMassProperties3D(entity, body, info)){
        return info.mass;
    }
    // no collider with a volume: what Jolt has
    checkBody(body);
    return getBodyInterface().GetShape(body.body)->GetMassProperties().mMass;
}

void Rigidbody::setMass(float mass){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    if (!(mass > 0.0f) || !std::isfinite(mass)){
        Log::error("Rigidbody mass must be above 0 (got %g)", mass);
        return;
    }
    body.automaticMass = false;
    body.mass = mass;
    massChanged();
}

bool Rigidbody::isAutomaticMass() const{
    return getComponent<RigidbodyComponent>().automaticMass;
}

void Rigidbody::setAutomaticMass(bool automaticMass){
    getComponent<RigidbodyComponent>().automaticMass = automaticMass;
    massChanged();
}

Vector3 Rigidbody::getCenterOfMass() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    if (!body.automaticCenterOfMass){
        return body.centerOfMass;
    }
    PhysicsSystem::MassInfo3D info;
    if (scene->getSystem<PhysicsSystem>()->computeMassProperties3D(entity, body, info)){
        return info.centerOfMass;
    }
    return Vector3::ZERO;
}

void Rigidbody::setCenterOfMass(Vector3 centerOfMass){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.automaticCenterOfMass = false;
    body.centerOfMass = centerOfMass;
    massChanged();
}

bool Rigidbody::isAutomaticCenterOfMass() const{
    return getComponent<RigidbodyComponent>().automaticCenterOfMass;
}

void Rigidbody::setAutomaticCenterOfMass(bool automaticCenterOfMass){
    getComponent<RigidbodyComponent>().automaticCenterOfMass = automaticCenterOfMass;
    massChanged();
}

void Rigidbody::resetCenterOfMass(){
    setAutomaticCenterOfMass(true);
}

Vector3 Rigidbody::getInertiaTensor() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    if (!body.automaticInertiaTensor){
        return body.inertiaTensor;
    }
    PhysicsSystem::MassInfo3D info;
    if (scene->getSystem<PhysicsSystem>()->computeMassProperties3D(entity, body, info)){
        return info.inertiaDiagonal;
    }
    return Vector3::ZERO;
}

void Rigidbody::setInertiaTensor(Vector3 inertiaTensor){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.automaticInertiaTensor = false;
    body.inertiaTensor = inertiaTensor;
    massChanged();
}

Quaternion Rigidbody::getInertiaTensorRotation() const{
    return getComponent<RigidbodyComponent>().inertiaTensorRotation;
}

void Rigidbody::setInertiaTensorRotation(Quaternion rotation){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.automaticInertiaTensor = false;
    body.inertiaTensorRotation = rotation;
    massChanged();
}

bool Rigidbody::isAutomaticInertiaTensor() const{
    return getComponent<RigidbodyComponent>().automaticInertiaTensor;
}

void Rigidbody::setAutomaticInertiaTensor(bool automaticInertiaTensor){
    getComponent<RigidbodyComponent>().automaticInertiaTensor = automaticInertiaTensor;
    massChanged();
}

void Rigidbody::resetInertiaTensor(){
    setAutomaticInertiaTensor(true);
}

float Rigidbody::getLinearDamping() const{
    return getComponent<RigidbodyComponent>().linearDamping;
}

void Rigidbody::setLinearDamping(float linearDamping){
    getComponent<RigidbodyComponent>().linearDamping = linearDamping;
    parametersChanged();
}

float Rigidbody::getAngularDamping() const{
    return getComponent<RigidbodyComponent>().angularDamping;
}

void Rigidbody::setAngularDamping(float angularDamping){
    getComponent<RigidbodyComponent>().angularDamping = angularDamping;
    parametersChanged();
}

float Rigidbody::getMaxLinearVelocity() const{
    return getComponent<RigidbodyComponent>().maxLinearVelocity;
}

void Rigidbody::setMaxLinearVelocity(float maxLinearVelocity){
    getComponent<RigidbodyComponent>().maxLinearVelocity = maxLinearVelocity;
    parametersChanged();
}

float Rigidbody::getMaxAngularVelocity() const{
    return getComponent<RigidbodyComponent>().maxAngularVelocity;
}

void Rigidbody::setMaxAngularVelocity(float maxAngularVelocity){
    getComponent<RigidbodyComponent>().maxAngularVelocity = maxAngularVelocity;
    parametersChanged();
}

RigidbodyInterpolation Rigidbody::getInterpolation() const{
    return getComponent<RigidbodyComponent>().interpolation;
}

void Rigidbody::setInterpolation(RigidbodyInterpolation interpolation){
    getComponent<RigidbodyComponent>().interpolation = interpolation;
}

bool Rigidbody::isSleeping() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    checkBody(body);
    return !getBodyInterface().IsActive(body.body);
}

void Rigidbody::sleep(){
    deactivate();
}

void Rigidbody::wakeUp(){
    activate();
}

void Rigidbody::setMaterial(const std::string& path){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.material = path;
    body.materialInstance.reset();
    scene->getSystem<PhysicsSystem>()->refreshRigidbodyMaterials3D(entity, body, true);
}

void Rigidbody::setMaterial(const PhysicsMaterial& material){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.materialInstance = material.getRuntime();
    body.material = material.valid() ? material.getPath() : std::string();
    scene->getSystem<PhysicsSystem>()->refreshRigidbodyMaterials3D(entity, body, true);
}

void Rigidbody::clearMaterial(){
    setMaterial(std::string());
}

PhysicsMaterial Rigidbody::getMaterial() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    if (body.materialInstance) return PhysicsMaterial(body.materialInstance);
    if (!body.material.empty()) return PhysicsMaterial::load(body.material);
    return PhysicsMaterial();
}

std::string Rigidbody::getMaterialPath() const{
    return getComponent<RigidbodyComponent>().material;
}

// Kept in the component, so a body made later is made with it, and given at once to a live one
// (its mass properties are reshaped, as for setMass)
void Rigidbody::setOverrideMassAndInertia(Vector3 solidBoxSize, float solidBoxDensity){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.solidBoxSize = solidBoxSize;
    body.solidBoxDensity = solidBoxDensity;
    body.overrideMassProperties = true;
    massChanged();
}

bool Rigidbody::isOverrideMassProperties() const{
    return getComponent<RigidbodyComponent>().overrideMassProperties;
}

float Rigidbody::getGravityFactor() const{
    return getComponent<RigidbodyComponent>().gravityFactor;
}

void Rigidbody::setGravityFactor(float gravityFactor){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.gravityFactor = gravityFactor;
    // as before: no wake

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

// Kept in the component (a reloaded body keeps it), read by the scene's pair filter: bodies
// of the same group never collide
uint32_t Rigidbody::getCollisionGroupID() const{
    return getComponent<RigidbodyComponent>().collisionGroupID;
}

void Rigidbody::setCollisionGroupID(uint32_t group){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.collisionGroupID = group;
    if (!body.body.IsInvalid()){
        std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
        getJoltBodyWrite().SetCollisionGroup(JPH::CollisionGroup(&physics->getPairFilter3D(), group, (JPH::CollisionGroup::SubGroupID)entity));
        if (!physics->isSteppingWorld3D()){
            getBodyInterface().ActivateBody(body.body);
        }
    }
}

bool Rigidbody::isAllowSleeping() const{
    return getComponent<RigidbodyComponent>().allowSleeping;
}

void Rigidbody::setAllowSleeping(bool allowSleeping){
    getComponent<RigidbodyComponent>().allowSleeping = allowSleeping;
    parametersChanged();
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
        // the colliders without a material touch with it
        scene->getSystem<PhysicsSystem>()->refreshRigidbodyMaterials3D(entity, body, true);
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
        scene->getSystem<PhysicsSystem>()->refreshRigidbodyMaterials3D(entity, body, true);
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
    setPosition(position, false);
}

void Rigidbody::setPosition(Vector3 position, bool interpolate){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    // the script takes the position over from the Transform a kinematic body was following
    scene->getSystem<PhysicsSystem>()->stopFollowing3D(body, true, false);
    getBodyInterface().SetPosition(body.body, JPH::Vec3(position.x, position.y, position.z), JPH::EActivation::Activate);
    // jumps, unless asked to slide from the pose drawn from
    if (!interpolate){
        body.previousPosition = getPosition();
    }

    scene->getSystem<PhysicsSystem>()->updateTransformFromRigidbody(entity, body, (float)Engine::getInterpolationAlpha());
}

Quaternion Rigidbody::getRotation() const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();

    checkBody(body);
    JPH::Quat rotation = getBodyInterface().GetRotation(body.body);

    return Quaternion(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
}

void Rigidbody::setRotation(Quaternion rotation){
    setRotation(rotation, false);
}

void Rigidbody::setRotation(Quaternion rotation, bool interpolate){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    std::shared_ptr<PhysicsSystem> physicsSystem = scene->getSystem<PhysicsSystem>();

    checkBody(body);
    physicsSystem->stopFollowing3D(body, false, true);
    getBodyInterface().SetRotation(body.body, physicsSystem->toValidatedJoltRotation(rotation, entity, -1), JPH::EActivation::Activate);
    if (!interpolate){
        body.previousRotation = getRotation();
    }

    physicsSystem->updateTransformFromRigidbody(entity, body, (float)Engine::getInterpolationAlpha());
}

void Rigidbody::movePosition(Vector3 position){
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)){
        throw std::runtime_error("Rigidbody:movePosition: the position must be finite");
    }
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    // the script takes the position over from the Transform a kinematic body was following
    scene->getSystem<PhysicsSystem>()->stopFollowing3D(body, true, false);
    body.hasMovePosition = true;
    body.movePosition = position;
    body.moveFromFixedStep = Engine::isInFixedStep();
    body.moveStepsLeft = 0;
}

void Rigidbody::moveRotation(Quaternion rotation){
    if (!std::isfinite(rotation.w) || !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z)){
        throw std::runtime_error("Rigidbody:moveRotation: the rotation must be finite");
    }
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    scene->getSystem<PhysicsSystem>()->stopFollowing3D(body, false, true);
    body.hasMoveRotation = true;
    body.moveRotation = rotation;
    body.moveFromFixedStep = Engine::isInFixedStep();
    body.moveStepsLeft = 0;
}

void Rigidbody::movePositionAndRotation(Vector3 position, Quaternion rotation){
    movePosition(position);
    moveRotation(rotation);
}

bool Rigidbody::isAttachToParentBody() const{
    return getComponent<RigidbodyComponent>().attachToParentBody;
}

void Rigidbody::setAttachToParentBody(bool attach){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    if (body.attachToParentBody == attach){
        return;
    }
    body.attachToParentBody = attach;
    // the next step finds the ancestor (or makes this GameObject's own body again)
    body.needUpdateColliders = true;
}

Entity Rigidbody::getAttachedBody() const{
    const RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    if (!body.attachToParentBody){
        return NULL_ENTITY;
    }
    if (body.attachedTo != NULL_ENTITY && scene->isEntityCreated(body.attachedTo)){
        return body.attachedTo;
    }
    return AttachedColliders::findBody3D(scene, entity);
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
    scene->getSystem<PhysicsSystem>()->stopFollowing3D(body, true, false);
    getBodyInterface().SetLinearVelocity(body.body, JPH::Vec3(linearVelocity.x, linearVelocity.y, linearVelocity.z));
}

void Rigidbody::setLinearVelocityClamped(Vector3 linearVelocity){
    scene->getSystem<PhysicsSystem>()->stopFollowing3D(getComponent<RigidbodyComponent>(), true, false);
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
    scene->getSystem<PhysicsSystem>()->stopFollowing3D(body, false, true);
    getBodyInterface().SetAngularVelocity(body.body, JPH::Vec3(angularVelocity.x, angularVelocity.y, angularVelocity.z));
}

void Rigidbody::setAngularVelocityClamped(Vector3 angularVelocity){
    scene->getSystem<PhysicsSystem>()->stopFollowing3D(getComponent<RigidbodyComponent>(), false, true);
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

// ---- collider edits (physics-3d 6.1.7) ----

Collider3D& Rigidbody::colliderAt(size_t index) const{
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    if (index >= body.colliders.size()){
        // an exception (LuaBridge turns it into a Lua error)
        throw std::runtime_error("Rigidbody: no collider " + std::to_string(index) + " (it has " + std::to_string(body.colliders.size()) + ")");
    }
    return body.colliders[index];
}

void Rigidbody::colliderChanged(size_t index){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    if (index < body.colliders.size()){
        body.colliders[index].dirty = true;
    }
    body.needUpdateColliders = true;
}

void Rigidbody::collidersChanged(){
    getComponent<RigidbodyComponent>().needUpdateColliders = true;
}

void Rigidbody::massChanged(){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    body.massDirty = true;
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    // applied now, so velocities set right after see the new mass
    if (!body.body.IsInvalid() && !physics->isSteppingWorld3D() && !body.needReloadBody){
        physics->reshapeRigidbody3D(entity, body);
    }
}

void Rigidbody::parametersChanged(){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    if (!body.body.IsInvalid() && !physics->isSteppingWorld3D()){
        physics->applyRigidbodyParameters(entity, body);
    }else{
        body.needApplyParameters = true;
    }
}

bool Rigidbody::removeCollider(size_t index){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    return scene->getSystem<PhysicsSystem>()->removeCollider3D(entity, body, index);
}

void Rigidbody::removeAllColliders(){
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    // their pairs end at the next step with an invalid collider index, from the last
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    for (size_t i = body.colliders.size(); i-- > 0;){
        physics->contactColliderRemoved3D(entity, i);
    }
    body.colliders.clear();
    body.needUpdateColliders = true;
}

void Rigidbody::setColliderPosition(size_t index, Vector3 position){
    // the leaf stays, the body's shape places it anew
    colliderAt(index).position = position;
    collidersChanged();
}

Vector3 Rigidbody::getColliderPosition(size_t index) const{
    return colliderAt(index).position;
}

void Rigidbody::setColliderRotation(size_t index, Quaternion rotation){
    colliderAt(index).rotation = rotation;
    collidersChanged();
}

Quaternion Rigidbody::getColliderRotation(size_t index) const{
    return colliderAt(index).rotation;
}

void Rigidbody::setColliderBoxSize(size_t index, Vector3 size){
    Collider3D& collider = colliderAt(index);
    collider.width = size.x;
    collider.height = size.y;
    collider.depth = size.z;
    colliderChanged(index);
}

Vector3 Rigidbody::getColliderBoxSize(size_t index) const{
    const Collider3D& collider = colliderAt(index);
    return Vector3(collider.width, collider.height, collider.depth);
}

void Rigidbody::setColliderRadius(size_t index, float radius){
    colliderAt(index).radius = radius;
    colliderChanged(index);
}

float Rigidbody::getColliderRadius(size_t index) const{
    return colliderAt(index).radius;
}

void Rigidbody::setColliderHalfHeight(size_t index, float halfHeight){
    // 0: a capsule with no cylinder part, a sphere of its radius (S96 REQ-24)
    if (!(halfHeight >= 0.0f) || !std::isfinite(halfHeight)){
        throw std::runtime_error("Rigidbody:setColliderHalfHeight: halfHeight must be 0 or more (0: a capsule is a sphere of its radius)");
    }
    colliderAt(index).halfHeight = halfHeight;
    colliderChanged(index);
}

float Rigidbody::getColliderHalfHeight(size_t index) const{
    return colliderAt(index).halfHeight;
}

void Rigidbody::setColliderTaperedRadii(size_t index, float topRadius, float bottomRadius){
    Collider3D& collider = colliderAt(index);
    collider.topRadius = topRadius;
    collider.bottomRadius = bottomRadius;
    colliderChanged(index);
}

void Rigidbody::setColliderVertices(size_t index, std::vector<Vector3> vertices){
    Collider3D& collider = colliderAt(index);
    if (collider.type == Collider3DType::MESH){
        // a mesh keeps its triangles when only the points move
        collider.source = Collider3DSource::RAW_MESH;
    }else{
        collider.type = Collider3DType::CONVEX_HULL;
        collider.source = Collider3DSource::RAW_VERTICES;
    }
    collider.vertices = std::move(vertices);
    colliderChanged(index);
}

void Rigidbody::setColliderVertices(size_t index, std::vector<Vector3> vertices, std::vector<uint32_t> indices){
    Collider3D& collider = colliderAt(index);
    collider.type = Collider3DType::MESH;
    collider.source = Collider3DSource::RAW_MESH;
    collider.vertices = std::move(vertices);
    collider.indices = std::move(indices);
    colliderChanged(index);
}

std::vector<Vector3> Rigidbody::getColliderVertices(size_t index) const{
    return colliderAt(index).vertices;
}

std::vector<uint32_t> Rigidbody::getColliderIndices(size_t index) const{
    return colliderAt(index).indices;
}

void Rigidbody::setColliderTrigger(size_t index, bool trigger){
    Collider3D& collider = colliderAt(index);
    if (collider.isTrigger == trigger){
        return;
    }
    collider.isTrigger = trigger;
    // the surface says so at once; the mass, manifold reduction and kinematic contacts at the
    // next step (the shape is swapped, no leaf made again)
    if (collider.surface){
        collider.surface->isTrigger = trigger;
    }
    collidersChanged();
}

bool Rigidbody::isColliderTrigger(size_t index) const{
    return colliderAt(index).isTrigger;
}

void Rigidbody::setColliderMaterial(size_t index, const std::string& path){
    Collider3D& collider = colliderAt(index);
    collider.material = path;
    collider.materialInstance.reset();
    collider.ownsMaterialInstance = false;
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    scene->getSystem<PhysicsSystem>()->refreshRigidbodyMaterials3D(entity, body, true);
}

void Rigidbody::setColliderMaterial(size_t index, const PhysicsMaterial& material){
    Collider3D& collider = colliderAt(index);
    collider.materialInstance = material.getRuntime();
    collider.ownsMaterialInstance = false;
    collider.material = material.valid() ? material.getPath() : std::string();
    RigidbodyComponent& body = getComponent<RigidbodyComponent>();
    scene->getSystem<PhysicsSystem>()->refreshRigidbodyMaterials3D(entity, body, true);
}

void Rigidbody::clearColliderMaterial(size_t index){
    setColliderMaterial(index, std::string());
}

PhysicsMaterial Rigidbody::getColliderMaterial(size_t index) const{
    const Collider3D& collider = colliderAt(index);
    if (collider.materialInstance) return PhysicsMaterial(collider.materialInstance);
    if (!collider.material.empty()) return PhysicsMaterial::load(collider.material);
    return getMaterial();
}

std::string Rigidbody::getColliderMaterialPath(size_t index) const{
    return colliderAt(index).material;
}

namespace {
    // the collider's own material copy (Unity collider.material): of its asset, or of the body's
    // surface with the legacy combine rule when it has none
    std::shared_ptr<PhysicsMaterialRuntime> ownMaterial(Rigidbody& rigidbody, Collider3D& collider, size_t index){
        if (collider.materialInstance && collider.ownsMaterialInstance){
            return collider.materialInstance;
        }
        PhysicsMaterial material = rigidbody.getColliderMaterial(index);
        PhysicsMaterialData data;
        if (material.valid()){
            data = material.getRuntime()->toData();
        }else{
            const RigidbodyComponent& body = rigidbody.getComponent<RigidbodyComponent>();
            data.friction = body.friction;
            data.bounciness = body.restitution;
            data.frictionCombine = PhysicsMaterialCombine::GEOMETRIC_MEAN;
            data.bounceCombine = PhysicsMaterialCombine::MAXIMUM;
        }
        collider.materialInstance = std::make_shared<PhysicsMaterialRuntime>(data, std::string());
        collider.ownsMaterialInstance = true;
        return collider.materialInstance;
    }
}

void Rigidbody::setColliderFriction(size_t index, float friction){
    Collider3D& collider = colliderAt(index);
    ownMaterial(*this, collider, index)->setFriction(friction);
    scene->getSystem<PhysicsSystem>()->refreshRigidbodyMaterials3D(entity, getComponent<RigidbodyComponent>(), true);
}

void Rigidbody::setColliderBounciness(size_t index, float bounciness){
    Collider3D& collider = colliderAt(index);
    ownMaterial(*this, collider, index)->setBounciness(bounciness);
    scene->getSystem<PhysicsSystem>()->refreshRigidbodyMaterials3D(entity, getComponent<RigidbodyComponent>(), true);
}

float Rigidbody::getColliderFriction(size_t index) const{
    PhysicsMaterial material = getColliderMaterial(index);
    return material.valid() ? material.getFriction() : getComponent<RigidbodyComponent>().friction;
}

float Rigidbody::getColliderBounciness(size_t index) const{
    PhysicsMaterial material = getColliderMaterial(index);
    return material.valid() ? material.getBounciness() : getComponent<RigidbodyComponent>().restitution;
}
