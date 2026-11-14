// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Compound bodies from child GameObjects and Rigidbody.movePosition / moveRotation (S96 WP-C,
// docs/specs/S96-physics-gaps.md REQ-18..REQ-22, D6, D8): the free lookups of
// AttachedColliders.h and the PhysicsSystem steps that gather attached children, compose their
// ancestors again and move bodies to their move targets.

#include "physics/AttachedColliders.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/Transform.h"
#include "subsystem/PhysicsSystem.h"
#include "util/Angle.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "physics/JoltShapeBuilder.h"
#endif

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // A child moved on more consecutive steps than this composes its body every step: told once
    const int attachedRecomposeWarnSteps = 30;

    // The nearest ancestor with an enabled body of this kind not attached itself, else the
    // topmost attached one (its own body, nothing above it)
    template<typename Body>
    Entity findAttachBody(Scene* scene, Entity entity){
        if (!scene || !scene->isEntityCreated(entity)){
            return NULL_ENTITY;
        }
        const Transform* transform = scene->findComponent<Transform>(entity);
        Entity attached = NULL_ENTITY;
        for (int depth = 0; transform && transform->parent != NULL_ENTITY && depth < 256; depth++){
            const Entity parent = transform->parent;
            const Body* body = scene->findComponent<Body>(parent);
            if (body && body->enabled){
                if (!body->attachToParentBody){
                    return parent;
                }
                attached = parent;
            }
            transform = scene->findComponent<Transform>(parent);
        }
        return attached;
    }

    Vector3 attachedWorldScale(Scene* scene, Entity entity){
        const Transform* transform = scene->findComponent<Transform>(entity);
        return transform ? transform->worldScale : Vector3(1.0f, 1.0f, 1.0f);
    }

    // first-seen order of the bodies children were listed for, then the ones that hold
    // attached colliders and lost them all
    template<typename Child>
    std::vector<Entity> attachBodiesToCompose(const std::vector<Child>& children, const std::vector<Entity>& holding){
        std::vector<Entity> bodies;
        for (const Child& child : children){
            if (std::find(bodies.begin(), bodies.end(), child.body) == bodies.end()){
                bodies.push_back(child.body);
            }
        }
        for (Entity body : holding){
            if (std::find(bodies.begin(), bodies.end(), body) == bodies.end()){
                bodies.push_back(body);
            }
        }
        return bodies;
    }

}

bool AttachedColliders::poseInBody(Scene* scene, Entity ancestor, Entity child, Matrix4& matrix, Quaternion& rotation){
    std::vector<Entity> chain;
    Entity current = child;
    for (int depth = 0; current != NULL_ENTITY && current != ancestor && depth < 256; depth++){
        const Transform* transform = scene->findComponent<Transform>(current);
        if (!transform){
            return false;
        }
        chain.push_back(current);
        current = transform->parent;
    }
    if (current != ancestor || chain.empty()){
        return false;
    }
    const Transform* top = scene->findComponent<Transform>(ancestor);
    matrix = Matrix4::scaleMatrix(top ? top->worldScale : Vector3(1.0f, 1.0f, 1.0f));
    rotation = Quaternion::IDENTITY;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it){
        const Transform& transform = scene->getComponent<Transform>(*it);
        matrix = matrix * Matrix4::composeTRS(transform.position, transform.rotation, transform.scale);
        rotation = rotation * transform.rotation;
    }
    rotation = rotation.normalized();
    return true;
}

#ifdef JASPERENGINE_PHYSICS_3D
Entity AttachedColliders::findBody3D(Scene* scene, Entity entity){
    return findAttachBody<RigidbodyComponent>(scene, entity);
}

bool AttachedColliders::resolve3D(Scene* scene, Entity bodyEntity, int index, Entity& entity, int& colliderIndex){
    const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(bodyEntity);
    if (!body || index < 0){
        return false;
    }
    if ((size_t)index < body->colliders.size()){
        entity = bodyEntity;
        colliderIndex = index;
        return true;
    }
    const size_t slot = (size_t)index - body->colliders.size();
    if (slot >= body->attachedColliders.size()){
        return false;
    }
    entity = body->attachedColliders[slot].child;
    colliderIndex = (int)body->attachedColliders[slot].index;
    return true;
}

