// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsSystem.h"
#include "Scene.h"
#include "physics/JoltShapeBuilder.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/WaterSystem.h"
#include "object/environment/Water.h"
#include "util/Angle.h"

#ifdef JASPERENGINE_PHYSICS_2D
#include "util/Box2DAux.h"
#include "subsystem/CharacterController2DSystem.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "subsystem/CharacterControllerSystem.h"
#include "util/JoltPhysicsAux.h"
#include "util/MeshTriangles.h"
#endif

#include <algorithm>
#include <tuple>
#include <cmath>


using namespace jasperengine;

namespace {

    // Transform::worldScale is the entity's accumulated scale, resolved by
    // RenderSystem::updateTransform. Identity when there is no Transform, matching an
    // unscaled body.
    Vector3 entityWorldScale(Scene* scene, Entity entity){
        return JoltShapeBuilder::entityWorldScale(scene, entity);
    }

    Vector2 entityWorldScale2D(Scene* scene, Entity entity){
        const Vector3 scale = entityWorldScale(scene, entity);
        return Vector2(scale.x, scale.y);
    }

    // world pose of a transform the render pass has not updated yet, parents first
    void resolveWorldTransform(Scene* scene, Transform& transform, Entity entity, int depth = 0){
        if (transform.parent != NULL_ENTITY && depth < 64){
            Transform* parent = scene->findComponent<Transform>(transform.parent);
            if (parent && parent->needUpdate){
                resolveWorldTransform(scene, *parent, transform.parent, depth + 1);
            }
        }
        scene->getSystem<RenderSystem>()->updateTransform(transform, entity);
    }

    void resolveWorldPose(Scene* scene, Entity entity){
        Transform* transform = scene->findComponent<Transform>(entity);
        if (transform && transform->needUpdate){
            resolveWorldTransform(scene, *transform, entity);
        }
    }

    // still the pose physics and the Transform last agreed on: not moved by a script, an
    // animation or the editor since
    template<typename T>
    bool isSynced(const Transform& transform, const T& body){
        return body.hasSyncedPose && transform.worldPosition == body.syncedPosition && transform.worldRotation == body.syncedRotation;
    }

    template<typename T>
    void setSynced(T& body, const Vector3& position, const Quaternion& rotation){
        body.hasSyncedPose = true;
        body.syncedPosition = position;
        body.syncedRotation = rotation;
    }

    // A kinematic body moved by its Transform further than its own size in a frame, or turned
    // more than this, is teleported there (a respawn, a reset) instead of swept through
    // everything in between at that speed
    const float maxFollowedTurn = (float)M_PI / 2.0f;

    // Writes a world pose as the local one, and the world one too as RenderSystem derives it
    // back from it (also returned in position and rotation): the next fixed step of the
    // frame, before any transform update, syncs the body from the world pose. A part the
    // Transform drives (a kinematic body following it) is left as it is and returned.
    void setTransformWorldPose(Scene* scene, Transform& transform, Vector3& position, Quaternion& rotation,
                               bool keepPosition = false, bool keepRotation = false){
        Vector3 localPosition = position;
        Quaternion localRotation = rotation;

        Transform* parent = (transform.parent != NULL_ENTITY) ? scene->findComponent<Transform>(transform.parent) : NULL;
        if (parent){
            localPosition = parent->modelMatrix.inverse() * position;
            localRotation = parent->worldRotation.inverse() * rotation;
            position = parent->modelMatrix * localPosition;
            rotation = parent->worldRotation * localRotation;
        }

        if (keepPosition){
            localPosition = transform.position;
            position = transform.worldPosition;
        }
        if (keepRotation){
            localRotation = transform.rotation;
            rotation = transform.worldRotation;
        }

        if (transform.position != localPosition || transform.rotation != localRotation ||
                transform.worldPosition != position || transform.worldRotation != rotation){
            transform.position = localPosition;
            transform.rotation = localRotation;
            transform.worldPosition = position;
            transform.worldRotation = rotation;
            transform.needUpdate = true;
        }
    }
}

#ifdef JASPERENGINE_PHYSICS_3D
Vector3 PhysicsSystem::absScale(const Vector3& scale){
    return Vector3(std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z));
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
Vector2 PhysicsSystem::absScale2D(const Vector2& scale){
    return Vector2(std::fabs(scale.x), std::fabs(scale.y));
}

float PhysicsSystem::maxScaleXY(const Vector2& scale){
    return std::max(scale.x, scale.y);
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
float PhysicsSystem::maxScaleXZ(const Vector3& scale){
    return std::max(scale.x, scale.z);
}

float PhysicsSystem::maxScaleXYZ(const Vector3& scale){
    return std::max(scale.x, std::max(scale.y, scale.z));
}

JPH::Quat PhysicsSystem::toValidatedJoltRotation(const Quaternion& rotation, Entity entity, int colliderIndex){
    const double w = rotation.w;
    const double x = rotation.x;
    const double y = rotation.y;
    const double z = rotation.z;
    const double lengthSquared = w * w + x * x + y * y + z * z;

    constexpr double minimumLengthSquared = 1.0e-12;
    if (!std::isfinite(lengthSquared) || lengthSquared <= minimumLengthSquared){
        if (reportedInvalidRotations.insert(entity).second){
            const std::string entityName = scene->getEntityName(entity);
            if (colliderIndex < 0){
                Log::error(
                    "Rigidbody entity %u (%s) has an invalid transform rotation quaternion "
                    "(w=%g, x=%g, y=%g, z=%g). Using identity before passing it to Jolt.",
                    entity, entityName.c_str(), w, x, y, z);
            }else{
                Log::error(
                    "Rigidbody entity %u (%s) has an invalid shape %d rotation quaternion "
                    "(w=%g, x=%g, y=%g, z=%g). Using identity before passing it to Jolt.",
                    entity, entityName.c_str(), colliderIndex, w, x, y, z);
            }
        }
        return JPH::Quat::sIdentity();
    }

    // Jolt requires rotation quaternions to have unit length. Normalize at the
    // integration boundary in every build so Release editor and Debug exports
    // behave consistently instead of relying on Jolt's debug-only assertion.
    // Jolt's tolerance is applied to squared length (Vec4::IsNormalized).
    constexpr double joltNormalizationTolerance = 1.0e-5;
    if (std::abs(lengthSquared - 1.0) > joltNormalizationTolerance
        && warnedNonNormalizedRotations.insert(entity).second){
        const std::string entityName = scene->getEntityName(entity);
        if (colliderIndex < 0){
            Log::warn(
                "Rigidbody entity %u (%s) has a non-normalized transform rotation quaternion "
                "(squared length=%g). Normalizing it before passing it to Jolt.",
                entity, entityName.c_str(), lengthSquared);
        }else{
            Log::warn(
                "Rigidbody entity %u (%s) has a non-normalized shape %d rotation quaternion "
                "(squared length=%g). Normalizing it before passing it to Jolt.",
                entity, entityName.c_str(), colliderIndex, lengthSquared);
        }
    }

    const double inverseLength = 1.0 / std::sqrt(lengthSquared);
    return JPH::Quat(
        static_cast<float>(x * inverseLength),
        static_cast<float>(y * inverseLength),
        static_cast<float>(z * inverseLength),
        static_cast<float>(w * inverseLength));
}

JPH::EMotionQuality PhysicsSystem::getRigidbodyMotionQualityToJolt(RigidbodyMotionQuality motionQuality){
    switch (motionQuality){
        case RigidbodyMotionQuality::LINEAR_CAST:
            return JPH::EMotionQuality::LinearCast;
        case RigidbodyMotionQuality::DISCRETE:
        default:
            return JPH::EMotionQuality::Discrete;
    }
}
#endif

PhysicsSystem::PhysicsSystem(Scene* scene): SubSystem(scene){
#ifdef JASPERENGINE_PHYSICS_2D
	signature.set(scene->getComponentId<Rigidbody2DComponent>());
#endif
	// the types onComponentRemoved handles
	declareNoHookTypes();
#ifdef JASPERENGINE_PHYSICS_2D
	declareHookType(scene->getComponentId<Rigidbody2DComponent>());
	declareHookType(scene->getComponentId<Joint2DComponent>());
	declareHookType(scene->getComponentId<CharacterController2DComponent>());
#endif
#ifdef JASPERENGINE_PHYSICS_3D
	declareHookType(scene->getComponentId<RigidbodyComponent>());
	declareHookType(scene->getComponentId<Joint3DComponent>());
	declareHookType(scene->getComponentId<CharacterControllerComponent>());
#endif

	this->scene = scene;
	water = std::make_unique<WaterSystem>(scene, this);

    // Default gravity comes from SceneSettings, the single source of truth for scene-setting
    // defaults. The live gravity value is then owned/mutated here (pushed to the Box2D/Jolt
    // worlds); serialization and the editor overwrite it via setGravity2D/3D.
    SceneSettings settingsDefaults;
    this->gravity2D = settingsDefaults.gravity2D;
    this->gravity3D = settingsDefaults.gravity3D;
    this->interpolation = settingsDefaults.physicsInterpolation;
#ifdef JASPERENGINE_PHYSICS_2D
    this->pointsToMeterScale2D = 64.0;
    characters2D = std::make_unique<CharacterController2DSystem>(scene, this);

    // The Box2D world is created lazily (see ensureWorld2D): Box2D's global b2_worlds[] pool is
    // capped at B2_MAX_WORLDS (128) and is not thread-safe. Editor preview/thumbnail/gizmo scenes
    // (and pure-3D scenes) have no 2D bodies, so they must not consume a world slot.
    world2D = b2_nullWorldId;

    Physics2D::getLayerCollisionMatrix().getRows(appliedLayerRows2D);
    appliedLayerVersion2D = Physics2D::getLayerCollisionMatrix().getVersion();
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    // https://github.com/jrouwe/JoltPhysics/issues/244
    // Jolt globals are initialized once per process (see helper).
    JoltShapeBuilder::ensureJoltGlobalInit();

    const unsigned int cMaxBodies = 1024;
    const unsigned int cNumBodyMutexes = 0;
    const unsigned int cMaxBodyPairs = 1024;
    const unsigned int cMaxContactConstraints = 1024;

    objectLayerTable3D = new JoltObjectLayerTable();
    broad_phase_layer_interface = new JoltBroadPhaseLayers(MAX_BROADPHASELAYER_3D, *objectLayerTable3D);
    object_vs_broadphase_layer_filter = new JoltObjectVsBroadPhaseLayerFilter(*broad_phase_layer_interface);
    object_vs_object_layer_filter = new JoltObjectLayerPairFilter(*objectLayerTable3D, Physics::getLayerCollisionMatrix());

    // Bodies made from now on read the live matrix; only later changes need waking bodies
    Physics::getLayerCollisionMatrix().getRows(appliedLayerRows3D);
    appliedLayerVersion3D = Physics::getLayerCollisionMatrix().getVersion();

    // Now we can create the actual physics system.
    world3D.Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, *broad_phase_layer_interface, *object_vs_broadphase_layer_filter, *object_vs_object_layer_filter);
    world3D.SetGravity(JPH::Vec3(this->gravity3D.x, this->gravity3D.y, this->gravity3D.z));

    // Shared across all scenes (created once); physics updates are sequential on the main thread.
    temp_allocator = &JoltShapeBuilder::sharedPhysicsTempAllocator();
    job_system = &JoltShapeBuilder::sharedPhysicsJobSystem();

    activationListener3D = new JoltActivationListener(scene, this);
    world3D.SetBodyActivationListener(activationListener3D);

    contactListener3D = new JoltContactListener(scene, this);
    world3D.SetContactListener(contactListener3D);

    lock3DBodies = true;

    characters3D = std::make_unique<CharacterControllerSystem>(scene, this);
#endif
}

#ifdef JASPERENGINE_PHYSICS_2D
void PhysicsSystem::ensureWorld2D(){
    if (b2World_IsValid(world2D)){
        return;
    }

    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = {this->gravity2D.x, this->gravity2D.y};
    world2D = b2CreateWorld(&worldDef);

    if (!b2World_IsValid(world2D)){
        Log::error("Could not create Box2D world: the global world pool is exhausted");
        return;
    }

    // shapes made from now on take the live matrix
    Physics2D::getLayerCollisionMatrix().getRows(appliedLayerRows2D);
    appliedLayerVersion2D = Physics2D::getLayerCollisionMatrix().getVersion();

    b2World_SetPreSolveCallback(world2D, Box2DAux::PreSolve, scene);
    b2World_SetCustomFilterCallback(world2D, Box2DAux::CollisionFilter, scene);
}
#endif

PhysicsSystem::~PhysicsSystem(){
#ifdef JASPERENGINE_PHYSICS_3D
    // the characters release their object layers and inner bodies while both still exist
    characters3D.reset();
    flushPendingDestroys3D();
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    characters2D.reset();
    if (b2World_IsValid(world2D)){
        b2DestroyWorld(world2D);
    }
    world2D = b2_nullWorldId;
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    delete activationListener3D;
    delete contactListener3D;

    // temp_allocator and job_system are process-shared singletons (see constructor); not owned here.
    delete broad_phase_layer_interface;
    delete object_vs_broadphase_layer_filter;
    delete object_vs_object_layer_filter;
    delete objectLayerTable3D;

#endif
}

Vector2 PhysicsSystem::getGravity2D() const{
    return gravity2D;
}

void PhysicsSystem::setGravity2D(Vector2 gravity){
    if (this->gravity2D != gravity){
        this->gravity2D = gravity;
#ifdef JASPERENGINE_PHYSICS_2D
        if (b2World_IsValid(world2D)){
            b2World_SetGravity(world2D, {gravity.x, gravity.y});
        }
#endif
    }
}

void PhysicsSystem::setGravity2D(float x, float y){
    setGravity2D(Vector2(x, y));
}

Vector3 PhysicsSystem::getGravity3D() const{
    return gravity3D;
}

void PhysicsSystem::setGravity3D(Vector3 gravity){
    if (this->gravity3D != gravity){
        this->gravity3D = gravity;
#ifdef JASPERENGINE_PHYSICS_3D
        world3D.SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
#endif
    }
}

void PhysicsSystem::setGravity3D(float x, float y, float z){
    setGravity3D(Vector3(x, y, z));
}

Vector3 PhysicsSystem::getGravity() const{
    return gravity3D;
}

void PhysicsSystem::setGravity(Vector3 gravity){
    setGravity2D(Vector2(gravity.x, gravity.y));
    setGravity3D(gravity);
}

void PhysicsSystem::setGravity(float x, float y){
    setGravity(Vector3(x, y, 0));
}

void PhysicsSystem::setGravity(float x, float y, float z){
    setGravity(Vector3(x, y, z));
}

void PhysicsSystem::setInterpolation(bool interpolation){
    this->interpolation = interpolation;
}

bool PhysicsSystem::isInterpolation() const{
    return interpolation;
}

void PhysicsSystem::clearSubscriptions(){
#ifdef JASPERENGINE_PHYSICS_2D
    beginContact2D.clear();
    endContact2D.clear();
    beginSensorContact2D.clear();
    endSensorContact2D.clear();
    hitContact2D.clear();
    preSolve2D.clear();
    shouldCollide2D.clear();
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    onBodyActivated3D.clear();
    onBodyDeactivated3D.clear();
    onContactAdded3D.clear();
    onContactPersisted3D.clear();
    onContactRemoved3D.clear();
    shouldCollide3D.clear();
#endif
}

void PhysicsSystem::removeSubscriptionsByTag(const std::string& substring){
#ifdef JASPERENGINE_PHYSICS_2D
    beginContact2D.removeByTagSubstring(substring);
    endContact2D.removeByTagSubstring(substring);
    beginSensorContact2D.removeByTagSubstring(substring);
    endSensorContact2D.removeByTagSubstring(substring);
    hitContact2D.removeByTagSubstring(substring);
    preSolve2D.removeByTagSubstring(substring);
    shouldCollide2D.removeByTagSubstring(substring);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    onBodyActivated3D.removeByTagSubstring(substring);
    onBodyDeactivated3D.removeByTagSubstring(substring);
    onContactAdded3D.removeByTagSubstring(substring);
    onContactPersisted3D.removeByTagSubstring(substring);
    onContactRemoved3D.removeByTagSubstring(substring);
    shouldCollide3D.removeByTagSubstring(substring);
#endif
}

