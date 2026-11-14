// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Rigidbody2D.h"

#include "object/GameObject.h"
#include "component/Rigidbody2DComponent.h"
#include "subsystem/PhysicsSystem.h"
#include "box2d/box2d.h"
#include "object/physics/Contact2D.h"
#include "util/Angle.h"
#include "util/Box2DAux.h"
#include "component/Transform.h"
#include "physics/AttachedColliders.h"
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace jasperengine;

b2BodyType getBodyTypeToB2(BodyType type){
    if (type == BodyType::STATIC){
        return b2_staticBody;
    }else if (type == BodyType::KINEMATIC){
        return b2_kinematicBody;
    }else if (type == BodyType::DYNAMIC){
        return b2_dynamicBody;
    }

    return b2_dynamicBody;
}

BodyType getB2ToBodyType(b2BodyType type){
    if (type == b2_staticBody){
        return BodyType::STATIC;
    }else if (type == b2_kinematicBody){
        return BodyType::KINEMATIC;
    }else if (type == b2_dynamicBody){
        return BodyType::DYNAMIC;
    }

    return BodyType::DYNAMIC;
}

static int appendCollider2D(Rigidbody2DComponent& body){
    body.colliders.push_back(Collider2D());
    body.needUpdateColliders = true;

    return static_cast<int>(body.colliders.size() - 1);
}

// every Box2D shape of a collider: its convex pieces, or the segments of its chain
static std::vector<b2ShapeId> colliderShapeIds(const Collider2D& collider){
    std::vector<b2ShapeId> shapes;
    for (const b2ShapeId& shape : collider.shapes){
        if (b2Shape_IsValid(shape)) shapes.push_back(shape);
    }
    if (b2Chain_IsValid(collider.chain)){
        const int count = b2Chain_GetSegmentCount(collider.chain);
        if (count > 0){
            const size_t start = shapes.size();
            shapes.resize(start + (size_t)count);
            const int returned = b2Chain_GetSegments(collider.chain, shapes.data() + start, count);
            shapes.resize(start + (size_t)std::max(0, returned));
        }
    }
    return shapes;
}

Rigidbody2D::Rigidbody2D(Scene* scene, Entity entity): EntityHandle(scene, entity){

}

Rigidbody2D::~Rigidbody2D() = default;
Rigidbody2D::Rigidbody2D(const Rigidbody2D& rhs) = default;
Rigidbody2D& Rigidbody2D::operator=(const Rigidbody2D& rhs) = default;
Rigidbody2D::Rigidbody2D(Rigidbody2D&& rhs) noexcept = default;
Rigidbody2D& Rigidbody2D::operator=(Rigidbody2D&& rhs) noexcept = default;

void Rigidbody2D::checkBody(const Rigidbody2DComponent& body) const{
    // Scripts start (Lua init(), C++ constructors) before their scene enters the Engine,
    // which is what loads the physics bodies: make this one now rather than fail
    if (!b2Body_IsValid(body.body)){
        scene->getSystem<PhysicsSystem>()->ensureRigidbody2DLoaded(entity);
    }
    if (!b2Body_IsValid(body.body)){
        Log::error("Rigidbody2D is not loaded");
        throw std::runtime_error("Rigidbody2D is not loaded");
    }
}

Collider2D& Rigidbody2D::colliderAt(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (index >= body.colliders.size()){
        Log::error("Cannot find collider %zu of body (it has %zu)", index, body.colliders.size());
        throw std::runtime_error("Collider index " + std::to_string(index) + " out of range (the body has " + std::to_string(body.colliders.size()) + ")");
    }
    return body.colliders[index];
}

void Rigidbody2D::colliderChanged(size_t index){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (index < body.colliders.size()){
        body.colliders[index].dirty = true;
        body.needUpdateColliders = true;
    }
}

void Rigidbody2D::massChanged(){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (b2Body_IsValid(body.body)){
        scene->getSystem<PhysicsSystem>()->applyRigidbody2DMass(entity, body);
    }
}

b2BodyId Rigidbody2D::getBox2DBody() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return body.body;
}

b2ShapeId Rigidbody2D::getBox2DShape(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        return body.colliders[index].shapes.empty() ? b2_nullShapeId : body.colliders[index].shapes[0];
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return b2_nullShapeId;
}

std::vector<b2ShapeId> Rigidbody2D::getBox2DShapes(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (const Collider2D* collider = findCollider2D(body, (int)index)){
        return colliderShapeIds(*collider);
    }
    Log::error("Cannot find shape %zu of body", index);
    return {};
}

