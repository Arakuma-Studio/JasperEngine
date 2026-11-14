// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsSystem.h"
#include "terrain/TerrainEditModule.h"
#if JASPERENGINE_TERRAIN_EDIT
#include "physics/JoltTerrainRefresh.h"
#endif
#include "component/Joint3DComponent.h"
#include "component/Transform.h"
#include "Scene.h"
#include "physics/JoltShapeBuilder.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/WaterSystem.h"
#include "object/environment/Water.h"
#include "util/Angle.h"
#include "component/CameraComponent.h"
#include "component/SpriteComponent.h"
#include "sprite/SpriteFrameGeometry.h"
#include "physics/AttachedColliders.h"

#ifdef JASPERENGINE_PHYSICS_2D
#include "util/Box2DAux.h"
#include "subsystem/CharacterController2DSystem.h"
#include "object/physics/Joint2D.h"
#include "physics/PolygonDecomposition2D.h"
#include "pool/PhysicsMaterialPool.h"
#include "script/ScriptMessages.h"
#include "lua.hpp"
#include "LuaBridge.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "subsystem/CharacterControllerSystem.h"
#include "subsystem/VehicleSystem.h"
#include "util/JoltPhysicsAux.h"
#include "util/MeshTriangles.h"
#include "component/RagdollComponent.h"
#include "pool/PhysicsMaterialPool.h"
#include "physics/PhysicsMaterial.h"
#endif
#include "script/ScriptMessages.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>


using namespace jasperengine;

namespace {

#if defined(JASPERENGINE_PHYSICS_2D) || defined(JASPERENGINE_PHYSICS_3D)
    // Transform::worldScale is the entity's accumulated scale, resolved by
    // RenderSystem::updateTransform. Identity when there is no Transform, matching an
    // unscaled body.
    Vector3 entityWorldScale(Scene* scene, Entity entity){
        return JoltShapeBuilder::entityWorldScale(scene, entity);
    }
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    Vector2 entityWorldScale2D(Scene* scene, Entity entity){
        const Vector3 scale = entityWorldScale(scene, entity);
        return Vector2(scale.x, scale.y);
    }
#endif

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

#ifdef JASPERENGINE_PHYSICS_3D
namespace {
    struct WorldCapacity3D{
        unsigned int maxBodies = 8192;
        unsigned int maxBodyPairs = 8192;
        unsigned int maxContactConstraints = 8192;
    };

