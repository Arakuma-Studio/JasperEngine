// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Collision2D.h"

#include "Scene.h"
#include "object/GameObject.h"
#include "registry/TagManager.h"
#include "Log.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "Rigidbody2D.h"
#include "component/Rigidbody2DComponent.h"
#include "physics/AttachedColliders.h"
#endif

using namespace jasperengine;

namespace {

#ifdef JASPERENGINE_PHYSICS_2D
    const Rigidbody2DComponent* bodyOf(const ColliderRef2D& ref) {
        if (!ref.scene || ref.entity == NULL_ENTITY || !ref.scene->isEntityCreated(ref.entity)) {
            return nullptr;
        }
        return ref.scene->findComponent<Rigidbody2DComponent>(ref.entity);
    }

    const Collider2D* colliderOf(const ColliderRef2D& ref) {
        const Rigidbody2DComponent* body = bodyOf(ref);
        return body ? findCollider2D(*body, ref.index) : nullptr;
    }
#endif

}

ColliderRef2D::ColliderRef2D(Scene* scene, Entity entity, int index)
    : scene(scene), entity(entity), index(index) {
#ifdef JASPERENGINE_PHYSICS_2D
    // a body's attached key names an attached child's collider (S96 REQ-18)
    if (scene && isAttachedCollider2DKey(index) && entity != NULL_ENTITY && scene->isEntityCreated(entity)) {
        AttachedColliders::resolve2D(scene, entity, index, this->entity, this->index);
    }
#endif
}

bool ColliderRef2D::isValid() const {
#ifdef JASPERENGINE_PHYSICS_2D
    return colliderOf(*this) != nullptr;
#else
    return false;
#endif
}

bool ColliderRef2D::hasGameObject() const {
    return scene && entity != NULL_ENTITY && scene->isEntityCreated(entity);
}

GameObject ColliderRef2D::getGameObject() const {
    return GameObject(scene, hasGameObject() ? entity : NULL_ENTITY);
}

#ifdef JASPERENGINE_PHYSICS_2D
bool ColliderRef2D::hasRigidbody() const {
    return bodyOf(*this) != nullptr;
}

Rigidbody2D ColliderRef2D::getRigidbody() const {
    // an attached child's collider belongs to the body it joined (Unity attachedRigidbody)
    const Rigidbody2DComponent* body = bodyOf(*this);
    if (body && body->attachToParentBody && body->attachedTo != NULL_ENTITY && scene->isEntityCreated(body->attachedTo) &&
            scene->findComponent<Rigidbody2DComponent>(body->attachedTo)) {
        return Rigidbody2D(scene, body->attachedTo);
    }
    return Rigidbody2D(scene, body ? entity : NULL_ENTITY);
}
#endif

int ColliderRef2D::getType() const {
#ifdef JASPERENGINE_PHYSICS_2D
    if (const Collider2D* collider = colliderOf(*this)) {
        return (int)collider->type;
    }
#endif
    return -1;
}

bool ColliderRef2D::isTrigger() const {
#ifdef JASPERENGINE_PHYSICS_2D
    if (const Collider2D* collider = colliderOf(*this)) {
        return collider->sensor;
    }
#endif
    return false;
}

std::string ColliderRef2D::getTag() const {
    return hasGameObject() ? scene->getEntityTag(entity) : std::string();
}

bool ColliderRef2D::compareTag(const std::string& tag) const {
    if (!TagManager::isTagDefined(tag)) {
        Log::error("Tag: %s is not defined.", tag.c_str());
        return false;
    }
    return hasGameObject() && scene->getEntityTag(entity) == tag;
}

Entity ColliderRef2D::getReceiver() const {
#ifdef JASPERENGINE_PHYSICS_2D
    if (const Collider2D* collider = colliderOf(*this)) {
        if (collider->receiver != NULL_ENTITY && scene->isEntityCreated(collider->receiver)) {
            return collider->receiver;
        }
    }
#endif
    return NULL_ENTITY;
}

GameObject Collision2D::getGameObject() const {
    return otherCollider.getGameObject();
}

#ifdef JASPERENGINE_PHYSICS_2D
Rigidbody2D Collision2D::getRigidbody() const {
    return otherCollider.getRigidbody();
}

Rigidbody2D Collision2D::getThisRigidbody() const {
    return thisCollider.getRigidbody();
}
#endif

ContactPoint2D Collision2D::getContact(int index) const {
    if (index < 0 || (size_t)index >= contacts.size()) {
        return ContactPoint2D();
    }
    return contacts[(size_t)index];
}
