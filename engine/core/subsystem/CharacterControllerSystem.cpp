// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CharacterControllerSystem.h"

#ifdef JASPERENGINE_PHYSICS_3D

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "script/LuaBinding.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"
#include "util/JoltPhysicsAux.h"

#include "Jolt/Physics/Body/BodyFilter.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h"
#include "Jolt/Physics/Collision/ObjectLayer.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/ShapeFilter.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;

namespace{

    constexpr float DEGREES_TO_RADIANS = 0.017453292519943295f;

    JPH::Vec3 toJolt(const Vector3& v){
        return JPH::Vec3(v.x, v.y, v.z);
    }

    Vector3 fromJolt(JPH::Vec3Arg v){
        return Vector3(v.GetX(), v.GetY(), v.GetZ());
    }

    bool finite(const Vector3& v){
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    float Vec3Up(JPH::Vec3Arg v){
        return v.GetY();
    }

}

// The listener of every character of the scene: pushes, validation by layer and bits
class CharacterControllerSystem::ContactListener : public JPH::CharacterContactListener{
public:
    Scene* scene = nullptr;
    PhysicsSystem* physics = nullptr;

    const CharacterControllerComponent* controllerOf(const JPH::CharacterVirtual* character) const{
        const Entity entity = CharacterControllerSystem::entityOf(character);
        return entity != NULL_ENTITY ? scene->findComponent<CharacterControllerComponent>(entity) : nullptr;
    }

    void OnContactAdded(const JPH::CharacterVirtual* character, const JPH::BodyID& body, const JPH::SubShapeID& subShape,
                        JPH::RVec3Arg position, JPH::Vec3Arg normal, JPH::CharacterContactSettings& settings) override{
        // a controller that does not push leaves the bodies it touches alone
        const CharacterControllerComponent* controller = controllerOf(character);
        if (controller && !controller->pushBodies)
            settings.mCanReceiveImpulses = false;
    }

    bool OnCharacterContactValidate(const JPH::CharacterVirtual* character, const JPH::CharacterVirtual* other,
                                    const JPH::SubShapeID& subShape) override{
        const CharacterControllerComponent* a = controllerOf(character);
        const CharacterControllerComponent* b = controllerOf(other);
        if (!a || !b || !a->characterCollisions || !b->characterCollisions)
            return false;
        // the Layer Collision Matrix and the bits, as for bodies
        const Entity entityA = CharacterControllerSystem::entityOf(character);
        const Entity entityB = CharacterControllerSystem::entityOf(other);
        return Physics::getLayerCollisionMatrix().collides(scene->getEntityLayer(entityA), scene->getEntityLayer(entityB))
            && (a->categoryBits & b->maskBits) != 0 && (b->categoryBits & a->maskBits) != 0;
    }
};

// A character's moves skip every inner body: its own, and the other characters' (they meet
// through CharacterVsCharacterCollisionSimple, which characterCollisions switches)
class CharacterControllerSystem::InnerBodiesFilter : public JPH::BodyFilter{
public:
    const CharacterControllerSystem& system;
    explicit InnerBodiesFilter(const CharacterControllerSystem& system): system(system){}
    bool ShouldCollide(const JPH::BodyID& body) const override{
        return !system.isInnerBody(body);
    }
};

CharacterControllerSystem::CharacterControllerSystem(Scene* scene, PhysicsSystem* physics):
    scene(scene), physics(physics),
    characterVsCharacter(std::make_unique<JPH::CharacterVsCharacterCollisionSimple>()),
    listener(std::make_unique<ContactListener>()){
    listener->scene = scene;
    listener->physics = physics;
}

CharacterControllerSystem::~CharacterControllerSystem(){
    clear();
}

Entity CharacterControllerSystem::entityOf(const JPH::CharacterVirtual* character){
    return character ? static_cast<Entity>(character->GetUserData()) : NULL_ENTITY;
}

bool CharacterControllerSystem::isInnerBody(const JPH::BodyID& body) const{
    return !innerBodies.empty() && innerBodies.count(body.GetIndexAndSequenceNumber()) > 0;
}

CharacterControllerSystem::Runtime* CharacterControllerSystem::find(Entity entity){
    CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(entity);
    if (!controller || controller->handle == 0)
        return nullptr;
    auto it = runtimes.find(controller->handle);
    return it != runtimes.end() ? &it->second : nullptr;
}

Vector3 CharacterControllerSystem::worldScale(Entity entity) const{
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform)
        return Vector3(1.0f, 1.0f, 1.0f);
    return Vector3(std::fabs(transform->worldScale.x), std::fabs(transform->worldScale.y), std::fabs(transform->worldScale.z));
}