    WorldCapacity3D& worldCapacity3D(){
        static WorldCapacity3D capacity;
        return capacity;
    }
}

void PhysicsSystem::setWorldCapacity3D(unsigned int maxBodies, unsigned int maxBodyPairs, unsigned int maxContactConstraints){
    // Jolt needs room for a few bodies and pairs; its body ids hold 23 bits of index
    WorldCapacity3D& capacity = worldCapacity3D();
    capacity.maxBodies = std::min(std::max(maxBodies, 16u), (unsigned int)JPH::BodyID::cMaxBodyIndex);
    capacity.maxBodyPairs = std::max(maxBodyPairs, 16u);
    capacity.maxContactConstraints = std::max(maxContactConstraints, 16u);
}

unsigned int PhysicsSystem::getDefaultMaxBodies3D(){
    return worldCapacity3D().maxBodies;
}

unsigned int PhysicsSystem::getDefaultMaxBodyPairs3D(){
    return worldCapacity3D().maxBodyPairs;
}

unsigned int PhysicsSystem::getDefaultMaxContactConstraints3D(){
    return worldCapacity3D().maxContactConstraints;
}

JoltPairFilter3D& PhysicsSystem::getPairFilter3D(){
    return *pairFilter3D;
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
	declareHookType(scene->getComponentId<RagdollComponent>());
#endif

	this->scene = scene;
	water = std::make_unique<WaterSystem>(scene, this);

    // the contact callbacks follow the scripts that define them
    ScriptMessages::addReceiversChangedListener(&PhysicsSystem::onScriptReceiversChanged);

    // Default gravity comes from SceneSettings, the single source of truth for scene-setting
    // defaults. The live gravity value is then owned/mutated here (pushed to the Box2D/Jolt
    // worlds); serialization and the editor overwrite it via setGravity2D/3D.
    SceneSettings settingsDefaults;
    this->gravity2D = settingsDefaults.gravity2D;
    this->gravity3D = settingsDefaults.gravity3D;
    this->interpolation = settingsDefaults.physicsInterpolation;
    this->worldSettings2D = settingsDefaults.physics2DWorld;
    this->worldSettings3D = settingsDefaults.physics3DWorld;
#ifdef JASPERENGINE_PHYSICS_2D
    this->subSteps2D = settingsDefaults.physics2DSubSteps;
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    this->pointsToMeterScale2D = worldSettings2D.pointsPerMeter;
    characters2D = std::make_unique<CharacterController2DSystem>(scene, this);

    // The Box2D world is created lazily (see ensureWorld2D): Box2D's global b2_worlds[] pool is
    // capped at B2_MAX_WORLDS (128) and is not thread-safe. Editor preview/thumbnail/gizmo scenes
    // (and pure-3D scenes) have no 2D bodies, so they must not consume a world slot.
    world2D = b2_nullWorldId;

    Physics2D::getLayerCollisionMatrix().getRows(appliedLayerRows2D);
    appliedLayerVersion2D = Physics2D::getLayerCollisionMatrix().getVersion();
    contacts2D = makeContactState2D();
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    // https://github.com/jrouwe/JoltPhysics/issues/244
    // Jolt globals are initialized once per process (see helper).
    JoltShapeBuilder::ensureJoltGlobalInit();

    // the project setting physics3DWorld at the time the scene is made (editor and exports alike)
    maxBodies3D = worldCapacity3D().maxBodies;
    maxBodyPairs3D = worldCapacity3D().maxBodyPairs;
    maxContactConstraints3D = worldCapacity3D().maxContactConstraints;
    objectLayerTable3D = new JoltObjectLayerTable();
    broad_phase_layer_interface = new JoltBroadPhaseLayers(MAX_BROADPHASELAYER_3D, *objectLayerTable3D);
    object_vs_broadphase_layer_filter = new JoltObjectVsBroadPhaseLayerFilter(*broad_phase_layer_interface);
    object_vs_object_layer_filter = new JoltObjectLayerPairFilter(*objectLayerTable3D, Physics::getLayerCollisionMatrix());

    // Bodies made from now on read the live matrix; only later changes need waking bodies
    Physics::getLayerCollisionMatrix().getRows(appliedLayerRows3D);
    appliedLayerVersion3D = Physics::getLayerCollisionMatrix().getVersion();

    pairFilter3D = new JoltPairFilter3D();
    pairFilter3D->AddRef();

    // Shared across all scenes (created once); physics updates are sequential on the main thread.
    temp_allocator = &JoltShapeBuilder::sharedPhysicsTempAllocator();
    job_system = &JoltShapeBuilder::sharedPhysicsJobSystem();

    contactRecorder3D = new ContactRecorder3D();
    rawContacts3D = new std::vector<RawContact3D>();
    activationListener3D = new JoltActivationListener(scene, this, contactRecorder3D);
    contactListener3D = new JoltContactListener(scene, this, contactRecorder3D);

    lock3DBodies = true;

    characters3D = std::make_unique<CharacterControllerSystem>(scene, this);
    vehicles3D = std::make_unique<VehicleSystem>(scene, this);
    // the Jolt world itself: ensureWorld3D, when a body, a query or a CharacterController
    // first needs it
#endif
}

#ifdef JASPERENGINE_PHYSICS_3D
void PhysicsSystem::ensureWorld3D(){
    if (world3DReady){
        return;
    }
    world3DReady = true;
    const unsigned int cNumBodyMutexes = 0;
    world3D.Init(maxBodies3D, cNumBodyMutexes, maxBodyPairs3D, maxContactConstraints3D, *broad_phase_layer_interface, *object_vs_broadphase_layer_filter, *object_vs_object_layer_filter);
    world3D.SetGravity(JPH::Vec3(this->gravity3D.x, this->gravity3D.y, this->gravity3D.z));
    // the scene's solver settings (S96 WP-E; at their defaults, Jolt's own)
    JPH::PhysicsSettings joltSettings = world3D.GetPhysicsSettings();
    applyPhysicsWorldSettings(worldSettings3D, joltSettings);
    world3D.SetPhysicsSettings(joltSettings);
    worldSettings3DDirty = false;

    // per-collider materials and their combine modes; colliders without one combine as Jolt
    // always did (geometric mean friction, maximum restitution)
    world3D.SetCombineFriction(&combineFriction3D);
    world3D.SetCombineRestitution(&combineRestitution3D);

    world3D.SetBodyActivationListener(activationListener3D);
    world3D.SetContactListener(contactListener3D);
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
void PhysicsSystem::ensureWorld2D(){
    if (b2World_IsValid(world2D)){
        return;
    }

    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = {this->gravity2D.x, this->gravity2D.y};
    // the scene's rotation cap and tuning (S96 WP-E; at their defaults, Box2D's own)
    applyPhysicsWorldSettings(worldSettings2D, worldDef);
    world2D = b2CreateWorld(&worldDef);
    if (b2World_IsValid(world2D) && !worldSettings2D.warmStarting){
        b2World_EnableWarmStarting(world2D, false);
    }
    worldSettings2DDirty = false;
    configuredJoints2D.clear();

    if (!b2World_IsValid(world2D)){
        Log::error("Could not create Box2D world: the global world pool is exhausted");
        return;
    }

    // shapes made from now on take the live matrix
    Physics2D::getLayerCollisionMatrix().getRows(appliedLayerRows2D);
    appliedLayerVersion2D = Physics2D::getLayerCollisionMatrix().getVersion();

    b2World_SetPreSolveCallback(world2D, Box2DAux::PreSolve, scene);
    b2World_SetCustomFilterCallback(world2D, Box2DAux::CollisionFilter, scene);
    // per-collider combine modes of the materials; two colliders without one combine as
    // before (geometric mean friction, maximum restitution)
    b2World_SetFrictionCallback(world2D, Box2DAux::CombineFriction);
    b2World_SetRestitutionCallback(world2D, Box2DAux::CombineRestitution);
}

b2BodyId PhysicsSystem::getGroundBody2D(){
    ensureWorld2D();
    if (!b2World_IsValid(world2D)){
        return b2_nullBodyId;
    }
    if (!b2Body_IsValid(groundBody2D)){
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_staticBody;
        bodyDef.userData = reinterpret_cast<void*>((uint64_t)NULL_ENTITY);
        groundBody2D = b2CreateBody(world2D, &bodyDef);
    }
    return groundBody2D;
}

int PhysicsSystem::getSubSteps2D() const{
    return subSteps2D;
}

void PhysicsSystem::setSubSteps2D(int subSteps){
    subSteps2D = std::min(16, std::max(1, subSteps));
}
#endif

PhysicsSystem::~PhysicsSystem(){
#ifdef JASPERENGINE_PHYSICS_3D
    // S96 WP-C: its compound bodies go with it
    AttachedColliders::countAttached3D(-(int)attachedBodies3D.size());
    // the characters release their object layers and inner bodies while both still exist
    characters3D.reset();
    vehicles3D.reset();
    flushPendingDestroys3D();
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    characters2D.reset();
    if (b2World_IsValid(world2D)){
        b2DestroyWorld(world2D);
    }
    world2D = b2_nullWorldId;
    groundBody2D = b2_nullBodyId;
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    delete activationListener3D;
    delete contactListener3D;
    delete contactRecorder3D;
    delete rawContacts3D;

    // temp_allocator and job_system are process-shared singletons (see constructor); not owned here.
    delete broad_phase_layer_interface;
    delete object_vs_broadphase_layer_filter;
    delete object_vs_object_layer_filter;
    delete objectLayerTable3D;

    // bodies still in world3D (destroyed after this) hold their own references to it
    if (pairFilter3D){
        pairFilter3D->Release();
        pairFilter3D = nullptr;
    }

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
        if (world3DReady){
            world3D.SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
        }
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
    forEachEvent([](auto& event) { event.clear(); });
}

void PhysicsSystem::removeSubscriptionsByTag(const std::string& substring){
    forEachEvent([&substring](auto& event) { event.removeByTagSubstring(substring); });
}

#ifdef JASPERENGINE_PHYSICS_2D
float PhysicsSystem::getPointsToMeterScale2D() const{
    return pointsToMeterScale2D;
}

void PhysicsSystem::setPointsToMeterScale2D(float pointsToMeterScale2D){
    ScenePhysics2DSettings settings = worldSettings2D;
    settings.pointsPerMeter = pointsToMeterScale2D;
    setWorldSettings2D(settings);
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
    ensureWorld3D();
    return useLockedInterfaces3D() ? world3D.GetBodyInterface() : world3D.GetBodyInterfaceNoLock();
}

const JPH::BodyLockInterface& PhysicsSystem::getBodyLockInterface3D(){
    ensureWorld3D();
    return useLockedInterfaces3D() ? static_cast<const JPH::BodyLockInterface&>(world3D.GetBodyLockInterface())
                                   : static_cast<const JPH::BodyLockInterface&>(world3D.GetBodyLockInterfaceNoLock());
}

const JPH::NarrowPhaseQuery& PhysicsSystem::getNarrowPhaseQuery3D(){
    ensureWorld3D();
    return useLockedInterfaces3D() ? world3D.GetNarrowPhaseQuery() : world3D.GetNarrowPhaseQueryNoLock();
}

bool PhysicsSystem::isSteppingWorld3D() const{
    return steppingWorld3D;
}

std::recursive_mutex& PhysicsSystem::getCallbackMutex3D(){
    return callbackMutex3D;
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

void PhysicsSystem::flushPendingDestroys3D(){
    // the pair filter changes asked for while the job threads read it
    for (const auto& change : pendingPairFilter3D){
        change(*pairFilter3D);
    }
    pendingPairFilter3D.clear();
    if (!world3DReady){
        return;
    }
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
    if (!body->triggerBody.IsInvalid()){
        const JPH::ObjectLayer companionLayer = objectLayerTable3D->acquire(scene->getEntityLayer(entity), body->categoryBits, body->maskBits);
        const JPH::ObjectLayer companionCurrent = body_interface.GetObjectLayer(body->triggerBody);
        body_interface.SetObjectLayer(body->triggerBody, companionLayer);
        objectLayerTable3D->release(companionCurrent);
    }

    wakeAround3D(body->body);
}

void PhysicsSystem::syncLayerCollisionMatrix3D(){
    if (!world3DReady){
        return;
    }
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
    const RigidbodyInterpolation mode = interpolationOf(body.interpolation);
    if (mode == RigidbodyInterpolation::INTERPOLATE && alpha < 1.0f && !body.newBody){
        const b2Transform& previous = body.previousTransform;
        bTransform.p = previous.p + (bTransform.p - previous.p) * alpha;
        if (previous.q.c != bTransform.q.c || previous.q.s != bTransform.q.s){
            bTransform.q = b2NLerp(previous.q, bTransform.q, alpha);
        }
    }else if (mode == RigidbodyInterpolation::EXTRAPOLATE && alpha < 1.0f && !body.newBody){
        // ahead of the step by its velocity over the part of the next step the frame is at
        const float ahead = alpha * lastFixedStep;
        bTransform.p = bTransform.p + b2Body_GetLinearVelocity(body.body) * ahead;
        bTransform.q = b2MakeRot(b2Rot_GetAngle(bTransform.q) + b2Body_GetAngularVelocity(body.body) * ahead);
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
    const RigidbodyInterpolation mode = interpolationOf(body.interpolation);
    if (mode == RigidbodyInterpolation::INTERPOLATE && alpha < 1.0f && !body.newBody){
        position = body.previousPosition.lerpUnclamped(position, alpha);
        if (body.previousRotation != rotation){
            rotation = Quaternion::slerp(alpha, body.previousRotation, rotation);
        }
    }else if (mode == RigidbodyInterpolation::EXTRAPOLATE && alpha < 1.0f && !body.newBody && body.type != BodyType::STATIC){
        // ahead of the step by its velocities over the part of the next step the frame is at
        // (the angular velocity turns it about its centre of mass)
        const float ahead = alpha * lastFixedStep;
        JPH::BodyInterface& body_interface = getBodyInterface3D();
        JPH::Vec3 linearVelocity, angularVelocity;
        body_interface.GetLinearAndAngularVelocity(body.body, linearVelocity, angularVelocity);
        const JPH::RVec3 center = body_interface.GetCenterOfMassPosition(body.body);
        JPH::Quat turn = JPH::Quat::sIdentity();
        const float angularSpeed = angularVelocity.Length();
        if (angularSpeed > 1.0e-9f){
            turn = JPH::Quat::sRotation(angularVelocity / angularSpeed, angularSpeed * ahead);
        }
        const JPH::Vec3 centerToOrigin = jPosition - center;
        const JPH::Vec3 newOrigin = center + linearVelocity * ahead + turn * centerToOrigin;
        const JPH::Quat newRotation = (turn * jRotation).Normalized();
        position = Vector3(newOrigin.GetX(), newOrigin.GetY(), newOrigin.GetZ());
        rotation = Quaternion(newRotation.GetW(), newRotation.GetX(), newRotation.GetY(), newRotation.GetZ());
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

            // S96 WP-C: movePosition / moveRotation, after the pose drawn from is kept
            if (stepTime > 0.0f && (body.hasMovePosition || body.hasMoveRotation || body.movingToTarget)){
                applyMoveTarget2D(entity, body, stepTime);
            }
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

            // S96 WP-C: movePosition / moveRotation, after the pose drawn from is kept
            if (stepTime > 0.0f && (body.hasMovePosition || body.hasMoveRotation || body.movingToTarget)){
                applyMoveTarget3D(entity, body, stepTime);
            }
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
namespace {

    // Box2D welds points closer than its linear slop (B2_LINEAR_SLOP, metres)
    const float linearSlop2D = 0.005f;

    // FNV-1a over the bits of what a polygon decomposition is made of: same input, same key
    uint64_t hashFloats(uint64_t hash, const float* values, size_t count){
        for (size_t i = 0; i < count; i++){
            uint32_t bits;
            std::memcpy(&bits, &values[i], sizeof(bits));
            for (int b = 0; b < 4; b++){
                hash ^= (bits >> (b * 8)) & 0xFFu;
                hash *= 1099511628211ull;
            }
        }
        return hash;
    }

    // a body whose colliders or itself name a material, or whose shapes still have one
    bool usesMaterials(const Rigidbody2DComponent& body){
        if (!body.material.empty() || body.materialInstance){
            return true;
        }
        for (const Collider2D& collider : body.colliders){
            if (!collider.material.empty() || collider.materialInstance || collider.appliedUserMaterial > 0){
                return true;
            }
        }
        if (body.generated){
            for (const auto& [coord, chunk] : body.generated->chunks){
                for (const Collider2D& collider : chunk.colliders){
                    if (!collider.material.empty() || collider.appliedUserMaterial > 0){
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // the shapes of a collider, the segments of a chain included
    void colliderShapes(const Collider2D& collider, std::vector<b2ShapeId>& out){
        out.clear();
        for (const b2ShapeId& shape : collider.shapes){
            if (b2Shape_IsValid(shape)) out.push_back(shape);
        }
        if (b2Chain_IsValid(collider.chain)){
            const int count = b2Chain_GetSegmentCount(collider.chain);
            if (count > 0){
                const size_t start = out.size();
                out.resize(start + (size_t)count);
                const int returned = b2Chain_GetSegments(collider.chain, out.data() + start, count);
                out.resize(start + (size_t)std::max(0, returned));
            }
        }
    }

}

void PhysicsSystem::resolveCollider2DSurface(const Rigidbody2DComponent& body, const Collider2D& collider, float& friction, float& restitution, int& userMaterial) const{
    std::shared_ptr<PhysicsMaterialRuntime> material = collider.materialInstance;
    if (!material && !collider.material.empty()){
        material = PhysicsMaterialPool::getOrLoad(collider.material);
    }
    if (!material){
        material = body.materialInstance;
    }
    if (!material && !body.material.empty()){
        material = PhysicsMaterialPool::getOrLoad(body.material);
    }

    if (material){
        friction = material->getFriction();
        restitution = material->getBounciness();
        userMaterial = encodeSurfaceModes2D(material->getFrictionCombine(), material->getBounceCombine());
    }else{
        friction = collider.friction;
        restitution = collider.restitution;
        userMaterial = 0;
    }
}

bool PhysicsSystem::computeCollider2DPieces(Collider2D& collider, const Vector2& entityScale, bool log, size_t index, const std::string& entityName){
    if (collider.type != Collider2DType::POLYGON || collider.points.size() < 3){
        collider.pieces.clear();
        collider.droppedPieces = 0;
        collider.selfIntersecting = false;
        collider.decompositionError = collider.type == Collider2DType::POLYGON ? "a polygon needs at least 3 points" : "";
        collider.decompositionKey = 0;
        return false;
    }
    float keyValues[4] = {entityScale.x, entityScale.y, pointsToMeterScale2D, (float)collider.points.size()};
    uint64_t key = hashFloats(1469598103934665603ull, keyValues, 4);
    key = hashFloats(key, reinterpret_cast<const float*>(collider.points.data()), collider.points.size() * 2);
    if (key == collider.decompositionKey){
        return false;
    }

    std::vector<Vector2> points(collider.points.size());
    for (size_t j = 0; j < collider.points.size(); j++){
        const Vector2 scaled = collider.points[j] * entityScale;
        points[j] = Vector2(scaled.x / pointsToMeterScale2D, scaled.y / pointsToMeterScale2D);
    }
    Decomposition2D decomposition = decomposePolygon2D(points, linearSlop2D, B2_MAX_POLYGON_VERTICES);
    collider.pieces.clear();
    collider.droppedPieces = decomposition.droppedPieces;
    collider.selfIntersecting = decomposition.selfIntersecting;
    collider.decompositionError = decomposition.error;
    collider.decompositionKey = key;

    // only the pieces Box2D makes a polygon of
    int failedHulls = 0;
    for (std::vector<Vector2>& piece : decomposition.pieces){
        b2Vec2 vertices[B2_MAX_POLYGON_VERTICES];
        const int count = std::min((int)piece.size(), (int)B2_MAX_POLYGON_VERTICES);
        for (int j = 0; j < count; j++){
            vertices[j] = {piece[j].x, piece[j].y};
        }
        b2Hull hull = b2ComputeHull(vertices, count);
        if (!b2ValidateHull(&hull)){
            failedHulls++;
            continue;
        }
        collider.pieces.push_back(std::move(piece));
    }
    collider.droppedPieces += failedHulls;
    if (collider.pieces.empty() && collider.decompositionError.empty()){
        collider.decompositionError = "no piece wide enough for Box2D";
    }

    if (log){
        if (!collider.decompositionError.empty()){
            Log::error("Polygon collider %zu of '%s': %s", index, entityName.c_str(), collider.decompositionError.c_str());
        }else if (collider.selfIntersecting){
            Log::error("Polygon collider %zu of '%s' crosses itself: its convex hull is used instead. Reorder its points into one outline, or use several colliders.", index, entityName.c_str());
        }else if (failedHulls > 0){
            Log::warn("Polygon collider %zu of '%s': %d sliver piece(s) too thin for Box2D were left out", index, entityName.c_str(), failedHulls);
        }
    }
    return true;
}

bool PhysicsSystem::reshapeCollider2DPolygon(Entity entity, Rigidbody2DComponent& body, size_t index){
    if (steppingWorld2D || index >= body.colliders.size() || !b2Body_IsValid(body.body) || body.needReloadBody){
        return false;
    }
    Collider2D& collider = body.colliders[index];
    if (collider.type != Collider2DType::POLYGON || collider.dirty || collider.shapes.size() != 1 ||
            !b2Shape_IsValid(collider.shapes[0]) || b2Shape_GetType(collider.shapes[0]) != b2_polygonShape){
        return false;
    }
    const Vector2 entityScale = entityWorldScale2D(scene, entity);
    if (entityScale != body.loadedScale){
        return false;
    }
    computeCollider2DPieces(collider, entityScale, true, index, scene->getEntityName(entity));
    if (collider.pieces.size() != 1){
        return false;
    }
    const std::vector<Vector2>& piece = collider.pieces[0];
    b2Vec2 vertices[B2_MAX_POLYGON_VERTICES];
    const int count = std::min((int)piece.size(), (int)B2_MAX_POLYGON_VERTICES);
    for (int j = 0; j < count; j++){
        vertices[j] = {piece[j].x, piece[j].y};
    }
    b2Hull hull = b2ComputeHull(vertices, count);
    if (!b2ValidateHull(&hull)){
        return false;
    }
    const float radiusScale = maxScaleXY(absScale2D(entityScale));
    const b2Polygon polygon = b2MakePolygon(&hull, std::max(0.0f, collider.radius * radiusScale / pointsToMeterScale2D));
    b2Shape_SetPolygon(collider.shapes[0], &polygon);
    applyRigidbody2DMass(entity, body);
    return true;
}

void PhysicsSystem::updateCollider2DPieces(Entity entity, Collider2D& collider){
    computeCollider2DPieces(collider, entityWorldScale2D(scene, entity), false, 0, std::string());
}

std::vector<std::vector<Vector2>> PhysicsSystem::getCollider2DPiecesLocal(Entity entity, Collider2D& collider){
    const Vector2 scale = entityWorldScale2D(scene, entity);
    computeCollider2DPieces(collider, scale, false, 0, std::string());
    std::vector<std::vector<Vector2>> local;
    if (scale.x == 0.0f || scale.y == 0.0f){
        return local;
    }
    local.reserve(collider.pieces.size());
    for (const std::vector<Vector2>& piece : collider.pieces){
        std::vector<Vector2> points;
        points.reserve(piece.size());
        for (const Vector2& point : piece){
            points.push_back(Vector2(point.x * pointsToMeterScale2D / scale.x, point.y * pointsToMeterScale2D / scale.y));
        }
        local.push_back(std::move(points));
    }
    return local;
}

void PhysicsSystem::createCollider2DShapes(Entity entity, Rigidbody2DComponent& body, size_t i, const Vector2& entityScale, int layer){
    Collider2D& shapeData = body.colliders[i];
    if (!shapeData.slice.empty()){
        // a slice hidden on this frame (or not known yet): no shape until it shows
        if (!shapeData.sliceVisible || shapeData.slicePoints.size() < 3){
            return;
        }
        // the slice's box as a polygon for this build only: the saved type and points stay
        const Collider2DType savedType = shapeData.type;
        std::vector<Vector2> savedPoints = std::move(shapeData.points);
        shapeData.type = Collider2DType::POLYGON;
        shapeData.points = shapeData.slicePoints;
        createCollider2DShapesOf(entity, body, shapeData, i, entityScale, layer);
        Collider2D& built = body.colliders[i];
        built.type = savedType;
        built.points = std::move(savedPoints);
        return;
    }
    createCollider2DShapesOf(entity, body, shapeData, i, entityScale, layer);
}

void PhysicsSystem::createCollider2DShapesOf(Entity entity, Rigidbody2DComponent& body, Collider2D& shapeData, size_t i, const Vector2& entityScale, int layer){
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

    float friction = shapeData.friction;
    float restitution = shapeData.restitution;
    int userMaterial = 0;
    resolveCollider2DSurface(body, shapeData, friction, restitution, userMaterial);

    b2ShapeDef shapeDef = b2DefaultShapeDef();
    shapeDef.userData = reinterpret_cast<void*>(i);
    shapeDef.density = shapeData.density;
    shapeDef.friction = friction;
    shapeDef.restitution = restitution;
    shapeDef.userMaterialId = userMaterial;
    shapeDef.isSensor = shapeData.sensor;
    shapeDef.enableHitEvents = shapeData.enableHitEvents;
    shapeDef.enablePreSolveEvents = shapeData.preSolveEvents;
    // authored, or a script of the body listens (physics-contacts D16)
    shapeDef.enableContactEvents = shapeData.contactEvents || (body.listenerMask & PhysicsCallbacks::COLLISION_2D) != 0;
    shapeDef.enableSensorEvents = shapeData.sensorEvents || shapeData.sensor || (body.listenerMask & PhysicsCallbacks::TRIGGER_2D) != 0;
    shapeDef.filter = Box2DLayers::makeFilter(layer, shapeData.categoryBits, shapeData.maskBits, shapeData.groupIndex);
    // the body's mass is computed once its dirty colliders are all made
    shapeDef.updateBodyMass = false;

    shapeData.shapes.clear();
    shapeData.chain = b2_nullChainId;
    shapeData.appliedFriction = friction;
    shapeData.appliedRestitution = restitution;
    shapeData.appliedUserMaterial = userMaterial;

    auto addShape = [&](b2ShapeId shape){
        if (b2Shape_IsValid(shape)){
            shapeData.shapes.push_back(shape);
            shapesCreated2D++;
        }
    };

    // a box between pointA and pointB: polygons of fewer than 3 points, and of points that
    // make no polygon (as before)
    auto boxFallback = [&](std::vector<b2Vec2>& vertices){
        const Vector2 pointA = scaledPoint(shapeData.pointA);
        const Vector2 pointB = scaledPoint(shapeData.pointB);
        const Vector2 minPt(std::min(pointA.x, pointB.x), std::min(pointA.y, pointB.y));
        const Vector2 maxPt(std::max(pointA.x, pointB.x), std::max(pointA.y, pointB.y));
        if (minPt == maxPt){
            return false;
        }
        vertices.resize(4);
        vertices[0] = toBox2DPoint(minPt);
        vertices[1] = toBox2DPoint(Vector2(maxPt.x, minPt.y));
        vertices[2] = toBox2DPoint(maxPt);
        vertices[3] = toBox2DPoint(Vector2(minPt.x, maxPt.y));
        return true;
    };

    const std::string entityName = scene->getEntityName(entity);

    if (shapeData.type == Collider2DType::POLYGON){
        const float radius = std::max(0.0f, toBox2DRadius(shapeData.radius));

        if (shapeData.points.size() >= 3){
            // convex pieces of the polygon (one for a convex one of up to 8 points), cached
            computeCollider2DPieces(shapeData, entityScale, true, i, entityName);
            for (const std::vector<Vector2>& piece : shapeData.pieces){
                b2Vec2 vertices[B2_MAX_POLYGON_VERTICES];
                const int count = std::min((int)piece.size(), (int)B2_MAX_POLYGON_VERTICES);
                for (int j = 0; j < count; j++){
                    vertices[j] = {piece[j].x, piece[j].y};
                }
                b2Hull hull = b2ComputeHull(vertices, count);
                if (!b2ValidateHull(&hull)){
                    continue;
                }
                b2Polygon polygon = b2MakePolygon(&hull, radius);
                addShape(b2CreatePolygonShape(body.body, &shapeDef, &polygon));
            }
            if (!shapeData.shapes.empty()){
                return;
            }
        }

        // no polygon from the points: the box between pointA and pointB, as before
        std::vector<b2Vec2> vertices;
        if (!boxFallback(vertices)){
            Log::error("Cannot create polygon shape %zu: less than 3 vertices and invalid pointA/pointB fallback", i);
            return;
        }
        b2Hull hull = b2ComputeHull(vertices.data(), (int)vertices.size());
        if (!b2ValidateHull(&hull)){
            Log::error("Cannot create polygon shape %zu: invalid hull", i);
            return;
        }
        b2Polygon polygon = b2MakePolygon(&hull, radius);
        addShape(b2CreatePolygonShape(body.body, &shapeDef, &polygon));
    }else if (shapeData.type == Collider2DType::CIRCLE){
        float radius = toBox2DRadius(shapeData.radius);
        if (radius <= 0.0f){
            Log::error("Cannot create circle shape %zu: radius must be > 0", i);
            return;
        }

        b2Circle circle{};
        circle.center = toBox2DPoint(scaledPoint(shapeData.pointA));
        circle.radius = radius;
        addShape(b2CreateCircleShape(body.body, &shapeDef, &circle));
    }else if (shapeData.type == Collider2DType::CAPSULE){
        float radius = toBox2DRadius(shapeData.radius);
        if (radius <= 0.0f){
            Log::error("Cannot create capsule shape %zu: radius must be > 0", i);
            return;
        }

        Vector2 center1 = scaledPoint(shapeData.pointA);
        Vector2 center2 = scaledPoint(shapeData.pointB);
        if (center1 == center2){
            Log::error("Cannot create capsule shape %zu: pointA and pointB are equal", i);
            return;
        }

        b2Capsule capsule{};
        capsule.center1 = toBox2DPoint(center1);
        capsule.center2 = toBox2DPoint(center2);
        capsule.radius = radius;
        addShape(b2CreateCapsuleShape(body.body, &shapeDef, &capsule));
    }else if (shapeData.type == Collider2DType::SEGMENT){
        Vector2 point1 = scaledPoint(shapeData.pointA);
        Vector2 point2 = scaledPoint(shapeData.pointB);
        if (point1 == point2){
            Log::error("Cannot create segment shape %zu: pointA and pointB are equal", i);
            return;
        }

        b2Segment segment{};
        segment.point1 = toBox2DPoint(point1);
        segment.point2 = toBox2DPoint(point2);
        addShape(b2CreateSegmentShape(body.body, &shapeDef, &segment));
    }else if (shapeData.type == Collider2DType::CHAIN){
        const size_t count = shapeData.points.size();
        if (count < 2){
            Log::error("Cannot create chain shape %zu: it needs at least 2 points", i);
            return;
        }

        std::vector<b2Vec2> b2vertices(count);
        for (size_t j = 0; j < count; j++){
            b2vertices[j] = toBox2DPoint(scaledPoint(shapeData.points[j]));
        }
        // mirroring flips the winding, which would swap the colliding side
        if (entityScale.x * entityScale.y < 0.0f){
            std::reverse(b2vertices.begin(), b2vertices.end());
        }

        if (count < 4){
            // too short for a Box2D chain (Unity EdgeCollider2D takes 2 points): two-sided
            // segments, closed back to the first point for a loop of 3
            const size_t segments = (shapeData.loop && count == 3) ? 3 : count - 1;
            for (size_t j = 0; j < segments; j++){
                b2Segment segment{};
                segment.point1 = b2vertices[j];
                segment.point2 = b2vertices[(j + 1) % count];
                if (b2DistanceSquared(segment.point1, segment.point2) <= linearSlop2D * linearSlop2D){
                    continue;
                }
                addShape(b2CreateSegmentShape(body.body, &shapeDef, &segment));
            }
            return;
        }

        b2ChainDef chainDef = b2DefaultChainDef();
        chainDef.points = &b2vertices[0];
        chainDef.count = (int)count;
        chainDef.isLoop = shapeData.loop;
        // the segments report the collider index, like the other shapes
        chainDef.userData = reinterpret_cast<void*>(i);
        chainDef.filter = Box2DLayers::makeFilter(layer, shapeData.categoryBits, shapeData.maskBits, shapeData.groupIndex);
        chainDef.friction = friction;
        chainDef.restitution = restitution;
        chainDef.userMaterialId = userMaterial;

        shapeData.chain = b2CreateChain(body.body, &chainDef);
        if (b2Chain_IsValid(shapeData.chain)){
            chainsCreated2D++;
            // Box2D makes chain segments without events: a listening body turns them on
            if (body.listenerMask & PhysicsCallbacks::ALL_2D){
                std::vector<b2ShapeId> segments;
                colliderShapes(shapeData, segments);
                for (const b2ShapeId& segment : segments){
                    applyShapeEventFlags2D(shapeData, body.listenerMask, segment);
                }
            }
        }
    }
}

void PhysicsSystem::updateSliceColliders2D(Entity entity, Rigidbody2DComponent& body){
    const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
    if (!sprite || !sprite->sheetData){
        return;
    }
    bool uiCamera = false;
    if (const CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera())){
        uiCamera = camera->type == CameraType::CAMERA_UI;
    }
    for (Collider2D& collider : body.colliders){
        if (collider.slice.empty()){
            continue;
        }
        Rect bounds(0.0f, 0.0f, 0.0f, 0.0f);
        const bool visible = spriteSliceLocalRect(*sprite, *sprite->sheetData, uiCamera, collider.slice, bounds);
        const bool same = visible == collider.sliceVisible && (!visible || (bounds.getX() == collider.sliceBounds.getX() &&
                          bounds.getY() == collider.sliceBounds.getY() && bounds.getWidth() == collider.sliceBounds.getWidth() &&
                          bounds.getHeight() == collider.sliceBounds.getHeight()));
        if (same && (!visible || !collider.slicePoints.empty())){
            continue;
        }
        collider.sliceVisible = visible;
        collider.sliceBounds = bounds;
        collider.slicePoints.clear();
        if (visible){
            const float x0 = bounds.getX(), y0 = bounds.getY();
            const float x1 = x0 + bounds.getWidth(), y1 = y0 + bounds.getHeight();
            collider.slicePoints = {Vector2(x0, y0), Vector2(x1, y0), Vector2(x1, y1), Vector2(x0, y1)};
        }
        collider.dirty = true;
        body.needUpdateColliders = true;
    }
}

bool PhysicsSystem::syncRigidbody2DColliders(Entity entity, Rigidbody2DComponent& body, bool all){
    if (!b2Body_IsValid(body.body)){
        return false;
    }
    body.hasSliceColliders = std::any_of(body.colliders.begin(), body.colliders.end(), [](const Collider2D& c) { return !c.slice.empty(); });
    if (body.hasSliceColliders){
        updateSliceColliders2D(entity, body);
    }

    const Vector2 entityScale = entityWorldScale2D(scene, entity);
    // shapes are sized by the world scale
    if (entityScale != body.loadedScale){
        all = true;
    }

    const int layer = scene->getEntityLayer(entity);

    // the shapes made below take the events the scripts ask for
    std::vector<Entity> receivers;
    for (const Collider2D& collider : body.colliders){
        if (collider.receiver != NULL_ENTITY) receivers.push_back(collider.receiver);
    }
    for (const AttachedCollider2D& attached : body.attachedColliders){
        receivers.push_back(attached.child);
    }
    const uint16_t mask = computeListenerMask(entity, receivers);
    if (mask != body.listenerMask && !all){
        // the shapes kept need their flags too
        body.listenerMask = mask;
        refreshListenerMask2D(entity, body);
    }
    body.listenerMask = mask;

    bool rebuilt = false;
    for (size_t i = 0; i < body.colliders.size(); i++){
        Collider2D& collider = body.colliders[i];
        if (!all && !collider.dirty){
            continue;
        }
        destroyCollider2D(body, i);
        createCollider2DShapes(entity, body, i, entityScale, layer);
        collider.dirty = false;
        rebuilt = true;
    }

    if (body.generated && syncGeneratedColliders2D(entity, body, all, entityScale, layer)){
        rebuilt = true;
    }
    if (!body.attachedColliders.empty() && syncAttachedColliders2D(entity, body, all)){
        rebuilt = true;
    }

    if (rebuilt || all){
        applyRigidbody2DMass(entity, body);
    }

    body.needUpdateColliders = false;
    body.loadedScale = entityScale;
    body.appliedMaterialRevision = PhysicsMaterialRuntime::revision();

    return true;
}

void PhysicsSystem::applyRigidbody2DMass(Entity entity, Rigidbody2DComponent& body){
    if (!b2Body_IsValid(body.body)){
        return;
    }
    if (steppingWorld2D){
        body.needApplyMass = true;
        return;
    }
    body.needApplyMass = false;

    b2Body_ApplyMassFromShapes(body.body);

    // Box2D gives static and kinematic bodies no mass
    if (body.type != BodyType::DYNAMIC || b2Body_GetType(body.body) != b2_dynamicBody){
        return;
    }
    if (body.useAutoMass && body.autoCenterOfMass && body.autoInertia){
        return;
    }

    b2MassData massData = b2Body_GetMassData(body.body);
    const float shapesMass = massData.mass;
    const b2Vec2 shapesCenter = massData.center;

    if (!body.useAutoMass){
        const float mass = std::isfinite(body.mass) ? std::max(body.mass, 1.0e-6f) : 1.0f;
        // the colliders' inertia for the new mass (their density scaled), Unity
        if (shapesMass > 0.0f){
            massData.rotationalInertia *= mass / shapesMass;
        }
        massData.mass = mass;
    }
    if (!body.autoCenterOfMass){
        const Vector2 scale = entityWorldScale2D(scene, entity);
        const Vector2 center = body.centerOfMass * scale;
        massData.center = {center.x / pointsToMeterScale2D, center.y / pointsToMeterScale2D};
        // about the new centre (parallel axis)
        massData.rotationalInertia += massData.mass * b2DistanceSquared(massData.center, shapesCenter);
    }
    if (!body.autoInertia){
        massData.rotationalInertia = std::isfinite(body.inertia) ? std::max(body.inertia, 0.0f) : 0.0f;
    }
    if (body.fixedRotation){
        massData.rotationalInertia = 0.0f;
    }
    b2Body_SetMassData(body.body, massData);
}

void PhysicsSystem::applyRigidbody2DParameters(Entity entity, Rigidbody2DComponent& body){
    if (!b2Body_IsValid(body.body)){
        return;
    }
    const float gravityScale = std::isfinite(body.gravityScale) ? body.gravityScale : 1.0f;
    if (b2Body_GetGravityScale(body.body) != gravityScale){
        b2Body_SetGravityScale(body.body, gravityScale);
        // a sleeping body would keep its old load (and its joints their old forces) until touched
        if (b2Body_GetType(body.body) == b2_dynamicBody){
            b2Body_SetAwake(body.body, true);
        }
    }
    b2Body_SetLinearDamping(body.body, std::max(0.0f, body.linearDamping));
    b2Body_SetAngularDamping(body.body, std::max(0.0f, body.angularDamping));
    b2Body_SetBullet(body.body, body.collisionDetection == CollisionDetectionMode2D::CONTINUOUS);
    b2Body_SetAllowFastRotation(body.body, body.allowFastRotation);
    b2Body_EnableSleep(body.body, body.sleepingMode != RigidbodySleepMode2D::NEVER_SLEEP);
    // recomputes the mass
    b2Body_SetFixedRotation(body.body, body.fixedRotation);
    applyRigidbody2DMass(entity, body);
}

void PhysicsSystem::refreshRigidbody2DMaterials(Entity entity, Rigidbody2DComponent& body, bool force){
    (void)entity;
    if (!b2Body_IsValid(body.body)){
        return;
    }
    // Box2D is locked while it steps (a pre-solve callback): before the next step instead
    if (steppingWorld2D){
        body.appliedMaterialRevision = 0;
        return;
    }
    body.appliedMaterialRevision = PhysicsMaterialRuntime::revision();
    if (!force && !usesMaterials(body)){
        return;
    }

    bool changed = false;
    auto reapplySurface = [&](Collider2D& collider){
        float friction = 0.0f;
        float restitution = 0.0f;
        int userMaterial = 0;
        resolveCollider2DSurface(body, collider, friction, restitution, userMaterial);
        if (friction == collider.appliedFriction && restitution == collider.appliedRestitution && userMaterial == collider.appliedUserMaterial){
            return;
        }
        collider.appliedFriction = friction;
        collider.appliedRestitution = restitution;
        collider.appliedUserMaterial = userMaterial;
        for (const b2ShapeId& shape : collider.shapes){
            if (!b2Shape_IsValid(shape)) continue;
            b2Shape_SetFriction(shape, friction);
            b2Shape_SetRestitution(shape, restitution);
            b2Shape_SetUserMaterial(shape, userMaterial);
        }
        if (b2Chain_IsValid(collider.chain)){
            b2Chain_SetFriction(collider.chain, friction);
            b2Chain_SetRestitution(collider.chain, restitution);
            b2Chain_SetUserMaterial(collider.chain, userMaterial);
        }
        changed = true;
    };
    for (Collider2D& collider : body.colliders){
        reapplySurface(collider);
    }
    if (body.generated){
        for (auto& [coord, chunk] : body.generated->chunks){
            for (Collider2D& collider : chunk.colliders){
                reapplySurface(collider);
            }
        }
    }

    // the contacts mix the shapes' values again at their next update (Box2D patch): resting
    // contacts stay (no end / begin events); a sleeping body wakes to use them
    if (changed && b2Body_GetType(body.body) != b2_staticBody){
        b2Body_SetAwake(body.body, true);
    }
}

void PhysicsSystem::syncMaterials2D(){
    const uint64_t revision = PhysicsMaterialRuntime::revision();
    auto bodies2d = scene->getComponentArray<Rigidbody2DComponent>();
    for (size_t i = 0; i < bodies2d->size(); i++){
        Rigidbody2DComponent& body = bodies2d->getComponentFromIndex(i);
        if (body.appliedMaterialRevision != revision && b2Body_IsValid(body.body)){
            refreshRigidbody2DMaterials(bodies2d->getEntity(i), body, false);
        }
    }
}

bool PhysicsSystem::removeCollider2D(Entity entity, Rigidbody2DComponent& body, size_t index){
    if (index >= body.colliders.size()){
        return false;
    }

    if (!b2Body_IsValid(body.body)){
        body.colliders.erase(body.colliders.begin() + (std::ptrdiff_t)index);
        contactColliderRemoved2D(entity, index);
        body.needUpdateColliders = true;
        return true;
    }

    // Box2D is locked while it steps (a contact callback): the body is made again after it
    if (steppingWorld2D){
        body.colliders.erase(body.colliders.begin() + (std::ptrdiff_t)index);
        contactColliderRemoved2D(entity, index);
        body.needReloadBody = true;
        body.needUpdateColliders = true;
        return true;
    }

    destroyCollider2D(body, index);
    body.colliders.erase(body.colliders.begin() + (std::ptrdiff_t)index);
    contactColliderRemoved2D(entity, index);

    // the later colliders' shapes report their new index
    std::vector<b2ShapeId> shapes;
    for (size_t i = index; i < body.colliders.size(); i++){
        colliderShapes(body.colliders[i], shapes);
        for (const b2ShapeId& shape : shapes){
            b2Shape_SetUserData(shape, reinterpret_cast<void*>(i));
        }
    }

    applyRigidbody2DMass(entity, body);
    if (b2Body_GetType(body.body) != b2_staticBody){
        b2Body_SetAwake(body.body, true);
    }
    return true;
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
bool PhysicsSystem::createCollider3DForIndex(Entity bodyEntity, RigidbodyComponent& body, size_t index){
    Collider3D& shapeData = AttachedColliders::colliderAt(body, index);
    shapeData.shape = NULL;
    // an attached child's collider is sized by the child's scale, its mesh the child's
    const Entity entity = AttachedColliders::isAttached(body, index) ? body.attachedColliders[index - body.colliders.size()].child : bodyEntity;

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

    // the collider's surface rides on its leaf, so contacts find it from their sub-shape ids
    if (!shapeData.surface){
        shapeData.surface = new JoltSurfaceMaterial();
    }
    shapeData.surface->colliderIndex = (uint32_t)index;

    std::string error;
    shapeData.shape = JoltShapeBuilder::buildColliderShape(scene, entity, shapeData, &error, shapeData.surface.GetPtr());
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

    shapeData.dirty = false;
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

bool PhysicsSystem::buildColliderLeaves3D(Entity entity, RigidbodyComponent& body, bool all, bool& sourceLoading){
    sourceLoading = false;
    for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
        Collider3D& collider = AttachedColliders::colliderAt(body, i);
        const bool attached = AttachedColliders::isAttached(body, i);
        // an attached leaf kept from the last compose reports its index now (own colliders
        // before it may have come or gone)
        if (attached && !collider.dirty && collider.shape){
            const_cast<JPH::Shape*>(collider.shape.GetPtr())->SetUserData(i);
            if (collider.surface){
                collider.surface->colliderIndex = (uint32_t)i;
            }
        }
        // an attached leaf follows its child's scale, not this body's (composeAttached3D)
        if ((!all || attached) && !collider.dirty && collider.shape){
            continue;
        }
        // a source still loading is not a failure: fixedUpdate retries the body every step
        const Entity shapeEntity = attached ? body.attachedColliders[i - body.colliders.size()].child : entity;
        if (isShapeSourceLoading(shapeEntity, collider)){
            sourceLoading = true;
            return false;
        }
        if (!createCollider3DForIndex(entity, body, i)){
            Log::error("Cannot create runtime shape %zu for 3D Body entity: %u", i, entity);
            return false;
        }
    }
    return true;
}

namespace {

    // the material a collider uses: its own (a script's, else its asset), else the body's
    std::shared_ptr<PhysicsMaterialRuntime> colliderMaterial3D(const RigidbodyComponent& body, const Collider3D& collider){
        std::shared_ptr<PhysicsMaterialRuntime> material = collider.materialInstance;
        if (!material && !collider.material.empty()){
            material = PhysicsMaterialPool::getOrLoad(collider.material);
        }
        if (!material){
            material = body.materialInstance;
        }
        if (!material && !body.material.empty()){
            material = PhysicsMaterialPool::getOrLoad(body.material);
        }
        return material;
    }

    bool anyTrigger3D(const RigidbodyComponent& body){
        for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
            if (AttachedColliders::colliderAt(body, i).isTrigger) return true;
        }
        return false;
    }

    bool allTriggers3D(const RigidbodyComponent& body){
        for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
            if (!AttachedColliders::colliderAt(body, i).isTrigger) return false;
        }
        return AttachedColliders::colliderCount(body) > 0;
    }

    // a body with trigger and solid colliders keeps its triggers on a sensor body of their own
    bool splitsTriggers3D(const RigidbodyComponent& body){
        return !body.sensor && anyTrigger3D(body) && !allTriggers3D(body);
    }

    // a body of trigger colliders only is a sensor itself (Jolt's continuous collision then
    // skips it)
    bool isSensorBody3D(const RigidbodyComponent& body){
        return body.sensor || allTriggers3D(body);
    }

    // the collider that keeps a body static (Jolt cannot move a mesh or heightfield)
    int staticOnlyCollider3D(const RigidbodyComponent& body){
        for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
            const Collider3D& collider = AttachedColliders::colliderAt(body, i);
            if (collider.shape ? collider.shape->MustBeStatic()
                               : (collider.type == Collider3DType::MESH || collider.type == Collider3DType::HEIGHTFIELD)){
                return (int)i;
            }
        }
        return -1;
    }

    const char* collider3DTypeName(Collider3DType type){
        switch (type){
            case Collider3DType::SPHERE: return "Sphere";
            case Collider3DType::BOX: return "Box";
            case Collider3DType::CAPSULE: return "Capsule";
            case Collider3DType::TAPERED_CAPSULE: return "Tapered Capsule";
            case Collider3DType::CYLINDER: return "Cylinder";
            case Collider3DType::CONVEX_HULL: return "Convex Hull";
            case Collider3DType::MESH: return "Mesh";
            case Collider3DType::HEIGHTFIELD: return "Heightfield";
        }
        return "collider";
    }

    JPH::Vec3 toJolt(const Vector3& v){
        return JPH::Vec3(v.x, v.y, v.z);
    }

    Vector3 fromJolt(JPH::Vec3Arg v){
        return Vector3(v.GetX(), v.GetY(), v.GetZ());
    }

    // R diag(moments) R^T, the moments kept above 0 (Jolt inverts the tensor)
    JPH::Mat44 principalInertia(JPH::Vec3Arg moments, JPH::QuatArg rotation){
        const JPH::Mat44 r = JPH::Mat44::sRotation(rotation);
        const JPH::Vec3 safe = JPH::Vec3::sMax(moments, JPH::Vec3::sReplicate(1.0e-9f));
        JPH::Mat44 inertia = r * JPH::Mat44::sScale(safe) * r.Transposed3x3();
        inertia.SetColumn4(3, JPH::Vec4(0, 0, 0, 1));
        return inertia;
    }

    // m (|d|^2 I - d d^T): the inertia of a point mass m at offset d (parallel axis theorem)
    JPH::Mat44 parallelAxis(float mass, JPH::Vec3Arg offset){
        JPH::Mat44 inertia = JPH::Mat44::sScale(mass * offset.LengthSq()) - JPH::Mat44::sOuterProduct(offset, offset) * mass;
        inertia.SetColumn4(3, JPH::Vec4(0, 0, 0, 1));
        return inertia;
    }

}

void PhysicsSystem::resolveColliderSurfaces3D(const RigidbodyComponent& body){
    for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
        const Collider3D& collider = AttachedColliders::colliderAt(body, i);
        if (!collider.surface){
            continue;
        }
        JoltSurfaceMaterial& surface = *collider.surface;
        surface.colliderIndex = (uint32_t)i;
        surface.isTrigger = collider.isTrigger;
        if (std::shared_ptr<PhysicsMaterialRuntime> material = colliderMaterial3D(body, collider)){
            surface.friction = material->getFriction();
            surface.restitution = material->getBounciness();
            surface.frictionCombine = material->getFrictionCombine();
            surface.bounceCombine = material->getBounceCombine();
            // a mode is what tells "a material" from "none"
            if (surface.frictionCombine == PhysicsMaterialCombine::NONE) surface.frictionCombine = PhysicsMaterialCombine::AVERAGE;
            if (surface.bounceCombine == PhysicsMaterialCombine::NONE) surface.bounceCombine = PhysicsMaterialCombine::AVERAGE;
        }else{
            surface.friction = body.friction;
            surface.restitution = body.restitution;
            surface.frictionCombine = PhysicsMaterialCombine::NONE;
            surface.bounceCombine = PhysicsMaterialCombine::NONE;
        }
    }
}

// Jolt merges the manifolds of a body pair into one when it can; a body whose colliders differ
// (a trigger, another surface) needs each collider's own contacts (physics-contacts 6.1.4)
bool PhysicsSystem::usesManifoldReduction3D(const RigidbodyComponent& body) const{
    if (AttachedColliders::colliderCount(body) <= 1){
        return true;
    }
    // exact per-collider pairs for a compound body whose collisions a script listens to
    // (physics-contacts 6.1.4)
    if (body.listenerMask & PhysicsCallbacks::COLLISION_3D){
        return false;
    }
    // a child's collider gets its own contacts (its GameObject is told)
    if (!body.attachedColliders.empty()){
        return false;
    }
    const JoltSurfaceMaterial* first = body.colliders[0].surface.GetPtr();
    for (const Collider3D& collider : body.colliders){
        if (collider.isTrigger){
            return false;
        }
        const JoltSurfaceMaterial* surface = collider.surface.GetPtr();
        if (first && surface && (surface->friction != first->friction || surface->restitution != first->restitution ||
                surface->frictionCombine != first->frictionCombine || surface->bounceCombine != first->bounceCombine)){
            return false;
        }
    }
    return true;
}

// Unity's trigger matrix: a kinematic body with a trigger sees static and kinematic bodies too
bool PhysicsSystem::collidesKinematicVsNonDynamic3D(const RigidbodyComponent& body) const{
    // a trigger sees the kinematic inner bodies of CharacterControllers (and other kinematics)
    return body.collideKinematicVsNonDynamic || isSensorBody3D(body) || (body.type == BodyType::KINEMATIC && (anyTrigger3D(body) || body.sensor));
}

bool PhysicsSystem::computeMassProperties3D(Entity entity, const RigidbodyComponent& body, MassInfo3D& info){
    JPH::Mat44 inertia;
    return computeMass3D(entity, body, info, inertia);
}

bool PhysicsSystem::computeMass3D(Entity entity, const RigidbodyComponent& body, MassInfo3D& info, JPH::Mat44& inertiaOut){
    info = MassInfo3D();
    inertiaOut = JPH::Mat44::sIdentity();
    if (AttachedColliders::colliderCount(body) == 0){
        return false;
    }

    const Vector3 entityScale = entityWorldScale(scene, entity);
    // triggers carry no mass, unless every collider is one
    const bool triggersOnly = allTriggers3D(body);

    struct Part{
        float mass;
        JPH::Vec3 center;
        JPH::Mat44 inertia;
    };
    std::vector<Part> parts;
    parts.reserve(AttachedColliders::colliderCount(body));
    float totalMass = 0.0f;
    JPH::Vec3 weightedCenter = JPH::Vec3::sZero();

    for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
        const Collider3D& collider = AttachedColliders::colliderAt(body, i);
        const bool attached = AttachedColliders::isAttached(body, i);
        if (collider.isTrigger && !triggersOnly){
            continue;
        }
        // a body-less preview (the Inspector) builds the leaf it does not have
        JPH::ShapeRefC leaf = collider.shape;
        if (!leaf){
            const Entity shapeEntity = attached ? body.attachedColliders[i - body.colliders.size()].child : entity;
            if (isShapeSourceLoading(shapeEntity, collider)){
                continue;
            }
            leaf = JoltShapeBuilder::buildColliderShape(scene, shapeEntity, collider, nullptr);
            if (leaf && leaf->GetType() == JPH::EShapeType::Convex){
                const_cast<JPH::ConvexShape*>(static_cast<const JPH::ConvexShape*>(leaf.GetPtr()))->SetDensity(collider.density);
            }
        }
        if (!leaf || leaf->MustBeStatic()){
            continue;
        }

        JPH::MassProperties leafMass = leaf->GetMassProperties();
        if (!(leafMass.mMass > 0.0f) || !std::isfinite(leafMass.mMass)){
            continue;
        }
        const JPH::Quat rotation = toValidatedJoltRotation(collider.rotation, entity, (int)i);
        leafMass.Rotate(JPH::Mat44::sRotation(rotation));
        // an attached collider is posed in the body's frame already (world units)
        const JPH::Vec3 center = toJolt(attached ? collider.position : collider.position * entityScale) + rotation * leaf->GetCenterOfMass();
        parts.push_back({leafMass.mMass, center, leafMass.mInertia});
        totalMass += leafMass.mMass;
        weightedCenter += center * leafMass.mMass;
    }

    if (parts.empty() || !(totalMass > 0.0f)){
        if (body.automaticMass){
            return false;
        }
        // an explicit mass without any volume: a small solid sphere of it at the centre
        info.mass = std::max(body.mass, 1.0e-6f);
        info.centerOfMass = body.automaticCenterOfMass ? Vector3::ZERO : body.centerOfMass;
        const float inertia = 0.1f * info.mass;
        info.inertiaDiagonal = body.automaticInertiaTensor ? Vector3(inertia, inertia, inertia) : body.inertiaTensor;
        inertiaOut = principalInertia(toJolt(info.inertiaDiagonal), body.automaticInertiaTensor ? JPH::Quat::sIdentity()
                                      : toValidatedJoltRotation(body.inertiaTensorRotation, entity, -1));
        return true;
    }

    const JPH::Vec3 automaticCenter = weightedCenter / totalMass;
    const JPH::Vec3 center = body.automaticCenterOfMass ? automaticCenter : toJolt(body.centerOfMass);

    JPH::Mat44 inertia = JPH::Mat44::sZero();
    for (const Part& part : parts){
        inertia = inertia + part.inertia + parallelAxis(part.mass, part.center - center);
    }

    float mass = totalMass;
    if (!body.automaticMass && body.mass > 0.0f && std::isfinite(body.mass)){
        const float factor = body.mass / totalMass;
        inertia = inertia * factor;
        mass = body.mass;
    }

    info.mass = mass;
    info.centerOfMass = fromJolt(center);
    if (!body.automaticInertiaTensor){
        // Unity inertiaTensor + inertiaTensorRotation: R diag(I) R^T about the centre of mass
        info.inertiaDiagonal = body.inertiaTensor;
        inertiaOut = principalInertia(toJolt(body.inertiaTensor), toValidatedJoltRotation(body.inertiaTensorRotation, entity, -1));
    }else{
        info.inertiaDiagonal = Vector3(inertia(0, 0), inertia(1, 1), inertia(2, 2));
        inertia.SetColumn4(3, JPH::Vec4(0, 0, 0, 1));
        inertiaOut = inertia;
    }
    return true;
}

JPH::ShapeRefC PhysicsSystem::composeColliderShape3D(Entity entity, RigidbodyComponent& body, const std::vector<size_t>& indices){
    if (indices.empty()){
        return NULL;
    }
    JPH::ShapeRefC shape;
    const Vector3 entityScale = entityWorldScale(scene, entity);
    // an attached collider is posed in the body's frame already (world units)
    auto bodyPosition = [&](size_t i){
        const Collider3D& collider = AttachedColliders::colliderAt(body, i);
        return AttachedColliders::isAttached(body, i) ? collider.position : collider.position * entityScale;
    };
    if (indices.size() > 1){
        JPH::StaticCompoundShapeSettings compound_shape;
        for (size_t i : indices){
            const Collider3D& collider = AttachedColliders::colliderAt(body, i);
            Vector3 scaledPosition = bodyPosition(i);
            JPH::Vec3 jPosition(scaledPosition.x, scaledPosition.y, scaledPosition.z);
            JPH::Quat jQuat = toValidatedJoltRotation(collider.rotation, entity, static_cast<int>(i));
            compound_shape.AddShape(jPosition, jQuat, collider.shape);
        }

        JPH::ShapeSettings::ShapeResult shape_result = compound_shape.Create();
        if (!shape_result.IsValid()){
            Log::error("Cannot create StaticCompoundShape for 3D Body entity: %u", entity);
            return NULL;
        }
        shape = shape_result.Get();
    }else{
        const size_t index = indices[0];
        Collider3D& shapeData = AttachedColliders::colliderAt(body, index);
        if (shapeData.position != Vector3::ZERO || shapeData.rotation != Quaternion::IDENTITY){
            Vector3 scaledPosition = bodyPosition(index);
            JPH::Vec3 jPosition(scaledPosition.x, scaledPosition.y, scaledPosition.z);
            JPH::Quat jQuat = toValidatedJoltRotation(shapeData.rotation, entity, static_cast<int>(index));
            JPH::RotatedTranslatedShapeSettings rtShape(jPosition, jQuat, shapeData.shape);
            JPH::ShapeSettings::ShapeResult shape_result = rtShape.Create();
            if (!shape_result.IsValid()){
                Log::error("Cannot create RotatedTranslatedShapeSettings for 3D Body entity: %u", entity);
                return NULL;
            }
            shape = shape_result.Get();
        }else{
            shape = shapeData.shape;
        }
    }
    return shape;
}

JPH::ShapeRefC PhysicsSystem::buildBodyShape3D(Entity entity, RigidbodyComponent& body, JPH::MassProperties& mass, bool& overrideMass){
    overrideMass = false;
    if (AttachedColliders::colliderCount(body) == 0){
        return NULL;
    }

    // the body's own shape: every collider, or its solid ones when it has triggers too (those
    // are its trigger companion's, syncTriggerCompanion3D)
    std::vector<size_t> indices;
    const bool split = splitsTriggers3D(body);
    for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
        if (!split || !AttachedColliders::colliderAt(body, i).isTrigger){
            indices.push_back(i);
        }
    }
    JPH::ShapeRefC shape = composeColliderShape3D(entity, body, indices);
    if (!shape){
        return NULL;
    }

    // Jolt's own mass properties unless a mass field or a trigger collider changes them; the
    // legacy solid box override (setOverrideMassAndInertia) wins over both
    const bool customMass = !body.automaticMass || !body.automaticCenterOfMass || !body.automaticInertiaTensor ||
                            (anyTrigger3D(body) && !allTriggers3D(body) && !split);
    if (!customMass || body.overrideMassProperties || shape->MustBeStatic()){
        return shape;
    }

    MassInfo3D info;
    JPH::Mat44 inertia;
    if (!computeMass3D(entity, body, info, inertia)){
        return shape;
    }

    // the body's centre of mass is its shape's: moved there by an offset shape
    const JPH::Vec3 center = toJolt(info.centerOfMass);
    const JPH::Vec3 offset = center - shape->GetCenterOfMass();
    if (offset.LengthSq() > 1.0e-12f){
        JPH::OffsetCenterOfMassShapeSettings offsetShape(offset, shape);
        JPH::ShapeSettings::ShapeResult shape_result = offsetShape.Create();
        if (shape_result.IsValid()){
            shape = shape_result.Get();
        }else{
            Log::error("Cannot move the centre of mass of 3D Body entity %u: %s", entity, shape_result.GetError().c_str());
        }
    }

    mass.mMass = info.mass;
    mass.mInertia = inertia;
    overrideMass = true;
    return shape;
}

void PhysicsSystem::invalidateJoints3D(Entity bodyEntity){
    auto joints3d = scene->getComponentArray<Joint3DComponent>();
    for (size_t i = 0; i < joints3d->size(); i++){
        Joint3DComponent& joint = joints3d->getComponentFromIndex(i);
        if (joint.bodyA == bodyEntity || joint.bodyB == bodyEntity){
            // its constraint holds the body: it goes first, made again once the body is back
            if (joint.joint){
                destroyJoint3D(joint);
            }
            joint.needUpdateJoint = true;
        }
    }
}

bool PhysicsSystem::syncRigidbodyColliders(Entity entity, RigidbodyComponent& body){
    // an attached child has no body: its colliders are its ancestor's (composeAttached3D, which
    // sees its flags at the next step)
    if (body.attachToParentBody && AttachedColliders::findBody3D(scene, entity) != NULL_ENTITY){
        if (!body.body.IsInvalid()){
            contactBodyRemoved3D(entity);
            invalidateJoints3D(entity);
            destroyRigidbody(body);
        }
        return false;
    }

    // a live body whose shape can be swapped keeps its id, velocities, joints and contacts
    if (!body.body.IsInvalid() && !body.needReloadBody){
        return reshapeRigidbody3D(entity, body);
    }

    if (!body.body.IsInvalid()){
        invalidateJoints3D(entity);
        destroyRigidbody(body);
        body.body = JPH::BodyID();
    }

    if (AttachedColliders::colliderCount(body) == 0){
        return false;
    }

    bool sourceLoading = false;
    if (!buildColliderLeaves3D(entity, body, true, sourceLoading)){
        return false;
    }
    resolveColliderSurfaces3D(body);

    JPH::MassProperties mass;
    bool overrideMass = false;
    JPH::ShapeRefC shape = buildBodyShape3D(entity, body, mass, overrideMass);
    if (!shape){
        return false;
    }
    if (!createGenericJoltBody(entity, body, shape, overrideMass ? &mass : nullptr)){
        return false;
    }

    body.needReloadBody = false;
    body.needUpdateColliders = false;
    body.massDirty = false;
    body.needApplyParameters = false;
    body.newBody = true;
    body.loadedScale = entityWorldScale(scene, entity);
    body.appliedMaterialRevision = PhysicsMaterialRuntime::revision();
    syncTriggerCompanion3D(entity, body);

    return true;
}

bool PhysicsSystem::setPrebuiltBodyShape3D(Entity entity, const JPH::ShapeRefC& shape){
    ensureWorld3D();
    RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
    if (!body || !shape || steppingWorld3D){
        return false;
    }
    if (!body->body.IsInvalid()){
        getBodyInterface3D().SetShape(body->body, shape, false, JPH::EActivation::DontActivate);
    }else if (!createGenericJoltBody(entity, *body, shape)){
        return false;
    }
    body->needReloadBody = false;
    body->needUpdateColliders = false;
    body->massDirty = false;
    body->needApplyParameters = false;
    body->newBody = false;
    return true;
}

bool PhysicsSystem::reshapeRigidbody3D(Entity entity, RigidbodyComponent& body){
    if (body.body.IsInvalid()){
        return syncRigidbodyColliders(entity, body);
    }
    if (steppingWorld3D){
        // the step holds the bodies: the next sync does it
        body.needUpdateColliders = true;
        return false;
    }

    // no collider left: the body leaves the world until one is added (its joints with it)
    if (AttachedColliders::colliderCount(body) == 0){
        invalidateJoints3D(entity);
        destroyRigidbody(body);
        body.body = JPH::BodyID();
        body.needUpdateColliders = false;
        return false;
    }

    const Vector3 entityScale = entityWorldScale(scene, entity);
    const bool scaleChanged = entityScale != body.loadedScale;
    bool sourceLoading = false;
    if (!buildColliderLeaves3D(entity, body, scaleChanged, sourceLoading)){
        // a source still loading keeps the old shape until it is there
        body.needUpdateColliders = true;
        return false;
    }
    resolveColliderSurfaces3D(body);

    JPH::MassProperties mass;
    bool overrideMass = false;
    JPH::ShapeRefC shape = buildBodyShape3D(entity, body, mass, overrideMass);
    if (!shape){
        return false;
    }

    JPH::BodyInterface& body_interface = getBodyInterface3D();
    // a static-only shape on a body that may move, or a movable shape on a body made static-only:
    // only a new body changes that
    bool canMove = true;
    {
        JPH::BodyLockRead lock(getBodyLockInterface3D(), body.body);
        if (lock.Succeeded()){
            canMove = lock.GetBody().CanBeKinematicOrDynamic();
        }
    }
    if (shape->MustBeStatic() == canMove){
        body.needReloadBody = true;
        return syncRigidbodyColliders(entity, body);
    }

    const bool active = body.type != BodyType::STATIC && body.enabled && body_interface.IsAdded(body.body);
    const JPH::ShapeRefC previousShape = body_interface.GetShape(body.body);
    const JPH::Vec3 previousCenter = previousShape ? previousShape->GetCenterOfMass() : JPH::Vec3::sZero();
    body_interface.SetShape(body.body, shape, !overrideMass && !body.overrideMassProperties,
                            active ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    // the joints hold points relative to the centre of mass: told where it went
    const JPH::Vec3 deltaCenter = shape->GetCenterOfMass() - previousCenter;
    if (deltaCenter.LengthSq() > 0.0f){
        auto joints3d = scene->getComponentArray<Joint3DComponent>();
        for (size_t i = 0; i < joints3d->size(); i++){
            Joint3DComponent& joint = joints3d->getComponentFromIndex(i);
            if (joint.joint && (joint.bodyA == entity || joint.bodyB == entity)){
                joint.joint->NotifyShapeChanged(body.body, deltaCenter);
            }
        }
    }
    {
        JPH::BodyLockWrite lock(getBodyLockInterface3D(), body.body);
        if (lock.Succeeded()){
            JPH::Body& jbody = lock.GetBody();
            JPH::MotionProperties* motion = jbody.GetMotionPropertiesUnchecked();
            if (motion){
                if (body.overrideMassProperties){
                    JPH::MassProperties box;
                    box.SetMassAndInertiaOfSolidBox(toJolt(body.solidBoxSize), body.solidBoxDensity);
                    motion->SetMassProperties(motion->GetAllowedDOFs(), box);
                }else if (overrideMass){
                    motion->SetMassProperties(motion->GetAllowedDOFs(), mass);
                }else if (!shape->MustBeStatic()){
                    // Jolt's own again (SetShape skips it for the same shape: back to automatic)
                    motion->SetMassProperties(motion->GetAllowedDOFs(), shape->GetMassProperties());
                }
            }
            jbody.SetUseManifoldReduction(usesManifoldReduction3D(body));
            jbody.SetCollideKinematicVsNonDynamic(collidesKinematicVsNonDynamic3D(body));
            jbody.SetIsSensor(isSensorBody3D(body));
        }
    }

    body.needUpdateColliders = false;
    body.massDirty = false;
    body.loadedScale = entityScale;
    body.appliedMaterialRevision = PhysicsMaterialRuntime::revision();
    syncTriggerCompanion3D(entity, body);
    // what touches it sees the new shape (a resting contact keeps its old manifold otherwise)
    wakeAround3D(body.body);
    return true;
}

bool PhysicsSystem::removeCollider3D(Entity entity, RigidbodyComponent& body, size_t index){
    if (index >= body.colliders.size()){
        return false;
    }
    body.colliders.erase(body.colliders.begin() + (std::ptrdiff_t)index);
    // its pairs end at the next step with an invalid index, the later ones shift (physics-contacts 6.1.6)
    contactTracker3D.removeCollider(entity, (int)index);
    // the later leaves report their new index
    for (size_t i = index; i < body.colliders.size(); i++){
        Collider3D& collider = body.colliders[i];
        if (collider.shape){
            const_cast<JPH::Shape*>(collider.shape.GetPtr())->SetUserData(i);
        }
        if (collider.surface){
            collider.surface->colliderIndex = (uint32_t)i;
        }
    }
    body.needUpdateColliders = true;
    (void)entity;
    return true;
}

void PhysicsSystem::refreshRigidbodyMaterials3D(Entity entity, RigidbodyComponent& body, bool force){
    (void)entity;
    if (!force && body.appliedMaterialRevision == PhysicsMaterialRuntime::revision()){
        return;
    }
    body.appliedMaterialRevision = PhysicsMaterialRuntime::revision();
    if (body.body.IsInvalid()){
        return;
    }

    std::vector<float> before;
    before.reserve(AttachedColliders::colliderCount(body) * 4);
    for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
        const Collider3D& collider = AttachedColliders::colliderAt(body, i);
        if (!collider.surface) continue;
        before.push_back(collider.surface->friction);
        before.push_back(collider.surface->restitution);
        before.push_back((float)collider.surface->frictionCombine);
        before.push_back((float)collider.surface->bounceCombine);
    }
    resolveColliderSurfaces3D(body);
    size_t k = 0;
    bool changed = false;
    for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
        const Collider3D& collider = AttachedColliders::colliderAt(body, i);
        if (!collider.surface) continue;
        changed = changed || before[k] != collider.surface->friction || before[k + 1] != collider.surface->restitution ||
                  before[k + 2] != (float)collider.surface->frictionCombine || before[k + 3] != (float)collider.surface->bounceCombine;
        k += 4;
    }
    if (!changed){
        return;
    }

    // Jolt combines the surfaces at every step, cached contacts too: sleeping pairs are woken
    // to pick the new values up; manifold reduction follows the colliders' surfaces
    {
        JPH::BodyLockWrite lock(getBodyLockInterface3D(), body.body);
        if (lock.Succeeded()){
            lock.GetBody().SetUseManifoldReduction(usesManifoldReduction3D(body));
        }
    }
    wakeAround3D(body.body);
}

void PhysicsSystem::syncMaterials3D(){
    const uint64_t revision = PhysicsMaterialRuntime::revision();
    auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
    for (size_t i = 0; i < bodies3d->size(); i++){
        RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
        if (body.appliedMaterialRevision != revision){
            refreshRigidbodyMaterials3D(bodies3d->getEntity(i), body, false);
        }
    }
}

void PhysicsSystem::applyRigidbodyParameters(Entity entity, RigidbodyComponent& body){
    if (body.body.IsInvalid()){
        return;
    }
    JPH::BodyInterface& body_interface = getBodyInterface3D();
    // a type written into the component (the Inspector, set_properties): switched live
    {
        const JPH::EMotionType motion = body_interface.GetMotionType(body.body);
        const BodyType live = motion == JPH::EMotionType::Dynamic ? BodyType::DYNAMIC
                            : (motion == JPH::EMotionType::Kinematic ? BodyType::KINEMATIC : BodyType::STATIC);
        if (live != body.type && !body.hasPendingType){
            const BodyType wanted = body.type;
            body.type = live;
            setRigidbodyType(entity, body, wanted);
        }
    }
    {
        JPH::BodyLockWrite lock(getBodyLockInterface3D(), body.body);
        if (!lock.Succeeded()){
            return;
        }
        JPH::Body& jbody = lock.GetBody();
        JPH::MotionProperties* motion = jbody.GetMotionPropertiesUnchecked();
        if (motion){
            motion->SetLinearDamping(std::max(0.0f, body.linearDamping));
            motion->SetAngularDamping(std::max(0.0f, body.angularDamping));
            motion->SetMaxLinearVelocity(std::max(0.0f, body.maxLinearVelocity));
            motion->SetMaxAngularVelocity(std::max(0.0f, body.maxAngularVelocity));
            motion->SetGravityFactor(body.gravityFactor);
            jbody.SetAllowSleeping(body.allowSleeping);
        }
        jbody.SetCollideKinematicVsNonDynamic(collidesKinematicVsNonDynamic3D(body));
        jbody.SetFriction(body.friction);
        jbody.SetRestitution(body.restitution);
    }
    if (body.type != BodyType::STATIC){
        body_interface.SetMotionQuality(body.body, getRigidbodyMotionQualityToJolt(body.motionQuality));
        if (body_interface.IsAdded(body.body)){
            body_interface.ActivateBody(body.body);
        }
    }
    // the colliders without a material take the body's surface
    resolveColliderSurfaces3D(body);
}

bool PhysicsSystem::setRigidbodyType(Entity entity, RigidbodyComponent& body, BodyType type){
    if (!body.body.IsInvalid() && type != BodyType::STATIC){
        bool canMove = true;
        {
            JPH::BodyLockRead lock(getBodyLockInterface3D(), body.body);
            if (lock.Succeeded()){
                canMove = lock.GetBody().CanBeKinematicOrDynamic();
            }
        }
        if (!canMove){
            const int index = staticOnlyCollider3D(body);
            Log::error("Rigidbody '%s' cannot be %s: its collider %d (%s) is static-only (Jolt moves no mesh or heightfield collider: "
                       "Decompose Into Convex Hulls on that collider in the Inspector, or edit_colliders decompose, makes convex hulls of a mesh)",
                       scene->getEntityName(entity).c_str(), type == BodyType::DYNAMIC ? "dynamic" : "kinematic", index,
                       index >= 0 ? collider3DTypeName(AttachedColliders::colliderAt(body, (size_t)index).type) : "?");
            return false;
        }
    }else if (body.body.IsInvalid() && type != BodyType::STATIC && staticOnlyCollider3D(body) >= 0){
        const int index = staticOnlyCollider3D(body);
        Log::error("Rigidbody '%s' cannot be %s: its collider %d (%s) is static-only (Decompose Into Convex Hulls on that collider in the Inspector, "
                   "or edit_colliders decompose, makes convex hulls of a mesh)",
                   scene->getEntityName(entity).c_str(), type == BodyType::DYNAMIC ? "dynamic" : "kinematic", index,
                   collider3DTypeName(AttachedColliders::colliderAt(body, (size_t)index).type));
        return false;
    }

    // a contact callback of the step: the bodies are Jolt's until it ends
    if (steppingWorld3D){
        body.hasPendingType = true;
        body.pendingType = type;
        pendingTypeChanges3D.push_back(entity);
        return true;
    }

    // the script takes over a body that was following its Transform (a kinematic body switched
    // to dynamic keeps the velocity the following gave it: a ragdoll inherits the motion)
    const BodyType previous = body.type;
    if (previous == BodyType::KINEMATIC && type == BodyType::DYNAMIC){
        body.followingPosition = false;
        body.followingRotation = false;
    }else{
        stopFollowing3D(body, true, true);
    }
    body.type = type;
    body.hasPendingType = false;

    if (body.body.IsInvalid()){
        return true;
    }

    JPH::BodyInterface& bodyInterface = getBodyInterface3D();
    const bool added = bodyInterface.IsAdded(body.body);
    JPH::EMotionType joltType = JPH::EMotionType::Static;
    if (type == BodyType::KINEMATIC) joltType = JPH::EMotionType::Kinematic;
    else if (type == BodyType::DYNAMIC) joltType = JPH::EMotionType::Dynamic;
    bodyInterface.SetMotionType(body.body, joltType, (type != BodyType::STATIC && added) ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (type != BodyType::STATIC){
        bodyInterface.SetMotionQuality(body.body, getRigidbodyMotionQualityToJolt(body.motionQuality));
    }
    {
        JPH::BodyLockWrite lock(getBodyLockInterface3D(), body.body);
        if (lock.Succeeded()){
            lock.GetBody().SetCollideKinematicVsNonDynamic(collidesKinematicVsNonDynamic3D(body));
        }
    }
    // a dynamic body made kinematic stops where it is (Unity isKinematic = true): it moves
    // again only by its Transform or a script
    if (type == BodyType::STATIC || (type == BodyType::KINEMATIC && previous == BodyType::DYNAMIC)){
        bodyInterface.SetLinearAndAngularVelocity(body.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    }
    // the pairs it is in are tested again with its new type
    if (added && previous != type){
        wakeAround3D(body.body);
    }
    return true;
}

void PhysicsSystem::applyPendingTypeChanges3D(){
    if (pendingTypeChanges3D.empty()){
        return;
    }
    std::vector<Entity> pending;
    pending.swap(pendingTypeChanges3D);
    for (Entity entity : pending){
        RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
        if (body && body->hasPendingType){
            setRigidbodyType(entity, *body, body->pendingType);
        }
    }
}

void PhysicsSystem::setBodyPairIgnored3D(Entity a, Entity b, uint32_t reason, bool ignore){
    if (a == NULL_ENTITY || b == NULL_ENTITY || a == b){
        return;
    }
    if (steppingWorld3D){
        // the job threads read the filter: changed after the step
        changePairFilter3D([a, b, reason, ignore](JoltPairFilter3D& filter){ filter.setReason(a, b, reason, ignore); });
        return;
    }
    const bool before = pairFilter3D->isIgnored(a, b);
    pairFilter3D->setReason(a, b, reason, ignore);
    if (before != pairFilter3D->isIgnored(a, b)){
        for (Entity entity : {a, b}){
            RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
            if (body && !body->body.IsInvalid()){
                wakeAround3D(body->body);
            }
        }
    }
}

void PhysicsSystem::changePairFilter3D(const std::function<void(JoltPairFilter3D&)>& change){
    if (steppingWorld3D){
        pendingPairFilter3D.push_back(change);
        return;
    }
    change(*pairFilter3D);
}

bool PhysicsSystem::isTriggerCompanion3D(const JPH::BodyID& body) const{
    return !triggerCompanions3D.empty() && triggerCompanions3D.count(body.GetIndexAndSequenceNumber()) > 0;
}

static_assert((uint8_t)JPH::EAllowedDOFs::Plane2D == (RigidbodyAllowedDOF_TranslationX | RigidbodyAllowedDOF_TranslationY | RigidbodyAllowedDOF_RotationZ)
    && (uint8_t)JPH::EAllowedDOFs::All == RigidbodyAllowedDOF_All, "RigidbodyAllowedDOFFlags must keep Jolt's bits");

bool PhysicsSystem::createGenericJoltBody(Entity entity, RigidbodyComponent& body, const JPH::ShapeRefC shape, const JPH::MassProperties* mass){
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
        Log::warn("Rigidbody entity %u uses a static-only shape (Mesh/HeightField/Plane/...) and cannot be %s. Forcing body type to Static. "
                  "Decompose Into Convex Hulls on a Mesh collider (Inspector, or edit_colliders decompose) lets it move.",
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
    // the contact messages its scripts define, packed in the user data for the job threads
    {
        std::vector<Entity> receivers;
        for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
            const Collider3D& collider = AttachedColliders::colliderAt(body, i);
            if (collider.receiver != NULL_ENTITY) receivers.push_back(collider.receiver);
        }
        body.listenerMask = computeListenerMask(entity, receivers);
    }
    JPH::BodyCreationSettings settings(shape, createPosition, createRotation, joltType, layer);
    settings.mMotionQuality = getRigidbodyMotionQualityToJolt(body.motionQuality);
    settings.mIsSensor = isSensorBody3D(body);
    settings.mGravityFactor = body.gravityFactor;
    settings.mFriction = body.friction;
    settings.mRestitution = body.restitution;
    settings.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(body.allowedDOFs);
    // the Rigidbody fields (their defaults are Jolt's)
    settings.mLinearDamping = std::max(0.0f, body.linearDamping);
    settings.mAngularDamping = std::max(0.0f, body.angularDamping);
    settings.mMaxLinearVelocity = std::max(0.0f, body.maxLinearVelocity);
    settings.mMaxAngularVelocity = std::max(0.0f, body.maxAngularVelocity);
    settings.mAllowSleeping = body.allowSleeping;
    // any body can switch between static, kinematic and dynamic, except one Jolt cannot move
    settings.mAllowDynamicOrKinematic = !shape->MustBeStatic();
    settings.mCollideKinematicVsNonDynamic = collidesKinematicVsNonDynamic3D(body);
    settings.mUseManifoldReduction = usesManifoldReduction3D(body);
    settings.mCollisionGroup = JPH::CollisionGroup(pairFilter3D, body.collisionGroupID, (JPH::CollisionGroup::SubGroupID)entity);
    settings.mUserData = PhysicsCallbacks::packBodyUserData(entity, body.listenerMask);

    JPH::BodyInterface &body_interface = getBodyInterface3D();

    if(body.overrideMassProperties){
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        settings.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(JPH::Vec3(body.solidBoxSize.x, body.solidBoxSize.y, body.solidBoxSize.z), body.solidBoxDensity);
    }else if (mass){
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        settings.mMassPropertiesOverride = *mass;
    }

    // moved here from another scene (persistence): it goes on as it went there
    if (body.hasPendingVelocity){
        body.hasPendingVelocity = false;
        if (joltType != JPH::EMotionType::Static){
            settings.mLinearVelocity = JPH::Vec3(body.pendingLinearVelocity.x, body.pendingLinearVelocity.y, body.pendingLinearVelocity.z);
            settings.mAngularVelocity = JPH::Vec3(body.pendingAngularVelocity.x, body.pendingAngularVelocity.y, body.pendingAngularVelocity.z);
            if (!body.pendingAwake){
                activation = JPH::EActivation::DontActivate;
            }
        }
    }

    JPH::Body* jbody = body_interface.CreateBody(settings);
    if (!jbody){
        objectLayerTable3D->release(layer);
        if (!reportedBodyLimit3D){
            reportedBodyLimit3D = true;
            Log::error("Cannot create the 3D body of '%s': the world holds %u bodies at most. Raise Project Settings > Physics > 3D World Capacity > Max Bodies (physics3DWorld.maxBodies).",
                       scene->getEntityName(entity).c_str(), maxBodies3D);
        }
        return false;
    }

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
        invalidateJoints3D(entity);
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
JPH::BodyID PhysicsSystem::createStaticBodyForShape(Entity entity, const JPH::Shape* shape){
    RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!body || !transform || !shape){
        return JPH::BodyID();
    }
    ensureWorld3D();
    const JPH::ObjectLayer layer = objectLayerTable3D->acquire(scene->getEntityLayer(entity), body->categoryBits, body->maskBits);
    const JPH::Vec3 position(transform->worldPosition.x, transform->worldPosition.y, transform->worldPosition.z);
    const JPH::Quat rotation = toValidatedJoltRotation(transform->worldRotation, entity, -1);
    JPH::BodyCreationSettings settings(shape, position, rotation, JPH::EMotionType::Static, layer);
    settings.mFriction = body->friction;
    settings.mRestitution = body->restitution;
    settings.mCollisionGroup = JPH::CollisionGroup(pairFilter3D, body->collisionGroupID, (JPH::CollisionGroup::SubGroupID)entity);
    settings.mUserData = PhysicsCallbacks::packBodyUserData(entity, 0);
    JPH::Body* created = getBodyInterface3D().CreateBody(settings);
    if (!created){
        objectLayerTable3D->release(layer);
        if (!reportedBodyLimit3D){
            reportedBodyLimit3D = true;
            Log::error("Cannot create the 3D body of '%s': the world holds %u bodies at most. Raise Project Settings > Physics > Max Bodies",
                       scene->getEntityName(entity).c_str(), maxBodies3D);
        }
        return JPH::BodyID();
    }
    body->body = created->GetID();
    body->previousPosition = transform->worldPosition;
    body->previousRotation = transform->worldRotation;
    body->needReloadBody = false;
    body->needUpdateColliders = false;
    return body->body;
}

JPH::BodyInterface::AddState PhysicsSystem::prepareBodies(std::span<JPH::BodyID> ids){
    return getBodyInterface3D().AddBodiesPrepare(ids.data(), static_cast<int>(ids.size()));
}

void PhysicsSystem::finalizeBodies(std::span<JPH::BodyID> ids, JPH::BodyInterface::AddState state){
    getBodyInterface3D().AddBodiesFinalize(ids.data(), static_cast<int>(ids.size()), state, JPH::EActivation::DontActivate);
}

JPH::PhysicsSystem* PhysicsSystem::getWorld3D(){
    ensureWorld3D();
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

    // an attached child has no body: its colliders are its ancestor's (composeAttached2D, which
    // sees its flags at the next step)
    if (body.attachToParentBody && AttachedColliders::findBody2D(scene, entity) != NULL_ENTITY){
        if (b2Body_IsValid(body.body)){
            contactBodyRemoved2D(entity);
            destroyRigidbody2D(body);
        }
        return false;
    }

    if (!b2Body_IsValid(body.body) || body.needReloadBody){
        if (b2Body_IsValid(body.body)){
            b2DestroyBody(body.body);
            body.body = b2_nullBodyId;
        }
        for (Collider2D& collider : body.colliders){
            collider.shapes.clear();
            collider.chain = b2_nullChainId;
        }
        if (body.generated){
            forgetGeneratedShapes2D(*body.generated);
        }
        for (AttachedCollider2D& attached : body.attachedColliders){
            attached.collider.shapes.clear();
            attached.collider.chain = b2_nullChainId;
            attached.collider.dirty = true;
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
        // the Unity Rigidbody2D fields (their defaults are Box2D's)
        bodyDef.gravityScale = std::isfinite(body.gravityScale) ? body.gravityScale : 1.0f;
        bodyDef.linearDamping = std::max(0.0f, body.linearDamping);
        bodyDef.angularDamping = std::max(0.0f, body.angularDamping);
        bodyDef.fixedRotation = body.fixedRotation;
        // not capped by the scene's Max Rotation Per Step (S96 WP-E)
        bodyDef.allowFastRotation = body.allowFastRotation;
        bodyDef.isBullet = (body.collisionDetection == CollisionDetectionMode2D::CONTINUOUS);
        bodyDef.enableSleep = (body.sleepingMode != RigidbodySleepMode2D::NEVER_SLEEP);
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
        body.needApplyMass = false;

        const bool synced = syncRigidbody2DColliders(entity, body, true);
        Rigidbody2DComponent& made = scene->getComponent<Rigidbody2DComponent>(entity);
        if (made.hasPendingVelocity && b2Body_IsValid(made.body)){
            // moved here from another scene (persistence): it goes on as it went there, its
            // mass known now that it has its colliders
            made.hasPendingVelocity = false;
            if (made.type != BodyType::STATIC){
                b2Body_SetLinearVelocity(made.body, {made.pendingLinearVelocity.x, made.pendingLinearVelocity.y});
                b2Body_SetAngularVelocity(made.body, made.pendingAngularVelocity);
                b2Body_SetAwake(made.body, made.pendingAwake);
            }
        }else if (made.sleepingMode == RigidbodySleepMode2D::START_ASLEEP && made.type != BodyType::STATIC){
            // made asleep (after its shapes, whose creation wakes it)
            b2Body_SetAwake(made.body, false);
        }
        return synced;
    }

    Vector2 entityScale = entityWorldScale2D(scene, entity);
    if (entityScale != body.loadedScale){
        body.needUpdateColliders = true;
    }

    if (body.needUpdateColliders){
        return syncRigidbody2DColliders(entity, body, false);
    }
    if (body.needApplyMass){
        applyRigidbody2DMass(entity, body);
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
        // its shapes, chains and joints go with it
        b2DestroyBody(body.body);

        body.body = b2_nullBodyId;
    }
    for (Collider2D& collider : body.colliders){
        collider.shapes.clear();
        collider.chain = b2_nullChainId;
        collider.dirty = true;
    }
    if (body.generated){
        forgetGeneratedShapes2D(*body.generated);
    }
    for (AttachedCollider2D& attached : body.attachedColliders){
        attached.collider.shapes.clear();
        attached.collider.chain = b2_nullChainId;
        attached.collider.dirty = true;
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

// ---- trigger companions ---------------------------------------------------------------------
// Jolt's continuous collision (LinearCast) keeps only the closest hit of a cast and skips sensor
// bodies, not sensor contacts: a trigger collider inside a solid body would be that closest hit
// and the body would pass through what is behind it. So the trigger colliders of a body that
// also has solid ones live on a sensor body of their own, the same GameObject (user data, group,
// layer), moved with the body before each step. Contacts, rays and callbacks report the owning
// body and the trigger's collider index (its leaf keeps it), as before.

void PhysicsSystem::destroyTriggerCompanion3D(RigidbodyComponent& body){
    if (body.triggerBody.IsInvalid()){
        return;
    }
    triggerCompanions3D.erase(body.triggerBody.GetIndexAndSequenceNumber());
    if (steppingWorld3D){
        pendingBodyDestroy3D.push_back(body.triggerBody);
    }else{
        JPH::BodyInterface& body_interface = getBodyInterface3D();
        if (body_interface.IsAdded(body.triggerBody)){
            body_interface.RemoveBody(body.triggerBody);
        }
        releaseObjectLayer3D(body.triggerBody);
        body_interface.DestroyBody(body.triggerBody);
    }
    body.triggerBody = JPH::BodyID();
}

void PhysicsSystem::syncTriggerCompanion3D(Entity entity, RigidbodyComponent& body){
    if (body.body.IsInvalid() || !splitsTriggers3D(body)){
        destroyTriggerCompanion3D(body);
        return;
    }
    std::vector<size_t> triggers;
    for (size_t i = 0; i < AttachedColliders::colliderCount(body); i++){
        const Collider3D& collider = AttachedColliders::colliderAt(body, i);
        if (collider.isTrigger && collider.shape){
            triggers.push_back(i);
        }
    }
    JPH::ShapeRefC shape = composeColliderShape3D(entity, body, triggers);
    if (!shape){
        destroyTriggerCompanion3D(body);
        return;
    }
    JPH::BodyInterface& body_interface = getBodyInterface3D();
    JPH::RVec3 position;
    JPH::Quat rotation;
    body_interface.GetPositionAndRotation(body.body, position, rotation);
    const bool movable = body.type != BodyType::STATIC && !shape->MustBeStatic();
    if (!body.triggerBody.IsInvalid()){
        body_interface.SetShape(body.triggerBody, shape, false, JPH::EActivation::DontActivate);
        body_interface.SetPositionAndRotation(body.triggerBody, position, rotation, JPH::EActivation::DontActivate);
        return;
    }
    const JPH::ObjectLayer layer = objectLayerTable3D->acquire(scene->getEntityLayer(entity), body.categoryBits, body.maskBits);
    JPH::BodyCreationSettings settings(shape, position, rotation, movable ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static, layer);
    settings.mIsSensor = true;
    settings.mAllowDynamicOrKinematic = !shape->MustBeStatic();
    // Unity: a Rigidbody's trigger also sees static colliders
    settings.mCollideKinematicVsNonDynamic = true;
    settings.mAllowSleeping = false;
    settings.mCollisionGroup = JPH::CollisionGroup(pairFilter3D, body.collisionGroupID, (JPH::CollisionGroup::SubGroupID)entity);
    // the owner's GameObject and listener mask (its contacts are the owner's)
    settings.mUserData = PhysicsCallbacks::packBodyUserData(entity, body.listenerMask);
    JPH::Body* companion = body_interface.CreateBody(settings);
    if (!companion){
        objectLayerTable3D->release(layer);
        if (!reportedBodyLimit3D){
            reportedBodyLimit3D = true;
            Log::error("Cannot create the trigger colliders' body of '%s': the world holds %u bodies at most. Raise Project Settings > Physics > 3D World Capacity > Max Bodies (physics3DWorld.maxBodies).",
                       scene->getEntityName(entity).c_str(), maxBodies3D);
        }
        return;
    }
    body.triggerBody = companion->GetID();
    triggerCompanions3D.insert(body.triggerBody.GetIndexAndSequenceNumber());
    if (body_interface.IsAdded(body.body)){
        body_interface.AddBody(body.triggerBody, movable ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    }
}

void PhysicsSystem::followTriggerCompanions3D(){
    if (triggerCompanions3D.empty()){
        return;
    }
    JPH::BodyInterface& body_interface = getBodyInterface3D();
    auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
    for (size_t i = 0; i < bodies3d->size(); i++){
        RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
        if (body.triggerBody.IsInvalid() || body.body.IsInvalid()){
            continue;
        }
        const bool added = body_interface.IsAdded(body.body);
        if (body_interface.IsAdded(body.triggerBody) != added){
            if (added){
                body_interface.AddBody(body.triggerBody, JPH::EActivation::DontActivate);
            }else{
                body_interface.RemoveBody(body.triggerBody);
                continue;
            }
        }
        if (!added){
            continue;
        }
        // static or kinematic like its body (a static body's triggers do not move)
        const JPH::EMotionType wanted = body.type == BodyType::STATIC ? JPH::EMotionType::Static : JPH::EMotionType::Kinematic;
        if (body_interface.GetMotionType(body.triggerBody) != wanted){
            JPH::BodyLockRead lock(getBodyLockInterface3D(), body.triggerBody);
            const bool canMove = lock.Succeeded() && lock.GetBody().CanBeKinematicOrDynamic();
            lock.ReleaseLock();
            if (canMove){
                body_interface.SetMotionType(body.triggerBody, wanted, JPH::EActivation::DontActivate);
            }
        }
        JPH::RVec3 position;
        JPH::Quat rotation;
        body_interface.GetPositionAndRotation(body.body, position, rotation);
        if (body_interface.GetMotionType(body.triggerBody) == JPH::EMotionType::Static){
            body_interface.SetPositionAndRotation(body.triggerBody, position, rotation, JPH::EActivation::DontActivate);
            continue;
        }
        // where its body is, moving as it does over the step (the velocity of its centre of
        // mass as a point of the body)
        const JPH::RVec3 center = body_interface.GetCenterOfMassPosition(body.body);
        const JPH::Vec3 angular = body_interface.GetAngularVelocity(body.body);
        body_interface.SetPositionAndRotation(body.triggerBody, position, rotation, JPH::EActivation::Activate);
        const JPH::RVec3 companionCenter = body_interface.GetCenterOfMassPosition(body.triggerBody);
        const JPH::Vec3 linear = body_interface.GetLinearVelocity(body.body) + angular.Cross(JPH::Vec3(companionCenter - center));
        body_interface.SetLinearAndAngularVelocity(body.triggerBody, linear, angular);
    }
}

void PhysicsSystem::destroyRigidbody(RigidbodyComponent& body){
    destroyTriggerCompanion3D(body);
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

    for (size_t i = 0; i < body.colliders.size(); i++){
        destroyCollider3D(body, i);
    }
    for (AttachedCollider3D& attached : body.attachedColliders){
        attached.collider.shape = nullptr;
        attached.collider.dirty = true;
    }

    body.body = JPH::BodyID();
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
void PhysicsSystem::destroyCollider2D(Rigidbody2DComponent& body, size_t index){
    if (index >= body.colliders.size()){
        return;
    }
    destroyCollider2DHandles(body.colliders[index]);
}

void PhysicsSystem::destroyCollider2DHandles(Collider2D& collider){
    for (const b2ShapeId& shape : collider.shapes){
        if (b2Shape_IsValid(shape)){
            b2DestroyShape(shape, false);
        }
    }
    collider.shapes.clear();
    if (b2Chain_IsValid(collider.chain)){
        b2DestroyChain(collider.chain);
    }
    collider.chain = b2_nullChainId;
}

bool PhysicsSystem::syncGeneratedColliders2D(Entity entity, Rigidbody2DComponent& body, bool all, const Vector2& entityScale, int layer){
    GeneratedColliders2D& generated = *body.generated;
    bool changed = !generated.staleShapes.empty() || !generated.staleChains.empty();
    for (const b2ShapeId& shape : generated.staleShapes){
        if (b2Shape_IsValid(shape)){
            b2DestroyShape(shape, false);
        }
    }
    generated.staleShapes.clear();
    for (const b2ChainId& chain : generated.staleChains){
        if (b2Chain_IsValid(chain)){
            b2DestroyChain(chain);
        }
    }
    generated.staleChains.clear();

    std::vector<int> retired;
    retired.swap(generated.retiredKeys);
    for (int key : retired){
        contactGeneratedRemoved2D(entity, key);
    }

    // in the map's (cy, cx) order: the same edits make the same world
    for (auto& [coord, chunk] : generated.chunks){
        if (!all && !chunk.dirty){
            continue;
        }
        for (Collider2D& collider : chunk.colliders){
            destroyCollider2DHandles(collider);
            createCollider2DShapesOf(entity, body, collider, (size_t)collider.generatedKey, entityScale, layer);
            collider.dirty = false;
            changed = true;
        }
        chunk.dirty = false;
    }
    return changed;
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
int PhysicsSystem::loadCollider3D(RigidbodyComponent& body, const Vector3& position, const Quaternion& rotation, JPH::ShapeSettings* shapeSettings){
    JPH::ShapeSettings::ShapeResult shape_result = shapeSettings->Create();
    if (shape_result.IsValid()){
        JPH::Shape* shape = shape_result.Get();
        const size_t index = body.colliders.size();
        shape->SetUserData(index);

        Collider3D collider;
        collider.shape = shape;
        collider.position = position;
        collider.rotation = rotation;
        // a shape made by hand: kept as it is by the shape-only rebuilds
        collider.dirty = false;
        body.colliders.push_back(collider);

        return (int)index;
    }else{
        Log::error("Cannot create shape for 3D Body");
    }

    return -1;
}

void PhysicsSystem::destroyCollider3D(RigidbodyComponent& body, size_t index){
    if (index >= body.colliders.size()){
        return;
    }
    // ShapeRefC::operator= releases the owned reference; do not call Shape::Release manually.
    body.colliders[index].shape = nullptr;
    body.colliders[index].dirty = true;
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
namespace {

    float nonNegative(float value){
        return (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
    }

    float clampAngleRad(float radians){
        if (!std::isfinite(radians)) return 0.0f;
        return std::min((float)M_PI, std::max(-(float)M_PI, radians));
    }

}

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
    // a joint of a scene saved before local anchors needed both bodies; a new one without
    // bodyB holds bodyA to the world (Unity)
    if (joint.bodyB == NULL_ENTITY && joint.anchorSpace == JointAnchorSpace::WORLD){
        return false;
    }

    return buildJoint2D(entity, joint);
}

// The b2MotorJoint linear offset (Box2D 3.1 holds bodyB's centre minus bodyA's centre at it, in
// world space). LOCAL (Unity RelativeJoint2D): the component's offset is bodyB's origin in bodyA's
// frame, turned into Box2D's before every step as bodyA turns; WORLD: Box2D's own, as given.
static b2Vec2 motorLinearOffset2D(const Joint2DComponent& joint, b2BodyId bodyA, b2BodyId bodyB, float ptm){
    const b2Vec2 offset = {joint.linearOffset.x / ptm, joint.linearOffset.y / ptm};
    if (joint.anchorSpace != JointAnchorSpace::LOCAL || !b2Body_IsValid(bodyA) || !b2Body_IsValid(bodyB)){
        return offset;
    }
    const b2Rot rotationA = b2Body_GetRotation(bodyA);
    return b2Add(b2RotateVector(rotationA, b2Sub(offset, b2Body_GetLocalCenterOfMass(bodyA))),
                 b2RotateVector(b2Body_GetRotation(bodyB), b2Body_GetLocalCenterOfMass(bodyB)));
}

bool PhysicsSystem::buildJoint2D(Entity entity, Joint2DComponent& joint){
    // tried again every step while it cannot be made (a body missing): reported once
    auto jointError = [&joint](const char* message){
        if (!joint.loadFailed){
            Log::error("%s", message);
        }
        joint.loadFailed = true;
    };

    const Entity bodyAEntity = joint.bodyA;
    const Entity bodyBEntity = joint.bodyB;

    if (!scene->isEntityCreated(bodyAEntity) || !scene->findComponent<Rigidbody2DComponent>(bodyAEntity) ||
            (bodyBEntity != NULL_ENTITY && (!scene->isEntityCreated(bodyBEntity) || !scene->findComponent<Rigidbody2DComponent>(bodyBEntity)))){
        jointError("Cannot create joint, error in bodyA or bodyB");
        return false;
    }

    if (joint.type == Joint2DType::MOUSE){
        if (!loadRigidbody2D(bodyAEntity) || (bodyBEntity != NULL_ENTITY && !loadRigidbody2D(bodyBEntity))){
            jointError("Cannot create mouse 2D joint, bodyA or bodyB could not be loaded");
            return false;
        }
        if (scene->getComponent<Rigidbody2DComponent>(bodyAEntity).type != BodyType::DYNAMIC){
            jointError("Cannot create mouse 2D joint, bodyA must be dynamic");
            return false;
        }
        if (bodyAEntity == bodyBEntity){
            jointError("Cannot create mouse 2D joint with the same bodyA and bodyB");
            return false;
        }
    }else{
        if (!b2Body_IsValid(scene->getComponent<Rigidbody2DComponent>(bodyAEntity).body)){
            loadRigidbody2D(bodyAEntity);
        }
        if (bodyBEntity != NULL_ENTITY && !b2Body_IsValid(scene->getComponent<Rigidbody2DComponent>(bodyBEntity).body)){
            loadRigidbody2D(bodyBEntity);
        }
    }

    // the bodies at their Transforms, on copies of their components (the follow state of
    // the step stays as it is)
    auto syncPose = [&](Entity bodyEntity){
        Rigidbody2DComponent& real = scene->getComponent<Rigidbody2DComponent>(bodyEntity);
        std::vector<Collider2D> colliders;
        colliders.swap(real.colliders);
        Rigidbody2DComponent copy = real;
        real.colliders.swap(colliders);
        updateRigidbody2DPosition(scene->getSignature(bodyEntity), bodyEntity, copy);
    };
    syncPose(bodyAEntity);
    if (bodyBEntity != NULL_ENTITY){
        syncPose(bodyBEntity);
    }

    const b2BodyId bodyA = scene->getComponent<Rigidbody2DComponent>(bodyAEntity).body;
    const b2BodyId bodyB = (bodyBEntity != NULL_ENTITY) ? scene->getComponent<Rigidbody2DComponent>(bodyBEntity).body : getGroundBody2D();
    if (!b2Body_IsValid(bodyA) || !b2Body_IsValid(bodyB)){
        jointError("Cannot create 2D joint, bodyA or bodyB has no valid physics body");
        return false;
    }

    const float ptm = pointsToMeterScale2D;
    auto toBox2D = [ptm](const Vector2& value){
        return b2Vec2{value.x / ptm, value.y / ptm};
    };

    const bool local = (joint.anchorSpace == JointAnchorSpace::LOCAL);
    // made again only because a body was (a collider or body edit in Play): a LOCAL joint keeps
    // what it captured when it was made (auto anchor B, length, offsets, reference angle) instead
    // of capturing the poses of now; an edit of the joint itself (needUpdateJoint) captures again
    const bool reuseConfigured = local && !joint.needUpdateJoint && configuredJoints2D.count(entity) > 0;
    const Vector2 scaleA = entityWorldScale2D(scene, bodyAEntity);
    const Vector2 scaleB = (bodyBEntity != NULL_ENTITY) ? entityWorldScale2D(scene, bodyBEntity) : Vector2(1.0f, 1.0f);

    // the anchors in each body's frame. LOCAL: as given (GameObject units, scaled), anchorB
    // where anchorA is now with autoAnchors (Unity autoConfigureConnectedAnchor, written back);
    // WORLD: the world points of the scene's time, as joints always were made
    b2Vec2 localAnchorA = b2Vec2_zero;
    b2Vec2 localAnchorB = b2Vec2_zero;
    b2Vec2 worldPivotA = b2Vec2_zero;
    b2Vec2 worldPivotB = b2Vec2_zero;
    if (local){
        localAnchorA = toBox2D(joint.anchorA * scaleA);
        if (joint.autoAnchors && joint.type != Joint2DType::DISTANCE && !reuseConfigured){
            localAnchorB = b2Body_GetLocalPoint(bodyB, b2Body_GetWorldPoint(bodyA, localAnchorA));
            if (scaleB.x != 0.0f && scaleB.y != 0.0f){
                joint.anchorB = Vector2(localAnchorB.x * ptm / scaleB.x, localAnchorB.y * ptm / scaleB.y);
            }
        }else{
            localAnchorB = toBox2D(joint.anchorB * scaleB);
        }
        worldPivotA = b2Body_GetWorldPoint(bodyA, localAnchorA);
        worldPivotB = b2Body_GetWorldPoint(bodyB, localAnchorB);
    }else if (joint.type == Joint2DType::DISTANCE){
        worldPivotA = toBox2D(joint.anchorA);
        worldPivotB = toBox2D(joint.anchorB);
        if (joint.autoAnchors){
            worldPivotA = b2Body_GetPosition(bodyA);
            worldPivotB = b2Body_GetPosition(bodyB);
        }else if (joint.anchorA == Vector2::ZERO && joint.anchorB == Vector2::ZERO){
            jointError("Cannot create distance 2D joint: Auto Anchors is disabled and both anchors are zero");
            return false;
        }
        localAnchorA = b2Body_GetLocalPoint(bodyA, worldPivotA);
        localAnchorB = b2Body_GetLocalPoint(bodyB, worldPivotB);
    }else{
        const b2Vec2 worldPivot = toBox2D(joint.anchorA);
        worldPivotA = worldPivot;
        worldPivotB = worldPivot;
        localAnchorA = b2Body_GetLocalPoint(bodyA, worldPivot);
        localAnchorB = b2Body_GetLocalPoint(bodyB, worldPivot);
    }

    // the line of motion in bodyA's frame
    auto localAxis = [&](const char* typeName){
        Vector2 axis = local ? joint.axis * scaleA : joint.axis;
        if (axis.x * axis.x + axis.y * axis.y < 1.0e-6f){
            Log::warn("%s 2D joint axis is zero. Using default axis (1, 0).", typeName);
            axis = Vector2(1.0f, 0.0f);
        }
        if (local){
            return b2Normalize(b2Vec2{axis.x, axis.y});
        }
        return b2Body_GetLocalVector(bodyA, b2Vec2{axis.x, axis.y});
    };

    // the relative angle the joint keeps; 0 for joints of scenes saved before (as they were)
    const float referenceAngle = !local ? 0.0f
        : (reuseConfigured ? joint.referenceAngle : b2RelativeAngle(b2Body_GetRotation(bodyB), b2Body_GetRotation(bodyA)));
    joint.referenceAngle = referenceAngle;

    void* userData = reinterpret_cast<void*>((uint64_t)entity);
    b2JointId created = b2_nullJointId;

    switch (joint.type){
        case Joint2DType::DISTANCE: {
            b2DistanceJointDef jointDef = b2DefaultDistanceJointDef();
            jointDef.bodyIdA = bodyA;
            jointDef.bodyIdB = bodyB;
            jointDef.localAnchorA = localAnchorA;
            jointDef.localAnchorB = localAnchorB;
            jointDef.length = b2Distance(worldPivotA, worldPivotB);
            if (!joint.autoLength || reuseConfigured){
                jointDef.length = nonNegative(joint.length) / ptm;
            }else{
                joint.length = jointDef.length * ptm;
            }
            // Box2D asserts a length above 0 (two anchors at one point), then clamps to its linear slop
            jointDef.length = std::max(jointDef.length, 0.005f);
            jointDef.enableSpring = joint.enableSpring;
            jointDef.hertz = nonNegative(joint.frequency);
            jointDef.dampingRatio = nonNegative(joint.dampingRatio);
            if (joint.rope){
                // a zero-hertz spring frees the length, the limit caps it
                jointDef.enableSpring = true;
                if (!joint.enableSpring){
                    jointDef.hertz = 0.0f;
                    jointDef.dampingRatio = 0.0f;
                }
                jointDef.enableLimit = true;
                jointDef.minLength = joint.useLimits ? nonNegative(joint.minLength) / ptm : 0.0f;
                jointDef.maxLength = jointDef.length;
            }else if (joint.useLimits){
                jointDef.enableLimit = true;
                jointDef.minLength = nonNegative(joint.minLength) / ptm;
                jointDef.maxLength = joint.maxLength > 0.0f ? nonNegative(joint.maxLength) / ptm : jointDef.length;
            }
            jointDef.enableMotor = joint.useMotor;
            jointDef.maxMotorForce = nonNegative(joint.maxMotorForce);
            jointDef.motorSpeed = std::isfinite(joint.motorSpeed) ? joint.motorSpeed / ptm : 0.0f;
            jointDef.collideConnected = joint.enableCollision;
            jointDef.userData = userData;
            created = b2CreateDistanceJoint(world2D, &jointDef);
            break;
        }
        case Joint2DType::REVOLUTE: {
            b2RevoluteJointDef jointDef = b2DefaultRevoluteJointDef();
            jointDef.bodyIdA = bodyA;
            jointDef.bodyIdB = bodyB;
            jointDef.localAnchorA = localAnchorA;
            jointDef.localAnchorB = localAnchorB;
            jointDef.referenceAngle = referenceAngle;
            jointDef.enableSpring = joint.enableSpring;
            jointDef.hertz = nonNegative(joint.frequency);
            jointDef.dampingRatio = nonNegative(joint.dampingRatio);
            jointDef.enableLimit = joint.useLimits;
            const float lower = clampAngleRad(Angle::defaultToRad(joint.lowerAngle));
            const float upper = clampAngleRad(Angle::defaultToRad(joint.upperAngle));
            jointDef.lowerAngle = std::min(lower, upper);
            jointDef.upperAngle = std::max(lower, upper);
            jointDef.enableMotor = joint.useMotor;
            jointDef.maxMotorTorque = nonNegative(joint.maxMotorTorque);
            jointDef.motorSpeed = std::isfinite(joint.motorSpeed) ? Angle::defaultToRad(joint.motorSpeed) : 0.0f;
            jointDef.collideConnected = joint.enableCollision;
            jointDef.userData = userData;
            created = b2CreateRevoluteJoint(world2D, &jointDef);
            break;
        }
        case Joint2DType::PRISMATIC: {
            b2PrismaticJointDef jointDef = b2DefaultPrismaticJointDef();
            jointDef.bodyIdA = bodyA;
            jointDef.bodyIdB = bodyB;
            jointDef.localAnchorA = localAnchorA;
            jointDef.localAnchorB = localAnchorB;
            jointDef.localAxisA = localAxis("Prismatic");
            joint.localAxisA = Vector2(b2Normalize(jointDef.localAxisA).x, b2Normalize(jointDef.localAxisA).y);
            jointDef.referenceAngle = referenceAngle;
            jointDef.enableSpring = joint.enableSpring;
            jointDef.hertz = nonNegative(joint.frequency);
            jointDef.dampingRatio = nonNegative(joint.dampingRatio);
            jointDef.enableLimit = joint.useLimits;
            const float lower = std::isfinite(joint.lowerTranslation) ? joint.lowerTranslation / ptm : 0.0f;
            const float upper = std::isfinite(joint.upperTranslation) ? joint.upperTranslation / ptm : 0.0f;
            jointDef.lowerTranslation = std::min(lower, upper);
            jointDef.upperTranslation = std::max(lower, upper);
            jointDef.enableMotor = joint.useMotor;
            jointDef.maxMotorForce = nonNegative(joint.maxMotorForce);
            jointDef.motorSpeed = std::isfinite(joint.motorSpeed) ? joint.motorSpeed / ptm : 0.0f;
            jointDef.collideConnected = joint.enableCollision;
            jointDef.userData = userData;
            created = b2CreatePrismaticJoint(world2D, &jointDef);
            break;
        }
        case Joint2DType::MOUSE: {
            b2MouseJointDef jointDef = b2DefaultMouseJointDef();
            // Box2D drags its bodyB: the joint's body
            jointDef.bodyIdA = bodyB;
            jointDef.bodyIdB = bodyA;
            jointDef.target = toBox2D(joint.target);
            jointDef.hertz = nonNegative(joint.frequency);
            jointDef.dampingRatio = nonNegative(joint.dampingRatio);
            jointDef.maxForce = joint.maxForce > 0.0f ? nonNegative(joint.maxForce) : std::max(10.0f, 1000.0f * b2Body_GetMass(bodyA));
            jointDef.collideConnected = joint.enableCollision;
            jointDef.userData = userData;
            created = b2CreateMouseJoint(world2D, &jointDef);
            break;
        }
        case Joint2DType::WHEEL: {
            b2WheelJointDef jointDef = b2DefaultWheelJointDef();
            jointDef.bodyIdA = bodyA;
            jointDef.bodyIdB = bodyB;
            jointDef.localAnchorA = localAnchorA;
            jointDef.localAnchorB = localAnchorB;
            jointDef.localAxisA = localAxis("Wheel");
            joint.localAxisA = Vector2(b2Normalize(jointDef.localAxisA).x, b2Normalize(jointDef.localAxisA).y);
            jointDef.enableSpring = joint.enableSpring;
            jointDef.hertz = nonNegative(joint.frequency);
            jointDef.dampingRatio = nonNegative(joint.dampingRatio);
            jointDef.enableLimit = joint.useLimits;
            const float lower = std::isfinite(joint.lowerTranslation) ? joint.lowerTranslation / ptm : 0.0f;
            const float upper = std::isfinite(joint.upperTranslation) ? joint.upperTranslation / ptm : 0.0f;
            jointDef.lowerTranslation = std::min(lower, upper);
            jointDef.upperTranslation = std::max(lower, upper);
            jointDef.enableMotor = joint.useMotor;
            jointDef.maxMotorTorque = nonNegative(joint.maxMotorTorque);
            jointDef.motorSpeed = std::isfinite(joint.motorSpeed) ? Angle::defaultToRad(joint.motorSpeed) : 0.0f;
            jointDef.collideConnected = joint.enableCollision;
            jointDef.userData = userData;
            created = b2CreateWheelJoint(world2D, &jointDef);
            break;
        }
        case Joint2DType::WELD: {
            b2WeldJointDef jointDef = b2DefaultWeldJointDef();
            jointDef.bodyIdA = bodyA;
            jointDef.bodyIdB = bodyB;
            jointDef.localAnchorA = localAnchorA;
            jointDef.localAnchorB = localAnchorB;
            jointDef.referenceAngle = referenceAngle;
            jointDef.linearHertz = nonNegative(joint.linearFrequency);
            jointDef.angularHertz = nonNegative(joint.angularFrequency);
            jointDef.linearDampingRatio = nonNegative(joint.linearDampingRatio);
            jointDef.angularDampingRatio = nonNegative(joint.angularDampingRatio);
            jointDef.collideConnected = joint.enableCollision;
            jointDef.userData = userData;
            created = b2CreateWeldJoint(world2D, &jointDef);
            break;
        }
        case Joint2DType::MOTOR: {
            b2MotorJointDef jointDef = b2DefaultMotorJointDef();
            jointDef.bodyIdA = bodyA;
            jointDef.bodyIdB = bodyB;
            if (joint.autoOffset && !reuseConfigured){
                // holds the pose the bodies have now (Unity autoConfigureOffset, written back):
                // LOCAL, bodyB's origin in bodyA's frame (Unity); WORLD, Box2D's own offset
                const b2Vec2 offset = local ? b2Body_GetLocalPoint(bodyA, b2Body_GetPosition(bodyB))
                                            : b2Sub(b2Body_GetWorldCenterOfMass(bodyB), b2Body_GetWorldCenterOfMass(bodyA));
                joint.linearOffset = Vector2(offset.x * ptm, offset.y * ptm);
                joint.angularOffset = Angle::radToDefault(b2RelativeAngle(b2Body_GetRotation(bodyB), b2Body_GetRotation(bodyA)));
            }
            jointDef.linearOffset = motorLinearOffset2D(joint, bodyA, bodyB, ptm);
            jointDef.angularOffset = std::isfinite(joint.angularOffset) ? Angle::defaultToRad(joint.angularOffset) : 0.0f;
            jointDef.maxForce = nonNegative(joint.maxForce);
            jointDef.maxTorque = nonNegative(joint.maxTorque);
            jointDef.correctionFactor = std::min(1.0f, nonNegative(joint.correctionFactor));
            jointDef.collideConnected = joint.enableCollision;
            jointDef.userData = userData;
            created = b2CreateMotorJoint(world2D, &jointDef);
            break;
        }
        default:
            return false;
    }

    if (!b2Joint_IsValid(created)){
        jointError("Cannot create 2D joint");
        return false;
    }

    joint.joint = created;
    joint.loadFailed = false;
    joint.needApplyParameters = false;
    joint.broken = false;
    joint.breakArmed = true;
    configuredJoints2D.insert(entity);
    return true;
}

void PhysicsSystem::applyJoint2DParameters(Joint2DComponent& joint){
    joint.needApplyParameters = false;
    if (!b2Joint_IsValid(joint.joint) || steppingWorld2D){
        joint.needApplyParameters = b2Joint_IsValid(joint.joint);
        return;
    }

    const float ptm = pointsToMeterScale2D;
    const b2JointId id = joint.joint;

    switch (joint.type){
        case Joint2DType::DISTANCE: {
            // Box2D's length setters drop the joint's impulses (its warm start): only on a change
            auto lengthOf = [](float metres){
                return std::min(100000.0f, std::max(0.005f, metres));
            };
            auto differs = [](float a, float b){
                return std::fabs(a - b) > 1.0e-6f * std::max(1.0f, std::fabs(b));
            };
            if (!joint.autoLength && differs(b2DistanceJoint_GetLength(id), lengthOf(nonNegative(joint.length) / ptm))){
                b2DistanceJoint_SetLength(id, nonNegative(joint.length) / ptm);
            }
            const float length = b2DistanceJoint_GetLength(id);
            auto setRange = [&](float minLength, float maxLength){
                const float lower = std::min(lengthOf(minLength), lengthOf(maxLength));
                const float upper = std::max(lengthOf(minLength), lengthOf(maxLength));
                if (differs(b2DistanceJoint_GetMinLength(id), lower) || differs(b2DistanceJoint_GetMaxLength(id), upper)){
                    b2DistanceJoint_SetLengthRange(id, minLength, maxLength);
                }
            };
            b2DistanceJoint_EnableSpring(id, joint.enableSpring || joint.rope);
            b2DistanceJoint_SetSpringHertz(id, (joint.rope && !joint.enableSpring) ? 0.0f : nonNegative(joint.frequency));
            b2DistanceJoint_SetSpringDampingRatio(id, (joint.rope && !joint.enableSpring) ? 0.0f : nonNegative(joint.dampingRatio));
            if (joint.rope){
                b2DistanceJoint_EnableLimit(id, true);
                setRange(joint.useLimits ? nonNegative(joint.minLength) / ptm : 0.0f, length);
            }else{
                b2DistanceJoint_EnableLimit(id, joint.useLimits);
                if (joint.useLimits){
                    setRange(nonNegative(joint.minLength) / ptm, joint.maxLength > 0.0f ? nonNegative(joint.maxLength) / ptm : length);
                }
            }
            b2DistanceJoint_EnableMotor(id, joint.useMotor);
            b2DistanceJoint_SetMaxMotorForce(id, nonNegative(joint.maxMotorForce));
            b2DistanceJoint_SetMotorSpeed(id, std::isfinite(joint.motorSpeed) ? joint.motorSpeed / ptm : 0.0f);
            break;
        }
        case Joint2DType::REVOLUTE: {
            b2RevoluteJoint_EnableSpring(id, joint.enableSpring);
            b2RevoluteJoint_SetSpringHertz(id, nonNegative(joint.frequency));
            b2RevoluteJoint_SetSpringDampingRatio(id, nonNegative(joint.dampingRatio));
            b2RevoluteJoint_EnableLimit(id, joint.useLimits);
            b2RevoluteJoint_SetLimits(id, clampAngleRad(Angle::defaultToRad(joint.lowerAngle)), clampAngleRad(Angle::defaultToRad(joint.upperAngle)));
            b2RevoluteJoint_EnableMotor(id, joint.useMotor);
            b2RevoluteJoint_SetMaxMotorTorque(id, nonNegative(joint.maxMotorTorque));
            b2RevoluteJoint_SetMotorSpeed(id, std::isfinite(joint.motorSpeed) ? Angle::defaultToRad(joint.motorSpeed) : 0.0f);
            break;
        }
        case Joint2DType::PRISMATIC: {
            b2PrismaticJoint_EnableSpring(id, joint.enableSpring);
            b2PrismaticJoint_SetSpringHertz(id, nonNegative(joint.frequency));
            b2PrismaticJoint_SetSpringDampingRatio(id, nonNegative(joint.dampingRatio));
            b2PrismaticJoint_EnableLimit(id, joint.useLimits);
            const float lower = std::isfinite(joint.lowerTranslation) ? joint.lowerTranslation / ptm : 0.0f;
            const float upper = std::isfinite(joint.upperTranslation) ? joint.upperTranslation / ptm : 0.0f;
            b2PrismaticJoint_SetLimits(id, std::min(lower, upper), std::max(lower, upper));
            b2PrismaticJoint_EnableMotor(id, joint.useMotor);
            b2PrismaticJoint_SetMaxMotorForce(id, nonNegative(joint.maxMotorForce));
            b2PrismaticJoint_SetMotorSpeed(id, std::isfinite(joint.motorSpeed) ? joint.motorSpeed / ptm : 0.0f);
            break;
        }
        case Joint2DType::MOUSE: {
            b2MouseJoint_SetTarget(id, b2Vec2{joint.target.x / ptm, joint.target.y / ptm});
            b2MouseJoint_SetSpringHertz(id, nonNegative(joint.frequency));
            b2MouseJoint_SetSpringDampingRatio(id, nonNegative(joint.dampingRatio));
            const b2BodyId dragged = b2Joint_GetBodyB(id);
            b2MouseJoint_SetMaxForce(id, joint.maxForce > 0.0f ? nonNegative(joint.maxForce) : std::max(10.0f, 1000.0f * b2Body_GetMass(dragged)));
            break;
        }
        case Joint2DType::WHEEL: {
            b2WheelJoint_EnableSpring(id, joint.enableSpring);
            b2WheelJoint_SetSpringHertz(id, nonNegative(joint.frequency));
            b2WheelJoint_SetSpringDampingRatio(id, nonNegative(joint.dampingRatio));
            b2WheelJoint_EnableLimit(id, joint.useLimits);
            const float lower = std::isfinite(joint.lowerTranslation) ? joint.lowerTranslation / ptm : 0.0f;
            const float upper = std::isfinite(joint.upperTranslation) ? joint.upperTranslation / ptm : 0.0f;
            b2WheelJoint_SetLimits(id, std::min(lower, upper), std::max(lower, upper));
            b2WheelJoint_EnableMotor(id, joint.useMotor);
            b2WheelJoint_SetMaxMotorTorque(id, nonNegative(joint.maxMotorTorque));
            b2WheelJoint_SetMotorSpeed(id, std::isfinite(joint.motorSpeed) ? Angle::defaultToRad(joint.motorSpeed) : 0.0f);
            break;
        }
        case Joint2DType::WELD: {
            b2WeldJoint_SetLinearHertz(id, nonNegative(joint.linearFrequency));
            b2WeldJoint_SetAngularHertz(id, nonNegative(joint.angularFrequency));
            b2WeldJoint_SetLinearDampingRatio(id, nonNegative(joint.linearDampingRatio));
            b2WeldJoint_SetAngularDampingRatio(id, nonNegative(joint.angularDampingRatio));
            break;
        }
        case Joint2DType::MOTOR: {
            if (!joint.autoOffset){
                b2MotorJoint_SetLinearOffset(id, motorLinearOffset2D(joint, b2Joint_GetBodyA(id), b2Joint_GetBodyB(id), ptm));
                b2MotorJoint_SetAngularOffset(id, std::isfinite(joint.angularOffset) ? Angle::defaultToRad(joint.angularOffset) : 0.0f);
            }
            b2MotorJoint_SetMaxForce(id, nonNegative(joint.maxForce));
            b2MotorJoint_SetMaxTorque(id, nonNegative(joint.maxTorque));
            b2MotorJoint_SetCorrectionFactor(id, std::min(1.0f, nonNegative(joint.correctionFactor)));
            break;
        }
        default:
            break;
    }

    b2Joint_SetCollideConnected(id, joint.enableCollision);
    b2Joint_WakeBodies(id);
}

bool PhysicsSystem::loadDistanceJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchorA, Vector2 anchorB, bool autoAnchors, bool rope){
    joint.type = Joint2DType::DISTANCE;
    joint.anchorSpace = JointAnchorSpace::WORLD;
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchorA = anchorA;
    joint.anchorB = anchorB;
    joint.autoAnchors = autoAnchors;
    joint.rope = rope;
    return loadJoint2D(entity, joint);
}

bool PhysicsSystem::loadRevoluteJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor){
    joint.type = Joint2DType::REVOLUTE;
    joint.anchorSpace = JointAnchorSpace::WORLD;
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchorA = anchor;
    return loadJoint2D(entity, joint);
}

bool PhysicsSystem::loadPrismaticJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor, Vector2 axis){
    joint.type = Joint2DType::PRISMATIC;
    joint.anchorSpace = JointAnchorSpace::WORLD;
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchorA = anchor;
    joint.axis = axis;
    return loadJoint2D(entity, joint);
}

bool PhysicsSystem::loadMouseJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 target){
    joint.type = Joint2DType::MOUSE;
    joint.anchorSpace = JointAnchorSpace::WORLD;
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.target = target;
    return loadJoint2D(entity, joint);
}

bool PhysicsSystem::loadWheelJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor, Vector2 axis){
    joint.type = Joint2DType::WHEEL;
    joint.anchorSpace = JointAnchorSpace::WORLD;
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchorA = anchor;
    joint.axis = axis;
    return loadJoint2D(entity, joint);
}

bool PhysicsSystem::loadWeldJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB, Vector2 anchor){
    joint.type = Joint2DType::WELD;
    joint.anchorSpace = JointAnchorSpace::WORLD;
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    joint.anchorA = anchor;
    return loadJoint2D(entity, joint);
}

bool PhysicsSystem::loadMotorJoint2D(Entity entity, Joint2DComponent& joint, Entity bodyA, Entity bodyB){
    joint.type = Joint2DType::MOTOR;
    joint.anchorSpace = JointAnchorSpace::WORLD;
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;
    return loadJoint2D(entity, joint);
}

void PhysicsSystem::destroyJoint2D(Joint2DComponent& joint){
    if (b2Joint_IsValid(joint.joint)){
        b2DestroyJoint(joint.joint);
    }
    joint.joint = b2_nullJointId;
}

void PhysicsSystem::checkJointBreaks2D(){
    auto joints2d = scene->getComponentArray<Joint2DComponent>();
    for (size_t i = 0; i < joints2d->size(); i++){
        Joint2DComponent& joint = joints2d->getComponentFromIndex(i);
        // broken with DISABLE, then enabled again some other way (Inspector, Catalog): it can break again
        if (joint.broken && joint.enabled && joint.breakAction != JointBreakAction::DESTROY){
            joint.broken = false;
            joint.breakArmed = true;
        }
        if (joint.broken || !b2Joint_IsValid(joint.joint)){
            continue;
        }
        const bool finiteForce = std::isfinite(joint.breakForce);
        const bool finiteTorque = std::isfinite(joint.breakTorque);
        if (!finiteForce && !finiteTorque){
            continue;
        }

        const float force = b2Length(b2Joint_GetConstraintForce(joint.joint));
        const float torque = std::fabs(b2Joint_GetConstraintTorque(joint.joint));
        const bool over = (finiteForce && force > joint.breakForce) || (finiteTorque && torque > joint.breakTorque);
        if (!over){
            // a callback-only joint reports again once the load went back under the thresholds
            joint.breakArmed = true;
            continue;
        }
        if (!joint.breakArmed || joint.breakAction == JointBreakAction::IGNORE){
            continue;
        }

        const Entity entity = joints2d->getEntity(i);
        joint.breakArmed = false;
        switch (joint.breakAction){
            case JointBreakAction::CALLBACK_ONLY:
                pendingJointBreaks2D.push_back({entity, joint.bodyA, false});
                break;
            case JointBreakAction::DISABLE:
                // leaves the world before the next step, as any disabled joint
                joint.enabled = false;
                joint.broken = true;
                pendingJointBreaks2D.push_back({entity, joint.bodyA, false});
                break;
            case JointBreakAction::DESTROY:
                joint.broken = true;
                pendingJointBreaks2D.push_back({entity, joint.bodyA, true});
                break;
            default:
                break;
        }
    }
}

void PhysicsSystem::dispatchJointBreaks2D(){
    if (pendingJointBreaks2D.empty()){
        return;
    }
    std::vector<JointBreak2D> breaks;
    breaks.swap(pendingJointBreaks2D);

    // onJointBreak2D(joint) on the joint's GameObject, and on bodyA's when it is another one
    // (Unity's joint lives on its Rigidbody 2D)
    for (const JointBreak2D& jointBreak : breaks){
        const Entity jointEntity = jointBreak.joint;
        auto pushJoint = [this, jointEntity](lua_State* L){
            if (!luabridge::push(L, Joint2D(scene, jointEntity))){
                lua_pushnil(L);
            }
            return 1;
        };
        auto callCpp = [this, jointEntity](ScriptBase& script){
            Joint2D joint(scene, jointEntity);
            script.onJointBreak2D(joint);
        };
        // a contact callback of the step may have removed the joint already: no message then
        if (!scene->isEntityCreated(jointEntity) || !scene->findComponent<Joint2DComponent>(jointEntity)){
            continue;
        }
        ScriptMessages::send(scene, jointEntity, ScriptMessage::JointBreak2D, pushJoint, callCpp);
        if (jointBreak.bodyA != NULL_ENTITY && jointBreak.bodyA != jointEntity && scene->isEntityCreated(jointBreak.bodyA) &&
                scene->findComponent<Joint2DComponent>(jointEntity)){
            ScriptMessages::send(scene, jointBreak.bodyA, ScriptMessage::JointBreak2D, pushJoint, callCpp);
        }
    }

    // DESTROY: the component goes after every callback of the step
    for (const JointBreak2D& jointBreak : breaks){
        if (!jointBreak.destroy || !scene->isEntityCreated(jointBreak.joint)){
            continue;
        }
        Joint2DComponent* joint = scene->findComponent<Joint2DComponent>(jointBreak.joint);
        if (joint && joint->broken){
            scene->removeComponent<Joint2DComponent>(jointBreak.joint);
        }
    }
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
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

        if (body.attachToParentBody && attachChild2D(entity, body)){
            continue;
        }
        if (loadRigidbody2D(entity) && b2Body_IsValid(body.body)){
            updateRigidbody2DPosition(scene->getSignature(entity), entity, body);
        }
    }
    composeAttached2D();

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

        if (body.attachToParentBody && attachChild3D(entity, body)){
            continue;
        }
        if (loadRigidbody(entity) && !body.body.IsInvalid()){
            updateRigidbodyPosition(scene->getSignature(entity), entity, body);
        }
    }
    // S96 WP-C: the children's colliders in their ancestors' bodies from the first frame
    composeAttached3D();

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

    [[maybe_unused]] const float fixedStep = (float)dt;

    // world settings changed since the last step (S96 WP-E)
    applyWorldSettings();

    // scripts defining contact messages appeared or went since the last step
    if (listenerMasksDirty){
        refreshListenerMasks();
    }

#ifdef JASPERENGINE_PHYSICS_2D
	auto bodies2d = scene->getComponentArray<Rigidbody2DComponent>();

	for (int i = 0; i < bodies2d->size(); i++){
		Rigidbody2DComponent& body = bodies2d->getComponentFromIndex(i);
		Entity entity = bodies2d->getEntity(i);
		Signature signature = scene->getSignature(entity);

        // S96 WP-C: an attached child's colliders are its ancestor's
        if (body.attachToParentBody && attachChild2D(entity, body)){
            continue;
        }
        if (body.hasSliceColliders){
            updateSliceColliders2D(entity, body);
        }
        if (!b2Body_IsValid(body.body) || body.needReloadBody || body.needUpdateColliders || body.needApplyMass || entityWorldScale2D(scene, entity) != body.loadedScale){
            loadRigidbody2D(entity);
        }
        if (body.needApplyParameters && b2Body_IsValid(body.body)){
            body.needApplyParameters = false;
            applyRigidbody2DParameters(entity, body);
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
                // Collision2D.relativeVelocity of the pairs that start in this step
                body.preStepLinearVelocity = b2Body_GetLinearVelocity(body.body);

                body.newBody = false;
            }
        }
    }
    composeAttached2D();
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
        }else if (joint.needApplyParameters){
            applyJoint2DParameters(joint);
        }
    }

    // With no 2D bodies the world was never created (lazy): skip all Box2D stepping/events.
    if (b2World_IsValid(world2D)){
        syncLayerCollisionMatrix2D();
        // a material asset or a script's PhysicsMaterial changed
        syncMaterials2D();

        if (bodies2d->size() > 0){
            // LOCAL motor joints: their offset follows bodyA's angle
            for (size_t i = 0; i < joints2d->size(); i++){
                const Joint2DComponent& joint = joints2d->getComponentFromIndex(i);
                if (joint.type == Joint2DType::MOTOR && joint.anchorSpace == JointAnchorSpace::LOCAL && b2Joint_IsValid(joint.joint)){
                    b2MotorJoint_SetLinearOffset(joint.joint, motorLinearOffset2D(joint, b2Joint_GetBodyA(joint.joint),
                                                                                  b2Joint_GetBodyB(joint.joint), pointsToMeterScale2D));
                }
            }
            steppingWorld2D = true;
            {
                // what the PreSolve and filter callbacks destroy goes after the step
                StructuralDeferScope deferScope(*scene);
                b2World_Step(world2D, fixedStep, subSteps2D);
            }
            steppingWorld2D = false;

            // reaction forces of this step against breakForce / breakTorque
            checkJointBreaks2D();
        }

        // layers changed by callbacks inside the step
        for (size_t i = 0; i < pendingLayerRefresh2D.size(); i++){
            refreshRigidbody2DLayer(pendingLayerRefresh2D[i]);
        }
        pendingLayerRefresh2D.clear();
        // Physics2D.ignoreCollision changed by callbacks inside the step
        if (!pendingRefilter2D.empty()){
            std::vector<Entity> refilter;
            refilter.swap(pendingRefilter2D);
            for (Entity entity : refilter){
                repairBody2D(entity);
            }
        }

        // the moved bodies' Transforms, parents first (writeBackBodies2D)
        b2BodyEvents events = b2World_GetBodyEvents(world2D);
        movedBodies2D.clear();
        for (int i = 0; i < events.moveCount; ++i){
            const b2BodyMoveEvent* event = events.moveEvents + i;
            Entity entity = reinterpret_cast<uintptr_t>(event->userData);
            if (scene->findComponent<Rigidbody2DComponent>(entity)){
                movedBodies2D.push_back(entity);
            }
        }
        writeBackBodies2D(1.0f, false);

        // the global events, then the per-object callbacks (physics-contacts 6.1.4)
        processContacts2D();
        // after the contact events of the step (physics-contacts D10)
        dispatchJointBreaks2D();
    }

    // the 2D controllers ride what moved in the step and see what is under them now
    characters2D->postStep(fixedStep);
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    lastFixedStep = fixedStep;

    // a material asset or a script's PhysicsMaterial changed
    syncMaterials3D();
    // the ragdolls switch their parts before the bodies sync
    updateRagdolls();
    // terrain edits reach their heightfields; what must be built again is, in the sync below
#if JASPERENGINE_TERRAIN_EDIT
    JoltTerrainRefresh::apply(scene, *this);
#endif

    auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
    bool stepped3D = false;

	for (int i = 0; i < bodies3d->size(); i++){
		RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
		Entity entity = bodies3d->getEntity(i);
		Signature signature = scene->getSignature(entity);

        // S96 WP-C: an attached child's colliders are its ancestor's
        if (body.attachToParentBody && attachChild3D(entity, body)){
            continue;
        }
        if (body.body.IsInvalid() || body.needReloadBody || body.needUpdateColliders || body.massDirty || entityWorldScale(scene, entity) != body.loadedScale){
            if (!body.body.IsInvalid() && !body.needReloadBody && (body.needUpdateColliders || body.massDirty) && entityWorldScale(scene, entity) == body.loadedScale){
                // collider or mass edits: the shape is swapped, the body stays
                reshapeRigidbody3D(entity, body);
            }else{
                loadRigidbody(entity);
            }
        }
        if (body.needApplyParameters && !body.body.IsInvalid()){
            body.needApplyParameters = false;
            applyRigidbodyParameters(entity, body);
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
    composeAttached3D();
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
        }else if (joint.needApplyParameters){
            applyJoint3DParameters(entity, joint);
        }
    }

    syncLayerCollisionMatrix3D();

    // characters made and removed as their components and GameObjects come and go, and moved
    // where their Transforms were put; the first one makes the 3D world
    if (scene->getComponentArray<CharacterControllerComponent>()->size() > 0){
        ensureWorld3D();
    }
    characters3D->preStep();
    vehicles3D->preStep();

    // the world steps for the characters too (their platforms, root motion, inner bodies)
    if ((bodies3d->size() > 0 || characters3D->count() > 0) && world3DReady){
		// the scene setting physics3DCollisionSteps (1 by default)
		const int cCollisionSteps = worldSettings3D.collisionSteps;

		// Clears the flag and drains the teardown queue from the destructor, so a C++
		// subscriber throwing out of a callback leaves neither behind.
		struct StepScope {
			PhysicsSystem* system;
			explicit StepScope(PhysicsSystem* s): system(s) { s->steppingWorld3D = true; }
			~StepScope() { system->steppingWorld3D = false; system->flushPendingDestroys3D(); }
		};

		// the trigger colliders' bodies with their bodies, the damper-only drives' forces
		followTriggerCompanions3D();
		updateSixDOFDampers3D();

		// the bodies floating in a library:water (Water.h)
		Water::applyBuoyancy(scene, fixedStep);
		// and the ones a Buoyancy floats in Water Volumes, by its columns (WaterSystem)
		water->applyBuoyancy(fixedStep);

		JPH::EPhysicsUpdateError errors = JPH::EPhysicsUpdateError::None;
		{
			std::lock_guard<std::mutex> lock(debugContactsMutex3D);
			debugContacts3D.clear();
		}
		// a step synchronous on request, or while a Lua shouldCollide3D must run in it, runs
		// its jobs on this (the Lua) thread (physics-contacts D12)
		const bool luaFilter = shouldCollide3D.luaSubscriberCount() > 0;
		if (luaFilter && !synchronousContactEvents3D && !loggedAutoSynchronous3D){
			loggedAutoSynchronous3D = true;
			Log::print("Physics: shouldCollide3D has a Lua handler, so the 3D world steps on the main thread (synchronousContactEvents3D). Physics.ignoreCollision or layers filter without it");
		}
		stepSynchronous3D = synchronousContactEvents3D || luaFilter;
		// a shouldCollide3D answers inside the step: with any subscriber (C++ too) every job
		// runs on this thread, the contact events still after the step unless synchronous
		JPH::JobSystem* jobs = (stepSynchronous3D || shouldCollide3D.hasSubscribers()) ? &JoltShapeBuilder::callingThreadPhysicsJobSystem() : job_system;

		contactRecorder3D->beginStep();
		{
			const auto stepStart = std::chrono::steady_clock::now();
			StepScope stepScope(this);
			// the contact listener runs on Jolt's threads, serialized by getCallbackMutex3D, while
			// this one waits: what its callbacks destroy goes after the step
			StructuralDeferScope deferScope(*scene, true);
			errors = world3D.Update(fixedStep, cCollisionSteps, temp_allocator, jobs);
			stepSeconds3D += std::chrono::duration<double>(std::chrono::steady_clock::now() - stepStart).count();
		}
		stepped3D = true;
		reportUpdateErrors3D(errors);

		// reaction forces of this step against breakForce / breakTorque, and the read-outs
		checkJointBreaks3D(fixedStep);
	}

    // layers and filter bits changed by callbacks inside the step
    for (size_t i = 0; i < pendingLayerRefresh3D.size(); i++){
        refreshRigidbodyFilter3D(pendingLayerRefresh3D[i]);
    }
    pendingLayerRefresh3D.clear();
    // and body types
    applyPendingTypeChanges3D();

    // the bodies' Transforms, parents first, so nested bodies (a ragdoll) are exact
    writeBackBodies3D(1.0f, false);

    // the global events (after the step), then the per-object callbacks, all on this thread
    if (stepped3D){
        const auto contactsStart = std::chrono::steady_clock::now();
        processContacts3D();
        contactSeconds3D += std::chrono::duration<double>(std::chrono::steady_clock::now() - contactsStart).count();
    }

    // after the step's callbacks (physics-contacts D10)
    dispatchJointBreaks3D();

    // the characters ride what moved, see what is under them now and take their root motion
    characters3D->postStep(fixedStep);
    vehicles3D->postStep(fixedStep);
#endif
}

void PhysicsSystem::interpolate(double alpha){
    if (paused){
        return;
    }

    // only bodies whose Transform is still where physics left it: one a script, an animation
    // or the editor moved since keeps that pose, the next step puts the body there
#ifdef JASPERENGINE_PHYSICS_2D
    writeBackBodies2D((float)alpha, true);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    writeBackBodies3D((float)alpha, true);
#endif
}

// The order of the write-back: the Transform array keeps parents before their children, so a
// body is placed from its parent's pose of this step. A Transform between two bodies (a bone
// without a body) is brought up to date from the body above it first.
void PhysicsSystem::orderByHierarchy(std::vector<Entity>& entities){
    auto transforms = scene->getComponentArray<Transform>();
    writeOrderScratch.clear();
    writeOrderScratch.reserve(entities.size());
    for (Entity entity : entities){
        if (transforms->hasEntity(entity)){
            writeOrderScratch.emplace_back(transforms->getIndex(entity), entity);
        }
    }
    std::sort(writeOrderScratch.begin(), writeOrderScratch.end());
    entities.clear();
    for (const auto& item : writeOrderScratch){
        entities.push_back(item.second);
    }
}

bool PhysicsSystem::hasWrittenAncestor(Entity entity) const{
    if (writtenThisPass.empty()){
        return false;
    }
    const Transform* transform = scene->findComponent<Transform>(entity);
    while (transform && transform->parent != NULL_ENTITY){
        if (writtenThisPass.count(transform->parent)){
            return true;
        }
        transform = scene->findComponent<Transform>(transform->parent);
    }
    return false;
}

void PhysicsSystem::refreshAncestors(Entity entity){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || transform->parent == NULL_ENTITY || writtenThisPass.empty()){
        return;
    }
    // the chain up to the highest ancestor written in this pass (or still waiting for an update)
    ancestorScratch.clear();
    Entity current = transform->parent;
    size_t staleFrom = std::string::npos;
    for (int depth = 0; current != NULL_ENTITY && depth < 256; depth++){
        Transform* ancestor = scene->findComponent<Transform>(current);
        if (!ancestor){
            break;
        }
        ancestorScratch.push_back(current);
        if (writtenThisPass.count(current) > 0){
            staleFrom = ancestorScratch.size() - 1;
        }
        current = ancestor->parent;
    }
    if (staleFrom == std::string::npos){
        return;
    }
    // the written one is up to date: its descendants down to the body's parent are not
    std::shared_ptr<RenderSystem> render = scene->getSystem<RenderSystem>();
    for (size_t i = staleFrom; i-- > 0;){
        const Entity ancestorEntity = ancestorScratch[i];
        render->updateTransform(scene->getComponent<Transform>(ancestorEntity), ancestorEntity);
        writtenThisPass.insert(ancestorEntity);
    }
}

void PhysicsSystem::markWritten(Entity entity){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform){
        return;
    }
    // its world matrix now, so the bodies below it are placed from it
    scene->getSystem<RenderSystem>()->updateTransform(*transform, entity);
    writtenThisPass.insert(entity);
}

