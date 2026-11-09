// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R12, D12, §12 "Retargeting")
// - Purpose: the one humanoid retarget path of the engine. The Animator and the Animation Player
//   both play humanoid motion through it: a clip (a humanoid clip's tracks, or a generic clip on
//   another humanoid model, read through that model's rig) gives a HumanoidSample, and
//   realiseHumanoid poses a target rig from it. Nothing else turns clip values into the bones of
//   another rig.
// - The maths is PoseRig's (D12): a slot's local rotation is `left * E * neutral` on every rig,
//   E the joint rotation relative to the neutral pose in the canonical frame (rig-independent).
//   So a source local maps to E as `E = A_s * local * B_s` with A_s, B_s fixed per slot
//   (left_s^-1 and neutral_s^-1, with the source's bridge folded into A_s), and E to the target as
//   `left_t * E * neutral_t`: per slot the two quaternion products of D12 on each side. A slot the
//   target lacks folds into its next present descendant (E_t(b) = the product of the sample's E
//   along the chain from b's effective target parent, parent first); a slot the sample lacks is
//   neutral (E = I).
// - The body (hips) position and the two foot goals are model-space points in the canonical frame
//   relative to the neutral (grounded) hips, in humanScale units: the target places them at
//   neutralHips + C * (value * humanScale), which scales hips height and stride (D12's ratio).
// - Extra bones (fingers, eyes, jaw: upstream's anim-retarget) carry their delta from the rest
//   local rotation in the bone's canonical neutral frame: d = W * rest^-1 * local * W^-1.
// - Mirror swaps left and right and maps (w, x, y, z) -> (w, x, -y, -z) (x is the canonical left
//   axis), positions x -> -x; Foot IK moves both legs to the (mirrored) foot goals, the feet
//   keeping their world rotation (pose::solveLimbIK), before any blending.
// - Sources and avatars are a model file's skin skeleton (pose::buildSkeletonFromGltf: the
//   loaded model's bones, non-bone ancestors folded into bridges) with its `.humanoid` sidecar
//   (bones, extra bones, twist corrections), shared per (model, sidecar text): retargetSource.
//   The source's per-slot maps are what Convert to Humanoid Clip writes key for key, so a
//   converted clip and its generic original play the same on any rig.
// - Thread-safety: retargetSource is thread safe (the clip pool converts on the thread pool);
//   bindings and targets are values owned by one player state or Animator; sampling and
//   realisation are pure (Foot IK's scratch is per thread) and allocation-free after binding.

#ifndef ANIMATOR_RETARGET_H
#define ANIMATOR_RETARGET_H

#include "Export.h"
#include "animation/AnimationClip.h"
#include "animation/AnimatorPose.h"
#include "animation/KeyframeSampling.h"
#include "math/Matrix4.h"
#include "pose/HumanBones.h"
#include "pose/HumanExtraBones.h"
#include "pose/HumanoidMap.h"
#include "pose/PoseMath.h"
#include "pose/PoseRig.h"
#include "pose/PoseTypes.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tinygltf{
    class Model;
}

namespace jasperengine::anim{

    class AnimatorRig;

    // ---- the sample ----

    // One humanoid sample: slot joint rotations E, extra-bone deltas, the body offset and the
    // foot goals, each with its presence bit
    struct JASPERENGINE_API HumanoidSample{
        std::array<Quaternion, pose::HUMAN_BONE_COUNT> E;
        std::bitset<pose::HUMAN_BONE_COUNT> hasE;
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extra;
        std::bitset<pose::HUMAN_EXTRA_BONE_COUNT> hasExtra;
        Vector3 hips = Vector3(0, 0, 0);
        bool hasHips = false;
        Vector3 feet[2] = {Vector3(0, 0, 0), Vector3(0, 0, 0)};
        bool hasFeet[2] = {false, false};

