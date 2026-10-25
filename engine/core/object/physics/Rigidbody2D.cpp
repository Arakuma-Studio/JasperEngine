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

#include <algorithm>
#include <cmath>

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
    if (!body.colliders.validIndex(body.numColliders)){
        Log::error("Cannot add more shapes in this body");
        return -1;
    }

    size_t index = body.numColliders;
    body.colliders[index] = Collider2D();
    body.numColliders++;
    body.needUpdateColliders = true;

    return static_cast<int>(index);
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

b2BodyId Rigidbody2D::getBox2DBody() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return body.body;
}

b2ShapeId Rigidbody2D::getBox2DShape(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (body.colliders.validIndex(index)){
        return body.colliders[index].shape;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return b2_nullShapeId;
}

b2ChainId Rigidbody2D::getBox2DChain(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (body.colliders.validIndex(index)){
        return body.colliders[index].chain;
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
    if (index < 0) return index;

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::POLYGON;
    shape.numVertices = 4;
    shape.vertices[0] = Vector2(0, 0);
    shape.vertices[1] = Vector2(width, 0);
    shape.vertices[2] = Vector2(width, height);
    shape.vertices[3] = Vector2(0, height);

    return index;
}

int Rigidbody2D::createCenteredBoxCollider(float width, float height){
    return createCenteredBoxCollider(width, height, Vector2(0, 0), 0);
}

int Rigidbody2D::createCenteredBoxCollider(float width, float height, Vector2 center, float angle){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);
    if (index < 0) return index;

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::POLYGON;
    shape.numVertices = 4;

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
        shape.vertices[i] = Vector2((x * c) - (y * s), (x * s) + (y * c)) + center;
    }

    return index;
}

int Rigidbody2D::createRoundedBoxCollider(float width, float height, float radius){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);
    if (index < 0) return index;

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::POLYGON;
    shape.radius = radius;
    shape.numVertices = 4;

    float halfW = width / 2.0f;
    float halfH = height / 2.0f;
    shape.vertices[0] = Vector2(-halfW, -halfH);
    shape.vertices[1] = Vector2(halfW, -halfH);
    shape.vertices[2] = Vector2(halfW, halfH);
    shape.vertices[3] = Vector2(-halfW, halfH);

    return index;
}

int Rigidbody2D::createPolygonCollider(std::vector<Vector2> vertices){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);
    if (index < 0) return index;

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::POLYGON;
    shape.numVertices = vertices.size();
    for (size_t i = 0; i < shape.numVertices; i++){
        if (!shape.vertices.validIndex(i)){
            Log::warn("Polygon shape vertices truncated from %d to %zu", shape.numVertices, i);
            shape.numVertices = i;
            break;
        }
        shape.vertices[i] = vertices[i];
    }

    return index;
}

int Rigidbody2D::createCircleCollider(Vector2 center, float radius){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);
    if (index < 0) return index;

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::CIRCLE;
    shape.pointA = center;
    shape.radius = radius;

    return index;
}

int Rigidbody2D::createCapsuleCollider(Vector2 center1, Vector2 center2, float radius){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);
    if (index < 0) return index;

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
    if (index < 0) return index;

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::SEGMENT;
    shape.pointA = point1;
    shape.pointB = point2;

    return index;
}

int Rigidbody2D::createChainCollider(std::vector<Vector2> vertices, bool loop){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    int index = appendCollider2D(body);
    if (index < 0) return index;

    Collider2D& shape = body.colliders[index];
    shape.type = Collider2DType::CHAIN;
    shape.loop = loop;
    shape.numVertices = vertices.size();
    for (size_t i = 0; i < shape.numVertices; i++){
        if (!shape.vertices.validIndex(i)){
            Log::warn("Chain shape vertices truncated from %d to %zu", shape.numVertices, i);
            shape.numVertices = i;
            break;
        }
        shape.vertices[i] = vertices[i];
    }

    return index;
}

void Rigidbody2D::removeAllColliders(){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    body.numColliders = 0;
    body.needUpdateColliders = true;
    markChanged<Rigidbody2DComponent>();
}

std::vector<Contact2D> Rigidbody2D::getBodyContacts(){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);

    std::vector<Contact2D> contacts;

    int bodyContactCapacity = b2Body_GetContactCapacity(body.body);
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

    if (index < body.numColliders && body.colliders[index].type != Collider2DType::CHAIN){
        int shapeContactCapacity = b2Shape_GetContactCapacity(body.colliders[index].shape);
        std::vector<b2ContactData> contactData(shapeContactCapacity);
        int shapeContactCount = b2Shape_GetContactData(body.colliders[index].shape, &contactData.front(), shapeContactCapacity);

        for (int i = 0; i < shapeContactCount; ++i){
            contacts.push_back(Contact2D(scene, contactData[i]));
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return contacts;
}

size_t Rigidbody2D::getNumColliders() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    return body.numColliders;
}

