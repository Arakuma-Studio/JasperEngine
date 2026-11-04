// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "subsystem/FoliagePhysics.h"

#include "Log.h"
#include "Scene.h"
#include "component/Transform.h"
#include "subsystem/NavigationSystem.h"
#include "component/NavMeshSurfaceComponent.h"
#include "math/AABB.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#include "object/physics/Contact3D.h"
#include "object/physics/Rigidbody.h"
#include "subsystem/PhysicsSystem.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/StaticCompoundShape.h"
#endif

#include <algorithm>
#include <cmath>

using namespace jasperengine;

bool foliagephysics::simulating(Scene& scene) {
#ifdef JASPERENGINE_PHYSICS_3D
    const std::shared_ptr<PhysicsSystem> physics = scene.getSystem<PhysicsSystem>();
    return physics && !physics->isPaused();
#else
    (void)scene;
    return false;
#endif
}

void foliagephysics::destroyCellBody(Scene& scene, CellBody& body) {
    if (body.entity != NULL_ENTITY && scene.isEntityCreated(body.entity)) {
        scene.destroyEntity(body.entity);
    }
    body.entity = NULL_ENTITY;
    body.ids.clear();
}

void foliagephysics::setCellBody(Scene& scene, Entity terrain, CellBody& body, const std::vector<TreeCapsule>& capsules) {
#ifdef JASPERENGINE_PHYSICS_3D
    if (capsules.empty()) {
        destroyCellBody(scene, body);
        return;
    }
    const Transform* terrainTransform = scene.findComponent<Transform>(terrain);
    if (!terrainTransform) return;
    const Vector3 scale = terrainTransform->worldScale;
    const float across = std::max(std::abs(scale.x), std::abs(scale.z));
    const float along = std::abs(scale.y);

    // R18: one capsule per tree, each knowing its slot, so colliderIndex names the tree
    JPH::StaticCompoundShapeSettings compound;
    std::vector<uint32_t> ids;
    ids.reserve(capsules.size());
    for (const TreeCapsule& capsule : capsules) {
        const float radius = std::max(capsule.radius * across, 0.001f);
        const float height = capsule.height * along;
        // Jolt's half height is of the cylinder between the caps
        const float halfCylinder = std::max(height * 0.5f - radius, 0.001f);
        JPH::Ref<JPH::CapsuleShape> shape = new JPH::CapsuleShape(halfCylinder, radius);
        shape->SetUserData(static_cast<JPH::uint64>(ids.size()));
        const JPH::Vec3 position(capsule.base.x * scale.x, capsule.base.y * scale.y + std::max(height * 0.5f, radius), capsule.base.z * scale.z);
        compound.AddShape(position, JPH::Quat::sIdentity(), shape);
        ids.push_back(capsule.id);
    }
    JPH::ShapeSettings::ShapeResult result = compound.Create();
    if (!result.IsValid()) {
        Log::error("Cannot create the tree capsules of a Terrain cell: %s", result.GetError().c_str());
        return;
    }

    std::shared_ptr<PhysicsSystem> physics = scene.getSystem<PhysicsSystem>();
    if (body.entity == NULL_ENTITY) {
        // engine-owned, unsaved and outside the hierarchy, as the foliage precedent
        body.entity = scene.createEntity();
        scene.addComponent<Transform>(body.entity);
        Transform& transform = scene.getComponent<Transform>(body.entity);
        transform.position = terrainTransform->worldPosition;
        transform.rotation = terrainTransform->worldRotation;
        transform.worldPosition = terrainTransform->worldPosition;
        transform.worldRotation = terrainTransform->worldRotation;
        scene.addComponent<RigidbodyComponent>(body.entity);
        scene.getComponent<RigidbodyComponent>(body.entity).type = BodyType::STATIC;
    }
    physics->setPrebuiltBodyShape3D(body.entity, result.Get());
    body.ids = std::move(ids);
#else
    (void)scene;
    (void)terrain;
    (void)body;
    (void)capsules;
#endif
}

Subscription foliagephysics::subscribeContacts(Scene& scene, ContactFn handler) {
#ifdef JASPERENGINE_PHYSICS_3D
    std::shared_ptr<PhysicsSystem> physics = scene.getSystem<PhysicsSystem>();
    if (!physics) return Subscription();
    return physics->onContactAdded3D.subscribe([handler = std::move(handler)](Rigidbody first, Rigidbody second, Contact3D contact) {
        const Vector3 point = contact.getBaseOffset();
        const Vector3 normal = contact.getWorldSpaceNormal();
        const float speed = (first.getLinearVelocity() - second.getLinearVelocity()).length();
        // each body may be a cell's: the handler knows, and the other never is a tree's at the same time for static pairs
        handler(first.getEntity(), contact.getColliderIndex1(), second.getEntity(), point, normal, speed);
        handler(second.getEntity(), contact.getColliderIndex2(), first.getEntity(), point, normal, speed);
    });
#else
    (void)scene;
    (void)handler;
    return Subscription();
#endif
}

void foliagephysics::updateNavigationAround(Scene& scene, const Vector3& position, float radius) {
    std::shared_ptr<NavigationSystem> navigation = scene.getSystem<NavigationSystem>();
    if (!navigation) return;
    auto surfaces = scene.getComponentArray<NavMeshSurfaceComponent>();
    const Vector3 reach(radius, radius, radius);
    const AABB bounds(position - reach, position + reach);
    for (int i = 0; i < surfaces->size(); i++) {
        std::string error;
        if (!navigation->updateTiles(surfaces->getEntity(i), bounds, error) && !error.empty()) {
            Log::warn("A removed tree could not update its navmesh tiles: %s", error.c_str());
        }
    }
}
