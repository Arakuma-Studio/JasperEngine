// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 3.2.2, 4.2)
// - Purpose: the vertices a mesh draws, computed on the CPU exactly as the vertex stage computes
//   them (shaderlib/mesh.vert with includes/morphtarget.glsl, skinning.glsl and lattice.glsl), for
//   every vertex of every submesh: what the Vertex Animation baker samples per frame. pose::PoseSkin
//   (a <= 6000 vertex position subsample for the lowest point) stays as it is; nothing here
//   replaces a GPU path.
// - Order (mesh.vert): morph targets -> stage-0 lattices (before skinning, only for a submesh with
//   skinning) -> linear blend skinning -> (the instance matrix: not applicable, this is one mesh)
//   -> stage-1 lattices (every binding of a submesh without skinning). Positions, normals and
//   tangents come out in mesh space (the MeshComponent entity's local space, before modelMatrix).
//   Normals and tangents are normalised where the shader normalises them (after skinning, and by
//   each lattice that moves the vertex); a tangent keeps its w (handedness).
// - Vertex data: read through the same bindings RenderSystem::loadMesh gives the GPU (the
//   render-attribute buffers in buffer-name order, then the submesh's attributes, which win),
//   decoded as the vertex fetch does (SokolObject::getVertexFormat: unsigned shorts are always
//   normalised, signed normalised values clamp at -1, missing components read (0, 0, 0, 1)).
//   Bone ids and weights are the shader's: int(a_boneIds[k] * normAdjust.x) and
//   a_boneWeights[k] * normAdjust.y with normAdjust as applySkinningUniform uploads it. A skinned
//   position is divided by its w (the sum of the weights), as getSkinPosition does.
// - Vertices: per submesh, the vertices its draw reads (the indices it references, ascending, or
//   0..vertexCount-1 for a submesh without indices), so one buffer shared by several submeshes
//   gives one vertex per submesh using it. `canonical` names the first vertex reading the very
//   same bytes through the same bindings: callers dedupe with it (the deformed values are equal
//   unless a lattice binding limited to one submesh moves one of them).
// - What must be current: Transform::modelMatrix of the mesh entity and of every joint (and, for
//   lattices, of the lattice and mesh entities), then MeshComponent::lattice (LatticeRender::prepare).
//   RenderSystem::update does both every frame. After posing a hierarchy outside the frame
//   (ActionSystem::previewPlayer, PoseAnimator samples) call updateTransforms(scene), then
//   LatticeRender::prepare(scene) when lattices deform the mesh, then deformEntity /
//   computeSkinMatrices + cpuDeform. morphWeights are MeshComponent::morphWeights.
// - Errors: a layout the renderer cannot draw (a vertex format getVertexFormat maps to INVALID, a
//   buffer the mesh does not have, a stride of 0, an index type other than 16/32 bit, an index or
//   attribute read past its buffer, a submesh without positions) fails with a message naming the
//   submesh and the attribute; nothing is guessed.
// - Thread-safety: buildSource, computeSkinMatrices, latticeInputs, updateTransforms and
//   deformEntity read the scene (main thread). cpuDeform only reads its arguments (any thread,
//   several threads may share one source).
// - Precision: float math in the shader's expression order; GPUs may differ in the last bits
//   (fused multiply-add, the rounding of normalised integer conversion): parity is within 1e-5
//   relative, not bitwise.

#ifndef CPU_SKIN_H
#define CPU_SKIN_H

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "render/Render.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    class Scene;
    struct MeshComponent;

}

namespace jasperengine::cpuskin{

    // one submesh of a source: its vertices are source vertices [firstVertex, firstVertex + vertexCount)
    struct JASPERENGINE_API CpuSkinSubmesh{
        unsigned int submesh = 0;     // index in MeshComponent::submeshes: its material slot (Submesh::material, LatticeBinding::submesh)
        PrimitiveType primitive = PrimitiveType::TRIANGLES;
        uint32_t firstVertex = 0;
        uint32_t vertexCount = 0;
        // the draw's index list (GPU order, as many as it draws) as source vertex indices
        std::vector<uint32_t> indices;