JPH::RefConst<JPH::Shape> CharacterControllerSystem::makeShape(float height, float radius, const Vector3& scale) const{
    const float scaledRadius = radius * std::max(scale.x, scale.z);
    const float scaledHeight = height * scale.y;
    if (!(scaledRadius > 0.0f) || !(scaledHeight > 0.0f) || !std::isfinite(scaledRadius) || !std::isfinite(scaledHeight))
        return nullptr;
    // a capsule standing on (0, 0, 0), as CharacterVirtual wants; too short for its radius, a sphere
    const float halfCylinder = std::max(0.0f, scaledHeight * 0.5f - scaledRadius);
    const float halfTotal = halfCylinder + scaledRadius;
    JPH::RefConst<JPH::Shape> capsule;
    if (halfCylinder > 1.0e-4f)
        capsule = new JPH::CapsuleShape(halfCylinder, scaledRadius);
    else
        capsule = new JPH::SphereShape(scaledRadius);
    return JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, halfTotal, 0.0f), JPH::Quat::sIdentity(), capsule).Create().Get();
}

bool CharacterControllerSystem::create(Entity entity, CharacterControllerComponent& controller){
    if (scene->findComponent<RigidbodyComponent>(entity)){
        // the two cannot move one GameObject (R26): the controller gives way (its saved
        // checkbox is left alone), logged once
        if (refusedWithRigidbody.insert(entity).second)
            Log::error("CharacterController of entity %u is disabled: its GameObject has a Rigidbody (remove one of them)", entity);
        controller.refusedWithRigidbody = true;
        return false;
    }
    refusedWithRigidbody.erase(entity);
    controller.refusedWithRigidbody = false;
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform){
        Log::error("CharacterController of entity %u needs a Transform", entity);
        return false;
    }
    physics->resolveTransformWorldPose(entity);

    const Vector3 scale = worldScale(entity);
    JPH::RefConst<JPH::Shape> shape = makeShape(controller.height, controller.radius, scale);
    if (!shape){
        Log::error("CharacterController of entity %u: height %g and radius %g make no capsule", entity, controller.height, controller.radius);
        return false;
    }

    Runtime runtime;
    runtime.entity = entity;
    runtime.height = controller.height;
    runtime.radius = controller.radius;
    runtime.center = controller.center;
    runtime.scale = scale;
    runtime.skinWidth = controller.skinWidth;
    runtime.detectCollisions = controller.detectCollisions;
    runtime.characterCollisions = controller.characterCollisions;
    runtime.layer = scene->getEntityLayer(entity);
    runtime.categoryBits = controller.categoryBits;
    runtime.maskBits = controller.maskBits;
    runtime.objectLayer = physics->objectLayerTable3D->acquire(runtime.layer, controller.categoryBits, controller.maskBits);

    const float scaledRadius = controller.radius * std::max(scale.x, scale.z);
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mUp = JPH::Vec3::sAxisY();
    settings->mMaxSlopeAngle = controller.slopeLimit * DEGREES_TO_RADIANS;
    settings->mShape = shape;
    // contacts below the bottom sphere's centre hold the character up (OpenEngine's choice)
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -scaledRadius);
    settings->mCharacterPadding = std::max(0.0f, controller.skinWidth);
    settings->mMass = std::max(0.0f, controller.mass);
    settings->mMaxStrength = std::max(0.0f, controller.pushStrength);
    settings->mEnhancedInternalEdgeRemoval = controller.enhancedEdgeRemoval;
    if (controller.detectCollisions){
        settings->mInnerBodyShape = shape;
        settings->mInnerBodyLayer = runtime.objectLayer;
    }

    const Vector3 foot = transform->worldPosition + controller.center * scale - Vector3(0.0f, controller.height * scale.y * 0.5f, 0.0f);
    runtime.character = new JPH::CharacterVirtual(settings, toJolt(foot), JPH::Quat::sIdentity(), (JPH::uint64)entity, &physics->world3D);
    runtime.character->SetListener(listener.get());
    if (controller.characterCollisions){
        runtime.character->SetCharacterVsCharacterCollision(characterVsCharacter.get());
        characterVsCharacter->Add(runtime.character);
    }
    if (!runtime.character->GetInnerBodyID().IsInvalid())
        innerBodies.insert(runtime.character->GetInnerBodyID().GetIndexAndSequenceNumber());
    runtime.syncedPosition = transform->worldPosition;

    const uint32_t handle = nextHandle++;
    controller.handle = handle;
    Runtime& stored = runtimes.emplace(handle, std::move(runtime)).first->second;
    // its first contacts and ground, so isGrounded is right before the first move
    JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*physics->object_vs_broadphase_layer_filter, stored.objectLayer);
    JPH::DefaultObjectLayerFilter objectFilter(*physics->object_vs_object_layer_filter, stored.objectLayer);
    InnerBodiesFilter bodyFilter(*this);
    JPH::ShapeFilter shapeFilter;
    stored.character->RefreshContacts(broadPhaseFilter, objectFilter, bodyFilter, shapeFilter, *physics->temp_allocator);
    readGroundState(stored, controller);
    return true;
}

