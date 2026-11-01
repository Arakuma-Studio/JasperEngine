// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim-retarget spec 7.1-7.3)
// - Purpose: humanoid clips, the rig-independent runtime format (Unity's muscle clips): the
//   conversion of a generic clip on its source rig to humanoid tracks, and the realisation of a
//   humanoid sample on any other humanoid rig (Mirror, Foot IK, the missing-slot fold), plus the
//   cache of source rigs built from the glTF data (no scene).
// - The maths is the pose system's (PoseRig): a slot's humanoid value is its joint rotation E
//   relative to the neutral pose in the canonical frame (local = left * E * neutral), so rest
//   poses (T or A), bone axis conventions and facing cancel out. With G_b = W_b * N_b^-1,
//   G_b = G_p * E_b: E is the rotation between consecutive slots' neutral-relative frames, and a
//   slot the target lacks folds into its next present descendant (E_t(b) = product of the clip's
//   E over the slots strictly after b's effective target parent, parent first).
// - The body (hips) position and the foot IK goals are model-space positions in the canonical
//   frame relative to the neutral (grounded) hips, in humanScale units: the target places them at
//   neutralHips + C * (value * humanScale), which scales hips height and stride.
// - Extra bones (fingers, eyes, jaw) carry their delta from the rest local rotation expressed in
//   the canonical frame of the bone in the rig's neutral pose: d = W δ W^-1 with δ = rest^-1 *
//   local and W the bone's canonical world rotation in the neutral pose.
// - Thread-safety: pure functions; HumanoidSourceCache is thread safe (the pool converts on the
//   thread pool). A HumanoidBinding is owned by one player state (main thread).
// - No allocation per frame: a binding's sample, pose and outputs are sized at bind.

#ifndef HUMANOID_CLIP_H
#define HUMANOID_CLIP_H

#include "Export.h"
#include "animation/AnimationClip.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "pose/HumanExtraBones.h"
#include "pose/ModelRig.h"
#include "pose/PoseTypes.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
}

namespace jasperengine::anim{

    struct KeyCursor;

    // ---- the source: a generic clip's rig ----

    // A source rig and how a clip's paths map to its bones (built from the glTF data)
    struct JASPERENGINE_API HumanoidSource{
        std::string modelPath;                      // as given (asset-relative)
        std::shared_ptr<const pose::PoseRig> rig;   // twist corrections and skin applied
        pose::HumanoidMap humanoid;
        pose::HumanExtraMap extra;
        std::vector<int> extraBones;                // per extra bone: its skeleton bone (-1)
        std::vector<Matrix4> bridges;               // per skeleton bone: skeleton local = bridge * Transform local
        std::vector<std::string> bonePaths;         // per skeleton bone: its path from the model root
        std::vector<Quaternion> restRotation;       // per skeleton bone: the Transform-local rest rotation
        std::vector<Vector3> restPosition;          // per skeleton bone: the Transform-local rest position
        std::vector<std::string> notes;             // the humanoid and extra auto-detect notes
    };

    // the skeleton bone a clip track path animates: its exact path from the model root, else the
    // path without its first segments (a clip made on a parent of the model), else a unique bone
    // name equal to the last segment; -1 when none
    JASPERENGINE_API int humanoidSourceBone(const HumanoidSource& source, const std::string& path);

    // Builds the source of a parsed model (glTF data, the sidecar overrides as text: "" = none):
    // pose::buildSkeletonFromGltf + buildSkinFromGltf + resolveHumanoid (with the extra bones) +
    // PoseRig::build with the twist corrections. False with `error` when the model has no skeleton
    // or is not a valid humanoid.
    JASPERENGINE_API bool buildHumanoidSource(const tinygltf::Model& gltf, const std::string& modelPath, const std::string& sidecarText,
                                              HumanoidSource& out, std::string* error);