        void clear(){
            hasE.reset();
            hasExtra.reset();
            hasHips = false;
            hasFeet[0] = hasFeet[1] = false;
        }
    };

    // Left/right swapped, quaternions (w, x, y, z) -> (w, x, -y, -z), positions x negated: the
    // mirror of pose::mirrorBodyPose in E space; twice is the identity
    JASPERENGINE_API void mirrorHumanoidSample(const HumanoidSample& in, HumanoidSample& out);

    // ---- a humanoid rig (source or target) ----

    // A PoseRig with what retargeting needs beside it: the extra bones' rest and canonical
    // neutral frames, and per present slot the slots it stands for when another rig has them
    struct JASPERENGINE_API RetargetRig{
        std::shared_ptr<const pose::PoseRig> rig;
        pose::HumanExtraMap extra;
        std::array<int, pose::HUMAN_EXTRA_BONE_COUNT> extraBones{};         // skeleton bone, -1 unmapped
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraRest;     // skeleton rest local
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraFrame;    // canonical world rotation in the neutral pose
        // per present slot: the slots folded into it, parent first (the slot itself last)
        std::array<std::vector<uint8_t>, pose::HUMAN_BONE_COUNT> chains;

        RetargetRig(){ extraBones.fill(-1); }
        bool valid() const { return rig && rig->valid(); }
        // fills everything from a built rig and its extra map
        void build(const std::shared_ptr<const pose::PoseRig>& poseRig, const pose::HumanExtraMap& extraMap);
    };

    // Realises one sample on `target` (spec D12, anim-retarget 7.3): the local skeleton rotation
    // of every present slot (folds for slots the target lacks, neutral for slots the sample
    // lacks), the hips local position (the neutral one without a body offset), Mirror, then Foot IK
    // (both legs to their goals), and the local rotation of every mapped extra bone (its rest when
    // the (mirrored) sample has no value: extraLive says which have one). Rotations are skeleton
    // locals (before the bridge). `extraLive` may be null.
    JASPERENGINE_API void realiseHumanoid(const RetargetRig& target, const HumanoidSample& sample, bool mirror, bool footIK,
                                          pose::HumanPose& out, std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT>& extraLocal,
                                          std::bitset<pose::HUMAN_EXTRA_BONE_COUNT>* extraLive = nullptr);

    // ---- a source: a model file's humanoid ----

    // What a generic clip track of a source model is to the humanoid
    enum class RetargetTrackRole : uint8_t {
        Generic,        // not a humanoid bone (props, unmapped leaf bones, morph, lattice, property): plays by path
        Slot,           // a slot bone's rotation
        Extra,          // an extra bone's rotation
        Hips,           // the hips' translation
        Dropped         // a mapped bone's translation or scale, a bone between slots: not retargeted
    };

    // A model's humanoid as a retargeting source (and as an Animator's avatar): its skin skeleton,
    // the paths of its bones, and the source half of the path (the per-slot maps)
    struct JASPERENGINE_API RetargetSource : RetargetRig{
        std::string modelPath;                      // as given (asset-relative)
        pose::HumanoidMap humanoid;
        std::vector<Matrix4> bridges;               // per skeleton bone: skeleton local = bridge * Transform local
        std::vector<std::string> bonePaths;         // per skeleton bone: its path from the model root
        std::vector<Quaternion> restRotation;       // per skeleton bone: the Transform-local rest rotation
        std::vector<Vector3> restPosition;          // per skeleton bone: the Transform-local rest position
        std::vector<Vector3> restScale;             // per skeleton bone: the Transform-local rest scale
        std::vector<std::string> notes;             // the humanoid and extra auto-detect notes
        // per skeleton bone: between two slots (neither slot nor extra, an ancestor of a slot)
        std::vector<uint8_t> between;
        // the source maps: E = slotA * transformLocal * slotB, d = extraA * transformLocal * extraB
        // (relative tracks: the rest first), offset = hipsLinear * transformLocal + hipsOrigin
        std::array<pose::Quatd, pose::HUMAN_BONE_COUNT> slotA, slotB;
        std::array<pose::Quatd, pose::HUMAN_EXTRA_BONE_COUNT> extraA, extraB;
        pose::Vec3d hipsLinear[3];                  // columns
        pose::Vec3d hipsOrigin;
        std::array<Quaternion, pose::HUMAN_BONE_COUNT> restE;      // per mapped slot: E at rest
        Vector3 restHips = Vector3(0, 0, 0);        // the body offset at rest
        std::array<Quaternion, pose::HUMAN_BONE_COUNT> slotBridge; // per slot: the bridge's rotation (skeleton local = bridge * local)
        Matrix4 hipsBridge;

