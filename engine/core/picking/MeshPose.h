// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESHPOSE_H
#define MESHPOSE_H

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "picking/PickGeometry.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine {

    struct MeshComponent;

    namespace picking {

        // a skinned or morphed mesh above this many vertices picks with its bind-pose
        // triangles, kept to its posed bounds (skinnedAABB), instead of a posed copy
        inline constexpr uint32_t MAX_POSED_VERTICES = 200000u;

        // One vertex's skin and morph inputs, as the vertex shaders receive them: bone ids
        // and weights already as floats (a normalized attribute divided by its type's range,
        // as the GPU reads it), the morph targets' offsets
        struct VertexPoseInput {
            Vector3 position;
            float boneIds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float boneWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            Vector3 morphTargets[8];
            uint32_t morphCount = 0;
            bool skinned = false;
        };

        struct PoseParameters {
            std::span<const Matrix4> bones;
            const float* morphWeights = nullptr; // MAX_MORPHTARGETS of them
            float normAdjustJoint = 1.0f;
            float normAdjustWeight = 1.0f;
        };

        // The pose math of the vertex shaders, the one place it lives on the CPU
        // (engine/shaderlib/includes/morphtarget.glsl getMorphPosition, then skinning.glsl
        // getBoneTransform and getSkinPosition): the morph targets' weighted offsets are added,
        // then the bone matrices weighted and summed, and the vertex transformed and divided
        // by w. A bone index outside the bones contributes nothing. `boneTransform`, when
        // given, receives the summed matrix (normals: its upper 3x3).
        JASPERENGINE_API Vector3 poseVertex(const VertexPoseInput& input, const PoseParameters& parameters, Matrix4* boneTransform = nullptr);

        // The mesh draws posed: a skinned submesh, or a morphed one with a non-zero weight
        JASPERENGINE_API bool isPosed(const MeshComponent& mesh);
        // What a pose depends on (bonesMatrix, morphWeights): equal hashes, equal pose. This is
        // the pose epoch posed copies are cached by: those arrays are written in many places
        // (animation, scripts, the editor), so the epoch is read from their content.
        JASPERENGINE_API uint64_t poseHash(const MeshComponent& mesh);

        // The pose input of vertex `v` of a submesh's source (layout from a gather)
        JASPERENGINE_API bool readPoseInput(const MeshComponent& mesh, const PickGeometryLayout& layout, uint32_t submesh, uint32_t vertex,
                                            VertexPoseInput& input);
        JASPERENGINE_API PoseParameters poseParameters(const MeshComponent& mesh, uint32_t submesh);

        // The NORMAL of vertex `v` of a submesh's source, false when it has none
        JASPERENGINE_API bool readVertexNormal(const MeshComponent& mesh, uint32_t submesh, uint32_t vertex, Vector3& normal);

        // Every gathered position posed (positions: PickGeometry::positions of that gather),
        // into `posed` (same order). False when the layout has no source to pose.
        JASPERENGINE_API bool poseGeometry(const MeshComponent& mesh, const PickGeometryLayout& layout,
                                           std::span<const Vector3> positions, std::vector<Vector3>& posed);

    }

}

#endif // MESHPOSE_H
