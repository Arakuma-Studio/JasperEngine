// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef ATTACHEDCOLLIDERS_H
#define ATTACHEDCOLLIDERS_H

// Compound bodies from child GameObjects (docs/specs/S96-physics-gaps.md REQ-18, REQ-19, D6):
// a Rigidbody (2D) with attachToParentBody puts its colliders in the body of its nearest
// ancestor with an enabled Rigidbody (2D) of the same dimension, Unity's colliders below a
// Rigidbody. The ancestor holds copies of them posed in its frame, after its own colliders in a
// combined collider index (3D) or under ATTACHED_COLLIDER2D_BIT keys (2D); a ColliderRef made
// from such an index names the child's own collider. PhysicsSystem gathers the children each
// step and composes the ancestor again when they changed (AttachedColliders.cpp).

#include "Export.h"
#include "Entity.h"
#include "component/RigidbodyComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"

#include <cstddef>

namespace jasperengine{

    class Scene;

    namespace AttachedColliders{

#ifdef JASPERENGINE_PHYSICS_3D
        // The body the colliders of an attachToParentBody GameObject join: the nearest ancestor
        // with an enabled Rigidbody that is not attached itself, else the topmost attached one
        // (which has no body above it, so it is its own). NULL_ENTITY: none, the GameObject is
        // its own body. Does not look at the flag of `entity` itself.
        JASPERENGINE_API Entity findBody3D(Scene* scene, Entity entity);

        // own colliders, then the attached ones
        inline size_t colliderCount(const RigidbodyComponent& body){
            return body.colliders.size() + body.attachedColliders.size();
        }
        inline bool isAttached(const RigidbodyComponent& body, size_t index){
            return index >= body.colliders.size();
        }
        inline Collider3D& colliderAt(RigidbodyComponent& body, size_t index){
            return index < body.colliders.size() ? body.colliders[index] : body.attachedColliders[index - body.colliders.size()].collider;
        }
        inline const Collider3D& colliderAt(const RigidbodyComponent& body, size_t index){
            return index < body.colliders.size() ? body.colliders[index] : body.attachedColliders[index - body.colliders.size()].collider;
        }
        // The GameObject and collider index a combined index of the body's names (an attached
        // child's own collider), false when out of range
        JASPERENGINE_API bool resolve3D(Scene* scene, Entity bodyEntity, int index, Entity& entity, int& colliderIndex);
        // The other way: the body and combined index an attached child's collider is in; false
        // when the child is not attached (or that collider is not in its body yet)
        JASPERENGINE_API bool bodyIndexOf3D(Scene* scene, Entity child, int colliderIndex, Entity& bodyEntity, int& index);
        // Bodies holding attached colliders, in every scene: a ColliderRef looks its index up only
        // while there are some, so contacts cost nothing more without compound bodies
        JASPERENGINE_API bool anyAttached3D();
        JASPERENGINE_API void countAttached3D(int delta);
#endif

#ifdef JASPERENGINE_PHYSICS_2D
        JASPERENGINE_API Entity findBody2D(Scene* scene, Entity entity);
        inline int attachedKey2D(size_t slot){
            return ATTACHED_COLLIDER2D_BIT | (int)slot;
        }
        JASPERENGINE_API bool resolve2D(Scene* scene, Entity bodyEntity, int key, Entity& entity, int& colliderIndex);
        JASPERENGINE_API bool bodyKeyOf2D(Scene* scene, Entity child, int colliderIndex, Entity& bodyEntity, int& key);
#endif

        // The child's frame in the ancestor's body frame (its world pose, unscaled): the local
        // Transforms from the ancestor down, after the ancestor's world scale. Local values,
        // so a step that moved the ancestor's body (its Transform written, its children not yet
        // updated) gives the same answer. `rotation` is the turn alone. False when `child` is
        // not below `ancestor`.
        JASPERENGINE_API bool poseInBody(Scene* scene, Entity ancestor, Entity child, Matrix4& matrix, Quaternion& rotation);

    }

}

#endif //ATTACHEDCOLLIDERS_H