void CharacterControllerSystem::destroy(CharacterControllerComponent& controller){
    auto it = runtimes.find(controller.handle);
    controller.handle = 0;
    controller.isGrounded = false;
    controller.isOnSteepSlope = false;
    controller.groundEntity = NULL_ENTITY;
    if (it == runtimes.end())
        return;
    Runtime& runtime = it->second;
    if (runtime.character){
        if (!runtime.character->GetInnerBodyID().IsInvalid())
            innerBodies.erase(runtime.character->GetInnerBodyID().GetIndexAndSequenceNumber());
        characterVsCharacter->Remove(runtime.character);
        // the inner body goes with the character
        runtime.character = nullptr;
    }
    physics->objectLayerTable3D->release(runtime.objectLayer);
    runtimes.erase(it);
}

CharacterControllerSystem::Runtime* CharacterControllerSystem::ensure(Entity entity, CharacterControllerComponent& controller){
    if (Runtime* runtime = find(entity))
        return runtime;
    if (!controller.enabled || !scene->isEntityActiveInHierarchy(entity))
        return nullptr;
    // Scripts start (Lua init()) before the first step: a move then makes the character
    if (physics->steppingWorld3D || !create(entity, controller))
        return nullptr;
    return find(entity);
}

void CharacterControllerSystem::applySettings(Runtime& runtime, CharacterControllerComponent& controller){
    JPH::CharacterVirtual& character = *runtime.character;
    const float slope = controller.slopeLimit * DEGREES_TO_RADIANS;
    if (std::fabs(std::acos(std::clamp(character.GetCosMaxSlopeAngle(), -1.0f, 1.0f)) - slope) > 1.0e-5f)
        character.SetMaxSlopeAngle(slope);
    character.SetMass(std::max(0.0f, controller.mass));
    character.SetMaxStrength(std::max(0.0f, controller.pushStrength));
    character.SetEnhancedInternalEdgeRemoval(controller.enhancedEdgeRemoval);

    const int layer = scene->getEntityLayer(runtime.entity);
    if (layer != runtime.layer || controller.categoryBits != runtime.categoryBits || controller.maskBits != runtime.maskBits){
        const JPH::ObjectLayer next = physics->objectLayerTable3D->acquire(layer, controller.categoryBits, controller.maskBits);
        physics->objectLayerTable3D->release(runtime.objectLayer);
        runtime.objectLayer = next;
        runtime.layer = layer;
        runtime.categoryBits = controller.categoryBits;
        runtime.maskBits = controller.maskBits;
        if (!character.GetInnerBodyID().IsInvalid())
            physics->getBodyInterface3D().SetObjectLayer(character.GetInnerBodyID(), next);
    }
}

void CharacterControllerSystem::followTransform(Runtime& runtime){
    Transform* transform = scene->findComponent<Transform>(runtime.entity);
    if (!transform)
        return;
    physics->resolveTransformWorldPose(runtime.entity);
    if (transform->worldPosition == runtime.syncedPosition)
        return;
    // a teleport (a script, an animation, the editor): no Unity "disable it first" trap
    const CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(runtime.entity);
    const Vector3 scale = runtime.scale;
    const Vector3 foot = transform->worldPosition + runtime.center * scale - Vector3(0.0f, runtime.height * scale.y * 0.5f, 0.0f);
    runtime.character->SetPosition(toJolt(foot));
    runtime.syncedPosition = transform->worldPosition;
    (void)controller;
}