    // The source rigs by model file and sidecar content (the PoseRigCache rule, from the files):
    // the pooled glTF when the model is loaded, else a parse of the file. Thread safe.
    class JASPERENGINE_API HumanoidSourceCache{
    public:
        static bool get(const std::string& modelPath, std::shared_ptr<const HumanoidSource>& out, std::string* error);
        // a model file or its sidecar changed (AnimationClipPool::invalidateModel); "" = all
        static void drop(const std::string& modelPath);
    };

    // ---- conversion ----

    struct JASPERENGINE_API HumanoidConversionReport{
        std::vector<std::string> mappedSlots;       // source slots written
        std::vector<std::string> mappedExtra;       // source extra bones written
        std::vector<std::string> unmappedSlots;     // slots the source rig does not have
        std::vector<std::string> droppedBones;      // animated bones dropped (between slots, translations of mapped bones)
        std::vector<std::string> genericTracks;     // paths copied unchanged
    };

    // Generic clip -> humanoid clip (spec 7.2). Every mapped slot gets a humanoid track: from its
    // bone's rotation track, key for key (E = A * q * B with fixed A, B: the same times, eases and
    // tangents reproduce the source exactly), or one key from its rest pose; the hips position
    // likewise (an affine map of the bone's translation); the mapped extra bones with a rotation
    // track; the two foot goals sampled with forward kinematics on the union of the leg tracks'
    // key times and a frame-rate grid (the clip's, at least 30 Hz), keyframe-reduced within 1e-4
    // humanScale. Other tracks (props, unmapped leaf bones, scale, morph, lattice, property) are
    // copied unchanged; animated bones between slots and translations of mapped bones other than
    // the hips are dropped with a warning. Events and settings are copied. A clip that is humanoid
    // already is copied as it is. Deterministic.
    JASPERENGINE_API bool convertToHumanoid(const ClipData& generic, const HumanoidSource& source, ClipData& out,
                                            std::vector<ClipWarning>& warnings, std::string* error,
                                            HumanoidConversionReport* report = nullptr);

    // ---- the target: realisation ----

    // One humanoid sample (spec 7.1): slot joint rotations E, extra deltas, the body offset and
    // the foot goals (each with its presence bit)
    struct JASPERENGINE_API HumanoidSample{
        std::array<Quaternion, pose::HUMAN_BONE_COUNT> E;
        std::bitset<pose::HUMAN_BONE_COUNT> hasE;
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extra;
        std::bitset<pose::HUMAN_EXTRA_BONE_COUNT> hasExtra;
        Vector3 hips = Vector3(0, 0, 0);
        bool hasHips = false;
        Vector3 feet[2] = {Vector3(0, 0, 0), Vector3(0, 0, 0)};
        bool hasFeet[2] = {false, false};
    };

    // Left/right swapped, quaternions (w, x, y, z) -> (w, x, -y, -z), positions x negated (x is the
    // canonical left axis): the mirror of mirrorBodyPose in E space; twice is the identity
    JASPERENGINE_API void mirrorHumanoidSample(const HumanoidSample& in, HumanoidSample& out);

    // The rig a humanoid clip plays on: the player root's first humanoid model (spec 5.9)
    struct JASPERENGINE_API HumanoidTarget{
        std::shared_ptr<const pose::PoseRig> rig;
        pose::HumanExtraMap extra;
        std::string modelPath;
        Entity model = NULL_ENTITY;
        std::array<Entity, pose::HUMAN_BONE_COUNT> slotEntities{};
        // Transform local = bridge^-1 * skeleton local (PoseAnimatorRuntime's rule)
        std::array<Quaternion, pose::HUMAN_BONE_COUNT> bridgeRotationInverse;
        Matrix4 hipsBridgeInverse;
        std::array<Entity, pose::HUMAN_EXTRA_BONE_COUNT> extraEntities{};
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraBridgeRotationInverse;
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraRest;    // skeleton rest local
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraFrame;   // canonical world rotation in the neutral pose
        // per present slot: the clip slots folded into it, parent first (the slot itself last)
        std::array<std::vector<uint8_t>, pose::HUMAN_BONE_COUNT> chains;