        bool hasNormal = false;       // a_normal (else normals out are 0)
        bool hasTangent = false;      // a_tangent (else tangents out are 0)
        bool hasUv0 = false;          // TEXCOORD1 (glTF TEXCOORD_0)
        bool hasUv1 = false;          // TEXCOORD2 (glTF TEXCOORD_1)
        int colorComponents = 0;      // 0 none, 3 or 4 (a_color; a 3 component colour reads alpha 1)
        bool hasSkinning = false;     // HAS_SKINNING: bone ids or weights bound
        Vector2 normAdjust = Vector2(1.0f, 1.0f); // u_vs_skinning (joint scale, weight scale)

        // morph targets the shader reads (HAS_MORPHTARGET; 2, 4 or 8 position targets, 0 when
        // the submesh has none) and which of them are bound (bit t: an attribute for target t;
        // an unbound one reads 0)
        int morphTargets = 0;
        int morphNormalTargets = 0;   // 0, 2 or 4 (HAS_MORPHNORMAL)
        int morphTangentTargets = 0;  // 0 or 2 (HAS_MORPHTANGENT)
        uint32_t morphPositionMask = 0;
        uint32_t morphNormalMask = 0;
        uint32_t morphTangentMask = 0;
    };

    // Every vertex of every submesh of one MeshComponent as the GPU reads it (buildSource)
    struct JASPERENGINE_API CpuSkinSource{
        std::vector<CpuSkinSubmesh> submeshes;

        // per source vertex (all the same size)
        std::vector<Vector3> positions;
        std::vector<Vector3> normals;       // 0 for a submesh without normals
        std::vector<Vector4> tangents;      // xyzw, 0 for a submesh without tangents
        std::vector<Vector2> uv0;           // 0 without
        std::vector<Vector2> uv1;           // 0 without
        std::vector<Vector4> colors;        // rgba, (1, 1, 1, 1) without
        std::vector<std::array<int, 4>> boneIds;  // int(a_boneIds * normAdjust.x), 0 without skinning
        std::vector<Vector4> boneWeights;   // a_boneWeights * normAdjust.y, 0 without skinning

        // morph deltas per target, each empty (no submesh binds it) or one per vertex (0 where
        // the vertex's submesh does not bind it)
        std::array<std::vector<Vector3>, MAX_MORPHTARGETS> morphPositions;
        std::array<std::vector<Vector3>, 4> morphNormals;
        std::array<std::vector<Vector3>, 2> morphTangents;

        // where each vertex comes from: the MeshComponent submesh and the index its draw uses
        // (the attribute element: attribute byte = offset + index * stride)
        std::vector<uint32_t> vertexSubmesh;
        std::vector<uint32_t> vertexIndex;
        // the first source vertex reading the same data through the same bindings (itself when
        // it is the first)
        std::vector<uint32_t> canonical;

        size_t vertexCount() const { return positions.size(); }
        // the CpuSkinSubmesh of a source vertex
        const CpuSkinSubmesh& submeshOf(uint32_t vertex) const;
    };

    // One lattice binding exactly as the GPU gets it (decoded from MeshComponent::lattice, the
    // record LatticeRender::prepare packs), ready for lattice::deformVertex
    struct JASPERENGINE_API CpuSkinLatticeBinding{
        // the record's grid settings (resolution clamped, interpolation, outside, fade distance),
        // enabled; its offsets are unused (the padded grid below holds them)
        LatticeComponent lattice;
        std::vector<Vector3> padded;
        Matrix4 objectToNormalized;
        Matrix4 normalizedToObject;
        float weight = 1.0f;
        LatticeMaskChannel mask = LatticeMaskChannel::NONE;
        int stage = 1;           // 0 before skinning, 1 after
        int submesh = -1;        // only this submesh, -1 all
        bool moving = false;     // LatticeMeshState::movingBindings: a binding at rest is left out of the draw
    };