b2ChainId Rigidbody2D::getBox2DChain(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (const Collider2D* collider = findCollider2D(body, (int)index)){
        return collider->chain;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return b2_nullChainId;
}

GameObject Rigidbody2D::getAttachedObject(){
    return GameObject(scene, entity);
}

float Rigidbody2D::getPointsToMeterScale() const{
    return scene->getSystem<PhysicsSystem>()->getPointsToMeterScale2D();
}

void Rigidbody2D::load(){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    body.needReloadBody = true;
    body.needUpdateColliders = true;
    markChanged<Rigidbody2DComponent>();

    scene->getSystem<PhysicsSystem>()->loadRigidbody2D(entity);
}

int Rigidbody2D::createBoxCollider(float width, float height){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::POLYGON;
    shape.points = {Vector2(0, 0), Vector2(width, 0), Vector2(width, height), Vector2(0, height)};

    return index;
}

int Rigidbody2D::createCenteredBoxCollider(float width, float height){
    return createCenteredBoxCollider(width, height, Vector2(0, 0), 0);
}

int Rigidbody2D::createCenteredBoxCollider(float width, float height, Vector2 center, float angle){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::POLYGON;
    shape.points.resize(4);

    float halfW = width / 2.0f;
    float halfH = height / 2.0f;

    Vector2 local[4] = {
        Vector2(-halfW, -halfH),
        Vector2(halfW, -halfH),
        Vector2(halfW, halfH),
        Vector2(-halfW, halfH)
    };

    float rad = Angle::defaultToRad(angle);
    float c = cosf(rad);
    float s = sinf(rad);

    for (int i = 0; i < 4; i++){
        float x = local[i].x;
        float y = local[i].y;
        shape.points[i] = Vector2((x * c) - (y * s), (x * s) + (y * c)) + center;
    }

    return index;
}

int Rigidbody2D::createRoundedBoxCollider(float width, float height, float radius){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::POLYGON;
    shape.radius = radius;

    float halfW = width / 2.0f;
    float halfH = height / 2.0f;
    shape.points = {Vector2(-halfW, -halfH), Vector2(halfW, -halfH), Vector2(halfW, halfH), Vector2(-halfW, halfH)};

    return index;
}

int Rigidbody2D::createPolygonCollider(std::vector<Vector2> vertices){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::POLYGON;
    shape.points = std::move(vertices);
    if (shape.points.size() < 3){
        Log::warn("Polygon collider %d has %zu points: it needs at least 3", index, shape.points.size());
    }

    return index;
}

int Rigidbody2D::createCircleCollider(Vector2 center, float radius){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::CIRCLE;
    shape.pointA = center;
    shape.radius = radius;

    return index;
}

int Rigidbody2D::createCapsuleCollider(Vector2 center1, Vector2 center2, float radius){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::CAPSULE;
    shape.pointA = center1;
    shape.pointB = center2;
    shape.radius = radius;

    return index;
}

int Rigidbody2D::createSegmentCollider(Vector2 point1, Vector2 point2){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::SEGMENT;
    shape.pointA = point1;
    shape.pointB = point2;

    return index;
}

int Rigidbody2D::createChainCollider(std::vector<Vector2> vertices, bool loop){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::CHAIN;
    shape.loop = loop;
    shape.points = std::move(vertices);
    if (shape.points.size() < 2){
        Log::warn("Chain collider %d has %zu points: it needs at least 2", index, shape.points.size());
    }

    return index;
}

void Rigidbody2D::removeAllColliders(){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    // their pairs end at the next step with an invalid collider index (physics-contacts 6.1.6);
    // from the last, so no index shifts onto a removed one
    for (size_t i = body.colliders.size(); i-- > 0;){
        physics->contactColliderRemoved2D(entity, i);
    }
    if (b2Body_IsValid(body.body) && physics->isSteppingWorld2D()){
        // Box2D is locked: the body is made again after the step
        body.colliders.clear();
        body.needReloadBody = true;
        body.needUpdateColliders = true;
        markChanged<Rigidbody2DComponent>();
        return;
    }
    for (size_t i = 0; i < body.colliders.size(); i++){
        physics->destroyCollider2D(body, i);
    }
    body.colliders.clear();
    body.needUpdateColliders = true;
    markChanged<Rigidbody2DComponent>();
    physics->applyRigidbody2DMass(entity, body);
}

bool Rigidbody2D::removeCollider(size_t index){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (index >= body.colliders.size()){
        Log::error("Cannot remove collider %zu of body (it has %zu)", index, body.colliders.size());
        throw std::runtime_error("Collider index " + std::to_string(index) + " out of range (the body has " + std::to_string(body.colliders.size()) + ")");
    }
    return scene->getSystem<PhysicsSystem>()->removeCollider2D(entity, body, index);
}

void Rigidbody2D::setColliderPoints(size_t index, std::vector<Vector2> points){
    Collider2D& collider = colliderAt(index);
    if (collider.type != Collider2DType::POLYGON && collider.type != Collider2DType::CHAIN){
        Log::error("Collider %zu is not a polygon or a chain: setColliderPoints changes nothing", index);
        return;
    }
    collider.points = std::move(points);
    // a polygon that stays one convex piece keeps its Box2D shape (b2Shape_SetPolygon)
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (collider.type == Collider2DType::POLYGON &&
            scene->getSystem<PhysicsSystem>()->reshapeCollider2DPolygon(entity, body, index)){
        return;
    }
    colliderChanged(index);
}

std::vector<Vector2> Rigidbody2D::getColliderPoints(size_t index) const{
    return colliderAt(index).points;
}

void Rigidbody2D::setColliderLoop(size_t index, bool loop){
    Collider2D& collider = colliderAt(index);
    if (collider.loop != loop){
        collider.loop = loop;
        colliderChanged(index);
    }
}

bool Rigidbody2D::isColliderLoop(size_t index) const{
    return colliderAt(index).loop;
}

void Rigidbody2D::setColliderRadius(size_t index, float radius){
    Collider2D& collider = colliderAt(index);
    if (collider.radius != radius){
        collider.radius = radius;
        colliderChanged(index);
    }
}

float Rigidbody2D::getColliderRadius(size_t index) const{
    return colliderAt(index).radius;
}

void Rigidbody2D::setColliderCenter(size_t index, Vector2 center){
    Collider2D& collider = colliderAt(index);
    if (collider.type != Collider2DType::CIRCLE){
        Log::error("Collider %zu is not a circle: setColliderCenter changes nothing", index);
        return;
    }
    if (collider.pointA != center){
        collider.pointA = center;
        colliderChanged(index);
    }
}

Vector2 Rigidbody2D::getColliderCenter(size_t index) const{
    return colliderAt(index).pointA;
}

void Rigidbody2D::setColliderSegment(size_t index, Vector2 pointA, Vector2 pointB){
    Collider2D& collider = colliderAt(index);
    if (collider.type != Collider2DType::SEGMENT && collider.type != Collider2DType::CAPSULE){
        Log::error("Collider %zu is not a segment or a capsule: setColliderSegment changes nothing", index);
        return;
    }
    collider.pointA = pointA;
    collider.pointB = pointB;
    colliderChanged(index);
}

void Rigidbody2D::setColliderMaterial(size_t index, const std::string& path){
    Collider2D& collider = colliderAt(index);
    collider.material = path;
    collider.materialInstance.reset();
    collider.ownsMaterialInstance = false;
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    scene->getSystem<PhysicsSystem>()->refreshRigidbody2DMaterials(entity, body, true);
}

void Rigidbody2D::setColliderMaterial(size_t index, const PhysicsMaterial& material){
    Collider2D& collider = colliderAt(index);
    collider.materialInstance = material.getRuntime();
    collider.ownsMaterialInstance = false;
    collider.material = material.valid() ? material.getPath() : std::string();
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    scene->getSystem<PhysicsSystem>()->refreshRigidbody2DMaterials(entity, body, true);
}

void Rigidbody2D::clearColliderMaterial(size_t index){
    setColliderMaterial(index, std::string());
}

PhysicsMaterial Rigidbody2D::getColliderMaterial(size_t index) const{
    const Collider2D& collider = colliderAt(index);
    if (collider.materialInstance) return PhysicsMaterial(collider.materialInstance);
    if (!collider.material.empty()) return PhysicsMaterial::load(collider.material);
    return getMaterial();
}

std::string Rigidbody2D::getColliderMaterialPath(size_t index) const{
    return colliderAt(index).material;
}

void Rigidbody2D::setColliderTrigger(size_t index, bool trigger){
    setColliderSensor(index, trigger);
}

bool Rigidbody2D::isColliderTrigger(size_t index) const{
    return colliderAt(index).sensor;
}

size_t Rigidbody2D::getColliderPieceCount(size_t index) const{
    const Collider2D& collider = colliderAt(index);
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();
    // an edit made since the step is counted as it will be built
    if (b2Body_IsValid(body.body) && (collider.dirty || body.needUpdateColliders) && !physics->isSteppingWorld2D()){
        physics->loadRigidbody2D(entity);
    }else if (!b2Body_IsValid(body.body)){
        checkBody(body);
    }
    return colliderShapeIds(body.colliders[index]).size();
}

std::vector<Contact2D> Rigidbody2D::getBodyContacts(){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);

    std::vector<Contact2D> contacts;

    int bodyContactCapacity = b2Body_GetContactCapacity(body.body);
    if (bodyContactCapacity == 0) return contacts;
    std::vector<b2ContactData> contactData(bodyContactCapacity);
    int bodyContactCount = b2Body_GetContactData(body.body, &contactData.front(), bodyContactCapacity);

    for (int i = 0; i < bodyContactCount; ++i){
        contacts.push_back(Contact2D(scene, contactData[i]));
    }

    return contacts;
}

