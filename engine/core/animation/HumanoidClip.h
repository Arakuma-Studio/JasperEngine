// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim-retarget spec 7.1-7.4, on S21's one retarget path: animation/AnimatorRetarget.h)
// - Purpose: humanoid clips, the rig-independent runtime format (Unity's muscle clips): the
//   conversion of a generic clip on its source rig to humanoid tracks (Convert / Bake to Humanoid
//   Clip, an Animation Type: Humanoid model's clips in the pool), the Animation Player's binding of
//   a humanoid clip (or of a generic clip retargeted from its source model) to a root's rig with
//   its bone entities, and the read-back of a rig's current pose as humanoid values (Record).
// - The maths is AnimatorRetarget's (the one path the Animator plays too): a source's per-slot
//   maps turn its rotation keys into E key for key (E = A * q * B with fixed A, B: the same
//   times, eases and tangents reproduce the source exactly), realiseHumanoid poses the target.
// - Thread-safety: conversion is pure; a HumanoidBinding is owned by one player state (main
//   thread). No allocation per frame: a binding's sample, pose and outputs are sized at bind.

#ifndef HUMANOID_CLIP_H
#define HUMANOID_CLIP_H

#include "Export.h"
#include "animation/AnimationClip.h"
#include "animation/AnimatorRetarget.h"
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

    // ---- the source: a generic clip's rig (AnimatorRetarget's RetargetSource) ----

    using HumanoidSource = RetargetSource;

    // the skeleton bone a clip track path animates (RetargetSource::boneOfPath)
    inline int humanoidSourceBone(const HumanoidSource& source, const std::string& path){ return source.boneOfPath(path); }

    // Builds the source of a parsed model (buildRetargetSource)
    inline bool buildHumanoidSource(const tinygltf::Model& gltf, const std::string& modelPath, const std::string& sidecarText,
                                    HumanoidSource& out, std::string* error){
        return buildRetargetSource(gltf, modelPath, sidecarText, out, error);
    }

    // The source rigs by model file and sidecar content (retargetSource). Thread safe.
    class HumanoidSourceCache{
    public:
        static bool get(const std::string& modelPath, std::shared_ptr<const HumanoidSource>& out, std::string* error){
            out = retargetSource(modelPath, error);
            return out != nullptr;
        }
        // a model file or its sidecar changed (AnimationClipPool::invalidateModel); "" = all
        static void drop(const std::string& modelPath){ dropRetargetSources(modelPath); }
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
    // humanScale. Other tracks (props, unmapped leaf bones and their scale, morph, lattice,
    // property) are copied unchanged; animated bones between slots, translations of mapped bones
    // other than the hips and the scale of mapped bones are dropped, with a warning unless every
    // key sits at the bone's rest. Events and settings are copied. A clip that is humanoid
    // already is copied as it is. Deterministic.
    JASPERENGINE_API bool convertToHumanoid(const ClipData& generic, const HumanoidSource& source, ClipData& out,
                                            std::vector<ClipWarning>& warnings, std::string* error,
                                            HumanoidConversionReport* report = nullptr);

    // `clip` as a humanoid clip: itself when it is one, else converted from its source model
    // (clipRetargetSource: a sub-clip's model, the .anim's source or Preview Model); null with
    // `error` when it names none or that model is no humanoid. Not cached (editor previews).
    JASPERENGINE_API std::shared_ptr<const AnimationClip> humanoidClipOf(const std::shared_ptr<const AnimationClip>& clip,
                                                                         const std::string& reference, std::string* error);

    // ---- the target: realisation ----

    // The rig a humanoid clip plays on: the player root's first humanoid model (spec 5.9)
    struct JASPERENGINE_API HumanoidTarget : RetargetRig{
        std::string modelPath;
        Entity model = NULL_ENTITY;
        std::array<Entity, pose::HUMAN_BONE_COUNT> slotEntities{};
        // Transform local = bridge^-1 * skeleton local (PoseAnimatorRuntime's rule)
        std::array<Quaternion, pose::HUMAN_BONE_COUNT> bridgeRotationInverse;
        Matrix4 hipsBridgeInverse;
        std::array<Entity, pose::HUMAN_EXTRA_BONE_COUNT> extraEntities{};
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraBridgeRotationInverse;

        HumanoidTarget(){ slotEntities.fill(NULL_ENTITY); extraEntities.fill(NULL_ENTITY); }
    };

    // the target of a rig (ModelRigInfo of pose::buildModelRig) with its bone entities and bridges
    JASPERENGINE_API bool buildHumanoidTarget(const pose::ModelRigInfo& info, const std::vector<Entity>& boneEntities,
                                              const std::vector<Matrix4>& bridges, Entity model, const std::string& modelPath,
                                              HumanoidTarget& out);

    // realiseHumanoid (AnimatorRetarget.h) poses a HumanoidTarget: it is a RetargetRig

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
        // extra bones: whether the output writes its bone unmirrored / mirrored (isLive). An extra
        // bone writes only while the (mirrored) sample has a value for it: a right-hand-only
        // clip leaves the left hand alone, and mirrored it drives the left hand and leaves the
        // right one alone (slots and the hips: always live)
        bool liveUnmirrored = true;
        bool liveMirrored = true;
        Quaternion rotation;                            // the Transform-local value of the last sample
        Vector3 position = Vector3(0, 0, 0);
    };

    struct JASPERENGINE_API HumanoidBinding{
        std::shared_ptr<const HumanoidTarget> target;
        // the clip as humanoid samples (a humanoid clip's tracks, or a generic clip through its
        // source: a Retarget entry), the one path the Animator reads clips with too
        RetargetBinding reader;
        // what the clip writes on the target: every present slot (when the clip has slot tracks),
        // the hips position (when it has a body offset), the mapped extra bones it has and the
        // mirror counterparts of those (each live only in the state that gives it a value)
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

    // whether `output` writes its bone in the binding's current Mirror state (an extra bone the
    // (mirrored) clip has no value for neither covers nor writes it). Every consumer of the
    // outputs (players, previews, reports) asks.
    inline bool isLive(const HumanoidBinding& binding, const HumanoidOutput& output){
        return binding.mirror ? output.liveMirrored : output.liveUnmirrored;
    }

    // Binds a humanoid clip, or a generic clip retargeted from `source` (its model's rig), to a
    // target: false (out empty) when the clip is neither or the target has no rig
    JASPERENGINE_API bool bindHumanoidClip(const AnimationClip& clip, const std::shared_ptr<const HumanoidTarget>& target,
                                           HumanoidBinding& out, const std::shared_ptr<const HumanoidSource>& source = nullptr);
    // Samples the clip at `clipTime` (the state's key cursors, per clip track), realises it and
    // fills every output's Transform-local value
    JASPERENGINE_API void sampleHumanoidBinding(const AnimationClip& clip, HumanoidBinding& binding, float clipTime,
                                                std::vector<KeyCursor>* cursors);

}

#endif //HUMANOID_CLIP_H