    struct JASPERENGINE_API CpuSkinLattice{
        std::vector<CpuSkinLatticeBinding> bindings; // in order, at most LATTICE_MAX_PER_MESH
    };

    // cpuDeform's output, one entry per source vertex, mesh space
    struct JASPERENGINE_API CpuSkinResult{
        std::vector<Vector3> positions;
        std::vector<Vector3> normals;   // unit, 0 for a submesh without normals
        std::vector<Vector4> tangents;  // unit xyz + the source w, 0 for a submesh without tangents
    };

    // The vertices of a MeshComponent (loaded by MeshSystem: its attributes and buffers, the GPU
    // load is not needed). False with `error` on a layout the renderer cannot draw.
    JASPERENGINE_API bool buildSource(const MeshComponent& mesh, CpuSkinSource& out, std::string* error = nullptr);
    // the same for an entity's MeshComponent (a Model root whose geometry sits on child mesh
    // entities, ModelComponent::meshNodesMapping, has none: the error says so)
    JASPERENGINE_API bool buildSource(Scene* scene, Entity meshEntity, CpuSkinSource& out, std::string* error = nullptr);

    // Every Transform's localMatrix / modelMatrix from its position, rotation and scale, parents
    // first (RenderSystem::updateTransform over the scene, as the frame does): what makes the
    // matrices current after a pose written outside the frame. Needs the scene's RenderSystem.
    JASPERENGINE_API void updateTransforms(Scene* scene);

    // The skinning matrices of a mesh entity, RenderSystem::update's formula from the current
    // Transform::modelMatrix values: per joint j of the ModelComponent::skinBindings entry for the
    // mesh, inverse(meshModel) * jointModel * inverseBind[j]; for a model without skin bindings
    // (the legacy BoneComponent path) inverse(modelEntityModel) * boneModel * offsetMatrix at
    // BoneComponent::index. Entries no joint writes keep MeshComponent::bonesMatrix's (what the
    // GPU keeps). A mesh without skin gets bonesMatrix as it is (cpuDeform ignores it). False
    // with `error` when the entity has no MeshComponent or Transform, or its model matrix is not
    // invertible (RenderSystem then keeps stale matrices).
    JASPERENGINE_API bool computeSkinMatrices(Scene* scene, Entity meshEntity, std::vector<Matrix4>& out, std::string* error = nullptr);

    // The lattice bindings of a mesh as LatticeRender::prepare last packed them
    // (MeshComponent::lattice; none when count is 0). False with `error` when the packed records
    // are malformed.
    JASPERENGINE_API bool latticeInputs(const MeshComponent& mesh, CpuSkinLattice& out, std::string* error = nullptr);

    // The deformed vertices of `source`: `skinMatrices` per bone index (computeSkinMatrices; a
    // bone index past its end reads a zero matrix, as robust buffer access does), `morphWeights`
    // MAX_MORPHTARGETS weights (null: all 0), `lattice` null or empty for none. False with `error`
    // when a submesh has skinning and `skinMatrices` is empty.
    JASPERENGINE_API bool cpuDeform(const CpuSkinSource& source, const std::vector<Matrix4>& skinMatrices, const float* morphWeights,
                                    const CpuSkinLattice* lattice, CpuSkinResult& out, std::string* error = nullptr);

    // All of it for a scene entity with the scene's current state: buildSource, computeSkinMatrices,
    // MeshComponent::morphWeights, latticeInputs, cpuDeform. `source` (optional) gets the source.
    // Transforms and the lattice packing must be current (see the contract above).
    JASPERENGINE_API bool deformEntity(Scene* scene, Entity meshEntity, CpuSkinResult& out, std::string* error = nullptr,
                                       CpuSkinSource* source = nullptr);

}

#endif // CPU_SKIN_H