std::vector<Contact2D> Rigidbody2D::getColliderContacts(size_t index){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    std::vector<Contact2D> contacts;

    if (index < body.colliders.size()){
        // every piece of the collider, the segments of a chain too
        for (const b2ShapeId& shape : colliderShapeIds(body.colliders[index])){
            int shapeContactCapacity = b2Shape_GetContactCapacity(shape);
            if (shapeContactCapacity == 0) continue;
            std::vector<b2ContactData> contactData(shapeContactCapacity);
            int shapeContactCount = b2Shape_GetContactData(shape, &contactData.front(), shapeContactCapacity);

            for (int i = 0; i < shapeContactCount; ++i){
                contacts.push_back(Contact2D(scene, contactData[i]));
            }
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return contacts;
}

size_t Rigidbody2D::getNumColliders() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    return body.colliders.size();
}

Collider2DType Rigidbody2D::getColliderType(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        return body.colliders[index].type;
    }else{
        Log::error("Cannot find shape %zu of body", index);
        throw std::runtime_error("Cannot find shape");
    }
}

void Rigidbody2D::setColliderDensity(float density){
    setColliderDensity(0, density);
}

void Rigidbody2D::setColliderFriction(float friction){
    setColliderFriction(0, friction);
}

void Rigidbody2D::setColliderRestitution(float restitution){
    setColliderRestitution(0, restitution);
}

