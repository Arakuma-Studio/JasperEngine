// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Collision.h"

#include "Scene.h"
#include "object/GameObject.h"
#include "registry/TagManager.h"
#include "Log.h"
#ifdef JASPERENGINE_PHYSICS_3D
#include "Rigidbody.h"
#include "component/RigidbodyComponent.h"
#include "physics/AttachedColliders.h"
#endif

using namespace jasperengine;

namespace {

#ifdef JASPERENGINE_PHYSICS_3D
    const RigidbodyComponent* bodyOf(const ColliderRef& ref) {
        if (!ref.scene || ref.entity == NULL_ENTITY || !ref.scene->isEntityCreated(ref.entity)) {
            return nullptr;
        }
        return ref.scene->findComponent<RigidbodyComponent>(ref.entity);
    }

    bool liveAttachedBody3D(Scene* scene, Entity body) {
        return scene->isEntityCreated(body) && scene->findComponent<RigidbodyComponent>(body) != nullptr;
    }

    const Collider3D* colliderOf(const ColliderRef& ref) {
        const RigidbodyComponent* body = bodyOf(ref);
        if (!body || ref.index < 0 || (size_t)ref.index >= body->colliders.size()) {
            return nullptr;
        }
        return &body->colliders[(size_t)ref.index];
    }
#endif

}

ColliderRef::ColliderRef(Scene* scene, Entity entity, int index)
    : scene(scene), entity(entity), index(index) {
#ifdef JASPERENGINE_PHYSICS_3D
    // a body's index past its own colliders names an attached child's collider (S96 REQ-18)
    if (AttachedColliders::anyAttached3D() && scene && index >= 0 && entity != NULL_ENTITY && scene->isEntityCreated(entity)) {
        const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
        if (body && !body->attachedColliders.empty() && (size_t)index >= body->colliders.size()) {
            AttachedColliders::resolve3D(scene, entity, index, this->entity, this->index);
        }
    }
#endif
}

bool ColliderRef::isValid() const {
#ifdef JASPERENGINE_PHYSICS_3D
    return colliderOf(*this) != nullptr;
#else
    return false;
#endif
}

bool ColliderRef::hasGameObject() const {
    return scene && entity != NULL_ENTITY && scene->isEntityCreated(entity);
}

GameObject ColliderRef::getGameObject() const {
    return GameObject(scene, hasGameObject() ? entity : NULL_ENTITY);
}

#ifdef JASPERENGINE_PHYSICS_3D
bool ColliderRef::hasRigidbody() const {
    return bodyOf(*this) != nullptr;
}

Rigidbody ColliderRef::getRigidbody() const {
    // an attached child's collider belongs to the body it joined (Unity attachedRigidbody)
    const RigidbodyComponent* body = bodyOf(*this);
    if (body && body->attachToParentBody && body->attachedTo != NULL_ENTITY && liveAttachedBody3D(scene, body->attachedTo)) {
        return Rigidbody(scene, body->attachedTo);
    }
    return Rigidbody(scene, body ? entity : NULL_ENTITY);
}
#endif

int ColliderRef::getType() const {
#ifdef JASPERENGINE_PHYSICS_3D
    if (const Collider3D* collider = colliderOf(*this)) {
        return (int)collider->type;
    }
#endif
    return -1;
}

bool ColliderRef::isTrigger() const {
#ifdef JASPERENGINE_PHYSICS_3D
    if (const RigidbodyComponent* body = bodyOf(*this)) {
        // the collider's isTrigger, or the whole body a sensor (legacy)
        return body->sensor || (index >= 0 && (size_t)index < body->colliders.size() && body->colliders[(size_t)index].isTrigger);
    }
#endif
    return false;
}

std::string ColliderRef::getTag() const {
    return hasGameObject() ? scene->getEntityTag(entity) : std::string();
}

bool ColliderRef::compareTag(const std::string& tag) const {
    if (!TagManager::isTagDefined(tag)) {
        Log::error("Tag: %s is not defined.", tag.c_str());
        return false;
    }
    return hasGameObject() && scene->getEntityTag(entity) == tag;
}

Entity ColliderRef::getReceiver() const {
#ifdef JASPERENGINE_PHYSICS_3D
    if (const Collider3D* collider = colliderOf(*this)) {
        if (collider->receiver != NULL_ENTITY && scene->isEntityCreated(collider->receiver)) {
            return collider->receiver;
        }
    }
#endif
    return NULL_ENTITY;
}

GameObject Collision::getGameObject() const {
    return otherCollider.getGameObject();
}

#ifdef JASPERENGINE_PHYSICS_3D
Rigidbody Collision::getRigidbody() const {
    return otherCollider.getRigidbody();
}

Rigidbody Collision::getThisRigidbody() const {
    return thisCollider.getRigidbody();
}
#endif

ContactPoint Collision::getContact(int index) const {
    if (index < 0 || (size_t)index >= contacts.size()) {
        return ContactPoint();
    }
    return contacts[(size_t)index];
}