#ifdef JASPERENGINE_PHYSICS_2D
void PhysicsSystem::writeBackBodies2D(float alpha, bool interpolating){
    std::vector<Entity>& entities = writeBackScratch;
    if (interpolating){
        entities.clear();
        auto bodies2d = scene->getComponentArray<Rigidbody2DComponent>();
        for (size_t i = 0; i < bodies2d->size(); i++){
            Rigidbody2DComponent& body = bodies2d->getComponentFromIndex(i);
            Entity entity = bodies2d->getEntity(i);
            Transform* transform = scene->findComponent<Transform>(entity);
            if (body.type != BodyType::STATIC && b2Body_IsValid(body.body) && b2Body_IsEnabled(body.body) &&
                    transform && isSynced(*transform, body) && interpolationOf(body.interpolation) != RigidbodyInterpolation::NONE){
                entities.push_back(entity);
            }
        }
    }else{
        entities = movedBodies2D;
    }
    if (entities.empty()){
        return;
    }
    orderByHierarchy(entities);
    writtenThisPass.clear();
    for (Entity entity : entities){
        Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity);
        if (!body){
            continue;
        }
        refreshAncestors(entity);
        // a following body is on its way to its Transform, the last step of the frame puts it there
        if (!interpolating && Engine::getFixedStepsLeft() == 1){
            finishFollowing2D(entity, *body);
        }
        // S96 WP-C: and a kinematic move's last step ends at its target
        if (!interpolating && body->moveSnap && b2Body_IsValid(body->body)){
            finishMoveTarget2D(*body);
        }
        updateTransformFromRigidbody2D(entity, *body, alpha);
        markWritten(entity);
    }
    writtenThisPass.clear();
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
void PhysicsSystem::writeBackBodies3D(float alpha, bool interpolating){
    if (!world3DReady){
        return;
    }
    std::vector<Entity>& entities = writeBackScratch;
    entities.clear();
    auto bodies3d = scene->getComponentArray<RigidbodyComponent>();
    JPH::BodyInterface& bodyInterface = getBodyInterface3D();
    for (size_t i = 0; i < bodies3d->size(); i++){
        RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
        // out of the world (disabled, inactive) it did not step and leaves its Transform alone
        if (body.body.IsInvalid() || !bodyInterface.IsAdded(body.body)){
            continue;
        }
        Entity entity = bodies3d->getEntity(i);
        if (interpolating){
            Transform* transform = scene->findComponent<Transform>(entity);
            if (body.type == BodyType::STATIC || !transform || !isSynced(*transform, body) ||
                    interpolationOf(body.interpolation) == RigidbodyInterpolation::NONE){
                continue;
            }
        }
        entities.push_back(entity);
    }
    if (entities.empty()){
        return;
    }
    orderByHierarchy(entities);
    writtenThisPass.clear();
    for (Entity entity : entities){
        RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
        if (!body){
            continue;
        }
        refreshAncestors(entity);
        if (!interpolating && Engine::getFixedStepsLeft() == 1){
            finishFollowing3D(entity, *body);
        }
        // S96 WP-C: and a kinematic move's last step ends at its target
        if (!interpolating && body->moveSnap && !body->body.IsInvalid()){
            finishMoveTarget3D(entity, *body);
        }
        // a static or kinematic body under a body written in this pass (a hitbox on a vehicle)
        // keeps its local pose and so moves with it; its body follows its Transform at the next
        // step (writing its own pose back would leave it behind, a step at a time)
        if (body->type != BodyType::DYNAMIC && hasWrittenAncestor(entity)){
            continue;
        }
        updateTransformFromRigidbody(entity, *body, alpha);
        markWritten(entity);
    }
    writtenThisPass.clear();
}