void Rigidbody2D::setColliderDensity(size_t index, float density){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        body.colliders[index].density = density;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set density of chain shape %zu", index);
        }else if (!body.colliders[index].shapes.empty()){
            for (const b2ShapeId& shape : body.colliders[index].shapes){
                if (b2Shape_IsValid(shape)) b2Shape_SetDensity(shape, density, false);
            }
            massChanged();
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderFriction(size_t index, float friction){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        Collider2D& collider = body.colliders[index];
        PhysicsMaterial material = getColliderMaterial(index);
        if (material.valid()){
            // a collider with a material gets its own copy of it (Unity collider.material)
            if (!collider.materialInstance || !collider.ownsMaterialInstance){
                collider.materialInstance = std::make_shared<PhysicsMaterialRuntime>(material.getRuntime()->toData(), std::string());
                collider.ownsMaterialInstance = true;
            }
            collider.materialInstance->setFriction(friction);
            scene->getSystem<PhysicsSystem>()->refreshRigidbody2DMaterials(entity, body, true);
            return;
        }
        collider.friction = friction;
        collider.appliedFriction = friction;
        for (const b2ShapeId& shape : collider.shapes){
            if (b2Shape_IsValid(shape)) b2Shape_SetFriction(shape, friction);
        }
        if (b2Chain_IsValid(collider.chain)){
            b2Chain_SetFriction(collider.chain, friction);
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderRestitution(size_t index, float restitution){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        Collider2D& collider = body.colliders[index];
        PhysicsMaterial material = getColliderMaterial(index);
        if (material.valid()){
            if (!collider.materialInstance || !collider.ownsMaterialInstance){
                collider.materialInstance = std::make_shared<PhysicsMaterialRuntime>(material.getRuntime()->toData(), std::string());
                collider.ownsMaterialInstance = true;
            }
            collider.materialInstance->setBounciness(restitution);
            scene->getSystem<PhysicsSystem>()->refreshRigidbody2DMaterials(entity, body, true);
            return;
        }
        collider.restitution = restitution;
        collider.appliedRestitution = restitution;
        for (const b2ShapeId& shape : collider.shapes){
            if (b2Shape_IsValid(shape)) b2Shape_SetRestitution(shape, restitution);
        }
        if (b2Chain_IsValid(collider.chain)){
            b2Chain_SetRestitution(collider.chain, restitution);
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderSensor(bool sensor){
    setColliderSensor(0, sensor);
}

void Rigidbody2D::setColliderSensor(size_t index, bool sensor){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set sensor of chain shape %zu", index);
        }else if (body.colliders[index].sensor != sensor){
            body.colliders[index].sensor = sensor;
            colliderChanged(index);
            markChanged<Rigidbody2DComponent>();
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

bool Rigidbody2D::isColliderSensor() const{
    return isColliderSensor(0);
}

bool Rigidbody2D::isColliderSensor(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        return body.colliders[index].sensor;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return false;
}

void Rigidbody2D::setColliderEnableHitEvents(bool hitEvents){
    setColliderEnableHitEvents(0, hitEvents);
}

void Rigidbody2D::setColliderContactEvents(bool contactEvents){
    setColliderContactEvents(0, contactEvents);
}

void Rigidbody2D::setColliderPreSolveEvents(bool preSolveEvent){
    setColliderPreSolveEvents(0, preSolveEvent);
}

void Rigidbody2D::setColliderSensorEvents(bool sensorEvents){
    setColliderSensorEvents(0, sensorEvents);
}


void Rigidbody2D::setColliderEnableHitEvents(size_t index, bool hitEvents){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        body.colliders[index].enableHitEvents = hitEvents;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set hit events of chain shape %zu", index);
        }else{
            for (const b2ShapeId& shape : body.colliders[index].shapes){
                if (b2Shape_IsValid(shape)) b2Shape_EnableHitEvents(shape, hitEvents);
            }
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderContactEvents(size_t index, bool contactEvents){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        body.colliders[index].contactEvents = contactEvents;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set shape contact events of chain shape %zu", index);
        }else{
            for (const b2ShapeId& shape : body.colliders[index].shapes){
                if (b2Shape_IsValid(shape)) b2Shape_EnableContactEvents(shape, contactEvents);
            }
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderPreSolveEvents(size_t index, bool preSolveEvent){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        body.colliders[index].preSolveEvents = preSolveEvent;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set presolve events of chain shape %zu", index);
        }else{
            for (const b2ShapeId& shape : body.colliders[index].shapes){
                if (b2Shape_IsValid(shape)) b2Shape_EnablePreSolveEvents(shape, preSolveEvent);
            }
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderSensorEvents(size_t index, bool sensorEvents){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        body.colliders[index].sensorEvents = sensorEvents;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set sensor events of chain shape %zu", index);
        }else{
            for (const b2ShapeId& shape : body.colliders[index].shapes){
                if (b2Shape_IsValid(shape)) b2Shape_EnableSensorEvents(shape, sensorEvents || body.colliders[index].sensor);
            }
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}


float Rigidbody2D::getColliderDensity() const{
    return getColliderDensity(0);
}

float Rigidbody2D::getColliderFriction() const{
    return getColliderFriction(0);
}

float Rigidbody2D::getColliderRestitution() const{
    return getColliderRestitution(0);
}


float Rigidbody2D::getColliderDensity(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        return body.colliders[index].density;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return -1;
}

float Rigidbody2D::getColliderFriction(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        // what the collider slides with: its material's, else its own
        PhysicsMaterial material = getColliderMaterial(index);
        return material.valid() ? material.getFriction() : body.colliders[index].friction;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return -1;
}

float Rigidbody2D::getColliderRestitution(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        PhysicsMaterial material = getColliderMaterial(index);
        return material.valid() ? material.getBounciness() : body.colliders[index].restitution;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return -1;
}

bool Rigidbody2D::isColliderEnableHitEvents() const{
    return isColliderEnableHitEvents(0);
}

bool Rigidbody2D::isColliderContactEvents() const{
    return isColliderContactEvents(0);
}

bool Rigidbody2D::isColliderPreSolveEvents() const{
    return isColliderPreSolveEvents(0);
}

bool Rigidbody2D::isColliderSensorEvents() const{
    return isColliderSensorEvents(0);
}

bool Rigidbody2D::isColliderEnableHitEvents(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        return body.colliders[index].enableHitEvents;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return false;
}

bool Rigidbody2D::isColliderContactEvents(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        return body.colliders[index].contactEvents;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return false;
}

bool Rigidbody2D::isColliderPreSolveEvents(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        return body.colliders[index].preSolveEvents;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return false;
}

bool Rigidbody2D::isColliderSensorEvents(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.colliders.size()){
        return body.colliders[index].sensorEvents || body.colliders[index].sensor;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return false;
}

Vector2 Rigidbody2D::getPosition() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    float pointsToMeterScale = getPointsToMeterScale();

    checkBody(body);
    b2Vec2 position = b2Body_GetPosition(body.body);

    return Vector2(position.x * pointsToMeterScale, position.y * pointsToMeterScale);
}

void Rigidbody2D::setPosition(Vector2 position){
    setPosition(position, false);
}

void Rigidbody2D::setPosition(Vector2 position, bool interpolate){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    float pointsToMeterScale = getPointsToMeterScale();

    checkBody(body);
    // the script takes the position over from the Transform a kinematic body was following
    scene->getSystem<PhysicsSystem>()->stopFollowing2D(body, true, false);
    b2Transform bTransform = b2Body_GetTransform(body.body);
    b2Body_SetTransform(body.body, {position.x / pointsToMeterScale, position.y / pointsToMeterScale}, bTransform.q);
    b2Body_SetAwake(body.body, true);
    // jumps, unless asked to slide from the pose drawn from
    if (!interpolate){
        body.previousTransform.p = b2Body_GetPosition(body.body);
    }

    scene->getSystem<PhysicsSystem>()->updateTransformFromRigidbody2D(entity, body, (float)Engine::getInterpolationAlpha());
}

float Rigidbody2D::getAngle() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return Angle::radToDefault(b2Rot_GetAngle(b2Body_GetRotation(body.body)));
}

void Rigidbody2D::setAngle(float angle){
    setAngle(angle, false);
}

void Rigidbody2D::setAngle(float angle, bool interpolate){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    scene->getSystem<PhysicsSystem>()->stopFollowing2D(body, false, true);
    b2Transform bTransform = b2Body_GetTransform(body.body);
    b2Body_SetTransform(body.body, bTransform.p, b2MakeRot(Angle::defaultToRad(angle)));
    b2Body_SetAwake(body.body, true);
    if (!interpolate){
        body.previousTransform.q = b2Body_GetRotation(body.body);
    }

    scene->getSystem<PhysicsSystem>()->updateTransformFromRigidbody2D(entity, body, (float)Engine::getInterpolationAlpha());
}

void Rigidbody2D::movePosition(Vector2 position){
    if (!std::isfinite(position.x) || !std::isfinite(position.y)){
        throw std::runtime_error("Rigidbody2D:movePosition: the position must be finite");
    }
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    scene->getSystem<PhysicsSystem>()->stopFollowing2D(body, true, false);
    body.hasMovePosition = true;
    body.movePosition = position;
    body.moveFromFixedStep = Engine::isInFixedStep();
    body.moveStepsLeft = 0;
}

void Rigidbody2D::moveRotation(float angle){
    if (!std::isfinite(angle)){
        throw std::runtime_error("Rigidbody2D:moveRotation: the angle must be finite");
    }
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    scene->getSystem<PhysicsSystem>()->stopFollowing2D(body, false, true);
    body.hasMoveRotation = true;
    body.moveAngle = angle;
    body.moveFromFixedStep = Engine::isInFixedStep();
    body.moveStepsLeft = 0;
}

void Rigidbody2D::movePositionAndRotation(Vector2 position, float angle){
    movePosition(position);
    moveRotation(angle);
}

bool Rigidbody2D::isAttachToParentBody() const{
    return getComponent<Rigidbody2DComponent>().attachToParentBody;
}

void Rigidbody2D::setAttachToParentBody(bool attach){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (body.attachToParentBody == attach){
        return;
    }
    body.attachToParentBody = attach;
    body.needUpdateColliders = true;
}

Entity Rigidbody2D::getAttachedBody() const{
    const Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (!body.attachToParentBody){
        return NULL_ENTITY;
    }
    if (body.attachedTo != NULL_ENTITY && scene->isEntityCreated(body.attachedTo)){
        return body.attachedTo;
    }
    return AttachedColliders::findBody2D(scene, entity);
}

void Rigidbody2D::setLinearVelocity(Vector2 linearVelocity){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    float pointsToMeterScale = getPointsToMeterScale();

    checkBody(body);
    scene->getSystem<PhysicsSystem>()->stopFollowing2D(body, true, false);
    // points per second, like positions
    b2Body_SetLinearVelocity(body.body, {linearVelocity.x / pointsToMeterScale, linearVelocity.y / pointsToMeterScale});
}

void Rigidbody2D::setAngularVelocity(float angularVelocity){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    scene->getSystem<PhysicsSystem>()->stopFollowing2D(body, false, true);
    b2Body_SetAngularVelocity(body.body, angularVelocity);
}

void Rigidbody2D::setLinearDamping(float linearDamping){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    linearDamping = std::isfinite(linearDamping) ? std::max(0.0f, linearDamping) : 0.0f;
    body.linearDamping = linearDamping;
    checkBody(body);
    b2Body_SetLinearDamping(body.body, linearDamping);
}

void Rigidbody2D::setAngularDamping(float angularDamping){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    angularDamping = std::isfinite(angularDamping) ? std::max(0.0f, angularDamping) : 0.0f;
    body.angularDamping = angularDamping;
    checkBody(body);
    b2Body_SetAngularDamping(body.body, angularDamping);
}

void Rigidbody2D::setEnableSleep(bool enableSleep){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (!enableSleep){
        body.sleepingMode = RigidbodySleepMode2D::NEVER_SLEEP;
    }else if (body.sleepingMode == RigidbodySleepMode2D::NEVER_SLEEP){
        body.sleepingMode = RigidbodySleepMode2D::START_AWAKE;
    }
    checkBody(body);
    b2Body_EnableSleep(body.body, enableSleep);
}

void Rigidbody2D::setAwake(bool awake){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Body_SetAwake(body.body, awake);
}

void Rigidbody2D::setFixedRotation(bool fixedRotation){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    body.fixedRotation = fixedRotation;
    checkBody(body);
    if (b2Body_IsFixedRotation(body.body) != fixedRotation){
        // Box2D computes the mass again
        b2Body_SetFixedRotation(body.body, fixedRotation);
        massChanged();
    }
}

void Rigidbody2D::setBullet(bool bullet){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    body.collisionDetection = bullet ? CollisionDetectionMode2D::CONTINUOUS : CollisionDetectionMode2D::DISCRETE;
    checkBody(body);
    b2Body_SetBullet(body.body, bullet);
}

void Rigidbody2D::setType(BodyType type){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    // the script takes over a body that was following its Transform
    scene->getSystem<PhysicsSystem>()->stopFollowing2D(body, true, true);

    body.type = type;
    if (b2Body_IsValid(body.body) && b2Body_GetType(body.body) != getBodyTypeToB2(type)){
        // Box2D computes the mass again
        b2Body_SetType(body.body, getBodyTypeToB2(type));
        massChanged();
    }
}

void Rigidbody2D::setEnabled(bool enabled){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    // the component flag is the setting; the physics system keeps an inactive
    // GameObject's body out of the world whatever it says
    body.enabled = enabled;

    checkBody(body);
    if (enabled && scene->isEntityActiveInHierarchy(entity)){
        b2Body_Enable(body.body);
    }else{
        b2Body_Disable(body.body);
    }
}

void Rigidbody2D::setGravityScale(float gravityScale){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    gravityScale = std::isfinite(gravityScale) ? gravityScale : 1.0f;
    body.gravityScale = gravityScale;
    checkBody(body);
    b2Body_SetGravityScale(body.body, gravityScale);
}

Vector2 Rigidbody2D::getLinearVelocity() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    float pointsToMeterScale = getPointsToMeterScale();
    b2Vec2 vec = b2Body_GetLinearVelocity(body.body);

    return Vector2(vec.x * pointsToMeterScale, vec.y * pointsToMeterScale);
}

float Rigidbody2D::getAngularVelocity() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_GetAngularVelocity(body.body);
}

float Rigidbody2D::getLinearDamping() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_GetLinearDamping(body.body);
}

float Rigidbody2D::getAngularDamping() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_GetAngularDamping(body.body);
}

bool Rigidbody2D::isEnableSleep() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_IsSleepEnabled(body.body);
}

bool Rigidbody2D::isAwake() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_IsAwake(body.body);
}

bool Rigidbody2D::isFixedRotation() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_IsFixedRotation(body.body);
}