Collider2DType Rigidbody2D::getColliderType(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
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

    if (index < body.numColliders){
        body.colliders[index].density = density;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set density of chain shape %zu", index);
        }else if (b2Shape_IsValid(body.colliders[index].shape)){
            b2Shape_SetDensity(body.colliders[index].shape, density, true);
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderFriction(size_t index, float friction){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
        body.colliders[index].friction = friction;
        if (body.colliders[index].type != Collider2DType::CHAIN && b2Shape_IsValid(body.colliders[index].shape)){
            b2Shape_SetFriction(body.colliders[index].shape, friction);
        }else if (b2Chain_IsValid(body.colliders[index].chain)){
            b2Chain_SetFriction(body.colliders[index].chain, friction);
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderRestitution(size_t index, float restitution){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
        body.colliders[index].restitution = restitution;
        if (body.colliders[index].type != Collider2DType::CHAIN && b2Shape_IsValid(body.colliders[index].shape)){
            b2Shape_SetRestitution(body.colliders[index].shape, restitution);
        }else if (b2Chain_IsValid(body.colliders[index].chain)){
            b2Chain_SetRestitution(body.colliders[index].chain, restitution);
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

    if (index < body.numColliders){
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set sensor of chain shape %zu", index);
        }else if (body.colliders[index].sensor != sensor){
            body.colliders[index].sensor = sensor;
            body.needUpdateColliders = true;
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

    if (index < body.numColliders){
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

    if (index < body.numColliders){
        body.colliders[index].enableHitEvents = hitEvents;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set hit events of chain shape %zu", index);
        }else if (b2Shape_IsValid(body.colliders[index].shape)){
            b2Shape_EnableHitEvents(body.colliders[index].shape, hitEvents);
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderContactEvents(size_t index, bool contactEvents){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
        body.colliders[index].contactEvents = contactEvents;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set shape contact events of chain shape %zu", index);
        }else if (b2Shape_IsValid(body.colliders[index].shape)){
            b2Shape_EnableContactEvents(body.colliders[index].shape, contactEvents);
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderPreSolveEvents(size_t index, bool preSolveEvent){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
        body.colliders[index].preSolveEvents = preSolveEvent;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set presolve events of chain shape %zu", index);
        }else if (b2Shape_IsValid(body.colliders[index].shape)){
            b2Shape_EnablePreSolveEvents(body.colliders[index].shape, preSolveEvent);
        }
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }
}

void Rigidbody2D::setColliderSensorEvents(size_t index, bool sensorEvents){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
        body.colliders[index].sensorEvents = sensorEvents;
        if (body.colliders[index].type == Collider2DType::CHAIN){
            Log::error("Cannot set sensor events of chain shape %zu", index);
        }else if (b2Shape_IsValid(body.colliders[index].shape)){
            b2Shape_EnableSensorEvents(body.colliders[index].shape, sensorEvents || body.colliders[index].sensor);
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

    if (index < body.numColliders){
        return body.colliders[index].density;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return -1;
}

float Rigidbody2D::getColliderFriction(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
        return body.colliders[index].friction;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return -1;
}

float Rigidbody2D::getColliderRestitution(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
        return body.colliders[index].restitution;
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

    if (index < body.numColliders){
        return body.colliders[index].enableHitEvents;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return false;
}

bool Rigidbody2D::isColliderContactEvents(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
        return body.colliders[index].contactEvents;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return false;
}

bool Rigidbody2D::isColliderPreSolveEvents(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
        return body.colliders[index].preSolveEvents;
    }else{
        Log::error("Cannot find shape %zu of body", index);
    }

    return false;
}

bool Rigidbody2D::isColliderSensorEvents(size_t index) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (index < body.numColliders){
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
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    float pointsToMeterScale = getPointsToMeterScale();

    checkBody(body);
    b2Transform bTransform = b2Body_GetTransform(body.body);
    b2Body_SetTransform(body.body, {position.x / pointsToMeterScale, position.y / pointsToMeterScale}, bTransform.q);
    b2Body_SetAwake(body.body, true);

    scene->getSystem<PhysicsSystem>()->updateTransformFromRigidbody2D(entity, position, getAngle());
}

float Rigidbody2D::getAngle() const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    return Angle::radToDefault(b2Rot_GetAngle(b2Body_GetRotation(body.body)));
}

void Rigidbody2D::setAngle(float angle){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Transform bTransform = b2Body_GetTransform(body.body);
    b2Body_SetTransform(body.body, bTransform.p, b2MakeRot(Angle::defaultToRad(angle)));
    b2Body_SetAwake(body.body, true);

    scene->getSystem<PhysicsSystem>()->updateTransformFromRigidbody2D(entity, getPosition(), angle);
}

void Rigidbody2D::setLinearVelocity(Vector2 linearVelocity){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    float pointsToMeterScale = getPointsToMeterScale();

    checkBody(body);
    // points per second, like positions
    b2Body_SetLinearVelocity(body.body, {linearVelocity.x / pointsToMeterScale, linearVelocity.y / pointsToMeterScale});
}

void Rigidbody2D::setAngularVelocity(float angularVelocity){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Body_SetAngularVelocity(body.body, angularVelocity);
}

void Rigidbody2D::setLinearDamping(float linearDamping){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Body_SetLinearDamping(body.body, linearDamping);
}

void Rigidbody2D::setAngularDamping(float angularDamping){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Body_SetAngularDamping(body.body, angularDamping);
}

void Rigidbody2D::setEnableSleep(bool enableSleep){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

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

    checkBody(body);
    b2Body_SetFixedRotation(body.body, fixedRotation);
}

void Rigidbody2D::setBullet(bool bullet){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    checkBody(body);
    b2Body_SetBullet(body.body, bullet);
}

void Rigidbody2D::setType(BodyType type){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();

    if (b2Body_IsValid(body.body)){
        b2Body_SetType(body.body, getBodyTypeToB2(type));
    }
    body.type = type;
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

    if (body.colliders.validIndex(colliderIndex)){
        body.colliders[colliderIndex].categoryBits = categoryBits;
        body.colliders[colliderIndex].maskBits = maskBits;

        if (b2Shape_IsValid(body.colliders[colliderIndex].shape)){
            b2Filter filter = b2Shape_GetFilter(body.colliders[colliderIndex].shape);
            filter.categoryBits = Box2DLayers::withUserCategory(filter.categoryBits, categoryBits);
            filter.maskBits = maskBits;

            b2Shape_SetFilter(body.colliders[colliderIndex].shape, filter);
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

    if (body.colliders.validIndex(colliderIndex)){
        body.colliders[colliderIndex].categoryBits = categoryBits;
        if (b2Shape_IsValid(body.colliders[colliderIndex].shape)){
            b2Filter filter = b2Shape_GetFilter(body.colliders[colliderIndex].shape);

            filter.categoryBits = Box2DLayers::withUserCategory(filter.categoryBits, categoryBits);

            b2Shape_SetFilter(body.colliders[colliderIndex].shape, filter);
        }
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }
}

void Rigidbody2D::setMaskBitsFilter(size_t colliderIndex, uint16_t maskBits){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    
    if (body.colliders.validIndex(colliderIndex)){
        body.colliders[colliderIndex].maskBits = maskBits;
        if (b2Shape_IsValid(body.colliders[colliderIndex].shape)){
            b2Filter filter = b2Shape_GetFilter(body.colliders[colliderIndex].shape);

            filter.maskBits = maskBits;

            b2Shape_SetFilter(body.colliders[colliderIndex].shape, filter);
        }
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }
}

void Rigidbody2D::setGroupIndexFilter(size_t colliderIndex, int16_t groupIndex){
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    
    if (body.colliders.validIndex(colliderIndex)){
        body.colliders[colliderIndex].groupIndex = groupIndex;
        if (b2Shape_IsValid(body.colliders[colliderIndex].shape)){
            b2Filter filter = b2Shape_GetFilter(body.colliders[colliderIndex].shape);

            filter.groupIndex = groupIndex;

            b2Shape_SetFilter(body.colliders[colliderIndex].shape, filter);
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

    if (body.colliders.validIndex(colliderIndex)){
        return body.colliders[colliderIndex].categoryBits;
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }

    return 0;
}

uint16_t Rigidbody2D::getMaskBitsFilter(size_t colliderIndex) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    
    if (body.colliders.validIndex(colliderIndex)){
        return body.colliders[colliderIndex].maskBits;
    }else{
        Log::error("Cannot find shape %zu of body", colliderIndex);
    }

    return 0;
}

int16_t Rigidbody2D::getGroupIndexFilter(size_t colliderIndex) const{
    Rigidbody2DComponent& body = getComponent<Rigidbody2DComponent>();
    
    if (body.colliders.validIndex(colliderIndex)){
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
    b2Body_ApplyMassFromShapes(body.body);
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