bool AttachedColliders::bodyIndexOf3D(Scene* scene, Entity child, int colliderIndex, Entity& bodyEntity, int& index){
    const RigidbodyComponent* childBody = scene->findComponent<RigidbodyComponent>(child);
    if (!childBody || !childBody->attachToParentBody || childBody->attachedTo == NULL_ENTITY || !scene->isEntityCreated(childBody->attachedTo)){
        return false;
    }
    const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(childBody->attachedTo);
    if (!body){
        return false;
    }
    for (size_t slot = 0; slot < body->attachedColliders.size(); slot++){
        const AttachedCollider3D& attached = body->attachedColliders[slot];
        if (attached.child == child && (int)attached.index == colliderIndex){
            bodyEntity = childBody->attachedTo;
            index = (int)(body->colliders.size() + slot);
            return true;
        }
    }
    return false;
}

bool PhysicsSystem::attachChild3D(Entity entity, RigidbodyComponent& body){
    const Entity ancestor = AttachedColliders::findBody3D(scene, entity);
    if (ancestor == NULL_ENTITY){
        body.attachedTo = NULL_ENTITY;
        return false;
    }
    if (!body.body.IsInvalid()){
        // it was its own body (its ancestor's came, or the flag was set in Play)
        contactBodyRemoved3D(entity);
        invalidateJoints3D(entity);
        destroyRigidbody(body);
    }

    // what makes the ancestor build this child's leaves again; the flags are the ancestor's now
    const Vector3 scale = attachedWorldScale(scene, entity);
    bool changed = body.attachedTo != ancestor || body.needUpdateColliders || body.needReloadBody || body.massDirty || scale != body.loadedScale;
    for (Collider3D& collider : body.colliders){
        changed = changed || collider.dirty;
        collider.dirty = false;
    }
    body.needUpdateColliders = false;
    body.needReloadBody = false;
    body.massDirty = false;
    body.loadedScale = scale;
    body.attachedTo = ancestor;

    if (body.enabled && scene->isEntityActiveInHierarchy(entity) && !body.colliders.empty()){
        attachedChildren3D.push_back({ancestor, entity, changed});
    }
    return true;
}

void PhysicsSystem::composeAttached3D(){
    if (attachedChildren3D.empty() && attachedBodies3D.empty()){
        return;
    }
    std::vector<Entity> holding;
    for (Entity bodyEntity : attachBodiesToCompose(attachedChildren3D, attachedBodies3D)){
        RigidbodyComponent* body = scene->isEntityCreated(bodyEntity) ? scene->findComponent<RigidbodyComponent>(bodyEntity) : nullptr;
        if (!body){
            continue;
        }
        std::vector<AttachedCollider3D> list;
        bool leavesChanged = false;
        for (const AttachedChild& child : attachedChildren3D){
            if (child.body != bodyEntity){
                continue;
            }
            const RigidbodyComponent* childBody = scene->findComponent<RigidbodyComponent>(child.child);
            Matrix4 matrix;
            Quaternion rotation;
            if (!childBody || !AttachedColliders::poseInBody(scene, bodyEntity, child.child, matrix, rotation)){
                continue;
            }
            for (size_t i = 0; i < childBody->colliders.size(); i++){
                const Collider3D& source = childBody->colliders[i];
                AttachedCollider3D entry;
                entry.child = child.child;
                entry.index = i;
                entry.collider = source;
                entry.collider.shape = nullptr;
                entry.collider.surface = nullptr;
                entry.collider.dirty = true;
                entry.collider.position = matrix * source.position;
                entry.collider.rotation = (rotation * source.rotation).normalized();
                entry.collider.receiver = child.child;
                // the leaf of the last compose while the child's colliders and scale stay
                if (!child.changed){
                    for (const AttachedCollider3D& old : body->attachedColliders){
                        if (old.child == child.child && old.index == i && old.collider.shape){
                            entry.collider.shape = old.collider.shape;
                            entry.collider.surface = old.collider.surface;
                            entry.collider.dirty = false;
                            break;
                        }
                    }
                }
                leavesChanged = leavesChanged || entry.collider.dirty;
                list.push_back(std::move(entry));
            }
        }

        const std::vector<AttachedCollider3D>& old = body->attachedColliders;
        const bool sameList = list.size() == old.size();
        bool samePoses = sameList;
        for (size_t k = 0; samePoses && k < list.size(); k++){
            samePoses = list[k].child == old[k].child && list[k].index == old[k].index &&
                        list[k].collider.position == old[k].collider.position && list[k].collider.rotation == old[k].collider.rotation;
        }
        if (!leavesChanged && samePoses){
            body->attachedRecomposeSteps = 0;
            if (!list.empty()){
                holding.push_back(bodyEntity);
            }
            continue;
        }
        // only children moved: an animation moving one every step composes the body every step
        if (!leavesChanged && sameList){
            body->attachedRecomposeSteps++;
            if (body->attachedRecomposeSteps > attachedRecomposeWarnSteps && !body->warnedAttachedRecompose){
                body->warnedAttachedRecompose = true;
                Log::warn("Rigidbody '%s': a child with Attach To Parent Body moved on %d steps in a row, so the body's colliders are composed again "
                          "every step. Give a moving child its own kinematic Rigidbody (Attach To Parent Body off) instead.",
                          scene->getEntityName(bodyEntity).c_str(), body->attachedRecomposeSteps);
            }
        }else{
            body->attachedRecomposeSteps = 0;
        }

        body->attachedColliders = std::move(list);
        body->needUpdateColliders = true;
        refreshListenerMask3D(bodyEntity, *body);
        reshapeRigidbody3D(bodyEntity, *body);
        if (!body->attachedColliders.empty()){
            holding.push_back(bodyEntity);
        }
    }
    attachedBodies3D = std::move(holding);
    attachedChildren3D.clear();
}