bool Rigidbody2D::isBullet() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_IsBullet(body.body);
}

BodyType Rigidbody2D::getType() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (b2Body_IsValid(body.body)){
        return getB2ToBodyType(b2Body_GetType(body.body));
    }
    return body.type;
}

bool Rigidbody2D::isEnabled() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    return body.enabled;
}

float Rigidbody2D::getGravityScale() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_GetGravityScale(body.body);
}

void Rigidbody2D::setBitsFilter(uint16_t categoryBits, uint16_t maskBits){
    setBitsFilter(0, categoryBits, maskBits);
}

void Rigidbody2D::setBitsFilter(size_t colliderIndex, uint16_t categoryBits, uint16_t maskBits){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (colliderIndex < body.colliders.size()){
        body.colliders[colliderIndex].categoryBits = categoryBits;
        body.colliders[colliderIndex].maskBits = maskBits;

        for (const b2ShapeId& shape : colliderShapeIds(body.colliders[colliderIndex])){
            b2Filter filter = b2Shape_GetFilter(shape);
            filter.categoryBits = Box2DLayers::withUserCategory(filter.categoryBits, categoryBits);
            filter.maskBits = maskBits;

            b2Shape_SetFilter(shape, filter);
        }
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }
}

void Rigidbody2D::setCategoryBitsFilter(uint16_t categoryBits){
    setCategoryBitsFilter(0, categoryBits);
}

