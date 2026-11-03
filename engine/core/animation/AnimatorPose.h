// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R3, R17, D2, D3)
// - Purpose: the Animator's pose: local TRS of every rig bone as structure-of-arrays floats (tx ty
//   tz, qx qy qz qw, sx sy sz), plus float channels (morph weights, lattice offsets), and the
//   operations the evaluation runs on whole poses: copy, weighted accumulate, normalise, masked
//   override blend, additive deltas, local to rig space.
// - Layout: one 16-byte aligned block per pose; each array holds `stride` floats (the bone count
//   rounded up to 4), the channel array `channelStride` (rounded up to 4). Padding lanes are kept
//   at identity (q = (0,0,0,1), s = 1, t = 0, channels 0) so SIMD lanes never see garbage.
// - Kernels: templates on a SimdFloat4.h back end, instantiated for Scalar and Native. Every
//   operation gives the same bits on both (no FMA: animation sources build with
//   -ffp-contract=off; a square root and a divide, no estimates). Which one runs is
//   poseOpsBackend(): Native by default, Scalar when forced (tests, benchmarks).
// - Rotation maths: blends align each contribution to the hemisphere of what it is blended with
//   (dot < 0 negates it), then normalise (nlerp). A weight (times mask) of exactly 0 leaves the
//   base value bit-identical: the kernels select the base there.
// - Thread-safety: a Pose is a value owned by one Animator; the free functions are pure.

#ifndef ANIMATOR_POSE_H
#define ANIMATOR_POSE_H