#ifdef JASPERENGINE_PHYSICS_2D
float PhysicsSystem::getPointsToMeterScale2D() const{
    return pointsToMeterScale2D;
}

void PhysicsSystem::setPointsToMeterScale2D(float pointsToMeterScale2D){
    this->pointsToMeterScale2D = pointsToMeterScale2D;
}

bool PhysicsSystem::isSteppingWorld2D() const{
    return steppingWorld2D;
}

int PhysicsSystem::moveCharacter2D(Entity entity, const Vector2& motion){
    return characters2D->move(entity, motion);
}

bool PhysicsSystem::simpleMoveCharacter2D(Entity entity, const Vector2& speed){
    return characters2D->simpleMove(entity, speed);
}

// Box2D drops a shape's contacts and pairs it again whenever its filter changes. With
// toggleRefilterBit the filter only changes for that; with a layer, the shape's layer bit
// is set to it. Chain segments are shapes of the body too.
void PhysicsSystem::refilterShapes2D(b2BodyId bodyId, bool toggleRefilterBit, int layer){
    const int count = b2Body_GetShapeCount(bodyId);
    if (count <= 0){
        return;
    }
    if (shapeScratch2D.size() < (size_t)count){
        shapeScratch2D.resize((size_t)count);
    }
    const int returned = b2Body_GetShapes(bodyId, shapeScratch2D.data(), count);

    bool changed = false;
    for (int i = 0; i < returned; i++){
        b2Filter filter = b2Shape_GetFilter(shapeScratch2D[i]);
        uint64_t categoryBits = filter.categoryBits;
        if (toggleRefilterBit){
            categoryBits ^= Box2DLayers::REFILTER_BIT;
        }
        if (layer >= 0){
            categoryBits = (categoryBits & ~(0xFFFFFFFFull << Box2DLayers::LAYER_SHIFT)) | Box2DLayers::layerBit(layer);
        }
        if (categoryBits != filter.categoryBits){
            filter.categoryBits = categoryBits;
            b2Shape_SetFilter(shapeScratch2D[i], filter);
            changed = true;
        }
    }

    // A sleeping body with no contact left would not look for new ones
    if (changed && b2Body_GetType(bodyId) != b2_staticBody && b2Body_IsEnabled(bodyId)){
        b2Body_SetAwake(bodyId, true);
    }
}

void PhysicsSystem::refreshRigidbody2DLayer(Entity entity){
    Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
    if (!body || !b2Body_IsValid(body->body)){
        return;
    }

    // Box2D ignores filter changes while it steps
    if (steppingWorld2D){
        pendingLayerRefresh2D.push_back(entity);
        return;
    }

    refilterShapes2D(body->body, false, scene->getEntityLayer(entity));
}