void Rigidbody2D::setMaskBitsFilter(uint16_t maskBits){
    setMaskBitsFilter(0, maskBits);
}

void Rigidbody2D::setGroupIndexFilter(int16_t groupIndex){
    setGroupIndexFilter(0, groupIndex);
}

void Rigidbody2D::setCategoryBitsFilter(size_t colliderIndex, uint16_t categoryBits){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (colliderIndex < body.colliders.size()){
        body.colliders[colliderIndex].categoryBits = categoryBits;
        for (const b2ShapeId& shape : colliderShapeIds(body.colliders[colliderIndex])){
            b2Filter filter = b2Shape_GetFilter(shape);

            filter.categoryBits = Box2DLayers::withUserCategory(filter.categoryBits, categoryBits);

            b2Shape_SetFilter(shape, filter);
        }
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }
}

void Rigidbody2D::setMaskBitsFilter(size_t colliderIndex, uint16_t maskBits){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    
    if (colliderIndex < body.colliders.size()){
        body.colliders[colliderIndex].maskBits = maskBits;
        for (const b2ShapeId& shape : colliderShapeIds(body.colliders[colliderIndex])){
            b2Filter filter = b2Shape_GetFilter(shape);

            filter.maskBits = maskBits;

            b2Shape_SetFilter(shape, filter);
        }
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }
}

