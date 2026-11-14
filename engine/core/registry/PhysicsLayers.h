// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PHYSICSLAYERS_H
#define PHYSICSLAYERS_H

#include "Export.h"
#include "TagManager.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "physics/PhysicsQueryOptions.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace jasperengine{

    class Scene;

    // Unity's Layer Collision Matrix, one for 3D (Physics, Jolt) and one for 2D
    // (Physics2D, Box2D): process-wide like the layer names, set by the editor from
    // project.yaml and by an exported game from its generated init(). Row i has bit j set
    // when GameObjects on layers i and j collide; it is kept symmetric. All layers
    // collide by default.
    //
    // Bodies on a disabled pair neither collide nor report contact or trigger events. The
    // matrix gates first: a pair it enables still goes through the per-collider category
    // and mask bits (and the 2D group index) and the shouldCollide callbacks.
    // Changes reach live bodies before the next physics step.
    class JASPERENGINE_API LayerCollisionMatrix{
    private:
        std::atomic<uint32_t> rows[MAX_LAYERS];
        std::atomic<uint32_t> version;
        std::atomic<bool> allEnabled;

        void refreshAllEnabled();

    public:
        LayerCollisionMatrix();

        LayerCollisionMatrix(const LayerCollisionMatrix&) = delete;
        LayerCollisionMatrix& operator=(const LayerCollisionMatrix&) = delete;

        // false and an error for a layer outside 0..31
        bool setIgnore(int layer1, int layer2, bool ignore);
        bool isIgnored(int layer1, int layer2) const;
        // layers that 'layer' collides with, Everything when out of range
        uint32_t getMask(int layer) const;
        // sets the row and mirrors it into the column
        bool setMask(int layer, uint32_t layerMask);
        void reset();
        // Every row at once (index = layer); a pair collides only when both rows say so
        void setRows(const uint32_t newRows[MAX_LAYERS]);

        // Lock-free reads for the physics backends: row 'layer' of the matrix
        inline bool collides(unsigned int layer1, unsigned int layer2) const{
            return (rows[layer1 & 31u].load(std::memory_order_relaxed) >> (layer2 & 31u)) & 1u;
        }
        inline bool isAllEnabled() const{
            return allEnabled.load(std::memory_order_relaxed);
        }
        // Bumped by every change, so a physics world can tell it has rows to re-apply
        inline uint32_t getVersion() const{
            return version.load(std::memory_order_acquire);
        }
        // Every row, index = layer
        void getRows(uint32_t outRows[MAX_LAYERS]) const;
    };

    // Unity's Physics: layer collision matrix and layer-masked queries of the 3D world
    class JASPERENGINE_API Physics{
    public:
        // Unity's Physics.IgnoreRaycastLayer, DefaultRaycastLayers and AllLayers: masks
        static constexpr uint32_t IGNORE_RAYCAST_LAYER = 1u << IGNORE_RAYCAST_LAYER_INDEX;
        static constexpr uint32_t DEFAULT_RAYCAST_LAYERS = LAYER_MASK_DEFAULT_RAYCAST;
        static constexpr uint32_t ALL_LAYERS = LAYER_MASK_EVERYTHING;

        static LayerCollisionMatrix& getLayerCollisionMatrix();

        // Unity Physics.IgnoreLayerCollision / GetIgnoreLayerCollision
        static void ignoreLayerCollision(int layer1, int layer2, bool ignore = true);
        static bool getIgnoreLayerCollision(int layer1, int layer2);
        // Unity Physics.GetLayerCollisionMask: the layers 'layer' collides with
        static uint32_t getLayerCollisionMask(int layer);
        static void setLayerCollisionMask(int layer, uint32_t layerMask);
        // every pair collides again
        static void resetLayerCollisionMatrix();

        // The runtime collider and joint wireframes of every scene, 3D and 2D (Game view
        // "Physics" toggle, Lua Physics.debugDraw, MCP configure_game_view physics_debug), in Play
        // and exported builds; debugDrawContacts adds the contact points and normals. Off: nothing
        // is drawn or computed.
        static void setDebugDraw(bool debugDraw);
        static bool isDebugDraw();
        static void setDebugDrawContacts(bool debugDrawContacts);
        static bool isDebugDrawContacts();

        // Unity Physics.queriesHitTriggers (true) and queriesHitBackfaces (false): the defaults of
        // the queries' triggers (USE_GLOBAL) and hitBackfaces options, process-wide
        static void setQueriesHitTriggers(bool hitTriggers);
        static bool getQueriesHitTriggers();
        static void setQueriesHitBackfaces(bool hitBackfaces);
        static bool getQueriesHitBackfaces();
        // an options' rules with those defaults applied
        static bool hitsTriggers(const QueryOptions& options);
        static bool hitsBackfaces(const QueryOptions& options);

#ifdef JASPERENGINE_PHYSICS_3D
        // Closest hit of the 3D bodies on a layer in layerMask. With a Ray, distance is
        // the fraction of ray.direction (as Ray::intersects); with origin, direction and
        // maxDistance it is in world units along the normalized direction (Unity). A collider
        // containing the origin is not hit (Unity) unless options.hitFromInside.
        static RayReturn raycast(Scene* scene, const Ray& ray, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static RayReturn raycast(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static RayReturn raycast(Scene* scene, const Ray& ray, const QueryOptions& options);
        static RayReturn raycast(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, const QueryOptions& options);
        // Every collider hit, nearest first, one hit per collider
        static std::vector<RayReturn> raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static std::vector<RayReturn> raycastAll(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static std::vector<RayReturn> raycastAll(Scene* scene, const Ray& ray, const QueryOptions& options);
        static std::vector<RayReturn> raycastAll(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, const QueryOptions& options);
#endif
    };

    // Unity's Physics2D: the same for the 2D world. 2D distances are in scene units (points).
    class JASPERENGINE_API Physics2D{
    public:
        static constexpr uint32_t IGNORE_RAYCAST_LAYER = Physics::IGNORE_RAYCAST_LAYER;
        static constexpr uint32_t DEFAULT_RAYCAST_LAYERS = Physics::DEFAULT_RAYCAST_LAYERS;
        static constexpr uint32_t ALL_LAYERS = Physics::ALL_LAYERS;

        static LayerCollisionMatrix& getLayerCollisionMatrix();

        static void ignoreLayerCollision(int layer1, int layer2, bool ignore = true);
        static bool getIgnoreLayerCollision(int layer1, int layer2);
        static uint32_t getLayerCollisionMask(int layer);
        static void setLayerCollisionMask(int layer, uint32_t layerMask);
        static void resetLayerCollisionMatrix();

        // Unity Physics2D.queriesHitTriggers (true) and queriesStartInColliders (false here,
        // true in Unity: S96 D5), the defaults of the triggers and startInColliders options
        static void setQueriesHitTriggers(bool hitTriggers);
        static bool getQueriesHitTriggers();
        static void setQueriesStartInColliders(bool startInColliders);
        static bool getQueriesStartInColliders();
        static bool hitsTriggers(const QueryOptions& options);
        static bool startsInColliders(const QueryOptions& options);

#ifdef JASPERENGINE_PHYSICS_2D
        // A collider containing the origin is hit (at distance 0) only with startInColliders
        static RayReturn raycast(Scene* scene, const Ray& ray, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static RayReturn raycast(Scene* scene, Vector2 origin, Vector2 direction, float distance, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static RayReturn raycast(Scene* scene, const Ray& ray, const QueryOptions& options);
        static RayReturn raycast(Scene* scene, Vector2 origin, Vector2 direction, float distance, const QueryOptions& options);
        static std::vector<RayReturn> raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static std::vector<RayReturn> raycastAll(Scene* scene, Vector2 origin, Vector2 direction, float distance, uint32_t layerMask = DEFAULT_RAYCAST_LAYERS);
        static std::vector<RayReturn> raycastAll(Scene* scene, const Ray& ray, const QueryOptions& options);
        static std::vector<RayReturn> raycastAll(Scene* scene, Vector2 origin, Vector2 direction, float distance, const QueryOptions& options);
#endif
    };

}

#endif //PHYSICSLAYERS_H
