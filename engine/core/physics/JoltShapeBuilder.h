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
#include "Jolt/Physics/PhysicsSettings.h"
#include "Jolt/Physics/Collision/Shape/Shape.h"
#include "Jolt/Physics/Collision/PhysicsMaterial.h"
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

        // The linear part of the entity's modelMatrix with its world rotation taken out: entity
        // space into a frame at the entity's world position and rotation, scale and any shear (a
        // turned child of a non-uniformly scaled parent) included, as the renderer draws it
        static Matrix4 entityLinearMatrix(Scene* scene, Entity entity);
        // Determinant of a matrix's 3x3 linear part: negative when it mirrors
        static float linearDeterminant(const Matrix4& matrix);

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
        // The triangles of one mesh component, its vertices brought through toBody
        static void appendMeshTriangles(MeshComponent* mesh, const Matrix4& toBody,
                                        std::vector<Vector3>& vertices, std::vector<uint32_t>& indices);

        // Hull, mesh and heightfield shapes read their geometry from an entity, and that
        // geometry can land several frames after Scene::load: models are parsed on a worker
        // thread and terrains wait on their heightmap. The parent walk covers multi-node models,
        // whose child meshes are filled in by the ancestor holding the ModelComponent.
        static bool isGeometryLoading(Scene* scene, Entity entity);

        // One heightfield sample per heightmap texel (the smaller side), at least 4 and at most
        // MAX_HEIGHTFIELD_SAMPLES (a larger heightmap is resampled through sampleTerrainSurface)
        static constexpr unsigned int MAX_HEIGHTFIELD_SAMPLES = 1025;
        static unsigned int defaultHeightFieldSamples(const TerrainComponent& terrain);

#ifdef JASPERENGINE_PHYSICS_3D
        // Jolt's global allocator/factory/type registration is process-wide; run once.
        static void ensureJoltGlobalInit();

        // One temp allocator + job pool shared by every 3D world (the scenes' and the placement
        // worlds'). Updates run sequentially on the main thread, so a single pool avoids a
        // per-world thread pool and 10 MiB buffer. Intentionally leaked, like Jolt's Factory.
        // Past the 10 MiB (a world made with a large maxContactConstraints asks its step for
        // maxContactConstraints x ~450 bytes) it falls back to malloc instead of aborting.
        static JPH::TempAllocator& sharedPhysicsTempAllocator();
        static JPH::JobSystemThreadPool& sharedPhysicsJobSystem();

        // The builders return null and set *error (when there is something to say) on failure.
        // hullTolerance: how far a point may lie outside the hull Jolt builds (its default 1 mm,
        // too coarse for a body a few millimetres thick)
        static JPH::ShapeRefC buildConvexHullShape(const std::vector<Vector3>& points, std::string* error,
                                                   float maxConvexRadius = JPH::cDefaultConvexRadius, float hullTolerance = 1.0e-3f);
        // material: every triangle's (null: Jolt's default)
        static JPH::ShapeRefC buildMeshShape(const std::vector<Vector3>& vertices, const std::vector<uint32_t>& indices,
                                             std::string* error, const JPH::PhysicsMaterial* material = nullptr);
        // The rendered terrain surface (MeshSystem::sampleTerrainSurface) in terrain-local space,
        // scaled by scale (the entity's world scale, a negative x or z mirroring it):
        // samplesSize x samplesSize samples spanning the whole terrain, edge to edge.
        // samplesSize 0 is defaultHeightFieldSamples. A terrain without a heightmap is flat.
        static JPH::ShapeRefC buildHeightFieldShape(Scene* scene, TerrainComponent& terrain, const Vector3& scale,
                                                    unsigned int samplesSize, std::string* error, const JPH::PhysicsMaterial* material = nullptr);

        // One collider of entity's Rigidbody, sized by the entity's world scale, in the entity's
        // body space (its own position and rotation not applied). Source NONE reads the entity's
        // own mesh or heightfield. The collider itself is not changed. material: the surface of
        // the whole leaf (every triangle of a mesh or heightfield), null for Jolt's default.
        static JPH::ShapeRefC buildColliderShape(Scene* scene, Entity entity, const Collider3D& collider, std::string* error,
                                                 const JPH::PhysicsMaterial* material = nullptr);

    private:
        static JPH::ShapeRefC buildColliderLeaf(Scene* scene, Entity entity, const Collider3D& collider, std::string* error,
                                                const JPH::PhysicsMaterial* material);
#endif
    };

}