void Rigidbody2D::setGroupIndexFilter(size_t colliderIndex, int16_t groupIndex){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    
    if (colliderIndex < body.colliders.size()){
        body.colliders[colliderIndex].groupIndex = groupIndex;
        for (const b2ShapeId& shape : colliderShapeIds(body.colliders[colliderIndex])){
            b2Filter filter = b2Shape_GetFilter(shape);

            filter.groupIndex = groupIndex;

            b2Shape_SetFilter(shape, filter);
        }
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }
}

uint16_t Rigidbody2D::getCategoryBitsFilter() const{
    return getCategoryBitsFilter(0);
}

uint16_t Rigidbody2D::getMaskBitsFilter() const{
    return getMaskBitsFilter(0);
}

int16_t Rigidbody2D::getGroupIndexFilter() const{
    return getGroupIndexFilter(0);
}

uint16_t Rigidbody2D::getCategoryBitsFilter(size_t colliderIndex) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (colliderIndex < body.colliders.size()){
        return body.colliders[colliderIndex].categoryBits;
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }

    return 0;
}

uint16_t Rigidbody2D::getMaskBitsFilter(size_t colliderIndex) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    
    if (colliderIndex < body.colliders.size()){
        return body.colliders[colliderIndex].maskBits;
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }

    return 0;
}

int16_t Rigidbody2D::getGroupIndexFilter(size_t colliderIndex) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    
    if (colliderIndex < body.colliders.size()){
        return body.colliders[colliderIndex].groupIndex;
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }

    return -1;
}

float Rigidbody2D::getMass() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_GetMass(body.body);
}

float Rigidbody2D::getRotationalInertia() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return b2Body_GetRotationalInertia(body.body);
}

void Rigidbody2D::applyMassFromColliders(){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    scene->getSystem<PhysicsSystem>()->applyRigidbody2DMass(entity, body);
}

void Rigidbody2D::setMass(float mass){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    body.mass = (std::isfinite(mass) && mass > 0.0f) ? mass : body.mass;
    body.useAutoMass = false;
    massChanged();
}

bool Rigidbody2D::isUseAutoMass() const{
    return getComponent<Rigidbody2DComponent>().useAutoMass;
}

void Rigidbody2D::setUseAutoMass(bool useAutoMass){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (body.useAutoMass != useAutoMass){
        // keeps the mass the body has (Unity)
        if (!useAutoMass && b2Body_IsValid(body.body) && b2Body_GetMass(body.body) > 0.0f){
            body.mass = b2Body_GetMass(body.body);
        }
        body.useAutoMass = useAutoMass;
        massChanged();
    }
}

