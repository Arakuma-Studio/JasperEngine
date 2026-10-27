// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 2.2, 2.3, 4.2 steps 2-5)
// - Purpose: the numeric half of a Vertex Animation bake. The main thread poses the hierarchy
//   (VatBakeSession) and records, per sample, what the vertex stage needs (VatSampleState: each
//   renderer's skin matrices, morph weights, lattice records and its mesh-to-root matrix, the
//   sockets' root-space matrices); computeVatBake turns those into the asset on any thread:
//   * the merged render mesh (VatMeshData, spec 2.3): every renderer in root space, vertices
//     shared between submeshes of one renderer once (cpuskin `canonical`, unless a lattice
//     binding limited to one submesh deforms it), one submesh per distinct material, rest = each
//     renderer's mesh deformed at rest with identity skin matrices (bind pose plus its rest morph
//     weights and lattices), `vatIndex` = the vertex's row;
//   * Bones: one candidate bone per (skinned renderer, joint used) with matrix
//     restToRoot^-1-relative skin A_f * S_j * A_0^-1, and one per renderer for its unskinned
//     vertices (A_f * A_0^-1: rigid hierarchies, weight 1); candidates equal at every frame merge.
//     Each bone matrix decomposes into rotation (hemisphere-continuous per bone along a clip),
//     translation and a uniform scale (negative for a mirror); reconstructing every vertex from
//     the decomposed bones against the exact CpuSkin result is the bones error;
//   * Vertices: offsets from rest and the tangent-frame quaternion (columns tangent, normal x
//     tangent, normal; hemisphere-continuous per vertex along a clip); a vertex without a source
//     tangent takes a rest tangent made from its rest normal, projected per frame;
//   * the decisions: Auto = Bones when <= VAT_MAX_BONES bones reproduce every vertex within the tolerance
//     (max(toleranceMm, 0.05% of the bounds diagonal)), else Vertices, with `modeReason`;
//     precision Auto = half unless its quantization error exceeds the tolerance (then float and
//     a precision_switched warning); Bones payloads are always float;
//   * measurements: quantization (Vertices half) or decomposition (Bones) error at the frames,
//     interpolation error at the mid-frames (the exact mid-frame sample vs what the GPU
//     interpolates between the stored frames, quantized), max and mean over every vertex sample
//     measured, per clip; large_interpolation_error with the fps that would fix it;
//   * bounds per clip and in total (exact positions), socket tracks (position + rotation per
//     global frame, recipe order), the payload in the vatTexelIndex layout.
// - Thread-safety: computeVatBake reads only its input (cpuskin::cpuDeform is pure); `cancel`
//   and `progress` may be shared with another thread (progress is called on the calling thread).
// - Units: engine units are metres; errors and tolerances in millimetres.

#ifndef VAT_BAKE_COMPUTE_H
#define VAT_BAKE_COMPUTE_H

#include "vat/VatAssetWriter.h"
#include "vat/VatBakeTypes.h"