        HumanoidTarget(){ slotEntities.fill(NULL_ENTITY); extraEntities.fill(NULL_ENTITY); }
    };

    // the target of a rig (ModelRigInfo of pose::buildModelRig) with its bone entities and bridges
    JASPERENGINE_API bool buildHumanoidTarget(const pose::ModelRigInfo& info, const std::vector<Entity>& boneEntities,
                                              const std::vector<Matrix4>& bridges, Entity model, const std::string& modelPath,
                                              HumanoidTarget& out);

    // Realises one sample on the target (spec 7.3): the local skeleton rotation of every present
    // slot (the fold for slots the target lacks, neutral for slots the clip lacks), the hips local
    // position (the neutral one without a body offset), Mirror, then Foot IK (both legs to their
    // goals, the feet keeping their world rotation), and the local rotation of every mapped extra
    // bone in the sample. Rotations are skeleton locals (before the bridge).
    JASPERENGINE_API void realiseHumanoid(const HumanoidTarget& target, const HumanoidSample& sample, bool mirror, bool footIK,
                                          pose::HumanPose& out, std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT>& extraLocal);

    // The humanoid value of the target's current pose (anim-retarget 9: Record, Key All, MCP):
    // a slot's E from its bone's Transform (rig.jointRotationFromLocal of bridge * local), an extra
    // bone's canonical delta, the body offset or a foot goal from the current Transforms. False
    // when the target does not have the bone (or the registry lacks its entity).
    JASPERENGINE_API bool readHumanoidValue(EntityRegistry* registry, const HumanoidTarget& target, ClipChannel channel, int humanBone,
                                            std::vector<float>& out);
    // the target's current pose from its bone Transforms (skeleton locals, the hips position)
    JASPERENGINE_API bool currentHumanoidPose(EntityRegistry* registry, const HumanoidTarget& target, pose::HumanPose& out);

    // ---- a humanoid clip bound to a target (a player state or a preview) ----

    struct JASPERENGINE_API HumanoidOutput{
        enum class Kind : uint8_t { Slot, Extra, Hips };
        Entity entity = NULL_ENTITY;
        ClipChannel channel = ClipChannel::Rotation;    // Rotation (slots, extras) or Position (hips)
        Kind kind = Kind::Slot;
        uint8_t index = 0;                              // slot or extra index
        Quaternion rotation;                            // the Transform-local value of the last sample
        Vector3 position = Vector3(0, 0, 0);
    };

    struct JASPERENGINE_API HumanoidBinding{
        std::shared_ptr<const HumanoidTarget> target;
        // per clip track: the sample field it feeds (-1 none): 0..54 slot / extra, 100 + k a position
        std::vector<int16_t> trackField;
        // what the clip writes on the target: every present slot (when the clip has slot tracks),
        // the hips position (when it has a body offset), the mapped extra bones it has
        std::vector<HumanoidOutput> outputs;
        // playback options of the state, set by its owner before sampling
        bool mirror = false;
        bool footIK = false;
        // report (MCP, Inspector): target slots fed by a fold ("chest folded into upper_chest"),
        // clip bones the target lacks
        std::vector<std::string> folded;
        std::vector<std::string> missingOnTarget;
        // scratch
        HumanoidSample sample;
        pose::HumanPose pose;
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraLocal;
    };

    // Binds a humanoid clip to a target: false (out empty) when the clip is not humanoid or the
    // target has no rig
    JASPERENGINE_API bool bindHumanoidClip(const AnimationClip& clip, const std::shared_ptr<const HumanoidTarget>& target,
                                           HumanoidBinding& out);
    // Samples the clip's humanoid tracks at `clipTime` (the state's key cursors, per clip track),
    // realises them and fills every output's Transform-local value
    JASPERENGINE_API void sampleHumanoidBinding(const AnimationClip& clip, HumanoidBinding& binding, float clipTime,
                                                std::vector<KeyCursor>* cursors);

}

#endif //HUMANOID_CLIP_H