void PhysicsSystem::syncLayerCollisionMatrix2D(){
    const LayerCollisionMatrix& matrix = Physics2D::getLayerCollisionMatrix();
    const uint32_t version = matrix.getVersion();
    if (version == appliedLayerVersion2D){
        return;
    }
    appliedLayerVersion2D = version;

    uint32_t rows[MAX_LAYERS];
    matrix.getRows(rows);
    uint32_t changedLayers = 0;
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        if (rows[layer] != appliedLayerRows2D[layer]){
            changedLayers |= 1u << layer;
            appliedLayerRows2D[layer] = rows[layer];
        }
    }
    if (changedLayers == 0 || !b2World_IsValid(world2D)){
        return;
    }

    // New pairs read the matrix live, but Box2D keeps the contacts it has: shapes on a
    // changed layer drop theirs and pair again (so a touching pair that still collides
    // reports an end and a new begin, as Unity documents for IgnoreLayerCollision)
    auto bodies2d = scene->getComponentArray<Rigidbody2DComponent>();
    for (size_t i = 0; i < bodies2d->size(); i++){
        const Rigidbody2DComponent& body = bodies2d->getComponentFromIndex(i);
        if (!b2Body_IsValid(body.body)){
            continue;
        }
        if ((changedLayers >> scene->getEntityLayer(bodies2d->getEntity(i))) & 1u){
            refilterShapes2D(body.body, true, -1);
        }
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
void PhysicsSystem::setLock3DBodies(bool lock3DBodies){
    this->lock3DBodies = lock3DBodies;
}

bool PhysicsSystem::isLock3DBodies() const{
    return this->lock3DBodies;
}

// The step wins over lock3DBodies: a script re-arming the flag from a callback would
// otherwise restore the deadlock.
bool PhysicsSystem::useLockedInterfaces3D() const{
    return lock3DBodies && !steppingWorld3D;
}

JPH::BodyInterface& PhysicsSystem::getBodyInterface3D(){
    return useLockedInterfaces3D() ? world3D.GetBodyInterface() : world3D.GetBodyInterfaceNoLock();
}

const JPH::BodyLockInterface& PhysicsSystem::getBodyLockInterface3D(){
    return useLockedInterfaces3D() ? static_cast<const JPH::BodyLockInterface&>(world3D.GetBodyLockInterface())
                                   : static_cast<const JPH::BodyLockInterface&>(world3D.GetBodyLockInterfaceNoLock());
}

const JPH::NarrowPhaseQuery& PhysicsSystem::getNarrowPhaseQuery3D(){
    return useLockedInterfaces3D() ? world3D.GetNarrowPhaseQuery() : world3D.GetNarrowPhaseQueryNoLock();
}

bool PhysicsSystem::isSteppingWorld3D() const{
    return steppingWorld3D;
}

std::recursive_mutex& PhysicsSystem::getCallbackMutex3D(){
    return callbackMutex3D;
}

void PhysicsSystem::queueContact3D(uint8_t kind, Entity body1, Entity body2, unsigned long collider1, unsigned long collider2, const Contact3D& contact){
    std::lock_guard<std::recursive_mutex> lock(callbackMutex3D);
    PendingContact3D pending;
    pending.kind = kind;
    pending.body1 = body1;
    pending.body2 = body2;
    pending.collider1 = collider1;
    pending.collider2 = collider2;
    pending.contact = contact;
    pendingContacts3D.push_back(std::move(pending));
}

void PhysicsSystem::queueActivation3D(Entity body, bool activated){
    std::lock_guard<std::recursive_mutex> lock(callbackMutex3D);
    pendingActivations3D.emplace_back(body, activated);
}

void PhysicsSystem::applyContactOverride3D(const ContactPairKey3D& key, JPH::ContactSettings& settings){
    if (!hasContactOverrides3D.load(std::memory_order_acquire)){
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(callbackMutex3D);
    auto it = contactOverrides3D.find(key);
    if (it == contactOverrides3D.end()){
        return;
    }
    if (it->second.hasFriction) settings.mCombinedFriction = it->second.friction;
    if (it->second.hasRestitution) settings.mCombinedRestitution = it->second.restitution;
    if (it->second.hasSensor) settings.mIsSensor = it->second.sensor;
}

void PhysicsSystem::forgetContact3D(const ContactPairKey3D& key){
    if (!hasContactOverrides3D.load(std::memory_order_acquire)){
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(callbackMutex3D);
    contactOverrides3D.erase(key);
    hasContactOverrides3D.store(!contactOverrides3D.empty(), std::memory_order_release);
}

void PhysicsSystem::setContactFriction3D(const ContactPairKey3D& key, float friction){
    std::lock_guard<std::recursive_mutex> lock(callbackMutex3D);
    ContactOverride3D& entry = contactOverrides3D[key];
    entry.hasFriction = true;
    entry.friction = friction;
    hasContactOverrides3D.store(true, std::memory_order_release);
}

void PhysicsSystem::setContactRestitution3D(const ContactPairKey3D& key, float restitution){
    std::lock_guard<std::recursive_mutex> lock(callbackMutex3D);
    ContactOverride3D& entry = contactOverrides3D[key];
    entry.hasRestitution = true;
    entry.restitution = restitution;
    hasContactOverrides3D.store(true, std::memory_order_release);
}

void PhysicsSystem::setContactSensor3D(const ContactPairKey3D& key, bool sensor){
    std::lock_guard<std::recursive_mutex> lock(callbackMutex3D);
    ContactOverride3D& entry = contactOverrides3D[key];
    entry.hasSensor = true;
    entry.sensor = sensor;
    hasContactOverrides3D.store(true, std::memory_order_release);
}

void PhysicsSystem::deliverEvents3D(){
    std::vector<PendingContact3D> contacts;
    std::vector<std::pair<Entity, bool>> activations;
    {
        std::lock_guard<std::recursive_mutex> lock(callbackMutex3D);
        contacts.swap(pendingContacts3D);
        activations.swap(pendingActivations3D);
    }
    if (contacts.empty() && activations.empty()){
        return;
    }
    // Jolt's job threads reported them in whatever order they ran: delivered in one order (the
    // kind, then the bodies and colliders), the same on every run
    std::stable_sort(contacts.begin(), contacts.end(), [](const PendingContact3D& a, const PendingContact3D& b){
        return std::tie(a.kind, a.body1, a.body2, a.collider1, a.collider2) < std::tie(b.kind, b.body1, b.body2, b.collider1, b.collider2);
    });
    std::stable_sort(activations.begin(), activations.end());

    for (const PendingContact3D& pending : contacts){
        const Rigidbody body1(scene, pending.body1);
        const Rigidbody body2(scene, pending.body2);
        if (pending.kind == 0){
            onContactAdded3D(body1, body2, pending.contact);
        }else if (pending.kind == 1){
            onContactPersisted3D(body1, body2, pending.contact);
        }else{
            onContactRemoved3D(body1, body2, pending.collider1, pending.collider2);
        }
    }
    for (const auto& [entity, activated] : activations){
        if (activated){
            onBodyActivated3D(Rigidbody(scene, entity));
        }else{
            onBodyDeactivated3D(Rigidbody(scene, entity));
        }
    }
}

void PhysicsSystem::flushPendingDestroys3D(){
    for (const JPH::Ref<JPH::TwoBodyConstraint>& constraint : pendingJointDestroy3D){
        world3D.RemoveConstraint(constraint);
    }
    pendingJointDestroy3D.clear();

    JPH::BodyInterface& body_interface = getBodyInterface3D();
    for (const JPH::BodyID& bodyId : pendingBodyDestroy3D){
        if (body_interface.IsAdded(bodyId)){
            body_interface.RemoveBody(bodyId);
        }
        releaseObjectLayer3D(bodyId);
        body_interface.DestroyBody(bodyId);
    }
    pendingBodyDestroy3D.clear();
}

const JoltObjectLayerTable& PhysicsSystem::getObjectLayerTable3D() const{
    return *objectLayerTable3D;
}

void PhysicsSystem::releaseObjectLayer3D(const JPH::BodyID& bodyId){
    objectLayerTable3D->release(getBodyInterface3D().GetObjectLayer(bodyId));
}

void PhysicsSystem::wakeAround3D(const JPH::BodyID& bodyId){
    JPH::BodyInterface& body_interface = getBodyInterface3D();
    if (!body_interface.IsAdded(bodyId)){
        return;
    }

    // a little beyond the bounds, as contacts start at the speculative distance
    JPH::AABox bounds = body_interface.GetTransformedShape(bodyId).GetWorldSpaceBounds();
    bounds.ExpandBy(JPH::Vec3::sReplicate(0.1f));

    // the body itself is in the box; Jolt skips static bodies
    body_interface.ActivateBodiesInAABox(bounds, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
}

void PhysicsSystem::refreshRigidbodyFilter3D(Entity entity){
    RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
    if (!body || body->body.IsInvalid()){
        return;
    }

    // the broad phase and the table are in use until the step ends
    if (steppingWorld3D){
        pendingLayerRefresh3D.push_back(entity);
        return;
    }

    JPH::BodyInterface& body_interface = getBodyInterface3D();
    const JPH::ObjectLayer current = body_interface.GetObjectLayer(body->body);
    const JPH::ObjectLayer wanted = objectLayerTable3D->acquire(scene->getEntityLayer(entity), body->categoryBits, body->maskBits);
    if (wanted == current){
        objectLayerTable3D->release(wanted);
        return;
    }

    body_interface.SetObjectLayer(body->body, wanted);
    objectLayerTable3D->release(current);

    wakeAround3D(body->body);
}

void PhysicsSystem::syncLayerCollisionMatrix3D(){
    const LayerCollisionMatrix& matrix = Physics::getLayerCollisionMatrix();
    const uint32_t version = matrix.getVersion();
    if (version == appliedLayerVersion3D){
        return;
    }
    appliedLayerVersion3D = version;

    uint32_t rows[MAX_LAYERS];
    matrix.getRows(rows);
    uint32_t changedLayers = 0;
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        if (rows[layer] != appliedLayerRows3D[layer]){
            changedLayers |= 1u << layer;
            appliedLayerRows3D[layer] = rows[layer];
        }
    }
    if (changedLayers == 0){
        return;
    }

    // The pair filter reads the matrix live, so awake bodies follow it on this step;
    // sleeping ones on a changed layer are woken to be tested again
    bodyScratch3D.clear();
    auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
    for (size_t i = 0; i < bodies3d->size(); i++){
        const RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
        if (body.body.IsInvalid() || body.type == BodyType::STATIC){
            continue;
        }
        if ((changedLayers >> scene->getEntityLayer(bodies3d->getEntity(i))) & 1u){
            bodyScratch3D.push_back(body.body);
        }
    }
    if (!bodyScratch3D.empty()){
        JPH::BodyInterface& body_interface = getBodyInterface3D();
        size_t count = 0;
        for (const JPH::BodyID& bodyId : bodyScratch3D){
            if (body_interface.IsAdded(bodyId)){
                bodyScratch3D[count++] = bodyId;
            }
        }
        body_interface.ActivateBodies(bodyScratch3D.data(), (int)count);
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
void PhysicsSystem::updateTransformFromRigidbody2D(Entity entity, Rigidbody2DComponent& body, float alpha){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || !b2Body_IsValid(body.body)){
        return;
    }

    // a body that never stepped has no pose before the step to draw from
    b2Transform bTransform = b2Body_GetTransform(body.body);
    if (interpolation && alpha < 1.0f && !body.newBody){
        const b2Transform& previous = body.previousTransform;
        bTransform.p = previous.p + (bTransform.p - previous.p) * alpha;
        if (previous.q.c != bTransform.q.c || previous.q.s != bTransform.q.s){
            bTransform.q = b2NLerp(previous.q, bTransform.q, alpha);
        }
    }

    // Box2D does not own Z, it is kept from the current world position
    Vector3 position(bTransform.p.x * pointsToMeterScale2D, bTransform.p.y * pointsToMeterScale2D, transform->worldPosition.z);
    Quaternion rotation(Angle::radToDefault(b2Rot_GetAngle(bTransform.q)), Vector3(0, 0, 1));

    // the parts a following body is on its way to stay as the Transform has them
    setTransformWorldPose(scene, *transform, position, rotation, body.followingPosition, body.followingRotation);
    setSynced(body, position, rotation);
}
#endif

void PhysicsSystem::resolveTransformWorldPose(Entity entity){
    resolveWorldPose(scene, entity);
}

void PhysicsSystem::writeTransformWorldPose(Entity entity, const Vector3& position, const Quaternion& rotation){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform){
        return;
    }
    Vector3 worldPosition = position;
    Quaternion worldRotation = rotation;
    setTransformWorldPose(scene, *transform, worldPosition, worldRotation);
}

#ifdef JASPERENGINE_PHYSICS_3D
void PhysicsSystem::updateTransformFromRigidbody(Entity entity, RigidbodyComponent& body, float alpha){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || body.body.IsInvalid()){
        return;
    }

    JPH::RVec3 jPosition;
    JPH::Quat jRotation;
    getBodyInterface3D().GetPositionAndRotation(body.body, jPosition, jRotation);
    if (std::isnan(jPosition.GetX()) || std::isnan(jPosition.GetY()) || std::isnan(jPosition.GetZ())){
        return;
    }

    Vector3 position(jPosition.GetX(), jPosition.GetY(), jPosition.GetZ());
    Quaternion rotation(jRotation.GetW(), jRotation.GetX(), jRotation.GetY(), jRotation.GetZ());
    // a body that never stepped has no pose before the step to draw from
    if (interpolation && alpha < 1.0f && !body.newBody){
        position = body.previousPosition.lerp(position, alpha);
        if (body.previousRotation != rotation){
            rotation = Quaternion::slerp(alpha, body.previousRotation, rotation);
        }
    }

    // the parts a following body is on its way to stay as the Transform has them
    setTransformWorldPose(scene, *transform, position, rotation, body.followingPosition, body.followingRotation);
    setSynced(body, position, rotation);
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
void PhysicsSystem::stopFollowing2D(Rigidbody2DComponent& body, bool position, bool rotation){
    position = position && body.followingPosition;
    rotation = rotation && body.followingRotation;
    if (!position && !rotation){
        return;
    }

    if (b2Body_IsValid(body.body)){
        if (position && b2Body_GetLinearVelocity(body.body) == body.followedLinearVelocity){
            b2Body_SetLinearVelocity(body.body, b2Vec2_zero);
        }
        if (rotation && b2Body_GetAngularVelocity(body.body) == body.followedAngularVelocity){
            b2Body_SetAngularVelocity(body.body, 0.0f);
        }
    }
    if (position){
        body.followingPosition = false;
    }
    if (rotation){
        body.followingRotation = false;
    }
}

void PhysicsSystem::updateRigidbody2DPosition(Signature signature, Entity entity, Rigidbody2DComponent& body, float stepTime){
    if (signature.test(scene->getComponentId<Transform>())){
        Transform& transform = scene->getComponent<Transform>(entity);
        if (b2Body_IsValid(body.body)){

            if (body.newBody){
                resolveWorldPose(scene, entity);
            }

            b2Transform bTransform = b2Body_GetTransform(body.body);
            float bAngle = b2Rot_GetAngle(bTransform.q);

            const b2Vec2 bWorldPosition = {transform.worldPosition.x / pointsToMeterScale2D, transform.worldPosition.y / pointsToMeterScale2D};
            const float bWorldAngle = Angle::defaultToRad(transform.worldRotation.getRoll());

            // Each part (position, angle) the Transform moved since physics and it last agreed
            // moves the body by as much, from where the body really is: a drawn interpolated
            // pose is behind it. A new body goes where the Transform is, and so does a part a
            // following body is on its way to: until the last step of the frame put it there,
            // then while the Transform moves on. A part nobody moved stays as physics has it.
            const bool absolute = body.newBody || !body.hasSyncedPose;
            const bool positionMovedOn = transform.worldPosition != body.syncedPosition;
            const bool rotationMovedOn = transform.worldRotation != body.syncedRotation;
            b2Vec2 bNewPosition = bTransform.p;
            float bNewAngle = bAngle;
            if (absolute || (body.followingPosition && (!body.followReached || positionMovedOn))){
                bNewPosition = bWorldPosition;
            }else if (positionMovedOn){
                const b2Vec2 bSyncedPosition = {body.syncedPosition.x / pointsToMeterScale2D, body.syncedPosition.y / pointsToMeterScale2D};
                bNewPosition = bTransform.p + (bWorldPosition - bSyncedPosition);
            }
            if (absolute || (body.followingRotation && (!body.followReached || rotationMovedOn))){
                bNewAngle = bWorldAngle;
            }else if (rotationMovedOn){
                bNewAngle = bAngle + b2UnwindAngle(bWorldAngle - Angle::defaultToRad(body.syncedRotation.getRoll()));
            }

            const bool positionMoved = bNewPosition != bTransform.p;
            const bool rotationMoved = bNewAngle != bAngle;

            // inside a step a kinematic body is moved there with a velocity, which carries and
            // pushes other bodies, spread over the steps left in the frame
            const int steps = Engine::getFixedStepsLeft();
            bool followPosition = false;
            bool followRotation = false;
            if ((positionMoved || rotationMoved) && stepTime > 0.0f && body.type == BodyType::KINEMATIC && !body.newBody){
                // a body without colliders (a joint anchor) has no size to go through
                const b2AABB bounds = b2Body_ComputeAABB(body.body);
                const float size = std::max(bounds.upperBound.x - bounds.lowerBound.x, bounds.upperBound.y - bounds.lowerBound.y);
                followPosition = positionMoved && (size <= 0.0f || b2Length(bNewPosition - bTransform.p) / steps <= size);
                followRotation = rotationMoved && std::fabs(b2UnwindAngle(bNewAngle - bAngle)) / steps <= maxFollowedTurn;
            }

            // the parts no longer followed stop
            stopFollowing2D(body, !followPosition, !followRotation);

            const float duration = stepTime * steps;
            if (followPosition){
                b2Body_SetLinearVelocity(body.body, (bNewPosition - bTransform.p) * (1.0f / duration));
                body.followedLinearVelocity = b2Body_GetLinearVelocity(body.body);
            }
            if (followRotation){
                b2Body_SetAngularVelocity(body.body, b2UnwindAngle(bNewAngle - bAngle) / duration);
                body.followedAngularVelocity = b2Body_GetAngularVelocity(body.body);
            }

            // and the parts moved but not followed jump there
            const bool jumpPosition = positionMoved && !followPosition;
            const bool jumpRotation = rotationMoved && !followRotation;
            if (jumpPosition || jumpRotation){
                b2Body_SetTransform(body.body, jumpPosition ? bNewPosition : bTransform.p, jumpRotation ? b2MakeRot(bNewAngle) : bTransform.q);
            }
            if (positionMoved || rotationMoved){
                b2Body_SetAwake(body.body, true);
            }

            if (followPosition || followRotation){
                body.followReached = false;
            }
            body.followingPosition = followPosition;
            body.followingRotation = followRotation;
            setSynced(body, transform.worldPosition, transform.worldRotation);

            // drawn from here towards where the step ends
            body.previousTransform = b2Body_GetTransform(body.body);
        }
    }
}

void PhysicsSystem::finishFollowing2D(Entity entity, Rigidbody2DComponent& body){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || !b2Body_IsValid(body.body) || (!body.followingPosition && !body.followingRotation)){
        return;
    }

    // the last step of the frame ends where the Transform is; the velocity stays until the
    // next step, which finds whether the Transform moves on
    b2Transform bTransform = b2Body_GetTransform(body.body);
    if (body.followingPosition){
        bTransform.p = {transform->worldPosition.x / pointsToMeterScale2D, transform->worldPosition.y / pointsToMeterScale2D};
    }
    if (body.followingRotation){
        bTransform.q = b2MakeRot(Angle::defaultToRad(transform->worldRotation.getRoll()));
    }
    b2Body_SetTransform(body.body, bTransform.p, bTransform.q);
    body.followReached = true;
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
void PhysicsSystem::stopFollowing3D(RigidbodyComponent& body, bool position, bool rotation){
    position = position && body.followingPosition;
    rotation = rotation && body.followingRotation;
    if (!position && !rotation){
        return;
    }

    if (!body.body.IsInvalid()){
        JPH::BodyInterface& body_interface = getBodyInterface3D();
        JPH::Vec3 linearVelocity;
        JPH::Vec3 angularVelocity;
        body_interface.GetLinearAndAngularVelocity(body.body, linearVelocity, angularVelocity);
        bool stopped = false;
        if (position && Vector3(linearVelocity.GetX(), linearVelocity.GetY(), linearVelocity.GetZ()) == body.followedLinearVelocity){
            linearVelocity = JPH::Vec3::sZero();
            stopped = true;
        }
        if (rotation && Vector3(angularVelocity.GetX(), angularVelocity.GetY(), angularVelocity.GetZ()) == body.followedAngularVelocity){
            angularVelocity = JPH::Vec3::sZero();
            stopped = true;
        }
        if (stopped){
            body_interface.SetLinearAndAngularVelocity(body.body, linearVelocity, angularVelocity);
        }
    }
    if (position){
        body.followingPosition = false;
    }
    if (rotation){
        body.followingRotation = false;
    }
}

void PhysicsSystem::updateRigidbodyPosition(Signature signature, Entity entity, RigidbodyComponent& body, float stepTime){
    if (signature.test(scene->getComponentId<Transform>())){
        Transform& transform = scene->getComponent<Transform>(entity);
        if (!body.body.IsInvalid()){
            if (body.newBody){
                resolveWorldPose(scene, entity);
            }

            JPH::BodyInterface &body_interface = world3D.GetBodyInterfaceNoLock();
            JPH::Vec3 jPosition;
            JPH::Quat jQuat;
            body_interface.GetPositionAndRotation(body.body, jPosition, jQuat);

            // Each part (position, rotation) the Transform moved since physics and it last
            // agreed moves the body by as much, from where the body really is: a drawn
            // interpolated pose is behind it. A new body goes where the Transform is, and so
            // does a part a following body is on its way to: until the last step of the frame
            // put it there, then while the Transform moves on (not the pose read back, which a
            // centre of mass off the origin rounds). A part nobody moved stays as physics has it.
            const bool absolute = body.newBody || !body.hasSyncedPose;
            const bool positionMovedOn = transform.worldPosition != body.syncedPosition;
            const bool rotationMovedOn = transform.worldRotation != body.syncedRotation;
            JPH::Vec3 jNewPosition = jPosition;
            JPH::Quat jNewQuat = jQuat;
            if (absolute || (body.followingPosition && (!body.followReached || positionMovedOn))){
                jNewPosition = JPH::Vec3(transform.worldPosition.x, transform.worldPosition.y, transform.worldPosition.z);
            }else if (positionMovedOn){
                const Vector3 delta = transform.worldPosition - body.syncedPosition;
                jNewPosition = jPosition + JPH::Vec3(delta.x, delta.y, delta.z);
            }
            if (absolute || (body.followingRotation && (!body.followReached || rotationMovedOn))){
                jNewQuat = toValidatedJoltRotation(transform.worldRotation, entity, -1);
            }else if (rotationMovedOn){
                const Quaternion bodyRotation(jQuat.GetW(), jQuat.GetX(), jQuat.GetY(), jQuat.GetZ());
                jNewQuat = toValidatedJoltRotation((bodyRotation * body.syncedRotation.inverse() * transform.worldRotation).normalized(), entity, -1);
            }

            const bool positionMoved = jNewPosition != jPosition;
            const bool rotationMoved = jNewQuat != jQuat;

            // inside a step a kinematic body is moved there with a velocity, which carries and
            // pushes other bodies, spread over the steps left in the frame
            const int steps = Engine::getFixedStepsLeft();
            bool followPosition = false;
            bool followRotation = false;
            if ((positionMoved || rotationMoved) && stepTime > 0.0f && body.type == BodyType::KINEMATIC && !body.newBody){
                const float size = body_interface.GetShape(body.body)->GetLocalBounds().GetSize().ReduceMax();
                followPosition = positionMoved && (size <= 0.0f || (jNewPosition - jPosition).Length() / steps <= size);
                JPH::Vec3 axis;
                float turn;
                (jNewQuat * jQuat.Conjugated()).Normalized().GetAxisAngle(axis, turn);
                followRotation = rotationMoved && std::fabs(turn) / steps <= maxFollowedTurn;
            }

            // the parts no longer followed stop
            stopFollowing3D(body, !followPosition, !followRotation);

            if (followPosition || followRotation){
                // a part not followed keeps its velocity
                JPH::Vec3 linearVelocity;
                JPH::Vec3 angularVelocity;
                body_interface.GetLinearAndAngularVelocity(body.body, linearVelocity, angularVelocity);

                float fraction = 1.0f / steps;
                JPH::Vec3 stepPosition = followPosition ? jPosition + (jNewPosition - jPosition) * fraction : jPosition;
                JPH::Quat stepRotation = followRotation ? jQuat.SLERP(jNewQuat, fraction).Normalized() : jQuat;
                body_interface.MoveKinematic(body.body, stepPosition, stepRotation, stepTime);

                if (!followPosition){
                    body_interface.SetLinearVelocity(body.body, linearVelocity);
                }
                if (!followRotation){
                    body_interface.SetAngularVelocity(body.body, angularVelocity);
                }

                body_interface.GetLinearAndAngularVelocity(body.body, linearVelocity, angularVelocity);
                if (followPosition){
                    body.followedLinearVelocity = Vector3(linearVelocity.GetX(), linearVelocity.GetY(), linearVelocity.GetZ());
                }
                if (followRotation){
                    body.followedAngularVelocity = Vector3(angularVelocity.GetX(), angularVelocity.GetY(), angularVelocity.GetZ());
                }
                body.followReached = false;
            }

            // and the parts moved but not followed jump there
            const bool jumpPosition = positionMoved && !followPosition;
            const bool jumpRotation = rotationMoved && !followRotation;
            if (jumpPosition || jumpRotation){
                body_interface.SetPositionAndRotation(body.body, jumpPosition ? jNewPosition : jPosition, jumpRotation ? jNewQuat : jQuat, JPH::EActivation::Activate);
            }

            body.followingPosition = followPosition;
            body.followingRotation = followRotation;
            setSynced(body, transform.worldPosition, transform.worldRotation);

            // drawn from here towards where the step ends
            body_interface.GetPositionAndRotation(body.body, jPosition, jQuat);
            body.previousPosition = Vector3(jPosition.GetX(), jPosition.GetY(), jPosition.GetZ());
            body.previousRotation = Quaternion(jQuat.GetW(), jQuat.GetX(), jQuat.GetY(), jQuat.GetZ());
        }
    }
}

void PhysicsSystem::finishFollowing3D(Entity entity, RigidbodyComponent& body){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || body.body.IsInvalid() || (!body.followingPosition && !body.followingRotation)){
        return;
    }

    // the last step of the frame ends where the Transform is; the velocity stays until the
    // next step, which finds whether the Transform moves on
    JPH::BodyInterface &body_interface = world3D.GetBodyInterfaceNoLock();
    JPH::Vec3 jPosition;
    JPH::Quat jQuat;
    body_interface.GetPositionAndRotation(body.body, jPosition, jQuat);
    if (body.followingPosition){
        jPosition = JPH::Vec3(transform->worldPosition.x, transform->worldPosition.y, transform->worldPosition.z);
    }
    if (body.followingRotation){
        jQuat = toValidatedJoltRotation(transform->worldRotation, entity, -1);
    }
    body_interface.SetPositionAndRotation(body.body, jPosition, jQuat, JPH::EActivation::DontActivate);
    body.followReached = true;
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
bool PhysicsSystem::loadJoint2D(Entity entity, Joint2DComponent& joint){
    ensureWorld2D();
    if (!b2World_IsValid(world2D)){
        return false;
    }

    if (b2Joint_IsValid(joint.joint)){
        destroyJoint2D(joint);
    }

    if (joint.bodyA == NULL_ENTITY){
        return false;
    }

    switch (joint.type){
        case Joint2DType::DISTANCE:
            if (joint.bodyB == NULL_ENTITY) return false;
            return loadDistanceJoint2D(entity, joint, joint.bodyA, joint.bodyB, joint.anchorA, joint.anchorB, joint.autoAnchors, joint.rope);
        case Joint2DType::REVOLUTE:
            if (joint.bodyB == NULL_ENTITY) return false;
            return loadRevoluteJoint2D(entity, joint, joint.bodyA, joint.bodyB, joint.anchorA);
        case Joint2DType::PRISMATIC:
            if (joint.bodyB == NULL_ENTITY) return false;
            return loadPrismaticJoint2D(entity, joint, joint.bodyA, joint.bodyB, joint.anchorA, joint.axis);
        case Joint2DType::MOUSE:
            if (joint.bodyB == NULL_ENTITY) return false;
            return loadMouseJoint2D(entity, joint, joint.bodyA, joint.bodyB, joint.target);
        case Joint2DType::WHEEL:
            if (joint.bodyB == NULL_ENTITY) return false;
            return loadWheelJoint2D(entity, joint, joint.bodyA, joint.bodyB, joint.anchorA, joint.axis);
        case Joint2DType::WELD:
            if (joint.bodyB == NULL_ENTITY) return false;
            return loadWeldJoint2D(entity, joint, joint.bodyA, joint.bodyB, joint.anchorA);
        case Joint2DType::MOTOR:
            if (joint.bodyB == NULL_ENTITY) return false;
            return loadMotorJoint2D(entity, joint, joint.bodyA, joint.bodyB);
        default:
            return false;
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
bool PhysicsSystem::loadJoint3D(Entity entity, Joint3DComponent& joint){
    // The step holds the constraint list; the update loop retries any joint without one.
    if (steppingWorld3D){
        Log::error("Cannot load a 3D joint while the physics world is stepping, retrying next update");
        return false;
    }

    if (joint.joint){
        destroyJoint3D(joint);
    }

    if (joint.bodyA == NULL_ENTITY || joint.bodyB == NULL_ENTITY){
        return false;
    }

    Signature signatureA = scene->getSignature(joint.bodyA);
    Signature signatureB = scene->getSignature(joint.bodyB);

    if (!signatureA.test(scene->getComponentId<RigidbodyComponent>()) || !signatureB.test(scene->getComponentId<RigidbodyComponent>())){
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    if (!loadRigidbody(joint.bodyA) || !loadRigidbody(joint.bodyB)){
        Log::error("Cannot create 3D joint, bodyA or bodyB has no valid physics shape/body");
        return false;
    }

    RigidbodyComponent& bodyAComp = scene->getComponent<RigidbodyComponent>(joint.bodyA);
    RigidbodyComponent& bodyBComp = scene->getComponent<RigidbodyComponent>(joint.bodyB);

    if (bodyAComp.body.IsInvalid() || bodyBComp.body.IsInvalid()){
        Log::error("Cannot create 3D joint, bodyA or bodyB body id is invalid");
        return false;
    }

    switch (joint.type){
        case Joint3DType::FIXED:
            return loadFixedJoint3D(joint, joint.bodyA, joint.bodyB);
        case Joint3DType::DISTANCE:
            return loadDistanceJoint3D(joint, joint.bodyA, joint.bodyB, joint.anchorA, joint.anchorB, joint.autoAnchors);
        case Joint3DType::POINT:
            return loadPointJoint3D(joint, joint.bodyA, joint.bodyB, joint.anchor);
        case Joint3DType::HINGE:
            return loadHingeJoint3D(joint, joint.bodyA, joint.bodyB, joint.anchor, joint.axis, joint.normal);
        case Joint3DType::CONE:
            return loadConeJoint3D(joint, joint.bodyA, joint.bodyB, joint.anchor, joint.twistAxis);
        case Joint3DType::PRISMATIC:
            return loadPrismaticJoint3D(joint, joint.bodyA, joint.bodyB, joint.axis, joint.limitsMin, joint.limitsMax);
        case Joint3DType::SWINGTWIST:
            return loadSwingTwistJoint3D(joint, joint.bodyA, joint.bodyB, joint.anchor, joint.twistAxis, joint.planeAxis, joint.normalHalfConeAngle, joint.planeHalfConeAngle, joint.twistMinAngle, joint.twistMaxAngle);
        case Joint3DType::SIXDOF:
            return loadSixDOFJoint3D(joint, joint.bodyA, joint.bodyB, joint.anchorA, joint.anchorB, joint.axisX, joint.axisY);
        case Joint3DType::PATH: {
            if (joint.pathPoints.size() < 2){
                Log::error("Cannot create path 3D joint, need at least 2 path points");
                return false;
            }

            std::vector<Vector3> tangents;
            std::vector<Vector3> normals;
            tangents.reserve(joint.pathPoints.size());
            normals.reserve(joint.pathPoints.size());

            for (size_t i = 0; i < joint.pathPoints.size(); i++){
                size_t prev = (i == 0) ? (joint.isLooping ? joint.pathPoints.size() - 1 : 0) : i - 1;
                size_t next = (i + 1 >= joint.pathPoints.size()) ? (joint.isLooping ? 0 : joint.pathPoints.size() - 1) : i + 1;

                Vector3 tangent(
                    joint.pathPoints[next].x - joint.pathPoints[prev].x,
                    joint.pathPoints[next].y - joint.pathPoints[prev].y,
                    joint.pathPoints[next].z - joint.pathPoints[prev].z
                );

                float tangentLengthSq = tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z;
                if (tangentLengthSq < 1.0e-6f){
                    tangent = Vector3(1.0f, 0.0f, 0.0f);
                }else{
                    float invTangentLength = 1.0f / std::sqrt(tangentLengthSq);
                    tangent = Vector3(tangent.x * invTangentLength, tangent.y * invTangentLength, tangent.z * invTangentLength);
                }

                Vector3 normal = (std::fabs(tangent.y) < 0.999f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
                float tangentDotNormal = tangent.x * normal.x + tangent.y * normal.y + tangent.z * normal.z;
                normal = Vector3(
                    normal.x - tangent.x * tangentDotNormal,
                    normal.y - tangent.y * tangentDotNormal,
                    normal.z - tangent.z * tangentDotNormal
                );

                float normalLengthSq = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
                if (normalLengthSq < 1.0e-6f){
                    normal = Vector3(0.0f, 0.0f, 1.0f);
                }else{
                    float invNormalLength = 1.0f / std::sqrt(normalLengthSq);
                    normal = Vector3(normal.x * invNormalLength, normal.y * invNormalLength, normal.z * invNormalLength);
                }

                tangents.push_back(tangent);
                normals.push_back(normal);
            }

            return loadPathJoint3D(joint, joint.bodyA, joint.bodyB, joint.pathPoints, tangents, normals, joint.pathPosition, joint.isLooping);
        }
        case Joint3DType::GEAR:
            if (joint.hingeA == NULL_ENTITY || joint.hingeB == NULL_ENTITY) return false;
            return loadGearJoint3D(joint, joint.bodyA, joint.bodyB, joint.hingeA, joint.hingeB, joint.numTeethGearA, joint.numTeethGearB);
        case Joint3DType::RACKANDPINON:
            if (joint.hinge == NULL_ENTITY || joint.slider == NULL_ENTITY) return false;
            return loadRackAndPinionJoint3D(joint, joint.bodyA, joint.bodyB, joint.hinge, joint.slider, joint.numTeethRack, joint.numTeethGear, joint.rackLength);
        case Joint3DType::PULLEY:
            return loadPulleyJoint3D(joint, joint.bodyA, joint.bodyB, joint.anchorA, joint.anchorB, joint.fixedPointA, joint.fixedPointB);
        default:
            return false;
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
bool PhysicsSystem::syncRigidbody2DColliders(Entity entity, Rigidbody2DComponent& body){
    if (!b2Body_IsValid(body.body)){
        return false;
    }

    const Vector2 entityScale = entityWorldScale2D(scene, entity);
    const float radiusScale = maxScaleXY(absScale2D(entityScale));

    auto scaledPoint = [&](const Vector2& point){
        return point * entityScale;
    };

    auto toBox2DPoint = [&](const Vector2& point){
        return b2Vec2{point.x / pointsToMeterScale2D, point.y / pointsToMeterScale2D};
    };

    auto toBox2DRadius = [&](float radius){
        return radius * radiusScale / pointsToMeterScale2D;
    };

    for (size_t i = 0; i < body.numColliders; i++){
        destroyCollider2D(body, i);
    }

    const int layer = scene->getEntityLayer(entity);

    for (size_t i = 0; i < body.numColliders; i++){
        Collider2D& shapeData = body.colliders[i];

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.userData = reinterpret_cast<void*>(i);
        shapeDef.density = shapeData.density;
        shapeDef.friction = shapeData.friction;
        shapeDef.restitution = shapeData.restitution;
        shapeDef.isSensor = shapeData.sensor;
        shapeDef.enableHitEvents = shapeData.enableHitEvents;
        shapeDef.enableContactEvents = shapeData.contactEvents;
        shapeDef.enablePreSolveEvents = shapeData.preSolveEvents;
        shapeDef.enableSensorEvents = shapeData.sensorEvents || shapeData.sensor;
        shapeDef.filter = Box2DLayers::makeFilter(layer, shapeData.categoryBits, shapeData.maskBits, shapeData.groupIndex);

        shapeData.shape = b2_nullShapeId;
        shapeData.chain = b2_nullChainId;

        if (shapeData.type == Collider2DType::POLYGON){
            std::vector<b2Vec2> b2vertices;

            if (shapeData.numVertices >= 3){
                b2vertices.resize(shapeData.numVertices);
                for (size_t j = 0; j < shapeData.numVertices; j++){
                    b2vertices[j] = toBox2DPoint(scaledPoint(shapeData.vertices[j]));
                }
            }else{
                const Vector2 pointA = scaledPoint(shapeData.pointA);
                const Vector2 pointB = scaledPoint(shapeData.pointB);
                const Vector2 minPt(std::min(pointA.x, pointB.x), std::min(pointA.y, pointB.y));
                const Vector2 maxPt(std::max(pointA.x, pointB.x), std::max(pointA.y, pointB.y));

                if (minPt == maxPt){
                    Log::error("Cannot create polygon shape %zu: less than 3 vertices and invalid pointA/pointB fallback", i);
                    continue;
                }

                b2vertices.resize(4);
                b2vertices[0] = toBox2DPoint(minPt);
                b2vertices[1] = toBox2DPoint(Vector2(maxPt.x, minPt.y));
                b2vertices[2] = toBox2DPoint(maxPt);
                b2vertices[3] = toBox2DPoint(Vector2(minPt.x, maxPt.y));
            }

            b2Hull hull = b2ComputeHull(&b2vertices[0], (int)b2vertices.size());
            if (!b2ValidateHull(&hull)){
                const Vector2 pointA = scaledPoint(shapeData.pointA);
                const Vector2 pointB = scaledPoint(shapeData.pointB);
                const Vector2 minPt(std::min(pointA.x, pointB.x), std::min(pointA.y, pointB.y));
                const Vector2 maxPt(std::max(pointA.x, pointB.x), std::max(pointA.y, pointB.y));

                if (minPt != maxPt){
                    b2vertices.resize(4);
                    b2vertices[0] = toBox2DPoint(minPt);
                    b2vertices[1] = toBox2DPoint(Vector2(maxPt.x, minPt.y));
                    b2vertices[2] = toBox2DPoint(maxPt);
                    b2vertices[3] = toBox2DPoint(Vector2(minPt.x, maxPt.y));
                    hull = b2ComputeHull(&b2vertices[0], (int)b2vertices.size());
                }

                if (!b2ValidateHull(&hull)){
                    Log::error("Cannot create polygon shape %zu: invalid hull", i);
                    continue;
                }
            }

            b2Polygon polygon = b2MakePolygon(&hull, std::max(0.0f, toBox2DRadius(shapeData.radius)));
            shapeData.shape = b2CreatePolygonShape(body.body, &shapeDef, &polygon);
        }else if (shapeData.type == Collider2DType::CIRCLE){
            float radius = toBox2DRadius(shapeData.radius);
            if (radius <= 0.0f){
                Log::error("Cannot create circle shape %zu: radius must be > 0", i);
                continue;
            }

            b2Circle circle = {0};
            circle.center = toBox2DPoint(scaledPoint(shapeData.pointA));
            circle.radius = radius;
            shapeData.shape = b2CreateCircleShape(body.body, &shapeDef, &circle);
        }else if (shapeData.type == Collider2DType::CAPSULE){
            float radius = toBox2DRadius(shapeData.radius);
            if (radius <= 0.0f){
                Log::error("Cannot create capsule shape %zu: radius must be > 0", i);
                continue;
            }

            Vector2 center1 = scaledPoint(shapeData.pointA);
            Vector2 center2 = scaledPoint(shapeData.pointB);
            if (center1 == center2){
                Log::error("Cannot create capsule shape %zu: pointA and pointB are equal", i);
                continue;
            }

            b2Capsule capsule = {0};
            capsule.center1 = toBox2DPoint(center1);
            capsule.center2 = toBox2DPoint(center2);
            capsule.radius = radius;
            shapeData.shape = b2CreateCapsuleShape(body.body, &shapeDef, &capsule);
        }else if (shapeData.type == Collider2DType::SEGMENT){
            Vector2 point1 = scaledPoint(shapeData.pointA);
            Vector2 point2 = scaledPoint(shapeData.pointB);
            if (point1 == point2){
                Log::error("Cannot create segment shape %zu: pointA and pointB are equal", i);
                continue;
            }

            b2Segment segment = {0};
            segment.point1 = toBox2DPoint(point1);
            segment.point2 = toBox2DPoint(point2);
            shapeData.shape = b2CreateSegmentShape(body.body, &shapeDef, &segment);
        }else if (shapeData.type == Collider2DType::CHAIN){
            if (shapeData.numVertices < 4){
                Log::error("Cannot create chain shape %zu: Box2D requires at least 4 vertices", i);
                continue;
            }

            std::vector<b2Vec2> b2vertices(shapeData.numVertices);
            for (size_t j = 0; j < shapeData.numVertices; j++){
                b2vertices[j] = toBox2DPoint(scaledPoint(shapeData.vertices[j]));
            }
            // mirroring flips the winding, which would swap the colliding side
            if (entityScale.x * entityScale.y < 0.0f){
                std::reverse(b2vertices.begin(), b2vertices.end());
            }

            b2ChainDef chainDef = b2DefaultChainDef();
            chainDef.points = &b2vertices[0];
            chainDef.count = (int)shapeData.numVertices;
            chainDef.isLoop = shapeData.loop;
            // the segments report the collider index, like the other shapes
            chainDef.userData = reinterpret_cast<void*>(i);
            chainDef.filter = Box2DLayers::makeFilter(layer, shapeData.categoryBits, shapeData.maskBits, shapeData.groupIndex);
            chainDef.friction = shapeData.friction;
            chainDef.restitution = shapeData.restitution;

            shapeData.chain = b2CreateChain(body.body, &chainDef);
        }
    }

    body.needUpdateColliders = false;
    body.loadedScale = entityScale;

    return true;
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
bool PhysicsSystem::createCollider3DForIndex(Entity entity, RigidbodyComponent& body, size_t index){
    Collider3D& shapeData = body.colliders[index];
    shapeData.shape = NULL;

    if (shapeData.source == Collider3DSource::NONE){
        if (shapeData.type == Collider3DType::CONVEX_HULL){
            shapeData.source = Collider3DSource::ENTITY_MESH;
            if (shapeData.sourceEntity == NULL_ENTITY){
                shapeData.sourceEntity = entity;
            }
            Log::warn("Rigidbody entity %u has CONVEX_HULL with Source=None. Falling back to Entity Mesh source.", entity);
        }else if (shapeData.type == Collider3DType::MESH){
            shapeData.source = Collider3DSource::ENTITY_MESH;
            if (shapeData.sourceEntity == NULL_ENTITY){
                shapeData.sourceEntity = entity;
            }
            Log::warn("Rigidbody entity %u has MESH with Source=None. Falling back to Entity Mesh source.", entity);
        }else if (shapeData.type == Collider3DType::HEIGHTFIELD){
            shapeData.source = Collider3DSource::ENTITY_HEIGHTFIELD;
            if (shapeData.sourceEntity == NULL_ENTITY){
                shapeData.sourceEntity = entity;
            }
            Log::warn("Rigidbody entity %u has HEIGHTFIELD with Source=None. Falling back to Entity Heightfield source.", entity);
        }
    }

    std::string error;
    shapeData.shape = JoltShapeBuilder::buildColliderShape(scene, entity, shapeData, &error);
    if (!error.empty()){
        Log::error("%s", error.c_str());
    }

    if (!shapeData.shape){
        return false;
    }

    JPH::Shape* mutableShape = const_cast<JPH::Shape*>(shapeData.shape.GetPtr());
    mutableShape->SetUserData(index);

    if (shapeData.shape->GetType() == JPH::EShapeType::Convex){
        JPH::ConvexShape* convexShape = (JPH::ConvexShape*)shapeData.shape.GetPtr();
        convexShape->SetDensity(shapeData.density);
    }

    return true;
}

// Hull, mesh and heightfield shapes read their geometry from an entity, and that geometry
// can land several frames after Scene::load: models are parsed on a worker thread and
// terrains wait on their heightmap. The parent walk covers multi-node models, whose child
// meshes are filled in by the ancestor holding the ModelComponent.
bool PhysicsSystem::isShapeSourceLoading(Entity entity, const Collider3D& shapeData) const{
    if (shapeData.type != Collider3DType::CONVEX_HULL && shapeData.type != Collider3DType::MESH && shapeData.type != Collider3DType::HEIGHTFIELD){
        return false;
    }
    if (shapeData.source == Collider3DSource::RAW_VERTICES || shapeData.source == Collider3DSource::RAW_MESH){
        return false;
    }

    return MeshTriangles::isGeometryLoading(scene, shapeData.sourceEntity == NULL_ENTITY ? entity : shapeData.sourceEntity);
}

bool PhysicsSystem::syncRigidbodyColliders(Entity entity, RigidbodyComponent& body){
    if (!body.body.IsInvalid()){
        destroyRigidbody(body);
        body.body = JPH::BodyID();
    }

    for (size_t i = 0; i < body.numColliders; i++){
        // a source still loading is not a failure: fixedUpdate retries the body every step
        if (isShapeSourceLoading(entity, body.colliders[i])){
            return false;
        }

        if (!createCollider3DForIndex(entity, body, i)){
            Log::error("Cannot create runtime shape %zu for 3D Body entity: %u", i, entity);
            return false;
        }
    }

    if (body.numColliders == 0){
        return false;
    }

    if (body.numColliders > 1){
        const Vector3 entityScale = entityWorldScale(scene, entity);
        JPH::StaticCompoundShapeSettings compound_shape;
        for (size_t i = 0; i < body.numColliders; i++){
            Vector3 scaledPosition = body.colliders[i].position * entityScale;
            JPH::Vec3 jPosition(scaledPosition.x, scaledPosition.y, scaledPosition.z);
            JPH::Quat jQuat = toValidatedJoltRotation(body.colliders[i].rotation, entity, static_cast<int>(i));
            compound_shape.AddShape(jPosition, jQuat, body.colliders[i].shape);
        }

        JPH::ShapeSettings::ShapeResult shape_result = compound_shape.Create();
        if (!shape_result.IsValid()){
            Log::error("Cannot create StaticCompoundShape for 3D Body entity: %u", entity);
            return false;
        }

        if (!createGenericJoltBody(entity, body, shape_result.Get())) return false;
    }else{
        Collider3D& shapeData = body.colliders[0];
        if (shapeData.position != Vector3::ZERO || shapeData.rotation != Quaternion::IDENTITY){
            Vector3 scaledPosition = shapeData.position * entityWorldScale(scene, entity);
            JPH::Vec3 jPosition(scaledPosition.x, scaledPosition.y, scaledPosition.z);
            JPH::Quat jQuat = toValidatedJoltRotation(shapeData.rotation, entity, 0);
            JPH::RotatedTranslatedShapeSettings rtShape(jPosition, jQuat, shapeData.shape);
            JPH::ShapeSettings::ShapeResult shape_result = rtShape.Create();
            if (!shape_result.IsValid()){
                Log::error("Cannot create RotatedTranslatedShapeSettings for 3D Body entity: %u", entity);
                return false;
            }

            if (!createGenericJoltBody(entity, body, shape_result.Get())) return false;
        }else{
            if (!createGenericJoltBody(entity, body, shapeData.shape)) return false;
        }
    }

    body.needReloadBody = false;
    body.needUpdateColliders = false;
    body.newBody = true;
    body.loadedScale = entityWorldScale(scene, entity);

    return true;
}

static_assert((uint8_t)JPH::EAllowedDOFs::Plane2D == (RigidbodyAllowedDOF_TranslationX | RigidbodyAllowedDOF_TranslationY | RigidbodyAllowedDOF_RotationZ)
    && (uint8_t)JPH::EAllowedDOFs::All == RigidbodyAllowedDOF_All, "RigidbodyAllowedDOFFlags must keep Jolt's bits");

bool PhysicsSystem::createGenericJoltBody(Entity entity, RigidbodyComponent& body, const JPH::ShapeRefC shape){
    JPH::EMotionType joltType = JPH::EMotionType::Static;
    JPH::EActivation activation = JPH::EActivation::DontActivate;
    if (body.type == BodyType::DYNAMIC){
        joltType = JPH::EMotionType::Dynamic;
        activation = JPH::EActivation::Activate;
    }else if (body.type == BodyType::KINEMATIC){
        joltType = JPH::EMotionType::Kinematic;
        activation = JPH::EActivation::Activate;
    }

    if (joltType != JPH::EMotionType::Static && shape && shape->MustBeStatic()){
        Log::warn("Rigidbody entity %u uses a static-only shape (Mesh/HeightField/Plane/...) and cannot be %s. Forcing body type to Static.",
                  entity,
                  body.type == BodyType::DYNAMIC ? "Dynamic" : "Kinematic");
        body.type = BodyType::STATIC;
        joltType = JPH::EMotionType::Static;
        activation = JPH::EActivation::DontActivate;
    }

    const JPH::ObjectLayer layer = objectLayerTable3D->acquire(scene->getEntityLayer(entity), body.categoryBits, body.maskBits);

    // made where its GameObject is, so a script reaching it before the first sync (Lua init())
    // finds it there
    JPH::Vec3 createPosition(0.0, 0.0, 0.0);
    JPH::Quat createRotation = JPH::Quat::sIdentity();
    if (Transform* transform = scene->findComponent<Transform>(entity)){
        createPosition = JPH::Vec3(transform->worldPosition.x, transform->worldPosition.y, transform->worldPosition.z);
        createRotation = toValidatedJoltRotation(transform->worldRotation, entity, -1);
    }
    JPH::BodyCreationSettings settings(shape, createPosition, createRotation, joltType, layer);
    settings.mMotionQuality = getRigidbodyMotionQualityToJolt(body.motionQuality);
    settings.mIsSensor = body.sensor;
    // a trigger sees the kinematic inner bodies of CharacterControllers (and other kinematics)
    settings.mCollideKinematicVsNonDynamic = body.sensor;
    settings.mGravityFactor = body.gravityFactor;
    settings.mFriction = body.friction;
    settings.mRestitution = body.restitution;
    settings.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(body.allowedDOFs);

    JPH::BodyInterface &body_interface = getBodyInterface3D();

    if(body.overrideMassProperties){
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        settings.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(JPH::Vec3(body.solidBoxSize.x, body.solidBoxSize.y, body.solidBoxSize.z), body.solidBoxDensity);
    }

    JPH::Body* jbody = body_interface.CreateBody(settings);
    if (!jbody){
        objectLayerTable3D->release(layer);
        Log::error("Cannot create 3D Body for entity %u: body limit reached", entity);
        return false;
    }
    jbody->SetUserData(entity);

    body.body = jbody->GetID();
    // drawn from where it is made until it steps
    JPH::RVec3 createdPosition = jbody->GetPosition();
    JPH::Quat createdRotation = jbody->GetRotation();
    body.previousPosition = Vector3(createdPosition.GetX(), createdPosition.GetY(), createdPosition.GetZ());
    body.previousRotation = Quaternion(createdRotation.GetW(), createdRotation.GetX(), createdRotation.GetY(), createdRotation.GetZ());
    body.followingPosition = false;
    body.followingRotation = false;
    body.followReached = false;

    body_interface.AddBody(body.body, activation);

    return true;
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
void PhysicsSystem::createRigidbody2D(Entity entity){
    Signature signature = scene->getSignature(entity);

    if (!signature.test(scene->getComponentId<Rigidbody2DComponent>())){
        scene->addComponent<Rigidbody2DComponent>(entity);
    }
}

void PhysicsSystem::removeRigidbody2D(Entity entity){
    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<Rigidbody2DComponent>())){
        destroyRigidbody2D(scene->getComponent<Rigidbody2DComponent>(entity));
        scene->removeComponent<Rigidbody2DComponent>(entity);
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
void PhysicsSystem::createRigidbody(Entity entity){
    Signature signature = scene->getSignature(entity);

    if (!signature.test(scene->getComponentId<RigidbodyComponent>())){
        scene->addComponent<RigidbodyComponent>(entity);
    }
}

void PhysicsSystem::removeRigidbody(Entity entity){
    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<RigidbodyComponent>())){
        destroyRigidbody(scene->getComponent<RigidbodyComponent>(entity));
        scene->removeComponent<RigidbodyComponent>(entity);
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
b2WorldId PhysicsSystem::getWorld2D() const{
     return world2D;
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
JPH::PhysicsSystem* PhysicsSystem::getWorld3D(){
    return &world3D;
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
b2BodyId PhysicsSystem::getBody(Entity entity){
    Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);

    if (body){
        return body->body;
    }

    return b2_nullBodyId;
}

bool PhysicsSystem::loadRigidbody2D(Entity entity){
    ensureWorld2D();
    if (!b2World_IsValid(world2D)){
        return false;
    }

    // shapes are sized by the world scale
    resolveWorldPose(scene, entity);

    Rigidbody2DComponent& body = scene->getComponent<Rigidbody2DComponent>(entity);

    if (!b2Body_IsValid(body.body) || body.needReloadBody){
        if (b2Body_IsValid(body.body)){
            b2DestroyBody(body.body);
            body.body = b2_nullBodyId;
        }

        b2BodyDef bodyDef = b2DefaultBodyDef();

        if (body.type == BodyType::STATIC){
            bodyDef.type = b2_staticBody;
        }else if (body.type == BodyType::KINEMATIC){
            bodyDef.type = b2_kinematicBody;
        }else{
            bodyDef.type = b2_dynamicBody;
        }
        bodyDef.userData = reinterpret_cast<void*>((uint64_t)entity);
        // made where its GameObject is, so a script reaching it before the first sync (Lua
        // init()) finds it there
        if (Transform* transform = scene->findComponent<Transform>(entity)){
            bodyDef.position = {transform->worldPosition.x / pointsToMeterScale2D, transform->worldPosition.y / pointsToMeterScale2D};
            bodyDef.rotation = b2MakeRot(Angle::defaultToRad(transform->worldRotation.getRoll()));
        }

        body.body = b2CreateBody(world2D, &bodyDef);
        // drawn from where it is made until it steps
        body.previousTransform = b2Body_GetTransform(body.body);
        body.followingPosition = false;
        body.followingRotation = false;
        body.followReached = false;
        body.newBody = true;
        body.needReloadBody = false;
        body.needUpdateColliders = true;

        return syncRigidbody2DColliders(entity, body);
    }

    Vector2 entityScale = entityWorldScale2D(scene, entity);
    if (entityScale != body.loadedScale){
        body.needUpdateColliders = true;
    }

    if (body.needUpdateColliders){
        return syncRigidbody2DColliders(entity, body);
    }

    return true;
}

bool PhysicsSystem::ensureRigidbody2DLoaded(Entity entity){
    Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
    if (!body){
        return false;
    }
    if (b2Body_IsValid(body->body)){
        return true;
    }

    if (loadRigidbody2D(entity)){
        body = scene->findComponent<Rigidbody2DComponent>(entity);
        if (body && b2Body_IsValid(body->body)){
            updateRigidbody2DPosition(scene->getSignature(entity), entity, *body);
        }
    }

    body = scene->findComponent<Rigidbody2DComponent>(entity);
    return body && b2Body_IsValid(body->body);
}

void PhysicsSystem::destroyRigidbody2D(Rigidbody2DComponent& body){
    // When a world leaves scope or is deleted by calling delete on a pointer
    // all the memory reserved for bodies, fixtures, and joints is freed
    if (b2Body_IsValid(body.body)){
        for (int i = 0; i < body.numColliders; i++){
            destroyCollider2D(body, i);
        }
        body.numColliders = 0;

        b2DestroyBody(body.body);

        body.body = b2_nullBodyId;
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
bool PhysicsSystem::loadRigidbody(Entity entity){
    // The step holds the broad phase; the update loop retries any body without a handle.
    if (steppingWorld3D){
        Log::error("Cannot load a 3D body while the physics world is stepping, retrying next update");
        return false;
    }

    resolveWorldPose(scene, entity);

    RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(entity);
    Vector3 entityScale = entityWorldScale(scene, entity);
    if (!body.body.IsInvalid() && entityScale != body.loadedScale){
        body.needUpdateColliders = true;
    }

    if (body.needReloadBody || body.needUpdateColliders || body.body.IsInvalid()){
        return syncRigidbodyColliders(entity, body);
    }

    return true;
}

bool PhysicsSystem::ensureRigidbodyLoaded(Entity entity){
    RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
    if (!body){
        return false;
    }
    if (!body->body.IsInvalid()){
        return true;
    }
    // a contact callback runs inside the step, which cannot take a new body
    if (steppingWorld3D){
        return false;
    }

    if (loadRigidbody(entity)){
        body = scene->findComponent<RigidbodyComponent>(entity);
        if (body && !body->body.IsInvalid()){
            updateRigidbodyPosition(scene->getSignature(entity), entity, *body);
        }
    }

    body = scene->findComponent<RigidbodyComponent>(entity);
    return body && !body->body.IsInvalid();
}

void PhysicsSystem::destroyRigidbody(RigidbodyComponent& body){
    if (body.body.IsInvalid()){
        return;
    }

    // Refusing would strand the body with user data pointing at an entity the caller is
    // about to drop, so park it for the flush. Shapes are refcounts, released either way.
    if (steppingWorld3D){
        pendingBodyDestroy3D.push_back(body.body);
    }else{
        JPH::BodyInterface &body_interface = getBodyInterface3D();
        // a disabled body was already taken out of the world
        if (body_interface.IsAdded(body.body)){
            body_interface.RemoveBody(body.body);
        }
        releaseObjectLayer3D(body.body);
        body_interface.DestroyBody(body.body);
    }

    for (int i = 0; i < body.numColliders; i++){
        destroyCollider3D(body, i);
    }

    body.body = JPH::BodyID();
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
int PhysicsSystem::loadCollider2D(Rigidbody2DComponent& body, void* shape, Collider2DType type){
    if (!b2Shape_IsValid(body.colliders[body.numColliders].shape)){
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.userData = reinterpret_cast<void*>(body.numColliders);

        if (!b2Body_IsValid(body.body)){
            Log::error("Cannot create 2D body shape without loaded body");
            return -1;
        }

        const int layer = scene->getEntityLayer(static_cast<Entity>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(body.body))));
        shapeDef.filter = Box2DLayers::makeFilter(layer, (uint16_t)shapeDef.filter.categoryBits, (uint16_t)shapeDef.filter.maskBits, shapeDef.filter.groupIndex);

        body.colliders[body.numColliders].shape = b2_nullShapeId;
        body.colliders[body.numColliders].chain = b2_nullChainId;

        if (type == Collider2DType::POLYGON){
            body.colliders[body.numColliders].shape = b2CreatePolygonShape(body.body, &shapeDef, (b2Polygon*)shape);
        }else if (type == Collider2DType::CIRCLE){
            body.colliders[body.numColliders].shape = b2CreateCircleShape(body.body, &shapeDef, (b2Circle*)shape);
        }else if (type == Collider2DType::CAPSULE){
            body.colliders[body.numColliders].shape = b2CreateCapsuleShape(body.body, &shapeDef, (b2Capsule*)shape);
        }else if (type == Collider2DType::SEGMENT){
            body.colliders[body.numColliders].shape = b2CreateSegmentShape(body.body, &shapeDef, (b2Segment*)shape);
        }else if (type == Collider2DType::CHAIN){
            b2ChainDef chainDef = *(b2ChainDef*)shape;
            chainDef.filter = Box2DLayers::makeFilter(layer, (uint16_t)chainDef.filter.categoryBits, (uint16_t)chainDef.filter.maskBits, chainDef.filter.groupIndex);
            body.colliders[body.numColliders].chain = b2CreateChain(body.body, &chainDef);
        }

        body.numColliders++;

        return (body.numColliders - 1);
    }

    return -1;
}

void PhysicsSystem::destroyCollider2D(Rigidbody2DComponent& body, size_t index){
    if (b2Shape_IsValid(body.colliders[index].shape)){
        b2DestroyShape(body.colliders[index].shape, true);

        body.colliders[index].shape = b2_nullShapeId;
    }
    if (b2Chain_IsValid(body.colliders[index].chain)){
        b2DestroyChain(body.colliders[index].chain);

        body.colliders[index].chain = b2_nullChainId;
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
int PhysicsSystem::loadCollider3D(RigidbodyComponent& body, const Vector3& position, const Quaternion& rotation, JPH::ShapeSettings* shapeSettings){
    JPH::ShapeSettings::ShapeResult shape_result = shapeSettings->Create();
    if (shape_result.IsValid()){
        JPH::Shape* shape = shape_result.Get();
        shape->SetUserData(body.numColliders);

        body.colliders[body.numColliders].shape = shape;
        body.colliders[body.numColliders].position = position;
        body.colliders[body.numColliders].rotation = rotation;

        body.numColliders++;

        return (body.numColliders - 1);
    }else{
        Log::error("Cannot create shape for 3D Body");
    }

    return -1;
}

void PhysicsSystem::destroyCollider3D(RigidbodyComponent& body, size_t index){
    // ShapeRefC::operator= releases the owned reference; do not call Shape::Release manually.
    body.colliders[index].shape = nullptr;
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
bool PhysicsSystem::loadDistanceJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchorA, Vector2 anchorB, bool autoAnchors, bool rope){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<Rigidbody2DComponent>()) && signatureB.test(scene->getComponentId<Rigidbody2DComponent>())){

        Rigidbody2DComponent myBodyA = scene->getComponent<Rigidbody2DComponent>(bodyA);
        Rigidbody2DComponent myBodyB = scene->getComponent<Rigidbody2DComponent>(bodyB);

        updateRigidbody2DPosition(signatureA, bodyA, myBodyA);
        updateRigidbody2DPosition(signatureB, bodyB, myBodyB);

        b2Vec2 worldPivotA = {anchorA.x / pointsToMeterScale2D, anchorA.y / pointsToMeterScale2D};
        b2Vec2 worldPivotB = {anchorB.x / pointsToMeterScale2D, anchorB.y / pointsToMeterScale2D};

        const bool isZeroAnchorA = (anchorA == Vector2::ZERO);
        const bool isZeroAnchorB = (anchorB == Vector2::ZERO);
        if (autoAnchors){
            worldPivotA = b2Body_GetPosition(myBodyA.body);
            worldPivotB = b2Body_GetPosition(myBodyB.body);
        }else if (isZeroAnchorA && isZeroAnchorB){
            Log::error("Cannot create distance 2D joint: Auto Anchors is disabled and both anchors are zero");
            return false;
        }

        b2DistanceJointDef jointDef = b2DefaultDistanceJointDef();
        jointDef.bodyIdA = myBodyA.body;
        jointDef.bodyIdB = myBodyB.body;
        jointDef.localAnchorA = b2Body_GetLocalPoint(myBodyA.body, worldPivotA);
        jointDef.localAnchorB = b2Body_GetLocalPoint(myBodyB.body, worldPivotB);
        jointDef.length = b2Distance(worldPivotA, worldPivotB);
        if (rope){
            // a zero-hertz spring frees the length, the limit caps it
            jointDef.enableSpring = true;
            jointDef.enableLimit = true;
            jointDef.minLength = 0;
            jointDef.maxLength = jointDef.length;
        }
        jointDef.userData = reinterpret_cast<void*>((uint64_t)entity);

        joint.joint = b2CreateDistanceJoint(world2D, &jointDef);
        joint.type = Joint2DType::DISTANCE;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadRevoluteJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<Rigidbody2DComponent>()) && signatureB.test(scene->getComponentId<Rigidbody2DComponent>())){

        Rigidbody2DComponent myBodyA = scene->getComponent<Rigidbody2DComponent>(bodyA);
        Rigidbody2DComponent myBodyB = scene->getComponent<Rigidbody2DComponent>(bodyB);

        updateRigidbody2DPosition(signatureA, bodyA, myBodyA);
        updateRigidbody2DPosition(signatureB, bodyB, myBodyB);

        b2Vec2 worldPivot = {anchor.x / pointsToMeterScale2D, anchor.y / pointsToMeterScale2D};

        b2RevoluteJointDef jointDef = b2DefaultRevoluteJointDef();
        jointDef.bodyIdA = myBodyA.body;
        jointDef.bodyIdB = myBodyB.body;
        jointDef.localAnchorA = b2Body_GetLocalPoint(myBodyA.body, worldPivot);
        jointDef.localAnchorB = b2Body_GetLocalPoint(myBodyB.body, worldPivot);
        jointDef.userData = reinterpret_cast<void*>((uint64_t)entity);

        joint.joint = b2CreateRevoluteJoint(world2D, &jointDef);
        joint.type = Joint2DType::REVOLUTE;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadPrismaticJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor, Vector2 axis){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<Rigidbody2DComponent>()) && signatureB.test(scene->getComponentId<Rigidbody2DComponent>())){

        Rigidbody2DComponent myBodyA = scene->getComponent<Rigidbody2DComponent>(bodyA);
        Rigidbody2DComponent myBodyB = scene->getComponent<Rigidbody2DComponent>(bodyB);

        updateRigidbody2DPosition(signatureA, bodyA, myBodyA);
        updateRigidbody2DPosition(signatureB, bodyB, myBodyB);

        b2Vec2 worldPivot = {anchor.x / pointsToMeterScale2D, anchor.y / pointsToMeterScale2D};
        float axisLengthSq = axis.x * axis.x + axis.y * axis.y;
        if (axisLengthSq < 1.0e-6f){
            Log::warn("Prismatic 2D joint axis is zero. Using default axis (1, 0).");
            axis = Vector2(1.0f, 0.0f);
        }
        b2Vec2 worldAxis = {axis.x, axis.y};

        b2PrismaticJointDef jointDef = b2DefaultPrismaticJointDef();
        jointDef.bodyIdA = myBodyA.body;
        jointDef.bodyIdB = myBodyB.body;
        jointDef.localAnchorA = b2Body_GetLocalPoint(myBodyA.body, worldPivot);
        jointDef.localAnchorB = b2Body_GetLocalPoint(myBodyB.body, worldPivot);
        jointDef.localAxisA = b2Body_GetLocalVector(myBodyA.body, worldAxis);
        jointDef.userData = reinterpret_cast<void*>((uint64_t)entity);

        joint.joint = b2CreatePrismaticJoint(world2D, &jointDef);
        joint.type = Joint2DType::PRISMATIC;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadMouseJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 target){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<Rigidbody2DComponent>()) && signatureB.test(scene->getComponentId<Rigidbody2DComponent>())){

        if (!loadRigidbody2D(bodyA) || !loadRigidbody2D(bodyB)){
            Log::error("Cannot create mouse 2D joint, bodyA or bodyB could not be loaded");
            return false;
        }

        Rigidbody2DComponent myBodyA = scene->getComponent<Rigidbody2DComponent>(bodyA);
        Rigidbody2DComponent myBodyB = scene->getComponent<Rigidbody2DComponent>(bodyB);

        if (myBodyA.type != BodyType::DYNAMIC){
            Log::error("Cannot create mouse 2D joint, bodyA must be dynamic");
            return false;
        }

        if (bodyA == bodyB){
            Log::error("Cannot create mouse 2D joint with the same bodyA and bodyB");
            return false;
        }

        updateRigidbody2DPosition(signatureA, bodyA, myBodyA);
        updateRigidbody2DPosition(signatureB, bodyB, myBodyB);

        if (!b2Body_IsValid(myBodyA.body) || !b2Body_IsValid(myBodyB.body)){
            Log::error("Cannot create mouse 2D joint, bodyA or bodyB has no valid physics body");
            return false;
        }

        b2Vec2 worldTarget = {target.x / pointsToMeterScale2D, target.y / pointsToMeterScale2D};

        b2MouseJointDef jointDef = b2DefaultMouseJointDef();
        jointDef.bodyIdA = myBodyB.body;
        jointDef.bodyIdB = myBodyA.body;
        jointDef.target = worldTarget;
        jointDef.maxForce = std::max(10.0f, 1000.0f * b2Body_GetMass(myBodyA.body));
        jointDef.userData = reinterpret_cast<void*>((uint64_t)entity);

        joint.joint = b2CreateMouseJoint(world2D, &jointDef);
        if (!b2Joint_IsValid(joint.joint)){
            Log::error("Cannot create mouse 2D joint");
            return false;
        }
        joint.type = Joint2DType::MOUSE;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadWheelJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor, Vector2 axis){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<Rigidbody2DComponent>()) && signatureB.test(scene->getComponentId<Rigidbody2DComponent>())){

        Rigidbody2DComponent myBodyA = scene->getComponent<Rigidbody2DComponent>(bodyA);
        Rigidbody2DComponent myBodyB = scene->getComponent<Rigidbody2DComponent>(bodyB);

        updateRigidbody2DPosition(signatureA, bodyA, myBodyA);
        updateRigidbody2DPosition(signatureB, bodyB, myBodyB);

        b2Vec2 worldPivot = {anchor.x / pointsToMeterScale2D, anchor.y / pointsToMeterScale2D};
        float axisLengthSq = axis.x * axis.x + axis.y * axis.y;
        if (axisLengthSq < 1.0e-6f){
            Log::warn("Wheel 2D joint axis is zero. Using default axis (1, 0).");
            axis = Vector2(1.0f, 0.0f);
        }
        b2Vec2 worldAxis = {axis.x, axis.y};

        b2WheelJointDef jointDef = b2DefaultWheelJointDef();
        jointDef.bodyIdA = myBodyA.body;
        jointDef.bodyIdB = myBodyB.body;
        jointDef.localAnchorA = b2Body_GetLocalPoint(myBodyA.body, worldPivot);
        jointDef.localAnchorB = b2Body_GetLocalPoint(myBodyB.body, worldPivot);
        jointDef.localAxisA = b2Body_GetLocalVector(myBodyA.body, worldAxis);
        jointDef.userData = reinterpret_cast<void*>((uint64_t)entity);

        joint.joint = b2CreateWheelJoint(world2D, &jointDef);
        joint.type = Joint2DType::WHEEL;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadWeldJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<Rigidbody2DComponent>()) && signatureB.test(scene->getComponentId<Rigidbody2DComponent>())){

        Rigidbody2DComponent myBodyA = scene->getComponent<Rigidbody2DComponent>(bodyA);
        Rigidbody2DComponent myBodyB = scene->getComponent<Rigidbody2DComponent>(bodyB);

        updateRigidbody2DPosition(signatureA, bodyA, myBodyA);
        updateRigidbody2DPosition(signatureB, bodyB, myBodyB);

        b2Vec2 worldPivot = {anchor.x / pointsToMeterScale2D, anchor.y / pointsToMeterScale2D};

        b2WeldJointDef jointDef = b2DefaultWeldJointDef();
        jointDef.bodyIdA = myBodyA.body;
        jointDef.bodyIdB = myBodyB.body;
        jointDef.localAnchorA = b2Body_GetLocalPoint(myBodyA.body, worldPivot);
        jointDef.localAnchorB = b2Body_GetLocalPoint(myBodyB.body, worldPivot);
        jointDef.userData = reinterpret_cast<void*>((uint64_t)entity);

        joint.joint = b2CreateWeldJoint(world2D, &jointDef);
        joint.type = Joint2DType::WELD;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadMotorJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<Rigidbody2DComponent>()) && signatureB.test(scene->getComponentId<Rigidbody2DComponent>())){

        Rigidbody2DComponent myBodyA = scene->getComponent<Rigidbody2DComponent>(bodyA);
        Rigidbody2DComponent myBodyB = scene->getComponent<Rigidbody2DComponent>(bodyB);

        updateRigidbody2DPosition(signatureA, bodyA, myBodyA);
        updateRigidbody2DPosition(signatureB, bodyB, myBodyB);

        b2MotorJointDef jointDef = b2DefaultMotorJointDef();
        jointDef.bodyIdA = myBodyA.body;
        jointDef.bodyIdB = myBodyB.body;
        jointDef.userData = reinterpret_cast<void*>((uint64_t)entity);

        joint.joint = b2CreateMotorJoint(world2D, &jointDef);
        joint.type = Joint2DType::MOTOR;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

void PhysicsSystem::destroyJoint2D(Joint2DComponent& joint){
    if (b2Joint_IsValid(joint.joint)){
        b2DestroyJoint(joint.joint);
        joint.joint = b2_nullJointId;
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
bool PhysicsSystem::loadFixedJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        JPH::FixedConstraintSettings settings;
        settings.mAutoDetectPoint = true;

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create fixed 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::FIXED;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadDistanceJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchorA, Vector3 anchorB, bool autoAnchors){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create distance 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        if (autoAnchors){
            JPH::RVec3 bodyPositionA = lockBodyA->GetPosition();
            anchorA = Vector3((float)bodyPositionA.GetX(), (float)bodyPositionA.GetY(), (float)bodyPositionA.GetZ());
        }

        if (autoAnchors){
            JPH::RVec3 bodyPositionB = lockBodyB->GetPosition();
            anchorB = Vector3((float)bodyPositionB.GetX(), (float)bodyPositionB.GetY(), (float)bodyPositionB.GetZ());
        }

        JPH::DistanceConstraintSettings settings;
        settings.mPoint1 = JPH::RVec3(anchorA.x, anchorA.y, anchorA.z);
        settings.mPoint2 = JPH::RVec3(anchorB.x, anchorB.y, anchorB.z);

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::DISTANCE;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadPointJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        JPH::PointConstraintSettings settings;
        settings.mPoint1 = settings.mPoint2 = JPH::Vec3(anchor.x, anchor.y, anchor.z);

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create point 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::POINT;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadHingeJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor, Vector3 axis, Vector3 normal){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        float axisLengthSq = axis.x * axis.x + axis.y * axis.y + axis.z * axis.z;
        if (axisLengthSq < 1.0e-6f){
            Log::warn("Hinge 3D joint axis is zero. Using default axis (1, 0, 0).");
            axis = Vector3(1.0f, 0.0f, 0.0f);
        }else{
            float invLength = 1.0f / std::sqrt(axisLengthSq);
            axis = Vector3(axis.x * invLength, axis.y * invLength, axis.z * invLength);
        }

        float normalLengthSq = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
        if (normalLengthSq < 1.0e-6f){
            Log::warn("Hinge 3D joint normal is zero. Using default normal.");
            normal = (std::fabs(axis.y) < 0.999f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
        }

        float axisDotNormal = axis.x * normal.x + axis.y * normal.y + axis.z * normal.z;
        normal = Vector3(normal.x - axis.x * axisDotNormal, normal.y - axis.y * axisDotNormal, normal.z - axis.z * axisDotNormal);
        normalLengthSq = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;

        if (normalLengthSq < 1.0e-6f){
            normal = Vector3(-axis.z, 0.0f, axis.x);
            normalLengthSq = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
        }

        if (normalLengthSq < 1.0e-6f){
            normal = Vector3(0.0f, 0.0f, 1.0f);
        }else{
            float invNormalLength = 1.0f / std::sqrt(normalLengthSq);
            normal = Vector3(normal.x * invNormalLength, normal.y * invNormalLength, normal.z * invNormalLength);
        }

        JPH::HingeConstraintSettings settings;
        settings.mPoint1 = settings.mPoint2 = JPH::Vec3(anchor.x, anchor.y, anchor.z);
        settings.mHingeAxis1 = settings.mHingeAxis2 = JPH::Vec3(axis.x, axis.y, axis.z);
        settings.mNormalAxis1 = settings.mNormalAxis2 = JPH::Vec3(normal.x, normal.y, normal.z);

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create hinge 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::HINGE;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadConeJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor, Vector3 twistAxis){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        float axisLengthSq = twistAxis.x * twistAxis.x + twistAxis.y * twistAxis.y + twistAxis.z * twistAxis.z;
        if (axisLengthSq < 1.0e-6f){
            Log::warn("Cone 3D joint twist axis is zero. Using default axis (1, 0, 0).");
            twistAxis = Vector3(1.0f, 0.0f, 0.0f);
        }else{
            float invLength = 1.0f / std::sqrt(axisLengthSq);
            twistAxis = Vector3(twistAxis.x * invLength, twistAxis.y * invLength, twistAxis.z * invLength);
        }

        JPH::ConeConstraintSettings settings;
        settings.mPoint1 = settings.mPoint2 = JPH::Vec3(anchor.x, anchor.y, anchor.z);
        settings.mTwistAxis1 = settings.mTwistAxis2 = JPH::Vec3(twistAxis.x, twistAxis.y, twistAxis.z);

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create cone 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::CONE;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadPrismaticJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 sliderAxis, float limitsMin, float limitsMax){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        if (limitsMax < limitsMin){
            std::swap(limitsMin, limitsMax);
        }

        // Jolt's slider starts at position 0 and requires 0 to be inside [min, max].
        // Older scenes may serialize absolute-only positive/negative ranges (e.g. [1.8, 27.4]),
        // so normalize them to a valid interval while preserving range size.
        if (limitsMin > 0.0f){
            float offset = limitsMin;
            limitsMin -= offset;
            limitsMax -= offset;
            Log::warn("Prismatic 3D joint limits did not include 0. Normalized range to [%f, %f]", limitsMin, limitsMax);
        }else if (limitsMax < 0.0f){
            float offset = limitsMax;
            limitsMin -= offset;
            limitsMax -= offset;
            Log::warn("Prismatic 3D joint limits did not include 0. Normalized range to [%f, %f]", limitsMin, limitsMax);
        }

        if (std::fabs(limitsMax - limitsMin) < 1.0e-6f){
            Log::warn("Prismatic 3D joint with equal limits is unsupported by Jolt SliderConstraint. Using fixed joint instead.");
            return loadFixedJoint3D(joint, bodyA, bodyB);
        }

        float axisLengthSq = sliderAxis.x * sliderAxis.x + sliderAxis.y * sliderAxis.y + sliderAxis.z * sliderAxis.z;
        if (axisLengthSq < 1.0e-6f){
            Log::warn("Prismatic 3D joint axis is zero. Using default axis (1, 0, 0).");
            sliderAxis = Vector3(1.0f, 0.0f, 0.0f);
        }else{
            float invLength = 1.0f / std::sqrt(axisLengthSq);
            sliderAxis = Vector3(sliderAxis.x * invLength, sliderAxis.y * invLength, sliderAxis.z * invLength);
        }

        JPH::SliderConstraintSettings settings;
        settings.mAutoDetectPoint = true;
        settings.SetSliderAxis(JPH::Vec3(sliderAxis.x, sliderAxis.y, sliderAxis.z));
		settings.mLimitsMin = limitsMin;
		settings.mLimitsMax = limitsMax;

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create prismatic 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::PRISMATIC;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadSwingTwistJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchor, Vector3 twistAxis, Vector3 planeAxis, float normalHalfConeAngle, float planeHalfConeAngle, float twistMinAngle, float twistMaxAngle){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        float twistAxisLengthSq = twistAxis.x * twistAxis.x + twistAxis.y * twistAxis.y + twistAxis.z * twistAxis.z;
        if (twistAxisLengthSq < 1.0e-6f){
            Log::warn("SwingTwist 3D joint twist axis is zero. Using default axis (1, 0, 0).");
            twistAxis = Vector3(1.0f, 0.0f, 0.0f);
        }else{
            float invLength = 1.0f / std::sqrt(twistAxisLengthSq);
            twistAxis = Vector3(twistAxis.x * invLength, twistAxis.y * invLength, twistAxis.z * invLength);
        }

        float planeAxisLengthSq = planeAxis.x * planeAxis.x + planeAxis.y * planeAxis.y + planeAxis.z * planeAxis.z;
        if (planeAxisLengthSq < 1.0e-6f){
            Log::warn("SwingTwist 3D joint plane axis is zero. Using default axis.");
            planeAxis = (std::fabs(twistAxis.y) < 0.999f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
        }

        float twistDotPlane = twistAxis.x * planeAxis.x + twistAxis.y * planeAxis.y + twistAxis.z * planeAxis.z;
        planeAxis = Vector3(
            planeAxis.x - twistAxis.x * twistDotPlane,
            planeAxis.y - twistAxis.y * twistDotPlane,
            planeAxis.z - twistAxis.z * twistDotPlane
        );

        planeAxisLengthSq = planeAxis.x * planeAxis.x + planeAxis.y * planeAxis.y + planeAxis.z * planeAxis.z;
        if (planeAxisLengthSq < 1.0e-6f){
            planeAxis = Vector3(-twistAxis.z, 0.0f, twistAxis.x);
            planeAxisLengthSq = planeAxis.x * planeAxis.x + planeAxis.y * planeAxis.y + planeAxis.z * planeAxis.z;
        }

        if (planeAxisLengthSq < 1.0e-6f){
            planeAxis = Vector3(0.0f, 0.0f, 1.0f);
        }else{
            float invPlaneLength = 1.0f / std::sqrt(planeAxisLengthSq);
            planeAxis = Vector3(planeAxis.x * invPlaneLength, planeAxis.y * invPlaneLength, planeAxis.z * invPlaneLength);
        }

        if (twistMaxAngle < twistMinAngle){
            std::swap(twistMinAngle, twistMaxAngle);
        }

        constexpr float kMinConeAngle = 0.0f;
        constexpr float kMaxConeAngle = 179.9f;
        normalHalfConeAngle = std::clamp(normalHalfConeAngle, kMinConeAngle, kMaxConeAngle);
        planeHalfConeAngle = std::clamp(planeHalfConeAngle, kMinConeAngle, kMaxConeAngle);

        JPH::SwingTwistConstraintSettings settings;
        settings.mPosition1 = settings.mPosition2 = JPH::Vec3(anchor.x, anchor.y, anchor.z);
        settings.mTwistAxis1 = settings.mTwistAxis2 = JPH::Vec3(twistAxis.x, twistAxis.y, twistAxis.z);
        settings.mPlaneAxis1 = settings.mPlaneAxis2 = JPH::Vec3(planeAxis.x, planeAxis.y, planeAxis.z);
        settings.mNormalHalfConeAngle = Angle::defaultToRad(normalHalfConeAngle);
        settings.mPlaneHalfConeAngle = Angle::defaultToRad(planeHalfConeAngle);
        settings.mTwistMinAngle = Angle::defaultToRad(twistMinAngle);
        settings.mTwistMaxAngle = Angle::defaultToRad(twistMaxAngle);

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create swing twist 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::SWINGTWIST;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadSixDOFJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchorA, Vector3 anchorB, Vector3 axisX, Vector3 axisY){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        float axisXLengthSq = axisX.x * axisX.x + axisX.y * axisX.y + axisX.z * axisX.z;
        if (axisXLengthSq < 1.0e-6f){
            Log::warn("SixDOF 3D joint axisX is zero. Using default axis (1, 0, 0).");
            axisX = Vector3(1.0f, 0.0f, 0.0f);
        }else{
            float invLength = 1.0f / std::sqrt(axisXLengthSq);
            axisX = Vector3(axisX.x * invLength, axisX.y * invLength, axisX.z * invLength);
        }

        float axisYLengthSq = axisY.x * axisY.x + axisY.y * axisY.y + axisY.z * axisY.z;
        if (axisYLengthSq < 1.0e-6f){
            Log::warn("SixDOF 3D joint axisY is zero. Using default axis.");
            axisY = (std::fabs(axisX.y) < 0.999f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
        }

        float axisDot = axisX.x * axisY.x + axisX.y * axisY.y + axisX.z * axisY.z;
        axisY = Vector3(
            axisY.x - axisX.x * axisDot,
            axisY.y - axisX.y * axisDot,
            axisY.z - axisX.z * axisDot
        );

        axisYLengthSq = axisY.x * axisY.x + axisY.y * axisY.y + axisY.z * axisY.z;
        if (axisYLengthSq < 1.0e-6f){
            axisY = Vector3(-axisX.z, 0.0f, axisX.x);
            axisYLengthSq = axisY.x * axisY.x + axisY.y * axisY.y + axisY.z * axisY.z;
        }

        if (axisYLengthSq < 1.0e-6f){
            axisY = Vector3(0.0f, 0.0f, 1.0f);
        }else{
            float invAxisYLength = 1.0f / std::sqrt(axisYLengthSq);
            axisY = Vector3(axisY.x * invAxisYLength, axisY.y * invAxisYLength, axisY.z * invAxisYLength);
        }

        JPH::SixDOFConstraintSettings settings;
        settings.mPosition1 = JPH::Vec3(anchorA.x, anchorA.y, anchorA.z);
        settings.mPosition2 = JPH::Vec3(anchorB.x, anchorB.y, anchorB.z);
        settings.mAxisX1 = settings.mAxisX2 = JPH::Vec3(axisX.x, axisX.y, axisX.z);
        settings.mAxisY1 = settings.mAxisY2 = JPH::Vec3(axisY.x, axisY.y, axisY.z);

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create six dof 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::SIXDOF;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadPathJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, std::vector<Vector3> positions, std::vector<Vector3> tangents, std::vector<Vector3> normals, Vector3 pathPosition, bool isLooping){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        JPH::Ref<JPH::PathConstraintPathHermite> path = new JPH::PathConstraintPathHermite;

        if (positions.size() != normals.size() || positions.size() != tangents.size()){
            Log::error("Cannot create joint, positions size is different from normals and tangents");
            return false;
        }

        for (int i = 0; i < positions.size(); i++){
            path->AddPoint(JPH::Vec3(positions[i].x, positions[i].y, positions[i].z), JPH::Vec3(tangents[i].x, tangents[i].y, tangents[i].z), JPH::Vec3(normals[i].x, normals[i].y, normals[i].z));
        }
        path->SetIsLooping(isLooping);

        JPH::PathConstraintSettings settings;
        settings.mPath = path;
        settings.mPathPosition = JPH::Vec3(pathPosition.x, pathPosition.y, pathPosition.z);

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create path 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::PATH;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadGearJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Entity hingeA, Entity hingeB, int numTeethGearA, int numTeethGearB){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);
    Signature signatureHingeA = scene->getSignature(hingeA);
    Signature signatureHingeB = scene->getSignature(hingeB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        if (signatureHingeA.test(scene->getComponentId<Joint3DComponent>()) && signatureHingeB.test(scene->getComponentId<Joint3DComponent>())){

            Joint3DComponent myHingeA = scene->getComponent<Joint3DComponent>(hingeA);
            Joint3DComponent myHingeB = scene->getComponent<Joint3DComponent>(hingeB);

            if ((myHingeA.type != Joint3DType::HINGE) || (myHingeB.type != Joint3DType::HINGE)){
                Log::error("Cannot create joint, hingeA or hingeB is not hinge type");
                return false;
            }

            const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
            JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
            JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

            JPH::Body* lockBodyA = lock.GetBody(0);
            JPH::Body* lockBodyB = lock.GetBody(1);
            if (!lockBodyA || !lockBodyB){
                Log::error("Cannot create gear 3D joint, bodyA or bodyB is invalid");
                return false;
            }

            // Disable collision between gears
            JPH::Ref<JPH::GroupFilterTable> group_filter = new JPH::GroupFilterTable(2);
            group_filter->DisableCollision(0, 1);
            lockBodyA->SetCollisionGroup(JPH::CollisionGroup(group_filter, 0, 0));
            lockBodyB->SetCollisionGroup(JPH::CollisionGroup(group_filter, 0, 1));

            JPH::HingeConstraintSettings* hingeSetA = (JPH::HingeConstraintSettings*)((JPH::HingeConstraint*)myHingeA.joint)->GetConstraintSettings().GetPtr();
            JPH::HingeConstraintSettings* hingeSetB = (JPH::HingeConstraintSettings*)((JPH::HingeConstraint*)myHingeB.joint)->GetConstraintSettings().GetPtr();

            if (numTeethGearA <= 0 || numTeethGearB <= 0){
                Log::warn("Gear 3D joint received non-positive teeth count. Clamping to at least 1.");
                numTeethGearA = std::max(1, numTeethGearA);
                numTeethGearB = std::max(1, numTeethGearB);
            }

            JPH::GearConstraintSettings settings;
            settings.mHingeAxis1 = hingeSetA->mHingeAxis1;
            settings.mHingeAxis2 = hingeSetB->mHingeAxis1;
            settings.SetRatio(numTeethGearA, numTeethGearB);

            joint.joint = settings.Create(*lockBodyA, *lockBodyB);

            ((JPH::GearConstraint *)joint.joint)->SetConstraints(myHingeA.joint, myHingeB.joint);
            world3D.AddConstraint(joint.joint);
            joint.type = Joint3DType::GEAR;
        }else{
            Log::error("Cannot create joint, error in hingeA or hingeB");
            return false;
        }

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadRackAndPinionJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Entity hinge, Entity slider, int numTeethRack, int numTeethGear, int rackLength){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);
    Signature signatureHinge = scene->getSignature(hinge);
    Signature signatureSlider = scene->getSignature(slider);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        if (signatureHinge.test(scene->getComponentId<Joint3DComponent>()) && signatureSlider.test(scene->getComponentId<Joint3DComponent>())){

            Joint3DComponent myHinge = scene->getComponent<Joint3DComponent>(hinge);
            Joint3DComponent mySlider = scene->getComponent<Joint3DComponent>(slider);

            if ((myHinge.type != Joint3DType::HINGE) || (mySlider.type != Joint3DType::PRISMATIC)){
                Log::error("Cannot create joint, hinge or slider is not hinge and prismatic (slider) types");
                return false;
            }

            const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
            JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
            JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

            JPH::Body* lockBodyA = lock.GetBody(0);
            JPH::Body* lockBodyB = lock.GetBody(1);
            if (!lockBodyA || !lockBodyB){
                Log::error("Cannot create rack and pinion 3D joint, bodyA or bodyB is invalid");
                return false;
            }

            // Disable collision between gears
            JPH::Ref<JPH::GroupFilterTable> group_filter = new JPH::GroupFilterTable(2);
            group_filter->DisableCollision(0, 1);
            lockBodyA->SetCollisionGroup(JPH::CollisionGroup(group_filter, 0, 0));
            lockBodyB->SetCollisionGroup(JPH::CollisionGroup(group_filter, 0, 1));

            JPH::HingeConstraintSettings* hingeSetA = (JPH::HingeConstraintSettings*)((JPH::HingeConstraint*)myHinge.joint)->GetConstraintSettings().GetPtr();
            JPH::SliderConstraintSettings* sliderSetB = (JPH::SliderConstraintSettings*)((JPH::SliderConstraint*)mySlider.joint)->GetConstraintSettings().GetPtr();

            if (numTeethRack <= 0 || numTeethGear <= 0 || rackLength <= 0){
                Log::warn("RackAndPinion 3D joint received non-positive ratio inputs. Clamping to at least 1.");
                numTeethRack = std::max(1, numTeethRack);
                numTeethGear = std::max(1, numTeethGear);
                rackLength = std::max(1, rackLength);
            }

            JPH::RackAndPinionConstraintSettings settings;
            settings.mHingeAxis = hingeSetA->mHingeAxis1;
            settings.mSliderAxis = sliderSetB->mSliderAxis2;
            settings.SetRatio(numTeethRack, rackLength, numTeethGear);

            joint.joint = settings.Create(*lockBodyA, *lockBodyB);

            ((JPH::GearConstraint *)joint.joint)->SetConstraints(myHinge.joint, mySlider.joint);
            world3D.AddConstraint(joint.joint);
            joint.type = Joint3DType::RACKANDPINON;
        }else{
            Log::error("Cannot create joint, error in hinge or slider");
            return false;
        }

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

bool PhysicsSystem::loadPulleyJoint3D(Joint3DComponent& joint, Entity bodyA, Entity bodyB, Vector3 anchorA, Vector3 anchorB, Vector3 fixedPointA, Vector3 fixedPointB){
    Signature signatureA = scene->getSignature(bodyA);
    Signature signatureB = scene->getSignature(bodyB);

    if (signatureA.test(scene->getComponentId<RigidbodyComponent>()) && signatureB.test(scene->getComponentId<RigidbodyComponent>())){

        RigidbodyComponent myBodyA = scene->getComponent<RigidbodyComponent>(bodyA);
        RigidbodyComponent myBodyB = scene->getComponent<RigidbodyComponent>(bodyB);

        updateRigidbodyPosition(signatureA, bodyA, myBodyA);
        updateRigidbodyPosition(signatureB, bodyB, myBodyB);

        JPH::PulleyConstraintSettings settings;
        settings.mBodyPoint1 = JPH::Vec3(anchorA.x, anchorA.y, anchorA.z);
        settings.mBodyPoint2 = JPH::Vec3(anchorB.x, anchorB.y, anchorB.z);
        settings.mFixedPoint1 = JPH::Vec3(fixedPointA.x, fixedPointA.y, fixedPointA.z);
        settings.mFixedPoint2 = JPH::Vec3(fixedPointB.x, fixedPointB.y, fixedPointB.z);

        const JPH::BodyLockInterface& bodyLockInterface = getBodyLockInterface3D();
        JPH::BodyID bodies[2] = {myBodyA.body, myBodyB.body};
        JPH::BodyLockMultiWrite lock(bodyLockInterface, bodies, 2);

        JPH::Body* lockBodyA = lock.GetBody(0);
        JPH::Body* lockBodyB = lock.GetBody(1);
        if (!lockBodyA || !lockBodyB){
            Log::error("Cannot create pulley 3D joint, bodyA or bodyB is invalid");
            return false;
        }

        joint.joint = settings.Create(*lockBodyA, *lockBodyB);

        world3D.AddConstraint(joint.joint);
        joint.type = Joint3DType::PULLEY;

    }else{
        Log::error("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    return true;
}

void PhysicsSystem::destroyJoint3D(Joint3DComponent& joint){
    if (joint.joint){
        // Parked like a body, the Ref keeping the constraint alive until the flush.
        if (steppingWorld3D){
            pendingJointDestroy3D.push_back(joint.joint);
        }else{
            world3D.RemoveConstraint(joint.joint);
        }

        joint.joint = NULL;
    }
}

void PhysicsSystem::addBroadPhaseLayer3D(uint8_t index, uint32_t groupsToInclude){
    addBroadPhaseLayer3D(index, groupsToInclude, 0);
}

void PhysicsSystem::addBroadPhaseLayer3D(uint8_t index, uint32_t groupsToInclude, uint32_t groupsToExclude){
    if (index < MAX_BROADPHASELAYER_3D){
        broad_phase_layer_interface->ConfigureLayer(JPH::BroadPhaseLayer(index), groupsToInclude, groupsToExclude);

        auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
        if (bodies3d->size() > 0){
            Log::warn("BroadPhaseLayers should be add before any 3D body creation");
        }
    }else{
        Log::error("BroadPhaseLayer index should be from 0 to %i, increase MAX_BROADPHASELAYER_3D to more layers", (MAX_BROADPHASELAYER_3D-1));
    }
}
#endif

void PhysicsSystem::load(){
    // A newly loaded scene can receive a variable-timestep gameplay update before
    // the fixed-step accumulator has enough time for its first physics update.
    // Materialize physics objects now so Rigidbody2D/Rigidbody APIs are valid from the
    // first gameplay callback, but do not advance either physics world here.
#ifdef JASPERENGINE_PHYSICS_2D
    auto bodies2d = scene->getComponentArray<Rigidbody2DComponent>();
    for (size_t i = 0; i < bodies2d->size(); i++){
        Rigidbody2DComponent& body = bodies2d->getComponentFromIndex(i);
        Entity entity = bodies2d->getEntity(i);

        if (loadRigidbody2D(entity) && b2Body_IsValid(body.body)){
            updateRigidbody2DPosition(scene->getSignature(entity), entity, body);
        }
    }

    auto joints2d = scene->getComponentArray<Joint2DComponent>();
    for (size_t i = 0; i < joints2d->size(); i++){
        Joint2DComponent& joint = joints2d->getComponentFromIndex(i);
        if ((joint.needUpdateJoint || !b2Joint_IsValid(joint.joint))
            && loadJoint2D(joints2d->getEntity(i), joint)){
            joint.needUpdateJoint = false;
        }
    }
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
    for (size_t i = 0; i < bodies3d->size(); i++){
        RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
        Entity entity = bodies3d->getEntity(i);

        if (loadRigidbody(entity) && !body.body.IsInvalid()){
            updateRigidbodyPosition(scene->getSignature(entity), entity, body);
        }
    }

    auto joints3d = scene->getComponentArray<Joint3DComponent>();
    for (size_t i = 0; i < joints3d->size(); i++){
        Joint3DComponent& joint = joints3d->getComponentFromIndex(i);
        if ((joint.needUpdateJoint || !joint.joint)
            && loadJoint3D(joints3d->getEntity(i), joint)){
            joint.needUpdateJoint = false;
        }
    }
#endif
}

void PhysicsSystem::destroy(){

}

void PhysicsSystem::update(double dt){
    // Variable-timestep update: physics intentionally does no work here.
    // All deterministic stepping happens in fixedUpdate(), driven by Engine's accumulator.
    (void)dt;
}

void PhysicsSystem::fixedUpdate(double dt){
    if (paused) {
        return;
    }

    const float fixedStep = (float)dt;

#ifdef JASPERENGINE_PHYSICS_2D
	auto bodies2d = scene->getComponentArray<Rigidbody2DComponent>();

	for (int i = 0; i < bodies2d->size(); i++){
		Rigidbody2DComponent& body = bodies2d->getComponentFromIndex(i);
		Entity entity = bodies2d->getEntity(i);
		Signature signature = scene->getSignature(entity);

        if (!b2Body_IsValid(body.body) || body.needReloadBody || body.needUpdateColliders || entityWorldScale2D(scene, entity) != body.loadedScale){
            loadRigidbody2D(entity);
        }

        if (b2Body_IsValid(body.body)){
            // a disabled Rigidbody 2D or inactive GameObject leaves the simulation
            // (Box2D keeps its state and contacts are dropped) until it is enabled again
            const bool simulate = body.enabled && scene->isEntityActiveInHierarchy(entity);
            if (b2Body_IsEnabled(body.body) != simulate){
                if (simulate){
                    b2Body_Enable(body.body);
                }else{
                    b2Body_Disable(body.body);
                }
            }

            if (simulate){
                updateRigidbody2DPosition(signature, entity, body, fixedStep);

                body.newBody = false;
            }
        }
    }
    auto joints2d = scene->getComponentArray<Joint2DComponent>();
    for (int i = 0; i < joints2d->size(); i++){
        Joint2DComponent& joint = joints2d->getComponentFromIndex(i);
        Entity entity = joints2d->getEntity(i);

        // a disabled joint, or one on or between inactive GameObjects, is removed
        // from the world and rebuilt when everything is active again
        if (!joint.enabled || !scene->isEntityActiveInHierarchy(entity) ||
                (joint.bodyA != NULL_ENTITY && !scene->isEntityActiveInHierarchy(joint.bodyA)) ||
                (joint.bodyB != NULL_ENTITY && !scene->isEntityActiveInHierarchy(joint.bodyB))){
            if (b2Joint_IsValid(joint.joint)){
                destroyJoint2D(joint);
            }
            joint.needUpdateJoint = true;
            continue;
        }

        if (joint.needUpdateJoint || !b2Joint_IsValid(joint.joint)){
            if (loadJoint2D(entity, joint)){
                joint.needUpdateJoint = false;
            }
        }
    }

    // With no 2D bodies the world was never created (lazy): skip all Box2D stepping/events.
    if (b2World_IsValid(world2D)){
        syncLayerCollisionMatrix2D();

        if (bodies2d->size() > 0){
            int32_t subSteps = 4;
            steppingWorld2D = true;
            {
                // what the PreSolve and filter callbacks destroy goes after the step
                StructuralDeferScope deferScope(*scene);
                b2World_Step(world2D, fixedStep, subSteps);
            }
            steppingWorld2D = false;
        }

        // layers changed by callbacks inside the step
        for (size_t i = 0; i < pendingLayerRefresh2D.size(); i++){
            refreshRigidbody2DLayer(pendingLayerRefresh2D[i]);
        }
        pendingLayerRefresh2D.clear();

        b2BodyEvents events = b2World_GetBodyEvents(world2D);
        for (int i = 0; i < events.moveCount; ++i){
            const b2BodyMoveEvent* event = events.moveEvents + i;

            Entity entity = reinterpret_cast<uintptr_t>(event->userData);
            Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
            if (!body){
                continue;
            }

            // a following body is on its way to its Transform, the last step of the frame puts it there
            if (Engine::getFixedStepsLeft() == 1){
                finishFollowing2D(entity, *body);
            }

            updateTransformFromRigidbody2D(entity, *body, 1.0f);
        }

        {
            // contact and sensor callbacks: what they destroy goes after the last of them
            StructuralDeferScope deferScope(*scene);
            Box2DAux::manageEvents(scene, world2D);
        }
    }

    // the 2D controllers ride what moved in the step and see what is under them now
    characters2D->postStep(fixedStep);
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    auto bodies3d = scene->getComponentArray<RigidbodyComponent>();

	for (int i = 0; i < bodies3d->size(); i++){
		RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
		Entity entity = bodies3d->getEntity(i);
		Signature signature = scene->getSignature(entity);

        if (body.body.IsInvalid() || body.needReloadBody || body.needUpdateColliders || entityWorldScale(scene, entity) != body.loadedScale){
            loadRigidbody(entity);
        }

        if (!body.body.IsInvalid()){
            // a disabled Rigidbody or inactive GameObject is taken out of the world
            // (the Jolt body and its state are kept) until it is enabled again
            const bool simulate = body.enabled && scene->isEntityActiveInHierarchy(entity);
            JPH::BodyInterface& bodyInterface = getBodyInterface3D();
            if (bodyInterface.IsAdded(body.body) != simulate){
                if (simulate){
                    bodyInterface.AddBody(body.body, body.type == BodyType::STATIC ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
                }else{
                    bodyInterface.RemoveBody(body.body);
                }
            }

            if (simulate){
                updateRigidbodyPosition(signature, entity, body, fixedStep);

                body.newBody = false;
            }
        }
    }
    auto joints3d = scene->getComponentArray<Joint3DComponent>();
    for (int i = 0; i < joints3d->size(); i++){
        Joint3DComponent& joint = joints3d->getComponentFromIndex(i);
        Entity entity = joints3d->getEntity(i);

        if (!joint.enabled || !scene->isEntityActiveInHierarchy(entity) ||
                (joint.bodyA != NULL_ENTITY && !scene->isEntityActiveInHierarchy(joint.bodyA)) ||
                (joint.bodyB != NULL_ENTITY && !scene->isEntityActiveInHierarchy(joint.bodyB))){
            if (joint.joint){
                destroyJoint3D(joint);
            }
            joint.needUpdateJoint = true;
            continue;
        }

        if (joint.needUpdateJoint || !joint.joint){
            if (loadJoint3D(entity, joint)){
                joint.needUpdateJoint = false;
            }
        }
    }

    syncLayerCollisionMatrix3D();

    // characters made and removed as their components and GameObjects come and go, and moved
    // where their Transforms were put
    characters3D->preStep();

    // the world steps for the characters too (their platforms, root motion, inner bodies)
    if (bodies3d->size() > 0 || characters3D->count() > 0){
		const int cCollisionSteps = 1;

		// Clears the flag and drains the teardown queue from the destructor, so a C++
		// subscriber throwing out of a callback leaves neither behind.
		struct StepScope {
			PhysicsSystem* system;
			explicit StepScope(PhysicsSystem* s): system(s) { s->steppingWorld3D = true; }
			~StepScope() { system->steppingWorld3D = false; system->flushPendingDestroys3D(); }
		};

		// the bodies floating in a library:water (Water.h)
		Water::applyBuoyancy(scene, fixedStep);
		// and the ones a Buoyancy floats in Water Volumes, by its columns (WaterSystem)
		water->applyBuoyancy(fixedStep);

		StepScope stepScope(this);
		// the contact listener runs on Jolt's threads, serialized by getCallbackMutex3D, while
		// this one waits: what its callbacks destroy goes after the step. Its contact and
		// activation events are queued and delivered after the step (deliverEvents3D); a
		// shouldCollide3D answers inside it, so with subscribers the step runs every job on this
		// thread (a Lua filter runs on the Lua thread only)
		StructuralDeferScope deferScope(*scene, true);
		JPH::JobSystem* jobs = shouldCollide3D.hasSubscribers() ? &JoltShapeBuilder::callingThreadPhysicsJobSystem() : job_system;
		world3D.Update(fixedStep, cCollisionSteps, temp_allocator, jobs);
	}

    // layers and filter bits changed by callbacks inside the step
    for (size_t i = 0; i < pendingLayerRefresh3D.size(); i++){
        refreshRigidbodyFilter3D(pendingLayerRefresh3D[i]);
    }
    pendingLayerRefresh3D.clear();

	for (int i = 0; i < bodies3d->size(); i++){
		RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
		Entity entity = bodies3d->getEntity(i);

        // out of the world (disabled, inactive) it did not step and leaves its Transform alone
        if (body.body.IsInvalid() || !getBodyInterface3D().IsAdded(body.body)){
            continue;
        }

        // a following body is on its way to its Transform, the last step of the frame puts it there
        if (Engine::getFixedStepsLeft() == 1){
            finishFollowing3D(entity, body);
        }

        updateTransformFromRigidbody(entity, body, 1.0f);
    }

    {
        // the step's contact and activation events, on this thread, the bodies where it put
        // them (as 2D's after its step): what they destroy goes after the last of them
        StructuralDeferScope deferScope(*scene);
        deliverEvents3D();
    }

    // the characters ride what moved, see what is under them now and take their root motion
    characters3D->postStep(fixedStep);
#endif
}

void PhysicsSystem::interpolate(double alpha){
    if (paused || !interpolation){
        return;
    }

    // only bodies whose Transform is still where physics left it: one a script, an animation
    // or the editor moved since keeps that pose, the next step puts the body there
#ifdef JASPERENGINE_PHYSICS_2D
    auto bodies2d = scene->getComponentArray<Rigidbody2DComponent>();
    for (size_t i = 0; i < bodies2d->size(); i++){
        Rigidbody2DComponent& body = bodies2d->getComponentFromIndex(i);
        Entity entity = bodies2d->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);

        if (body.type != BodyType::STATIC && b2Body_IsValid(body.body) && b2Body_IsEnabled(body.body) &&
                transform && isSynced(*transform, body)){
            updateTransformFromRigidbody2D(entity, body, (float)alpha);
        }
    }
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
    for (size_t i = 0; i < bodies3d->size(); i++){
        RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
        Entity entity = bodies3d->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);

        if (body.type != BodyType::STATIC && !body.body.IsInvalid() && getBodyInterface3D().IsAdded(body.body) &&
                transform && isSynced(*transform, body)){
            updateTransformFromRigidbody(entity, body, (float)alpha);
        }
    }
#endif
}

void PhysicsSystem::draw(){

}

void PhysicsSystem::onComponentAdded(Entity entity, ComponentId componentId) {

}

void PhysicsSystem::onEntityLayerChanged(Entity entity) {
#ifdef JASPERENGINE_PHYSICS_2D
    refreshRigidbody2DLayer(entity);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    refreshRigidbodyFilter3D(entity);
    characters3D->refreshLayer(entity);
#endif
}

void PhysicsSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
#ifdef JASPERENGINE_PHYSICS_2D
	if (componentId == scene->getComponentId<Rigidbody2DComponent>()) {
		Rigidbody2DComponent& rigidbody2d = scene->getComponent<Rigidbody2DComponent>(entity);
		destroyRigidbody2D(rigidbody2d);
	} else if (componentId == scene->getComponentId<Joint2DComponent>()) {
		Joint2DComponent& joint2d = scene->getComponent<Joint2DComponent>(entity);
		destroyJoint2D(joint2d);
	} else if (componentId == scene->getComponentId<CharacterController2DComponent>()) {
		characters2D->remove(entity);
	}
#endif
#ifdef JASPERENGINE_PHYSICS_3D
	if (componentId == scene->getComponentId<RigidbodyComponent>()) {
		RigidbodyComponent& rigidbody = scene->getComponent<RigidbodyComponent>(entity);
		destroyRigidbody(rigidbody);
		warnedNonNormalizedRotations.erase(entity);
		reportedInvalidRotations.erase(entity);
	} else if (componentId == scene->getComponentId<Joint3DComponent>()) {
		Joint3DComponent& joint3d = scene->getComponent<Joint3DComponent>(entity);
		destroyJoint3D(joint3d);
	} else if (componentId == scene->getComponentId<CharacterControllerComponent>()) {
		characters3D->remove(entity);
	}
#endif
}

#ifdef JASPERENGINE_PHYSICS_3D
int PhysicsSystem::moveCharacter(Entity entity, const Vector3& motion){
    return characters3D->move(entity, motion);
}

bool PhysicsSystem::simpleMoveCharacter(Entity entity, const Vector3& speed){
    return characters3D->simpleMove(entity, speed);
}

bool PhysicsSystem::trySetCharacterSize(Entity entity, float height, float radius, const Vector3& center){
    return characters3D->trySetSize(entity, height, radius, center);
}

void PhysicsSystem::setRootMotion(Entity entity, const Vector3& worldVelocity, float yawRate){
    characters3D->setRootMotion(entity, worldVelocity, yawRate);
}

bool PhysicsSystem::isCharacterInnerBody(const JPH::BodyID& body) const{
    return characters3D && characters3D->isInnerBody(body);
}
#endif

WaterSample PhysicsSystem::sampleWater(const Vector3& point) const{
    return water->sample(point);
}

float PhysicsSystem::sampleWaterHeight(float x, float z) const{
    return water->sampleHeight(x, z);
}
