// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COLLISION2D_H
#define COLLISION2D_H

// What the 2D contact callbacks get (Unity Collision2D, ContactPoint2D, Collider2D): plain
// values copied at the step, safe to keep after the callback (physics-contacts spec 6.1.3).
// Colliders are named by their body's GameObject and index, resolved when read.

#include "Export.h"
#include "Entity.h"
#include "math/Vector2.h"

#include <string>
#include <vector>

namespace jasperengine {

    class Scene;
    class GameObject;
#ifdef JASPERENGINE_PHYSICS_2D
    class Rigidbody2D;
#endif

    // A 2D collider: the `index` entry of its Rigidbody 2D's colliders (Lua class "Collider2D")
    class JASPERENGINE_API ColliderRef2D {
    public:
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        int index = -1;

        ColliderRef2D() = default;
        ColliderRef2D(Scene* scene, Entity entity, int index);

        // the GameObject still has a Rigidbody 2D with this collider
        bool isValid() const;
        // the GameObject of the body (exists while it does, whatever the collider)
        bool hasGameObject() const;
        GameObject getGameObject() const;
#ifdef JASPERENGINE_PHYSICS_2D
        bool hasRigidbody() const;
        Rigidbody2D getRigidbody() const;
#endif
        int getIndex() const { return index; }
        // Collider2DType as an int
        int getType() const;
        // a sensor collider (Unity isTrigger)
        bool isTrigger() const;
        std::string getTag() const;
        bool compareTag(const std::string& tag) const;
        // the collider's receiver GameObject (Inspector "Send Callbacks To"), NULL_ENTITY if none
        Entity getReceiver() const;

        bool operator==(const ColliderRef2D& other) const {
            return scene == other.scene && entity == other.entity && index == other.index;
        }
    };

    // One contact point (Box2D's post-step manifold). `normal` points from the other collider
    // toward this one; `separation` < 0 when penetrating (points, the 2D unit).
    struct JASPERENGINE_API ContactPoint2D {
        Vector2 point;
        Vector2 normal;
        float separation = 0.0f;
        // Box2D's impulses of the step (N s)
        float normalImpulse = 0.0f;
        float tangentImpulse = 0.0f;
        // velocity(other) - velocity(this) at the point (points/s)
        Vector2 relativeVelocity;
        ColliderRef2D thisCollider;
        ColliderRef2D otherCollider;

        Vector2 getPoint() const { return point; }
        Vector2 getNormal() const { return normal; }
        float getSeparation() const { return separation; }
        float getNormalImpulse() const { return normalImpulse; }
        float getTangentImpulse() const { return tangentImpulse; }
        Vector2 getRelativeVelocity() const { return relativeVelocity; }
        ColliderRef2D getThisCollider() const { return thisCollider; }
        ColliderRef2D getOtherCollider() const { return otherCollider; }
    };

    // Unity Collision2D, seen from "this" side. Unity names the *other* side collider /
    // rigidbody and this side otherCollider / otherRigidbody; thisCollider / thisRigidbody
    // say it plainly.
    class JASPERENGINE_API Collision2D {
    public:
        ColliderRef2D thisCollider;
        ColliderRef2D otherCollider;
        // velocity(other) - velocity(this) (points/s); Enter: before the step
        Vector2 relativeVelocity;
        // false when preSolve2D disabled the contact this step
        bool enabled = true;
        // at most 2 per Box2D shape pair, 8 per collider pair (the deepest)
        std::vector<ContactPoint2D> contacts;

        bool hasGameObject() const { return otherCollider.hasGameObject(); }
        GameObject getGameObject() const;
        ColliderRef2D getCollider() const { return otherCollider; }
        ColliderRef2D getThisCollider() const { return thisCollider; }
#ifdef JASPERENGINE_PHYSICS_2D
        Rigidbody2D getRigidbody() const;
        Rigidbody2D getThisRigidbody() const;
#endif
        Vector2 getRelativeVelocity() const { return relativeVelocity; }
        bool isEnabled() const { return enabled; }
        int getContactCount() const { return (int)contacts.size(); }
        // 0-based; out of range: a default point (Lua raises an error)
        ContactPoint2D getContact(int index) const;
        const std::vector<ContactPoint2D>& getContacts() const { return contacts; }
    };

}

#endif // COLLISION2D_H