        // the skeleton bone a clip track path animates: its exact path from the model root, else
        // the path without its first segments (a clip made on a parent of the model), else a
        // unique bone name equal to the last segment; -1 when none
        int boneOfPath(const std::string& path) const;
        // the role of a track of a generic clip (`bone` from boneOfPath)
        RetargetTrackRole roleOf(ClipChannel channel, int bone) const;
        // the body offset of a hips Transform-local position
        pose::Vec3d bodyOffset(const Vector3& transformLocal) const{
            const pose::Vec3d p(transformLocal);
            return hipsOrigin + hipsLinear[0] * p.x + hipsLinear[1] * p.y + hipsLinear[2] * p.z;
        }
    };

    // Builds the source of a parsed model (glTF data; the sidecar as text, "" = none):
    // pose::buildSkeletonFromGltf + buildSkinFromGltf + resolveHumanoid (with the extra bones) +
    // PoseRig::build with the twist corrections, then the maps. False with `error` when the model
    // has no skeleton or is not a valid humanoid.
    JASPERENGINE_API bool buildRetargetSource(const tinygltf::Model& gltf, const std::string& modelPath, const std::string& sidecarText,
                                              RetargetSource& out, std::string* error);
    // The same from a skeleton and its bone paths (identity bridges, Transform local = skeleton
    // local): an Animator's own rig when it has no readable model file
    JASPERENGINE_API bool buildRetargetSource(const pose::Skeleton& skeleton, const std::vector<std::string>& bonePaths,
                                              const std::string& modelPath, const std::string& sidecarText, RetargetSource& out,
                                              std::string* error);

    // Sets the rig of a source (its skeleton, bone paths, bridges and rests kept: the same model
    // file) and computes its maps again: an edited Avatar's working copy (the Avatar window)
    JASPERENGINE_API void setRetargetRig(RetargetSource& source, const std::shared_ptr<const pose::PoseRig>& rig,
                                         const pose::HumanoidMap& humanoid, const pose::HumanExtraMap& extra);

    // The foot goals of a source pose (skeleton locals of the slots, the hips position: forward
    // kinematics); `has` false for a foot the rig lacks
    JASPERENGINE_API void sourceFootGoals(const RetargetSource& source, const pose::HumanPose& skeletonPose, Vector3 goals[2], bool has[2]);

    // The shared source of a model file (by model key and sidecar content; the pooled glTF when the
    // model is loaded, else a parse of the file); null with `error` when it is not a usable humanoid
    JASPERENGINE_API std::shared_ptr<const RetargetSource> retargetSource(const std::string& modelPath, std::string* error);
    // drops the cached sources of a model ("" = all): a model or sidecar changed, project close
    JASPERENGINE_API void dropRetargetSources(const std::string& modelPath = std::string());

    // The model a generic clip was made on: a sub-clip's model, a .anim's source (kind "model")
    // or Preview Model; "" when it names none
    JASPERENGINE_API std::string clipSourceModel(const AnimationClip& clip, const std::string& reference);

