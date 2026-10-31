// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COLLISION_H
#define COLLISION_H

// What the 3D contact callbacks get (Unity Collision, ContactPoint, Collider): plain values
// copied at the step, safe to keep after the callback (physics-contacts spec 6.1.3). They
// name colliders by their body's GameObject and index, resolved when read: a GameObject or
// collider gone since makes them invalid (no throw).

#include "Export.h"
#include "Entity.h"
#include "math/Vector3.h"

#include <string>
#include <vector>

namespace jasperengine {

    class Scene;
    class GameObject;
#ifdef JASPERENGINE_PHYSICS_3D
    class Rigidbody;
#endif

    // A 3D collider: the `index` entry of its Rigidbody's colliders (Lua class "Collider")
    class JASPERENGINE_API ColliderRef {
    public:
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        int index = -1;

        ColliderRef() = default;
        ColliderRef(Scene* scene, Entity entity, int index);

        // the GameObject still has a Rigidbody with this collider
        bool isValid() const;
        // the GameObject of the body (exists while it does, whatever the collider)
        bool hasGameObject() const;
        GameObject getGameObject() const;
#ifdef JASPERENGINE_PHYSICS_3D
        bool hasRigidbody() const;
        Rigidbody getRigidbody() const;
#endif
        int getIndex() const { return index; }
        // Collider3DType as an int (the enum lives with the 3D component)
        int getType() const;
        // a trigger collider (its isTrigger, or its body a sensor)
        bool isTrigger() const;
        std::string getTag() const;
        bool compareTag(const std::string& tag) const;
        // the collider's receiver GameObject (Inspector "Send Callbacks To"), NULL_ENTITY if none
        Entity getReceiver() const;

        bool operator==(const ColliderRef& other) const {
            return scene == other.scene && entity == other.entity && index == other.index;
        }
    };

    // One contact point of a pair. `normal` points from the other collider toward this one
    // (v:reflect(normal) bounces); `separation` < 0 when penetrating (metres).
    struct JASPERENGINE_API ContactPoint {
        Vector3 point;
        Vector3 normal;
        float separation = 0.0f;
        // estimated impulse on this body (N s), zero for Exit and triggers
        Vector3 impulse;
        ColliderRef thisCollider;
        ColliderRef otherCollider;

        Vector3 getPoint() const { return point; }
        Vector3 getNormal() const { return normal; }
        float getSeparation() const { return separation; }
        Vector3 getImpulse() const { return impulse; }
        ColliderRef getThisCollider() const { return thisCollider; }
        ColliderRef getOtherCollider() const { return otherCollider; }
    };

    // Unity Collision, seen from "this" side
    class JASPERENGINE_API Collision {
    public:
        ColliderRef thisCollider;
        ColliderRef otherCollider;
        // estimated impulse on this body for the pair (N s, Jolt EstimateCollisionResponse)
        Vector3 impulse;
        // velocity(other) - velocity(this) at the contact (m/s): a ball falling on a floor sees +y
        Vector3 relativeVelocity;
        // at most 4 per manifold, 8 per collider pair (the deepest)
        std::vector<ContactPoint> contacts;

        // the other side (Unity's gameObject, collider, rigidbody, transform)
        bool hasGameObject() const { return otherCollider.hasGameObject(); }
        GameObject getGameObject() const;
        ColliderRef getCollider() const { return otherCollider; }
        ColliderRef getThisCollider() const { return thisCollider; }
#ifdef JASPERENGINE_PHYSICS_3D
        Rigidbody getRigidbody() const;
        Rigidbody getThisRigidbody() const;
#endif
        Vector3 getImpulse() const { return impulse; }
        Vector3 getRelativeVelocity() const { return relativeVelocity; }
        int getContactCount() const { return (int)contacts.size(); }
        // 0-based; out of range: a default point (Lua raises an error)
        ContactPoint getContact(int index) const;
        const std::vector<ContactPoint>& getContacts() const { return contacts; }
    };

}

#endif // COLLISION_H