#include "animation/AnimationClip.h"
#include "math/Matrix4.h"
#include "render/CpuSkin.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::vat{

    // IEEE 754 binary16, round to nearest even (what an RGBA16F upload stores)
    uint16_t floatToHalf(float value);
    float halfToFloat(uint16_t value);

    // One renderer (an entity with a MeshComponent) of the baked hierarchy, captured at rest
    struct VatBakeRenderer{
        std::string name;                       // its path below the root ("" = the root), for messages
        cpuskin::CpuSkinSource source;
        jasperengine::Matrix4 restToRoot;       // its mesh space -> the root's object space, at rest
        std::array<float, MAX_MORPHTARGETS> restMorph{};
        cpuskin::CpuSkinLattice restLattice;
        // per source.submeshes entry: an index into VatBakeInput::materials, -1 = default material
        std::vector<int> submeshMaterial;
    };

    // One renderer at one sample
    struct VatRendererState{
        std::vector<jasperengine::Matrix4> skin;            // cpuskin::computeSkinMatrices
        std::array<float, MAX_MORPHTARGETS> morph{};
        cpuskin::CpuSkinLattice lattice;
        jasperengine::Matrix4 toRoot;                       // mesh space -> root space now
    };

    struct VatSampleState{
        std::vector<VatRendererState> renderers;            // parallel to VatBakeInput::renderers
        std::vector<jasperengine::Matrix4> sockets;         // parallel to socketPaths: node -> root space
    };

    struct VatBakeClipInput{
        std::string name;
        ClipWrapMode wrap = ClipWrapMode::Loop;             // never Default
        float length = 0.0f;
        int frames = 1;                                     // clipFrames()
        float fps = 30.0f;
        float speed = 1.0f;
        bool interpolate = true;
        std::vector<ClipEventData> events;
        std::vector<VatSampleState> samples;                // `frames` of them, at clipSampleTime()
        // mid-frame samples: mids[i] lies halfway between frame midFrames[i] and the next one
        // (the loop's last frame is followed by frame 0)
        std::vector<int> midFrames;
        std::vector<VatSampleState> mids;
    };

    // the mid-frames measured per clip: every one up to this many, else evenly spread
    constexpr int VAT_MAX_MID_SAMPLES = 32;
    // which mid-frames a clip of `frames` samples measures (first frames of the pairs; a loop's
    // seam, its last frame to frame 0, always among them)
    std::vector<int> midFramesFor(int frames, ClipWrapMode wrap);

    struct VatBakeInput{
        std::vector<VatBakeRenderer> renderers;
        std::vector<VatMaterialData> materials;
        std::vector<VatBakeClipInput> clips;
        std::vector<std::string> socketPaths;               // recipe order
        VatModeChoice mode = VatModeChoice::Auto;
        VatPrecisionChoice precision = VatPrecisionChoice::Auto;
        bool normals = true;
        float toleranceMm = 1.0f;
        VatPreset preset = VatPreset::MobileWeb;
        // Vertices: use the first frame of the first clip as rest (the source has no usable bind
        // pose); set by computeVatBake itself when the rest is degenerate (mixed_rest)
        bool restFromFirstFrame = false;
    };

    struct VatBakeOutput{
        // data: mode, layout (hash not set), bounds, clips, sockets, stats. guid, mesh and the
        // recipe are the caller's (encodeVatFiles sets mesh and the hash)
        jasperengine::vat::VatData data;
        VatMeshData mesh;
        jasperengine::vat::VatPayload payload;
        jasperengine::vat::VatSocketTracks socketTracks;
        VatReport report;
        // Bones: per baked bone, the candidates it merged ("<renderer>/joint <j>", "<renderer> (rigid)")
        std::vector<std::string> boneNames;
        // what the decision measured: the bones error before and after decomposition, the
        // vertices' half quantization error (mm, at the frames)
        float bonesRawErrorMm = 0.0f;
        float bonesErrorMm = 0.0f;
        float halfErrorMm = 0.0f;
        bool morphAnimated = false;
        bool latticeAnimated = false;
    };

    // The bake. False with `error` (and output.report.warnings holding any error issue, e.g.
    // too_large, bone_limit, vertex_limit) when it can't produce an asset, or when cancelled
    // (error "cancelled"). `progress` gets 0..1.
    bool computeVatBake(const VatBakeInput& input, VatBakeOutput& output, std::string& error,
                        const std::atomic<bool>* cancel = nullptr, const std::function<void(float)>& progress = nullptr);

    // ---- helpers shared with the session and the tests ----

    // rotation (x, y, z, w), translation and uniform scale of an affine matrix: scale =
    // sign(det) * cbrt(|det|), rotation from the (orthonormalised) linear part / scale
    void decomposeBone(const jasperengine::Matrix4& matrix, float rotation[4], float translation[3], float& scale);
    // T(translation) * R(rotation) * S(scale) applied to a point (the shader's rebuild)
    jasperengine::Vector3 applyBone(const float rotation[4], const float translation[3], float scale, const jasperengine::Vector3& point);
    // the unit quaternion (x, y, z, w) whose matrix has columns (tangent, normal x tangent, normal)
    void tangentFrame(const jasperengine::Vector3& normal, const jasperengine::Vector3& tangent, float out[4]);

}

#endif //VAT_BAKE_COMPUTE_H