void PhysicsSystem::reportUpdateErrors3D(JPH::EPhysicsUpdateError errors){
    const uint32_t bits = (uint32_t)errors & ~reportedUpdateErrors3D;
    if (bits == 0){
        return;
    }
    reportedUpdateErrors3D |= bits;
    if (bits & (uint32_t)JPH::EPhysicsUpdateError::ManifoldCacheFull){
        Log::error("3D physics: too many contacts, some were dropped (the contact cache holds %u). Raise Project Settings > Physics > 3D World Capacity > Max Contact Constraints (physics3DWorld.maxContactConstraints).",
                   maxContactConstraints3D);
    }
    if (bits & (uint32_t)JPH::EPhysicsUpdateError::BodyPairCacheFull){
        Log::error("3D physics: too many touching body pairs, some contacts were dropped (the world holds %u pairs). Raise Project Settings > Physics > 3D World Capacity > Max Body Pairs (physics3DWorld.maxBodyPairs).",
                   maxBodyPairs3D);
    }
    if (bits & (uint32_t)JPH::EPhysicsUpdateError::ContactConstraintsFull){
        Log::error("3D physics: too many contact constraints, some contacts were dropped (the world holds %u). Raise Project Settings > Physics > 3D World Capacity > Max Contact Constraints (physics3DWorld.maxContactConstraints).",
                   maxContactConstraints3D);
    }
}
#endif