void CharacterControllerSystem::writeTransform(Runtime& runtime, const Vector3& worldPosition, float yawDelta){
    Transform* transform = scene->findComponent<Transform>(runtime.entity);
    if (!transform)
        return;
    Quaternion worldRotation = transform->worldRotation;
    if (yawDelta != 0.0f)
        worldRotation = Quaternion(Angle::radToDefault(yawDelta), Vector3(0.0f, 1.0f, 0.0f)) * worldRotation;
    physics->setTransformWorldPose(runtime.entity, worldPosition, worldRotation);
    runtime.syncedPosition = transform->worldPosition;
}

void CharacterControllerSystem::readGroundState(Runtime& runtime, CharacterControllerComponent& controller){
    const JPH::CharacterVirtual& character = *runtime.character;
    const JPH::CharacterBase::EGroundState state = character.GetGroundState();
    controller.isGrounded = state == JPH::CharacterBase::EGroundState::OnGround;
    controller.isOnSteepSlope = state == JPH::CharacterBase::EGroundState::OnSteepGround;
    if (character.IsSupported()){
        controller.groundNormal = fromJolt(character.GetGroundNormal());
        controller.groundVelocity = fromJolt(character.GetGroundVelocity());
        controller.groundEntity = static_cast<Entity>(character.GetGroundUserData());
    }else{
        controller.groundNormal = Vector3(0.0f, 0.0f, 0.0f);
        controller.groundVelocity = Vector3(0.0f, 0.0f, 0.0f);
        controller.groundEntity = NULL_ENTITY;
    }
}

