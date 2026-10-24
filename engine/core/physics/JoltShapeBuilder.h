// SPDX-License-Identifier: MIT
//
// Jolt shapes from scene data, shared by the scene's PhysicsSystem and the edit-time
// PlacementWorld: the process-wide Jolt setup, the mesh geometry a shape reads (submesh
// POSITION/INDEX views, multi-node models, child meshes in body space) and the builders of
// every Collider3D type.
//
// Engine-internal, not installed: Jolt is a static library linked into the shared engine, so
// only libjasperengine may call it.

#pragma once

#include "Entity.h"
#include "buffer/Buffer.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Core/JobSystemThreadPool.h"
#include "Jolt/Core/TempAllocator.h"
#include "Jolt/Physics/Collision/Shape/Shape.h"
#endif

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace jasperengine {

    class Scene;
    struct MeshComponent;
    struct TerrainComponent;
    struct Collider3D;

    class JoltShapeBuilder {
    public:
        // POSITION/INDEX buffers one submesh reads: component-level first, overridden by
        // any per-submesh attribute.
        struct MeshSourceView {
            Buffer* vertexBuffer = NULL;
            Attribute vertexAttr;
            Buffer* indexBuffer = NULL;
            Attribute indexAttr;
        };

        // Submeshes commonly share one POSITION buffer and differ only in their indices,
        // so a range is added once and its vertices reused.
        typedef std::tuple<const void*, size_t, size_t> VertexRange;

        static VertexRange vertexRange(const MeshSourceView& view);
        static std::vector<MeshSourceView> collectMeshSourceViews(MeshComponent* mesh);

        // A multi-node glTF root is only a hierarchy container, with the geometry living in
        // its mesh-node children, so a shape sourced from it falls back to those nodes.
        static std::vector<Entity> meshSourceEntities(Scene* scene, Entity meshEntity);

        // Source vertices are in their own entity's space, which is the body's only when one
        // entity owns both. Brings them into body-local (rotation-free, world-scaled) space.
        static Matrix4 meshToBodyMatrix(Scene* scene, Entity bodyEntity, Entity meshEntity);

        // Transform::worldScale is the entity's accumulated scale, resolved by
        // RenderSystem::updateTransform. Identity when there is no Transform, matching an
        // unscaled body.
        static Vector3 entityWorldScale(Scene* scene, Entity entity);

        // Every vertex of meshEntity's source meshes in bodyEntity's space, a shared vertex range
        // once. False when meshEntity has no mesh geometry.
        static bool appendHullPoints(Scene* scene, Entity bodyEntity, Entity meshEntity, std::vector<Vector3>& points);
        // The triangles of meshEntity's source meshes in bodyEntity's space; meshes without an
        // index buffer are plain triangle lists. False when meshEntity has no mesh geometry.
        static bool appendTriangles(Scene* scene, Entity bodyEntity, Entity meshEntity,
                                    std::vector<Vector3>& vertices, std::vector<uint32_t>& indices);

        // Hull, mesh and heightfield shapes read their geometry from an entity, and that
        // geometry can land several frames after Scene::load: models are parsed on a worker
        // thread and terrains wait on their heightmap. The parent walk covers multi-node models,
        // whose child meshes are filled in by the ancestor holding the ModelComponent.
        static bool isGeometryLoading(Scene* scene, Entity entity);

#ifdef JASPERENGINE_PHYSICS_3D
        // Jolt's global allocator/factory/type registration is process-wide; run once.
        static void ensureJoltGlobalInit();

        // One temp allocator + job pool shared by every 3D world (the scenes' and the placement
        // worlds'). Updates run sequentially on the main thread, so a single pool avoids a
        // per-world thread pool and 10 MiB buffer. Intentionally leaked, like Jolt's Factory.
        static JPH::TempAllocatorImpl& sharedPhysicsTempAllocator();
        static JPH::JobSystemThreadPool& sharedPhysicsJobSystem();

        // The builders return null and set *error (when there is something to say) on failure.
        static JPH::ShapeRefC buildConvexHullShape(const std::vector<Vector3>& points, std::string* error);
        static JPH::ShapeRefC buildMeshShape(const std::vector<Vector3>& vertices, const std::vector<uint32_t>& indices,
                                             std::string* error);
        // The terrain's heightmap; samplesSize 0 picks it from the heightmap's size.
        static JPH::ShapeRefC buildHeightFieldShape(TerrainComponent& terrain, unsigned int samplesSize, std::string* error);

        // One collider of entity's Rigidbody, sized by the entity's world scale, in the entity's
        // body space (its own position and rotation not applied). Source NONE reads the entity's
        // own mesh or heightfield. The collider itself is not changed.
        static JPH::ShapeRefC buildColliderShape(Scene* scene, Entity entity, const Collider3D& collider, std::string* error);
#endif
    };

}