void PhysicsSystem::applyMoveTarget3D(Entity entity, RigidbodyComponent& body, float stepTime){
    JPH::BodyInterface& bodyInterface = world3D.GetBodyInterfaceNoLock();
    JPH::Vec3 linearVelocity;
    JPH::Vec3 angularVelocity;
    bodyInterface.GetLinearAndAngularVelocity(body.body, linearVelocity, angularVelocity);

    if (!body.hasMovePosition && !body.hasMoveRotation){
        // a kinematic body that reached its target stops, unless a script gave it a velocity since
        body.movingToTarget = false;
        if (body.type == BodyType::KINEMATIC){
            if (Vector3(linearVelocity.GetX(), linearVelocity.GetY(), linearVelocity.GetZ()) == body.movedLinearVelocity){
                linearVelocity = JPH::Vec3::sZero();
            }
            if (Vector3(angularVelocity.GetX(), angularVelocity.GetY(), angularVelocity.GetZ()) == body.movedAngularVelocity){
                angularVelocity = JPH::Vec3::sZero();
            }
            bodyInterface.SetLinearAndAngularVelocity(body.body, linearVelocity, angularVelocity);
        }
        return;
    }

    JPH::Vec3 position;
    JPH::Quat rotation;
    bodyInterface.GetPositionAndRotation(body.body, position, rotation);
    const JPH::Vec3 targetPosition = body.hasMovePosition ? JPH::Vec3(body.movePosition.x, body.movePosition.y, body.movePosition.z) : position;
    const JPH::Quat targetRotation = body.hasMoveRotation ? toValidatedJoltRotation(body.moveRotation, entity, -1) : rotation;

    if (body.type == BodyType::KINEMATIC && !body.newBody){
        // by velocity, which pushes and carries what it touches: over this step when asked in a
        // fixed step, else over the steps of the frame (Transform following's rule)
        if (body.moveStepsLeft <= 0){
            body.moveStepsLeft = body.moveFromFixedStep ? 1 : std::max(1, Engine::getFixedStepsLeft());
        }
        const float fraction = 1.0f / (float)body.moveStepsLeft;
        const JPH::Vec3 stepPosition = position + (targetPosition - position) * fraction;
        const JPH::Quat stepRotation = rotation.SLERP(targetRotation, fraction).Normalized();
        bodyInterface.MoveKinematic(body.body, stepPosition, stepRotation, stepTime);
        // a part not moved keeps its own velocity
        if (!body.hasMovePosition){
            bodyInterface.SetLinearVelocity(body.body, linearVelocity);
        }
        if (!body.hasMoveRotation){
            bodyInterface.SetAngularVelocity(body.body, angularVelocity);
        }
        bodyInterface.GetLinearAndAngularVelocity(body.body, linearVelocity, angularVelocity);
        body.movedLinearVelocity = Vector3(linearVelocity.GetX(), linearVelocity.GetY(), linearVelocity.GetZ());
        body.movedAngularVelocity = Vector3(angularVelocity.GetX(), angularVelocity.GetY(), angularVelocity.GetZ());
        body.movingToTarget = true;
        if (--body.moveStepsLeft > 0){
            return;
        }
    }else{
        // static, dynamic, or a body not stepped yet: placed there before the step, its velocity
        // kept (Unity); the pose drawn before the step stays where it was, so it interpolates
        bodyInterface.SetPositionAndRotation(body.body, targetPosition, targetRotation,
                                             body.type == BodyType::STATIC ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        body.moveStepsLeft = 0;
    }
    body.hasMovePosition = false;
    body.hasMoveRotation = false;
}
#endif

#ifdef JASPERENGINE_PHYSICS_2D
Entity AttachedColliders::findBody2D(Scene* scene, Entity entity){
    return findAttachBody<Rigidbody2DComponent>(scene, entity);
}

bool AttachedColliders::resolve2D(Scene* scene, Entity bodyEntity, int key, Entity& entity, int& colliderIndex){
    const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(bodyEntity);
    if (!body || !isAttachedCollider2DKey(key)){
        return false;
    }
    const size_t slot = (size_t)(key & (ATTACHED_COLLIDER2D_BIT - 1));
    if (slot >= body->attachedColliders.size()){
        return false;
    }
    entity = body->attachedColliders[slot].child;
    colliderIndex = (int)body->attachedColliders[slot].index;
    return true;
}

bool AttachedColliders::bodyKeyOf2D(Scene* scene, Entity child, int colliderIndex, Entity& bodyEntity, int& key){
    const Rigidbody2DComponent* childBody = scene->findComponent<Rigidbody2DComponent>(child);
    if (!childBody || !childBody->attachToParentBody || childBody->attachedTo == NULL_ENTITY || !scene->isEntityCreated(childBody->attachedTo)){
        return false;
    }
    const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(childBody->attachedTo);
    if (!body){
        return false;
    }
    for (size_t slot = 0; slot < body->attachedColliders.size(); slot++){
        const AttachedCollider2D& attached = body->attachedColliders[slot];
        if (attached.child == child && (int)attached.index == colliderIndex){
            bodyEntity = childBody->attachedTo;
            key = attachedKey2D(slot);
            return true;
        }
    }
    return false;
}

bool PhysicsSystem::attachChild2D(Entity entity, Rigidbody2DComponent& body){
    const Entity ancestor = AttachedColliders::findBody2D(scene, entity);
    if (ancestor == NULL_ENTITY){
        body.attachedTo = NULL_ENTITY;
        return false;
    }
    if (b2Body_IsValid(body.body)){
        contactBodyRemoved2D(entity);
        destroyRigidbody2D(body);
    }
    // a collider following a sprite slice takes the frame's box first
    for (const Collider2D& collider : body.colliders){
        if (!collider.slice.empty()){
            updateSliceColliders2D(entity, body);
            break;
        }
    }

    const Vector3 worldScale = attachedWorldScale(scene, entity);
    const Vector2 scale(worldScale.x, worldScale.y);
    bool changed = body.attachedTo != ancestor || body.needUpdateColliders || body.needReloadBody || scale != body.loadedScale;
    for (Collider2D& collider : body.colliders){
        changed = changed || collider.dirty;
        collider.dirty = false;
    }
    body.needUpdateColliders = false;
    body.needReloadBody = false;
    body.needApplyMass = false;
    body.loadedScale = scale;
    body.attachedTo = ancestor;

    if (body.enabled && scene->isEntityActiveInHierarchy(entity) && !body.colliders.empty()){
        attachedChildren2D.push_back({ancestor, entity, changed});
    }
    return true;
}

namespace {

    bool sameAttachedGeometry2D(const Collider2D& a, const Collider2D& b){
        return a.type == b.type && a.pointA == b.pointA && a.pointB == b.pointB && a.radius == b.radius && a.points == b.points && a.loop == b.loop;
    }

}

void PhysicsSystem::composeAttached2D(){
    if (attachedChildren2D.empty() && attachedBodies2D.empty()){
        return;
    }
    std::vector<Entity> holding;
    for (Entity bodyEntity : attachBodiesToCompose(attachedChildren2D, attachedBodies2D)){
        Rigidbody2DComponent* body = scene->isEntityCreated(bodyEntity) ? scene->findComponent<Rigidbody2DComponent>(bodyEntity) : nullptr;
        if (!body){
            continue;
        }
        std::vector<AttachedCollider2D> list;
        bool changed = false;
        for (const AttachedChild& child : attachedChildren2D){
            if (child.body != bodyEntity){
                continue;
            }
            const Rigidbody2DComponent* childBody = scene->findComponent<Rigidbody2DComponent>(child.child);
            Matrix4 matrix;
            Quaternion rotation;
            if (!childBody || !AttachedColliders::poseInBody(scene, bodyEntity, child.child, matrix, rotation)){
                continue;
            }
            changed = changed || child.changed;
            auto toBody = [&](const Vector2& point){
                const Vector3 placed = matrix * Vector3(point.x, point.y, 0.0f);
                return Vector2(placed.x, placed.y);
            };
            // a mirroring pose flips a chain's winding, which would swap its colliding side
            const Vector2 origin = toBody(Vector2::ZERO);
            const Vector2 axisX = toBody(Vector2(1.0f, 0.0f)) - origin;
            const Vector2 axisY = toBody(Vector2(0.0f, 1.0f)) - origin;
            const bool mirrored = axisX.x * axisY.y - axisX.y * axisY.x < 0.0f;
            const Vector3 childScale = attachedWorldScale(scene, child.child);
            const float radiusScale = std::max(std::fabs(childScale.x), std::fabs(childScale.y));

            for (size_t i = 0; i < childBody->colliders.size(); i++){
                const Collider2D& source = childBody->colliders[i];
                AttachedCollider2D entry;
                entry.child = child.child;
                entry.index = i;
                entry.collider = source;
                entry.collider.shapes.clear();
                entry.collider.chain = b2_nullChainId;
                entry.collider.dirty = true;
                entry.collider.decompositionKey = 0;
                entry.collider.pieces.clear();
                entry.collider.generatedKey = -1;
                if (!source.slice.empty()){
                    if (!source.sliceVisible || source.slicePoints.size() < 3){
                        continue;
                    }
                    entry.collider.type = Collider2DType::POLYGON;
                    entry.collider.points = source.slicePoints;
                    entry.collider.slice.clear();
                }
                entry.collider.pointA = toBody(source.pointA);
                entry.collider.pointB = toBody(source.pointB);
                entry.collider.radius = source.radius * radiusScale;
                for (Vector2& point : entry.collider.points){
                    point = toBody(point);
                }
                if (mirrored && entry.collider.type == Collider2DType::CHAIN){
                    std::reverse(entry.collider.points.begin(), entry.collider.points.end());
                }
                entry.collider.receiver = child.child;
                list.push_back(std::move(entry));
            }
        }

        const std::vector<AttachedCollider2D>& old = body->attachedColliders;
        const bool sameList = list.size() == old.size();
        bool sameGeometry = sameList;
        for (size_t k = 0; sameGeometry && k < list.size(); k++){
            sameGeometry = list[k].child == old[k].child && list[k].index == old[k].index && sameAttachedGeometry2D(list[k].collider, old[k].collider);
        }
        if (!changed && sameGeometry){
            body->attachedRecomposeSteps = 0;
            if (!list.empty()){
                holding.push_back(bodyEntity);
            }
            continue;
        }
        if (!changed && sameList){
            body->attachedRecomposeSteps++;
            if (body->attachedRecomposeSteps > attachedRecomposeWarnSteps && !body->warnedAttachedRecompose){
                body->warnedAttachedRecompose = true;
                Log::warn("Rigidbody 2D '%s': a child with Attach To Parent Body moved on %d steps in a row, so the body's shapes are made again "
                          "every step. Give a moving child its own kinematic Rigidbody 2D (Attach To Parent Body off) instead.",
                          scene->getEntityName(bodyEntity).c_str(), body->attachedRecomposeSteps);
            }
        }else{
            body->attachedRecomposeSteps = 0;
        }

        for (AttachedCollider2D& attached : body->attachedColliders){
            destroyCollider2DHandles(attached.collider);
        }
        body->attachedColliders = std::move(list);
        // the new shapes take the events the children's scripts ask for
        refreshListenerMask2D(bodyEntity, *body);
        // a body not made yet makes them with its shapes (syncRigidbody2DColliders)
        if (b2Body_IsValid(body->body)){
            syncAttachedColliders2D(bodyEntity, *body, true);
            applyRigidbody2DMass(bodyEntity, *body);
            if (body->type != BodyType::STATIC){
                b2Body_SetAwake(body->body, true);
            }
        }
        if (!body->attachedColliders.empty()){
            holding.push_back(bodyEntity);
        }
    }
    attachedBodies2D = std::move(holding);
    attachedChildren2D.clear();
}

bool PhysicsSystem::syncAttachedColliders2D(Entity entity, Rigidbody2DComponent& body, bool all){
    bool made = false;
    // the body's own layer: one body, one layer (as in 3D)
    const int layer = scene->getEntityLayer(entity);
    for (size_t slot = 0; slot < body.attachedColliders.size(); slot++){
        Collider2D& collider = body.attachedColliders[slot].collider;
        if (!all && !collider.dirty){
            continue;
        }
        destroyCollider2DHandles(collider);
        // posed and scaled in the body's frame already
        createCollider2DShapesOf(entity, body, collider, (size_t)AttachedColliders::attachedKey2D(slot), Vector2(1.0f, 1.0f), layer);
        collider.dirty = false;
        made = true;
    }
    return made;
}

void PhysicsSystem::applyMoveTarget2D(Entity entity, Rigidbody2DComponent& body, float stepTime){
    (void)entity;
    if (!body.hasMovePosition && !body.hasMoveRotation){
        body.movingToTarget = false;
        if (body.type == BodyType::KINEMATIC){
            if (b2Body_GetLinearVelocity(body.body) == body.movedLinearVelocity){
                b2Body_SetLinearVelocity(body.body, b2Vec2_zero);
            }
            if (b2Body_GetAngularVelocity(body.body) == body.movedAngularVelocity){
                b2Body_SetAngularVelocity(body.body, 0.0f);
            }
        }
        return;
    }

    const b2Transform transform = b2Body_GetTransform(body.body);
    const float angle = b2Rot_GetAngle(transform.q);
    const b2Vec2 targetPosition = body.hasMovePosition ? b2Vec2{body.movePosition.x / pointsToMeterScale2D, body.movePosition.y / pointsToMeterScale2D} : transform.p;
    const float targetAngle = body.hasMoveRotation ? Angle::defaultToRad(body.moveAngle) : angle;

    if (body.type == BodyType::KINEMATIC && !body.newBody){
        if (body.moveStepsLeft <= 0){
            body.moveStepsLeft = body.moveFromFixedStep ? 1 : std::max(1, Engine::getFixedStepsLeft());
        }
        const float duration = stepTime * (float)body.moveStepsLeft;
        if (body.hasMovePosition){
            b2Body_SetLinearVelocity(body.body, (targetPosition - transform.p) * (1.0f / duration));
        }
        if (body.hasMoveRotation){
            b2Body_SetAngularVelocity(body.body, b2UnwindAngle(targetAngle - angle) / duration);
        }
        b2Body_SetAwake(body.body, true);
        body.movedLinearVelocity = b2Body_GetLinearVelocity(body.body);
        body.movedAngularVelocity = b2Body_GetAngularVelocity(body.body);
        body.movingToTarget = true;
        if (--body.moveStepsLeft > 0){
            return;
        }
    }else{
        b2Body_SetTransform(body.body, targetPosition, b2MakeRot(targetAngle));
        if (body.type != BodyType::STATIC){
            b2Body_SetAwake(body.body, true);
        }
        body.moveStepsLeft = 0;
    }
    body.hasMovePosition = false;
    body.hasMoveRotation = false;
}
#endif