int CharacterControllerSystem::moveBy(Runtime& runtime, CharacterControllerComponent& controller, const Vector3& motion, float dt, bool reportHits){
    JPH::CharacterVirtual& character = *runtime.character;
    const JPH::RVec3 before = character.GetPosition();
    const float scaleY = runtime.scale.y;
    const float radius = runtime.radius * std::max(runtime.scale.x, runtime.scale.z);
    const float stepOffset = std::max(0.0f, controller.stepOffset) * scaleY;

    // Jolt's ExtendedUpdate (CharacterVirtual.cpp), step by step, with OpenEngine's stair
    // settings (their JoltPhysicsBackend.cpp) and two checks of ours on a stair walk: what stopped
    // it is no higher than stepOffset, and it ends above it, so a riser taller than stepOffset is
    // neither climbed by sliding the round bottom up its edge nor by perching on it. Stick-to-
    // floor also slides down a nosing too steep to stand on (Jolt's sweeps straight down onto it).
    const JPH::Vec3 up = JPH::Vec3::sAxisY();
    const JPH::Vec3 desiredVelocity = toJolt(motion * (1.0f / dt));
    character.SetLinearVelocity(character.CancelVelocityTowardsSteepSlopes(desiredVelocity));
    InnerBodiesFilter bodyFilter(*this);
    JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*physics->object_vs_broadphase_layer_filter, runtime.objectLayer);
    JPH::DefaultObjectLayerFilter objectFilter(*physics->object_vs_object_layer_filter, runtime.objectLayer);
    JPH::ShapeFilter shapeFilter;
    const JPH::Vec3 gravity = toJolt(physics->getGravity3D());
    // supported now, or at the end of the last move: the post-step refresh at rest drops a
    // contact more than Jolt's collision tolerance (1 mm) away, which a character walking off an
    // edge already is, and stick-to-floor would never run there
    bool groundToAir = character.IsSupported() || runtime.supportedByMove;
    character.Update(dt, gravity, broadPhaseFilter, objectFilter, bodyFilter, shapeFilter, *physics->temp_allocator);
    if (character.IsSupported())
        groundToAir = false;

    if (groundToAir && stepOffset > 0.0f && Vec3Up(character.GetPosition() - before) / dt <= 1.0e-6f){
        character.StickToFloor(JPH::Vec3(0.0f, -stepOffset, 0.0f), broadPhaseFilter, objectFilter, bodyFilter, shapeFilter, *physics->temp_allocator);
        if (character.GetGroundState() != JPH::CharacterBase::EGroundState::OnGround){
            // off a nosing: down along it, kept only on walkable ground
            const JPH::RVec3 edge = character.GetPosition();
            const JPH::Vec3 velocity = character.GetLinearVelocity();
            character.SetLinearVelocity(JPH::Vec3(0.0f, -stepOffset / dt, 0.0f));
            character.Update(dt, JPH::Vec3::sZero(), broadPhaseFilter, objectFilter, bodyFilter, shapeFilter, *physics->temp_allocator);
            if (character.GetGroundState() != JPH::CharacterBase::EGroundState::OnGround){
                character.SetPosition(edge);
                character.RefreshContacts(broadPhaseFilter, objectFilter, bodyFilter, shapeFilter, *physics->temp_allocator);
            }
            character.SetLinearVelocity(velocity);
        }
    }

    if (stepOffset > 0.0f){
        JPH::Vec3 desiredStep = desiredVelocity * dt;
        desiredStep -= desiredStep.Dot(up) * up;
        const float desiredLength = desiredStep.Length();
        if (desiredLength > 0.0f){
            JPH::Vec3 achieved = JPH::Vec3(character.GetPosition() - before);
            achieved -= achieved.Dot(up) * up;
            const JPH::Vec3 forwardDirection = desiredStep / desiredLength;
            const float achievedLength = std::max(0.0f, achieved.Dot(forwardDirection));
            if (achievedLength + 1.0e-4f < desiredLength && character.CanWalkStairs(desiredVelocity)){
                // the top of what stopped it, as CanWalkStairs finds it
                const JPH::Vec3 horizontal = desiredVelocity - desiredVelocity.Dot(up) * up;
                float blockingTop = -std::numeric_limits<float>::max();
                for (const JPH::CharacterVirtual::Contact& contact : character.GetActiveContacts()){
                    if (contact.mHadCollision && contact.mSurfaceNormal.Dot(horizontal - contact.mLinearVelocity) < 0.0f &&
                            character.IsSlopeTooSteep(contact.mSurfaceNormal))
                        blockingTop = std::max(blockingTop, (float)contact.mPosition.GetY());
                }
                const JPH::Vec3 stepForward = forwardDirection * std::max(std::max(0.02f, radius), desiredLength - achievedLength);
                JPH::Vec3 stepForwardTest = -character.GetGroundNormal();
                stepForwardTest -= stepForwardTest.Dot(up) * up;
                stepForwardTest = stepForwardTest.NormalizedOr(forwardDirection);
                if (stepForwardTest.Dot(forwardDirection) < JPH::CharacterVirtual::ExtendedUpdateSettings().mWalkStairsCosAngleForwardContact)
                    stepForwardTest = forwardDirection;
                stepForwardTest *= std::max(0.15f, radius + 0.05f);

                const JPH::RVec3 blocked = character.GetPosition();
                if (character.WalkStairs(dt, JPH::Vec3(0.0f, stepOffset, 0.0f), stepForward, stepForwardTest, JPH::Vec3::sZero(),
                                         broadPhaseFilter, objectFilter, bodyFilter, shapeFilter, *physics->temp_allocator)){
                    const float foot = (float)character.GetPosition().GetY();
                    const float tolerance = character.GetCharacterPadding() + 0.01f;
                    // a step over what stopped it: no higher than stepOffset over the ground it
                    // left, and below the feet after (the round bottom slides up walkable edges
                    // in the forward move, so the rise itself may be more)
                    if (blockingTop > (float)blocked.GetY() + stepOffset + tolerance || blockingTop > foot + tolerance){
                        character.SetPosition(blocked);
                        character.RefreshContacts(broadPhaseFilter, objectFilter, bodyFilter, shapeFilter, *physics->temp_allocator);
                    }
                }
            }
        }
    }

    runtime.supportedByMove = character.IsSupported();
    const JPH::RVec3 after = character.GetPosition();
    controller.velocity = fromJolt(JPH::Vec3(after - before)) * (1.0f / dt);
    readGroundState(runtime, controller);

    // the parts of the capsule that touched: below the bottom sphere's centre, above the top one's, else the sides
    const float scaledHeight = runtime.height * scaleY;
    const float bottomCenter = (float)after.GetY() + radius;
    const float topCenter = (float)after.GetY() + std::max(radius, scaledHeight - radius);
    int flags = CharacterCollision_None;
    hitScratch.clear();
    const Vector3 direction = motion.length() > 0.0f ? motion.normalized() : Vector3(0.0f, 0.0f, 0.0f);
    for (const JPH::CharacterVirtual::Contact& contact : character.GetActiveContacts()){
        if (!contact.mHadCollision || contact.mIsSensorB)
            continue;
        const float y = (float)contact.mPosition.GetY();
        if (y <= bottomCenter) flags |= CharacterCollision_Below;
        else if (y >= topCenter) flags |= CharacterCollision_Above;
        else flags |= CharacterCollision_Sides;
        if (reportHits){
            ControllerColliderHit hit;
            hit.controller = runtime.entity;
            hit.entity = static_cast<Entity>(contact.mUserData);
            hit.point = fromJolt(JPH::Vec3(contact.mPosition));
            hit.normal = fromJolt(contact.mSurfaceNormal);
            hit.moveDirection = direction;
            hit.moveLength = motion.length();
            hitScratch.push_back(hit);
        }
    }
    if (controller.isGrounded)
        flags |= CharacterCollision_Below;
    controller.collisionFlags = flags;

    const Vector3 foot = fromJolt(JPH::Vec3(after));
    const Vector3 position = foot - runtime.center * runtime.scale + Vector3(0.0f, scaledHeight * 0.5f, 0.0f);
    writeTransform(runtime, position, 0.0f);
    return flags;
}