    // The source of a generic clip played retargeted (clipSourceModel's rig), with the errors a
    // Retarget entry and the Animator report: no source model, not a model, not a humanoid
    JASPERENGINE_API std::shared_ptr<const RetargetSource> clipRetargetSource(const AnimationClip& clip, const std::string& reference,
                                                                              std::string* error);

    // ---- a clip read as humanoid samples ----

    // One clip bound for humanoid playback: a humanoid clip's tracks, or a generic clip's through
    // its source. Built at bind (allocates), sampled allocation-free.
    struct JASPERENGINE_API RetargetBinding{
        bool active = false;
        std::shared_ptr<const RetargetSource> source;   // a generic clip's source; null for a humanoid clip
        // per clip track: the sample field it feeds, -1 none (0..54 slot or extra, FIELD_POSITION + k a position)
        std::vector<int16_t> trackField;
        // per clip track of a generic clip: its source skeleton bone (-1 none)
        std::vector<int> trackBone;
        // per clip track: true when it plays by path as an ordinary track (a generic track: props,
        // unmapped bones, morph, lattice, property)
        std::vector<uint8_t> byPath;
        // what the clip gives
        std::bitset<pose::HUMAN_BONE_COUNT> slots;
        std::bitset<pose::HUMAN_EXTRA_BONE_COUNT> extras;
        bool hips = false;
        bool feet[2] = {false, false};
        // generic clips: the animated source bones that are not retargeted (between slots,
        // translations and scale of mapped bones), for reports
        std::vector<std::string> dropped;

        static constexpr int FIELD_POSITION = 100;

        // false (inactive) when the clip has nothing humanoid: a generic clip needs `source`
        bool bind(const AnimationClip& clip, const std::shared_ptr<const RetargetSource>& sourceRig);
        // the clip at `clipTime` (per clip track `cursors`, may be null); a generic clip's foot
        // goals take forward kinematics of the source: only with `feet`
        // `stepFps` > 0: S48 Pose Frame Rate, every track held on steps from its previous key
        // except the hips' position (the root's translation stays smooth)
        void sample(const AnimationClip& clip, float clipTime, KeyCursor* cursors, bool withFeet, HumanoidSample& out,
                    float stepFps = 0.0f) const;
    };

    // ---- the Animator's target ----

    // The humanoid of an Animator's rig: the Animator's model file's avatar (its skin skeleton and
    // sidecar, mapped onto the rig's bones by path), else one built from the rig's own bones
    struct JASPERENGINE_API RetargetTarget{
        bool valid = false;
        std::string error;
        std::shared_ptr<const RetargetSource> avatar;   // also the source of the model's own clips
        std::shared_ptr<const pose::PoseRig> rig;       // avatar->rig
        std::array<int, pose::HUMAN_BONE_COUNT> slotBones{};            // rig bone per slot, -1 unmapped
        std::array<int, pose::HUMAN_EXTRA_BONE_COUNT> extraBones{};     // rig bone per extra bone, -1 unmapped
        // Transform local = bridge^-1 * skeleton local
        std::array<Quaternion, pose::HUMAN_BONE_COUNT> slotBridgeInverse;
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraBridgeInverse;
        Matrix4 hipsBridgeInverse;

        RetargetTarget(){ slotBones.fill(-1); extraBones.fill(-1); }
        // `modelPath`: the Animator's model file (its sidecar overrides the auto-detect), "" for none
        void build(const AnimatorRig& rig, const std::string& modelPath);
    };

    // Writes a realised sample into `out` (the target rig's local pose): every mapped slot, the
    // hips position when the sample has a body offset, the extra bones `extraLive` marks
    JASPERENGINE_API void writeRealised(const RetargetTarget& target, const pose::HumanPose& pose,
                                        const std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT>& extraLocal,
                                        const std::bitset<pose::HUMAN_EXTRA_BONE_COUNT>& extraLive, bool hips, bool slots, Pose& out);

}

#endif //ANIMATOR_RETARGET_H