#include "Export.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine::anim{

    class JASPERENGINE_API Pose{
    public:
        enum Array { TX, TY, TZ, QX, QY, QZ, QW, SX, SY, SZ, ARRAY_COUNT };

        Pose() = default;
        Pose(size_t bones, size_t channels);
        ~Pose();
        Pose(const Pose&) = delete;
        Pose& operator=(const Pose&) = delete;
        Pose(Pose&& other) noexcept;
        Pose& operator=(Pose&& other) noexcept;

        // sizes the pose (contents: identity / 0); a no-op when the sizes do not change
        void resize(size_t bones, size_t channels);
        void setIdentity();

        size_t boneCount() const { return bones; }
        size_t channelCount() const { return channels; }
        size_t stride() const { return boneStride; }
        size_t channelStride() const { return chanStride; }

        float* array(Array a){ return data + (size_t)a * boneStride; }
        const float* array(Array a) const { return data + (size_t)a * boneStride; }
        float* channelData(){ return data + ARRAY_COUNT * boneStride; }
        const float* channelData() const { return data + ARRAY_COUNT * boneStride; }
        // every float of the pose (arrays then channels), floatCount() of them
        float* raw(){ return data; }
        const float* raw() const { return data; }
        size_t floatCount() const { return ARRAY_COUNT * boneStride + chanStride; }

        Vector3 position(size_t bone) const;
        Quaternion rotation(size_t bone) const;
        Vector3 scale(size_t bone) const;
        void setPosition(size_t bone, const Vector3& v);
        void setRotation(size_t bone, const Quaternion& q);
        void setScale(size_t bone, const Vector3& v);

        void copyFrom(const Pose& other);

    private:
        float* data = nullptr;
        size_t bones = 0;
        size_t channels = 0;
        size_t boneStride = 0;
        size_t chanStride = 0;
    };

    enum class PoseBackend : uint8_t { Scalar, Native };
    // the back end the operations below run on (Native unless forced)
    JASPERENGINE_API PoseBackend poseOpsBackend();
    JASPERENGINE_API void setPoseOpsBackend(PoseBackend backend);
    // the Native back end's name ("sse2", "neon", "wasm", "scalar")
    JASPERENGINE_API const char* poseNativeBackendName();

    // ---- whole-pose operations (sizes must match; masks: one weight per bone, null = 1) ----

    // every array and channel to 0 (an accumulator)
    JASPERENGINE_API void poseClear(Pose& out);
    // acc += w * src (rotations aligned to acc's hemisphere first); channels too
    JASPERENGINE_API void poseAccumulate(Pose& acc, const Pose& src, float weight);
    // acc / total, rotations normalised (a zero rotation becomes `fallback`'s); total > 0
    JASPERENGINE_API void poseNormalize(Pose& acc, float total, const Pose& fallback);
    // out = base blended toward layer by weight * mask (nlerp for rotations, lerp otherwise);
    // a bone whose weight * mask is 0 keeps base exactly. `out` may alias base. Channels use
    // `channelMask` (null = 1).
    JASPERENGINE_API void poseBlend(Pose& out, const Pose& base, const Pose& layer, float weight,
                                    const float* mask, const float* channelMask);
    // out = base with the delta applied at weight * mask: q = base * nlerp(I, dq, w), t = base + dt * w,
    // s = base * lerp(1, ds, w), channels base + dc * w. `out` may alias base.
    JASPERENGINE_API void poseAdditive(Pose& out, const Pose& base, const Pose& delta, float weight,
                                       const float* mask, const float* channelMask);
    // delta = pose relative to reference: dq = conj(ref) * q, dt = t - ref, ds = s / ref (1 where ref is 0),
    // channels pose - ref
    JASPERENGINE_API void poseMakeDelta(Pose& delta, const Pose& pose, const Pose& reference);
    // the identity delta (dq = I, dt = 0, ds = 1, channels 0)
    JASPERENGINE_API void poseIdentityDelta(Pose& delta);

    // ---- one element (the Animation Player's layers compose per (entity, channel), S21's maths) ----

    // poseBlend of one rotation: base nlerped toward layer by weight (aligned to base's
    // hemisphere, normalised); weight 0 is base exactly, 1 the layer exactly
    JASPERENGINE_API Quaternion poseBlendRotation(const Quaternion& base, const Quaternion& layer, float weight);
    // poseBlend of one float: weight 0 is base, 1 is layer, else base + (layer - base) * weight
    JASPERENGINE_API float poseBlendValue(float base, float layer, float weight);
    // poseAdditive of one rotation: base * normalize(nlerp(I, delta, weight)), normalised (delta
    // aligned to the identity's hemisphere); weight 0 is base exactly
    JASPERENGINE_API Quaternion poseAdditiveRotation(const Quaternion& base, const Quaternion& delta, float weight);
    // poseAdditive of one scale component: base * (1 + (delta - 1) * weight)
    JASPERENGINE_API float poseAdditiveScale(float base, float delta, float weight);
    // poseAdditive of one translation / channel value: base + delta * weight
    JASPERENGINE_API float poseAdditiveValue(float base, float delta, float weight);

    // ---- rig space ----

    // A rig-space affine transform: column-major 3x3 (rotation * scale) and translation
    struct JASPERENGINE_API Affine{
        float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

        Vector3 transformPoint(const Vector3& p) const;
        Vector3 transformVector(const Vector3& v) const;
        Vector3 translation() const { return Vector3(m[9], m[10], m[11]); }
        Affine operator*(const Affine& b) const;
        // the inverse of an invertible affine (a degenerate one gives the identity)
        Affine inverse() const;
        static Affine compose(const Vector3& t, const Quaternion& r, const Vector3& s);
    };

    // Rig-space transforms of every bone: parents[i] < i, -1 for a bone under the rig root.
    // Rotations and scales follow the engine's Transform convention (world rotation = parent *
    // local, world scale = parent * local, componentwise); `world` is parent * T * R * S exactly
    // as the render system composes matrices.
    struct JASPERENGINE_API RigSpace{
        Affine* world = nullptr;
        Vector3* position = nullptr;
        Quaternion* rotation = nullptr;
        Vector3* scale = nullptr;
    };
    // (a rig holds every ancestor of its bones below the root, so a bone with parent -1 is a child
    // of the rig root)
    JASPERENGINE_API void poseToRigSpace(const Pose& local, const int* parents, RigSpace out);
    // the same for bones [from, count) only (the earlier ones are valid already)
    JASPERENGINE_API void poseToRigSpaceFrom(const Pose& local, const int* parents, size_t from, RigSpace out);
    // the same for `bone` and its descendants only: the bones in parents-first depth-first order
    // (a rig's), so a subtree is the bones from `bone` up to the first whose parent precedes it
    JASPERENGINE_API void poseToRigSpaceSubtree(const Pose& local, const int* parents, size_t bone, RigSpace out);

}

#endif //ANIMATOR_POSE_H