void CharacterControllerSystem::deliverHits(Entity entity, const Vector3& motion){
    if (hitScratch.empty())
        return;
    // the move is done: the callbacks may move or destroy things
    const std::vector<ControllerColliderHit> hits = hitScratch;
    for (const ControllerColliderHit& hit : hits){
        LuaBinding::callScriptHook(scene, entity, "onControllerColliderHit", hit);
        if (physics->onControllerColliderHit.hasSubscribers())
            physics->onControllerColliderHit.call(entity, hit);
        if (!scene->isEntityCreated(entity))
            return;
    }
    (void)motion;
}

float CharacterControllerSystem::moveDelta() const{
    // the fixed step inside the fixed phase, the frame's delta otherwise
    float dt = Input::isFixedPhase() ? Engine::getUpdateTime() : Engine::getDeltatime();
    return dt > 0.0f ? dt : 1.0f / 60.0f;
}

int CharacterControllerSystem::move(Entity entity, const Vector3& motion){
    CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(entity);
    if (!controller)
        return CharacterCollision_None;
    if (physics->steppingWorld3D){
        Log::error("CharacterController.move of entity %u from a physics callback is ignored (move in onUpdate or onFixedUpdate)", entity);
        return controller->collisionFlags;
    }
    Runtime* runtime = ensure(entity, *controller);
    if (!runtime)
        return CharacterCollision_None;
    if (!finite(motion)){
        Log::warn("CharacterController.move of entity %u: the motion is not finite, ignored", entity);
        return controller->collisionFlags;
    }
    applySettings(*runtime, *controller);
    followTransform(*runtime);
    if (motion.length() < controller->minMoveDistance){
        controller->velocity = Vector3(0.0f, 0.0f, 0.0f);
        return controller->collisionFlags;
    }
    const int flags = moveBy(*runtime, *controller, motion, moveDelta(), true);
    deliverHits(entity, motion);
    return flags;
}

bool CharacterControllerSystem::simpleMove(Entity entity, const Vector3& speed){
    CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(entity);
    if (!controller)
        return false;
    Runtime* runtime = physics->steppingWorld3D ? nullptr : ensure(entity, *controller);
    if (!runtime)
        return controller->isGrounded;
    const float dt = moveDelta();
    // its own gravity, from rest on the ground; speed.y is ignored (Unity)
    if (controller->isGrounded)
        runtime->verticalSpeed = 0.0f;
    runtime->verticalSpeed += physics->getGravity3D().y * dt;
    const Vector3 motion(speed.x * dt, runtime->verticalSpeed * dt, speed.z * dt);
    const int flags = move(entity, motion);
    runtime = find(entity);
    if (!runtime)
        return false;
    controller = scene->findComponent<CharacterControllerComponent>(entity);
    if (controller->isGrounded || ((flags & CharacterCollision_Above) && runtime->verticalSpeed > 0.0f))
        runtime->verticalSpeed = 0.0f;
    return controller->isGrounded;
}

