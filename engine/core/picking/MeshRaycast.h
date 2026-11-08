// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESHRAYCAST_H
#define MESHRAYCAST_H

// Ray casts against what is drawn, without physics: the triangles of meshes (sprites,
// tilemaps, posed skinned and morphed meshes, instances), terrains, UI rects and the bounds of
// points, lines and particles (docs/scripting/mesh-raycast.md). Physics.raycast hits colliders;
// MeshRaycast hits what the camera shows.
//
// A mesh's BVH is built the first time a ray reaches its bounds (built there and then) unless
// prepare() queued it as a background job. A game that never calls these builds nothing.

#include "Export.h"
#include "math/Ray.h"
#include "math/Vector3.h"
#include "picking/PickTypes.h"
#include "picking/SceneBvh.h"
#include "registry/TagManager.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    class Scene;

    struct MeshRaycastOptions {
        // hit triangles from both sides, whatever their submeshes cull (Unity's queriesHitBackfaces)
        bool hitBackfaces = false;
        // also hidden, inactive and disabled objects
        bool includeInactive = false;
        std::vector<Entity> ignore;
        // PICK_MESH | PICK_INSTANCE | ... (PICK_ICON is the editor's: never hit here)
        uint32_t kinds = PICK_ALL & ~PICK_ICON;
        // a mesh still building: wait for it (true), or report its bounds as a pending hit
        bool waitForBvh = true;
    };

    // RayReturn's fields and more
    struct MeshRaycastHit {
        bool hit = false;
        // a fraction of ray.direction (Ray overloads) or world units along the normalized
        // direction (origin, direction, maxDistance overloads), as Physics.raycast
        float distance = 0.0f;
        Vector3 point;
        // interpolated when the mesh has normals, else the face normal; both face the ray
        Vector3 normal;
        Vector3 faceNormal;
        Entity entity = NULL_ENTITY;
        // RayReturn's: the entity, and the submesh as the "collider"
        Entity body = NULL_ENTITY;
        size_t colliderIndex = 0;
        uint32_t submesh = 0;
        uint32_t triangle = 0;
        int32_t instance = -1;
        Vector3 barycentric;
        PickKind kind = PickKind::MESH;
        bool pending = false;
        // TREE: the tree's id and prototype, with entity the Terrain; DETAIL: the layer in prototype
        uint64_t foliageId = 0;
        int32_t prototype = -1;

        operator bool() const { return hit; }
        RayReturn toRayReturn() const;
    };

    struct MeshRaycastStats {
        size_t built = 0;
        size_t pending = 0;
        size_t bytes = 0;
        double buildMsTotal = 0.0;
        uint64_t inlineWaits = 0;
    };

    class JASPERENGINE_API MeshRaycast {
    public:
        static constexpr uint32_t MESH = PICK_MESH;
        static constexpr uint32_t INSTANCE = PICK_INSTANCE;
        static constexpr uint32_t TERRAIN = PICK_TERRAIN;
        static constexpr uint32_t UI = PICK_UI;
        static constexpr uint32_t BOUNDS = PICK_BOUNDS;
        static constexpr uint32_t TREE = PICK_TREE;
        static constexpr uint32_t DETAIL = PICK_DETAIL;
        static constexpr uint32_t ALL = PICK_ALL & ~PICK_ICON;

        // Closest hit on a layer of layerMask. With a Ray, distance is the fraction of
        // ray.direction; with origin, direction and maxDistance it is in world units.
        static MeshRaycastHit raycast(Scene* scene, const Ray& ray, uint32_t layerMask = LAYER_MASK_DEFAULT_RAYCAST,
                                      const MeshRaycastOptions& options = {});
        static MeshRaycastHit raycast(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance,
                                      uint32_t layerMask = LAYER_MASK_DEFAULT_RAYCAST, const MeshRaycastOptions& options = {});
        // Every object hit, nearest first, one hit per object (an instance is an object)
        static std::vector<MeshRaycastHit> raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask = LAYER_MASK_DEFAULT_RAYCAST,
                                                      const MeshRaycastOptions& options = {});
        static std::vector<MeshRaycastHit> raycastAll(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance,
                                                      uint32_t layerMask = LAYER_MASK_DEFAULT_RAYCAST, const MeshRaycastOptions& options = {});
        // One entity only, whatever its layer
        static MeshRaycastHit raycastEntity(Scene* scene, Entity entity, const Ray& ray, const MeshRaycastOptions& options = {});
        // Queues background builds for the meshes of these entities (all when empty)
        static void prepare(Scene* scene, const std::vector<Entity>& entities = {});

        static MeshRaycastStats getStats();

        // a PickFilter of these arguments
        static PickFilter makeFilter(uint32_t layerMask, const MeshRaycastOptions& options);
        static MeshRaycastHit toHit(const PickHit& hit, float distanceScale);
    };

    namespace picking {

        // The narrow phase of one SceneBvh leaf: the ray (unit direction, at most
        // maxDistance) against what the leaf's kind is drawn as. Main thread.
        // A mesh's triangles under a matrix of the caller's: foliage's instances, which have no leaves
        JASPERENGINE_API bool testMeshAt(SceneBvh& bvh, const SceneBvh::Leaf& leaf, const Matrix4& model, const Vector3& origin, const Vector3& direction,
                                         float maxDistance, const PickFilter& filter, PickHit& hit);
        JASPERENGINE_API bool testLeaf(SceneBvh& bvh, const SceneBvh::Leaf& leaf, const Vector3& origin, const Vector3& direction,
                                       float maxDistance, const PickFilter& filter, PickHit& hit);

    }

}

#endif // MESHRAYCAST_H
