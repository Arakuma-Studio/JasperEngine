// SPDX-License-Identifier: MIT
//
// The geometry of meshes, read without physics: the POSITION/INDEX views of every submesh, the
// meshes of multi-node models, triangles brought through a matrix (mirrors wound back), and
// whether a model or terrain is still loading. Shared by the Jolt shapes (JoltShapeBuilder,
// Physics Placement) and the navmesh bake (NavBakeCollector), which runs with
// JASPERENGINE_PHYSICS_3D off as well (docs/specs/S19-navigation.md R3).

#ifndef MESH_TRIANGLES_H
#define MESH_TRIANGLES_H

#include "Export.h"
#include "Entity.h"
#include "buffer/Buffer.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"

#include <cstdint>
#include <tuple>
#include <vector>

namespace jasperengine {

    class Scene;
    struct MeshComponent;

    class JASPERENGINE_API MeshTriangles {
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

        // Determinant of a matrix's 3x3 linear part: negative when it mirrors
        static float linearDeterminant(const Matrix4& matrix);

        // The triangles of one mesh component, its vertices brought through toTarget; meshes
        // without an index buffer are plain triangle lists, a mirroring matrix's triangles are
        // wound back so they keep facing out
        static void appendMeshTriangles(MeshComponent* mesh, const Matrix4& toTarget,
                                        std::vector<Vector3>& vertices, std::vector<uint32_t>& indices);

        // Hull, mesh and heightfield shapes read their geometry from an entity, and that
        // geometry can land several frames after Scene::load: models are parsed on a worker
        // thread and terrains wait on their heightmap. The parent walk covers multi-node models,
        // whose child meshes are filled in by the ancestor holding the ModelComponent.
        static bool isGeometryLoading(Scene* scene, Entity entity);
    };

}

#endif // MESH_TRIANGLES_H