bool CharacterControllerSystem::trySetSize(Entity entity, float height, float radius, const Vector3& center){
    CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(entity);
    if (!controller)
        return false;
    Runtime* runtime = physics->steppingWorld3D ? nullptr : find(entity);
    if (!runtime){
        // no character yet (disabled, inactive): the size is the one it will be made with
        if (physics->steppingWorld3D || !(height > 0.0f) || !(radius > 0.0f))
            return false;
        controller->height = height;
        controller->radius = radius;
        controller->center = center;
        return true;
    }
    JPH::RefConst<JPH::Shape> shape = makeShape(height, radius, runtime->scale);
    if (!shape)
        return false;

    // the Transform stays where it is: the feet move by the change of the capsule's bottom
    JPH::CharacterVirtual& character = *runtime->character;
    const Vector3 scale = runtime->scale;
    const Vector3 oldFoot = fromJolt(JPH::Vec3(character.GetPosition()));
    const Vector3 position = oldFoot - runtime->center * scale + Vector3(0.0f, runtime->height * scale.y * 0.5f, 0.0f);
    const Vector3 newFoot = position + center * scale - Vector3(0.0f, height * scale.y * 0.5f, 0.0f);
    character.SetPosition(toJolt(newFoot));

    JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*physics->object_vs_broadphase_layer_filter, runtime->objectLayer);
    JPH::DefaultObjectLayerFilter objectFilter(*physics->object_vs_object_layer_filter, runtime->objectLayer);
    InnerBodiesFilter bodyFilter(*this);
    JPH::ShapeFilter shapeFilter;
    // blocked (a ceiling over a crouch): the old shape stays
    if (!character.SetShape(shape, 1.5f * character.GetCharacterPadding(), broadPhaseFilter, objectFilter, bodyFilter, shapeFilter,
                            *physics->temp_allocator)){
        character.SetPosition(toJolt(oldFoot));
        controller->height = runtime->height;
        controller->radius = runtime->radius;
        controller->center = runtime->center;
        return false;
    }
    if (!character.GetInnerBodyID().IsInvalid())
        character.SetInnerBodyShape(shape);
    runtime->height = height;
    runtime->radius = radius;
    runtime->center = center;
    controller->height = height;
    controller->radius = radius;
    controller->center = center;
    return true;
}

void CharacterControllerSystem::setRootMotion(Entity entity, const Vector3& worldVelocity, float yawRate){
    CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(entity);
    if (!controller)
        return;
    Runtime* runtime = find(entity);
    if (!runtime)
        return;
    if (!finite(worldVelocity) || !std::isfinite(yawRate)){
        if (!runtime->rootMotionWarned){
            runtime->rootMotionWarned = true;
            Log::warn("CharacterController of entity %u: a root motion that is not finite is dropped", entity);
        }
        return;
    }
    runtime->hasRootMotion = true;
    runtime->rootVelocity = worldVelocity;
    runtime->rootYawRate = yawRate;
}

void CharacterControllerSystem::preStep(){
    auto controllers = scene->getComponentArray<CharacterControllerComponent>();
    for (size_t i = 0; i < controllers->size(); i++){
        CharacterControllerComponent& controller = controllers->getComponentFromIndex(i);
        const Entity entity = controllers->getEntity(i);
        const bool wanted = controller.enabled && scene->isEntityActiveInHierarchy(entity);
        Runtime* runtime = find(entity);
        if (!wanted){
            if (runtime)
                destroy(controller);
            continue;
        }
        if (!runtime){
            create(entity, controller);
            continue;
        }
        // remade for what has no setter: the skin, the inner body, a new scale
        const Vector3 scale = worldScale(entity);
        if (controller.skinWidth != runtime->skinWidth || controller.detectCollisions != runtime->detectCollisions || scale != runtime->scale){
            destroy(controller);
            create(entity, controller);
            continue;
        }
        if (controller.characterCollisions != runtime->characterCollisions){
            runtime->characterCollisions = controller.characterCollisions;
            if (controller.characterCollisions){
                runtime->character->SetCharacterVsCharacterCollision(characterVsCharacter.get());
                characterVsCharacter->Add(runtime->character);
            }else{
                runtime->character->SetCharacterVsCharacterCollision(nullptr);
                characterVsCharacter->Remove(runtime->character);
            }
        }
        // a size set on the component (the inspector, set_runtime_property) is tried like trySetSize
        if (controller.height != runtime->height || controller.radius != runtime->radius || controller.center != runtime->center){
            const float height = controller.height;
            const float radius = controller.radius;
            const Vector3 center = controller.center;
            if (!trySetSize(entity, height, radius, center))
                Log::warn("CharacterController of entity %u: the new size does not fit there, the old one stays", entity);
        }
        applySettings(*runtime, controller);
        followTransform(*runtime);
    }
}