Vector2 Rigidbody2D::getCenterOfMass() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (!body.autoCenterOfMass || !b2Body_IsValid(body.body)){
        return body.centerOfMass;
    }
    // body-local, in collider units like the field
    const b2Vec2 center = b2Body_GetLocalCenterOfMass(body.body);
    const float ptm = getPointsToMeterScale();
    Vector3 worldScale = Vector3(1.0f, 1.0f, 1.0f);
    if (Transform* transform = scene->findComponent<Transform>(entity)){
        worldScale = transform->worldScale;
    }
    return Vector2(worldScale.x != 0.0f ? center.x * ptm / worldScale.x : 0.0f,
                   worldScale.y != 0.0f ? center.y * ptm / worldScale.y : 0.0f);
}

void Rigidbody2D::setCenterOfMass(Vector2 center){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    body.centerOfMass = center;
    body.autoCenterOfMass = false;
    massChanged();
}

bool Rigidbody2D::isAutoCenterOfMass() const{
    return getComponent<Rigidbody2DComponent>().autoCenterOfMass;
}

void Rigidbody2D::setAutoCenterOfMass(bool autoCenterOfMass){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (body.autoCenterOfMass != autoCenterOfMass){
        body.autoCenterOfMass = autoCenterOfMass;
        massChanged();
    }
}

float Rigidbody2D::getInertia() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (b2Body_IsValid(body.body)){
        return b2Body_GetRotationalInertia(body.body);
    }
    return body.inertia;
}

void Rigidbody2D::setInertia(float inertia){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    body.inertia = (std::isfinite(inertia) && inertia >= 0.0f) ? inertia : body.inertia;
    body.autoInertia = false;
    massChanged();
}

bool Rigidbody2D::isAutoInertia() const{
    return getComponent<Rigidbody2DComponent>().autoInertia;
}

void Rigidbody2D::setAutoInertia(bool autoInertia){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (body.autoInertia != autoInertia){
        body.autoInertia = autoInertia;
        massChanged();
    }
}

CollisionDetectionMode2D Rigidbody2D::getCollisionDetection() const{
    return getComponent<Rigidbody2DComponent>().collisionDetection;
}

void Rigidbody2D::setCollisionDetection(CollisionDetectionMode2D mode){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    body.collisionDetection = mode;
    if (b2Body_IsValid(body.body)){
        b2Body_SetBullet(body.body, mode == CollisionDetectionMode2D::CONTINUOUS);
    }
}

RigidbodySleepMode2D Rigidbody2D::getSleepingMode() const{
    return getComponent<Rigidbody2DComponent>().sleepingMode;
}

void Rigidbody2D::setSleepingMode(RigidbodySleepMode2D mode){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    body.sleepingMode = mode;
    if (b2Body_IsValid(body.body)){
        b2Body_EnableSleep(body.body, mode != RigidbodySleepMode2D::NEVER_SLEEP);
    }
}

void Rigidbody2D::setMaterial(const std::string& path){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    body.material = path;
    body.materialInstance.reset();
    scene->getSystem<PhysicsSystem>()->refreshRigidbody2DMaterials(entity, body, true);
}

void Rigidbody2D::setMaterial(const PhysicsMaterial& material){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    body.materialInstance = material.getRuntime();
    body.material = material.valid() ? material.getPath() : std::string();
    scene->getSystem<PhysicsSystem>()->refreshRigidbody2DMaterials(entity, body, true);
}

void Rigidbody2D::clearMaterial(){
    setMaterial(std::string());
}

PhysicsMaterial Rigidbody2D::getMaterial() const{
    const Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    if (body.materialInstance) return PhysicsMaterial(body.materialInstance);
    if (!body.material.empty()) return PhysicsMaterial::load(body.material);
    return PhysicsMaterial();
}

std::string Rigidbody2D::getMaterialPath() const{
    return getComponent<Rigidbody2DComponent>().material;
}

void Rigidbody2D::applyForce(const Vector2& force, const Vector2& point, bool wake){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    float pointsToMeterScale = getPointsToMeterScale();

    checkBody(body);
    b2Body_ApplyForce(body.body, {force.x, force.y}, {point.x / pointsToMeterScale, point.y / pointsToMeterScale}, wake);
}

void Rigidbody2D::applyForceToCenter(const Vector2& force, bool wake){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Body_ApplyForceToCenter(body.body, {force.x, force.y}, wake);
}

void Rigidbody2D::applyTorque(float torque, bool wake){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Body_ApplyTorque(body.body, torque, wake);
}

void Rigidbody2D::applyLinearImpulse(const Vector2& impulse, const Vector2& point, bool wake){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    float pointsToMeterScale = getPointsToMeterScale();

    checkBody(body);
    b2Body_ApplyLinearImpulse(body.body, {impulse.x, impulse.y}, {point.x / pointsToMeterScale, point.y / pointsToMeterScale}, wake);
}

void Rigidbody2D::applyLinearImpulseToCenter(const Vector2& impulse, bool wake){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Body_ApplyLinearImpulseToCenter(body.body, {impulse.x, impulse.y}, wake);
}

void Rigidbody2D::applyAngularImpulse(float impulse, bool wake){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Body_ApplyAngularImpulse(body.body, impulse, wake);
}
