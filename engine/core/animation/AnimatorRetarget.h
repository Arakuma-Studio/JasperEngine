// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R12, D12)
// - Purpose: humanoid retargeting of the Animator: a clip made on one humanoid model plays on
//   another through upstream's pose rigs (pose/PoseRig). A slot's local rotation is
//   `left * E * neutral` on every rig with E rig-independent, so a source local rotation maps to
//   the target as `(left_t * left_s^-1) * local_s * (neutral_s^-1 * neutral_t)`: two quaternion
//   products per slot, both factors computed at bind. The hips position moves by its offset from
//   the neutral hips in the canonical character frame, scaled by the rigs' humanScale ratio.
//   Bones that are no slot keep the target's rest (fingers, twist helpers).
// - Sources: a model file's glTF nodes (readModelFileNodes) and its `.humanoid` sidecar, built
//   once per model and shared (retargetSource). Targets: the Animator's rig (bone names) and the
//   sidecar of the Animator's model.
// - Thread-safety: retargetSource is thread safe (a mutex around its cache); bindings are values
//   owned by one Animator; sampling is pure.

#ifndef ANIMATOR_RETARGET_H
#define ANIMATOR_RETARGET_H

#include "Export.h"
#include "animation/AnimationClip.h"
#include "animation/AnimatorPose.h"
#include "animation/KeyframeSampling.h"
#include "pose/HumanBones.h"
#include "pose/PoseRig.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::anim{

    class AnimatorRig;

    // The humanoid of a model file, as a retargeting source
    struct JASPERENGINE_API RetargetSource{
        std::string modelPath;
        pose::Skeleton skeleton;            // every glTF node
        std::vector<std::string> paths;     // per skeleton bone, relative to the model root
        pose::PoseRig rig;
        Affine hipsParent;                  // the hips' parent at rest, model space
    };

    // The humanoid of an Animator's rig
    struct JASPERENGINE_API RetargetTarget{
        bool valid = false;
        std::string error;
        pose::Skeleton skeleton;            // the rig's bones (entity names, rest)
        pose::PoseRig rig;
        std::array<int, pose::HUMAN_BONE_COUNT> slotBones{};    // rig bone per slot, -1 unmapped
        Affine hipsParentInverse;

        // `modelPath`: the Animator's model file (its sidecar overrides the auto-detect), "" for none
        void build(const AnimatorRig& rig, const std::string& modelPath);
    };

    // The shared source of a model file; null with `error` when it is not a usable humanoid
    JASPERENGINE_API std::shared_ptr<const RetargetSource> retargetSource(const std::string& modelPath, std::string* error);
    // drops the cached sources (a model or sidecar changed; project close)
    JASPERENGINE_API void clearRetargetSources();

    // One clip's retargeting from a source onto a target
    struct JASPERENGINE_API RetargetBinding{
        struct Slot{
            int targetBone = -1;
            int rotationTrack = -1;         // clip track of the source slot bone's rotation, -1 when none
            Quaternion sourceRest;
            Quaternion pre;                 // left_t * left_s^-1
            Quaternion post;                // neutral_s^-1 * neutral_t
        };
        bool active = false;
        std::array<Slot, pose::HUMAN_BONE_COUNT> slots;
        int hipsPositionTrack = -1;
        Vector3 sourceHipsRest = Vector3(0, 0, 0);
        bool sourceHipsRelative = false;
        bool hipsRotationRelative = false;
        Affine sourceHipsParent;
        Vector3 sourceNeutralHips = Vector3(0, 0, 0);
        Vector3 targetNeutralHips = Vector3(0, 0, 0);
        Quaternion canonical;               // C_t * C_s^-1
        float scale = 1.0f;                 // humanScale_t / humanScale_s
        int targetHips = -1;
        Affine targetHipsParentInverse;
        std::vector<KeyCursor> cursors;     // per clip track

        void bind(const AnimationClip& clip, const RetargetSource& source, const RetargetTarget& target);
    };

    // Writes the retargeted clip at `clipTime` into `out` (holding the target rest elsewhere)
    JASPERENGINE_API void sampleRetargeted(RetargetBinding& binding, const AnimationClip& clip, float clipTime, Pose& out);

}

#endif //ANIMATOR_RETARGET_H