void CharacterControllerSystem::postStep(float dt){
    if (runtimes.empty())
        return;
    JPH::BodyInterface& bodies = physics->getBodyInterface3D();
    // the handles first: a callback of the pass may add or remove characters
    handleScratch.clear();
    for (const auto& entry : runtimes)
        handleScratch.push_back(entry.first);

    for (uint32_t handle : handleScratch){
        auto it = runtimes.find(handle);
        if (it == runtimes.end())
            continue;
        Runtime& runtime = it->second;
        CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(runtime.entity);
        if (!controller)
            continue;
        JPH::CharacterVirtual& character = *runtime.character;
        followTransform(runtime);

        // the platform moved in the step: its velocity at the feet carries the character
        character.UpdateGroundVelocity();
        const JPH::BodyID ground = character.GetGroundBodyID();
        Vector3 carry(0.0f, 0.0f, 0.0f);
        float yaw = 0.0f;
        if (controller->movingPlatforms && character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
                !ground.IsInvalid() && bodies.GetMotionType(ground) != JPH::EMotionType::Static){
            carry = fromJolt(character.GetGroundVelocity());
            if (controller->inheritPlatformYaw)
                yaw = bodies.GetAngularVelocity(ground).GetY() * dt;
        }

        if (carry.x != 0.0f || carry.y != 0.0f || carry.z != 0.0f){
            // only carries: the move's own velocity is not in it
            const Vector3 velocityBefore = controller->velocity;
            const int flagsBefore = controller->collisionFlags;
            moveBy(runtime, *controller, carry * dt, dt, false);
            controller->velocity = velocityBefore;
            controller->collisionFlags = flagsBefore;
        }else if (character.IsSupported() && (ground.IsInvalid() || !bodies.IsAdded(ground) ||
                                                bodies.GetMotionType(ground) != JPH::EMotionType::Static)){
            // the ground under it may have gone (a platform dropping away; a static ground still
            // in the world is where it was, and with none there is nothing to lose). At rest:
            // Jolt counts only the contacts a character moves into, and the last move's velocity
            // would leave out the nosing it stands on while walking off it
            character.SetLinearVelocity(JPH::Vec3::sZero());
            JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(*physics->object_vs_broadphase_layer_filter, runtime.objectLayer);
            JPH::DefaultObjectLayerFilter objectFilter(*physics->object_vs_object_layer_filter, runtime.objectLayer);
            InnerBodiesFilter bodyFilter(*this);
            JPH::ShapeFilter shapeFilter;
            character.RefreshContacts(broadPhaseFilter, objectFilter, bodyFilter, shapeFilter, *physics->temp_allocator);
            readGroundState(runtime, *controller);
        }
        if (yaw != 0.0f){
            Transform* transform = scene->findComponent<Transform>(runtime.entity);
            if (transform)
                writeTransform(runtime, transform->worldPosition, yaw);
        }

        // root motion: its velocity for the step, with collisions, and its turn
        if (runtime.hasRootMotion){
            const Vector3 motion = runtime.rootVelocity * dt;
            const float rootYaw = runtime.rootYawRate * dt;
            if (motion.length() >= controller->minMoveDistance){
                moveBy(runtime, *controller, motion, dt, true);
                deliverHits(runtime.entity, motion);
            }
            auto again = runtimes.find(handle);
            if (again == runtimes.end())
                continue;
            if (rootYaw != 0.0f){
                Transform* transform = scene->findComponent<Transform>(again->second.entity);
                if (transform)
                    writeTransform(again->second, transform->worldPosition, rootYaw);
            }
        }
    }
}

void CharacterControllerSystem::remove(Entity entity){
    CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(entity);
    if (controller)
        destroy(*controller);
    refusedWithRigidbody.erase(entity);
}

void CharacterControllerSystem::refreshLayer(Entity entity){
    Runtime* runtime = find(entity);
    CharacterControllerComponent* controller = scene->findComponent<CharacterControllerComponent>(entity);
    if (runtime && controller && !physics->steppingWorld3D)
        applySettings(*runtime, *controller);
}

void CharacterControllerSystem::clear(){
    auto controllers = scene->getComponentArray<CharacterControllerComponent>();
    for (size_t i = 0; i < controllers->size(); i++){
        CharacterControllerComponent& controller = controllers->getComponentFromIndex(i);
        if (controller.handle != 0)
            destroy(controller);
    }
    // characters whose components are gone already
    for (auto& entry : runtimes){
        Runtime& runtime = entry.second;
        if (runtime.character){
            characterVsCharacter->Remove(runtime.character);
            runtime.character = nullptr;
        }
        physics->objectLayerTable3D->release(runtime.objectLayer);
    }
    runtimes.clear();
    innerBodies.clear();
}

#endif



