// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ray_h
#define ray_h

#include "Vector3.h"
#include "Plane.h"
#include "AABB.h"
#include "Sphere.h"
#include "OBB.h"
#include "Entity.h"
#include <cstdint>
#include <vector>

namespace jasperengine {

    class Scene;
    class Rigidbody2D;
    class Rigidbody;
    struct QueryOptions;

    enum class RayFilter{
        BODY_2D,
        BODY_3D,
        BOUNDS // mesh world AABBs, needs no physics backend; ignores onlyStatic and the collision bits
    };

    struct RayReturn{
        bool hit;
        float distance;
        Vector3 point;
        Vector3 normal;
        Entity body;
        size_t colliderIndex;
        // the scene and world of a physics hit (null for bounds and plain shapes): what the Lua
        // RaycastHit's collider, gameObject and rigidbody resolve through
        Scene* scene = nullptr;
        bool body2D = false;

        operator bool() const {
            return hit;
        }
    };

    class JASPERENGINE_API Ray{

    private:

        Vector3 origin;
        Vector3 direction; // direction and length of the ray (anything beyond this length will not be a hit)

    public:

        static const RayReturn NO_HIT;

        Ray();
        Ray(const Ray &ray);
        Ray(Vector3 origin, Vector3 direction);

        Ray &operator=(const Ray &);
        Vector3 operator*(float t);

        void setOrigin(Vector3 point);
        Vector3 getOrigin() const;

        void setDirection(Vector3 direction);
        Vector3 getDirection() const;

        Vector3 getPoint(float distance) const;

        RayReturn intersects(const Plane& plane) const;
        RayReturn intersects(const AABB& box) const;
        RayReturn intersects(const OBB& obb) const;
        RayReturn intersects(const Sphere& sphere) const;
#ifdef JASPERENGINE_PHYSICS_2D
        RayReturn intersects(const Rigidbody2D& body) const;
        RayReturn intersects(const Rigidbody2D& body, size_t shape) const;
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        RayReturn intersects(const Rigidbody& body) const;
        RayReturn intersects(const Rigidbody& body, size_t shape) const;
#endif
        RayReturn intersects(Scene* scene, RayFilter raytest) const;
        RayReturn intersects(Scene* scene, RayFilter raytest, bool onlyStatic) const;
        RayReturn intersects(Scene* scene, RayFilter raytest, uint16_t categoryBits, uint16_t maskBits) const;
        RayReturn intersects(Scene* scene, RayFilter raytest, bool onlyStatic, uint16_t categoryBits, uint16_t maskBits) const;
        RayReturn intersects(Scene* scene, RayFilter raytest, Entity ignoreEntity) const;
        RayReturn intersects(Scene* scene, RayFilter raytest, const std::vector<Entity>& ignoreEntities) const;
        RayReturn intersects(Scene* scene, RayFilter raytest, bool onlyStatic, uint16_t categoryBits, uint16_t maskBits, Entity ignoreEntity) const;
        RayReturn intersects(Scene* scene, RayFilter raytest, bool onlyStatic, uint16_t categoryBits, uint16_t maskBits, const std::vector<Entity>& ignoreEntities) const;
#ifdef JASPERENGINE_PHYSICS_3D
        RayReturn intersects(Scene* scene, uint8_t broadPhaseLayer3D) const; // only 3D bodies
        RayReturn intersects(Scene* scene, uint8_t broadPhaseLayer3D, uint16_t categoryBits, uint16_t maskBits) const; // only 3D bodies
        RayReturn intersects(Scene* scene, uint8_t broadPhaseLayer3D, Entity ignoreEntity) const; // only 3D bodies
        RayReturn intersects(Scene* scene, uint8_t broadPhaseLayer3D, const std::vector<Entity>& ignoreEntities) const; // only 3D bodies
        RayReturn intersects(Scene* scene, uint8_t broadPhaseLayer3D, uint16_t categoryBits, uint16_t maskBits, Entity ignoreEntity) const; // only 3D bodies
        RayReturn intersects(Scene* scene, uint8_t broadPhaseLayer3D, uint16_t categoryBits, uint16_t maskBits, const std::vector<Entity>& ignoreEntities) const; // only 3D bodies
#endif

    private:

        // Closest hit of the bodies (or mesh bounds) on a layer in layerMask; with allHits,
        // also every hit into it, nearest first and one per collider. broadPhaseLayer3D < 0
        // searches every 3D broad phase layer. The scene queries above skip the Ignore
        // Raycast layer (LAYER_MASK_DEFAULT_RAYCAST); Physics and Physics2D take a mask.
        // options (Physics.raycast's, S96): Unity's start-inside, trigger and back-face rules;
        // null keeps this API's own (convex shapes solid, triggers and the start's collider hit).
        RayReturn cast(Scene* scene, RayFilter raytest, int broadPhaseLayer3D, bool onlyStatic, uint16_t categoryBits, uint16_t maskBits, const std::vector<Entity>* ignoreEntities, uint32_t layerMask, std::vector<RayReturn>* allHits, const QueryOptions* options = nullptr) const;

        friend class Physics;
        friend class Physics2D;
    };
    
}


#endif /* ray_h */