void PhysicsSystem::draw(){
    updateDebugLines();
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
		// the other sides get their Exit now, while this GameObject still exists (D6)
		contactBodyRemoved2D(entity);
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
		contactBodyRemoved3D(entity);
		RigidbodyComponent& rigidbody = scene->getComponent<RigidbodyComponent>(entity);
		// its joints hold the Jolt body: they go first (made again if a body comes back)
		invalidateJoints3D(entity);
		destroyRigidbody(rigidbody);
		warnedNonNormalizedRotations.erase(entity);
		reportedInvalidRotations.erase(entity);
		pendingTypeChanges3D.erase(std::remove(pendingTypeChanges3D.begin(), pendingTypeChanges3D.end(), entity), pendingTypeChanges3D.end());
	} else if (componentId == scene->getComponentId<Joint3DComponent>()) {
		Joint3DComponent& joint3d = scene->getComponent<Joint3DComponent>(entity);
		destroyJoint3D(joint3d);
	} else if (componentId == scene->getComponentId<CharacterControllerComponent>()) {
		characters3D->remove(entity);
	} else if (componentId == scene->getComponentId<RagdollComponent>()) {
		releaseRagdoll(scene->getComponent<RagdollComponent>(entity));
	}
#endif
}

#ifdef JASPERENGINE_PHYSICS_3D
// a script may move a controller before the first step: its CharacterVirtual needs the world
int PhysicsSystem::moveCharacter(Entity entity, const Vector3& motion){
    ensureWorld3D();
    return characters3D->move(entity, motion);
}

bool PhysicsSystem::simpleMoveCharacter(Entity entity, const Vector3& speed){
    ensureWorld3D();
    return characters3D->simpleMove(entity, speed);
}

bool PhysicsSystem::trySetCharacterSize(Entity entity, float height, float radius, const Vector3& center){
    ensureWorld3D();
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
