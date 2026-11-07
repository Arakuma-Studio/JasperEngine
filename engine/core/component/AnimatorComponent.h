// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R1-R15, D1, D8, D10, D16)
// - Purpose: Unity's Animator: plays an Animator Controller (.controller) on its entity's
//   hierarchy. The controller layer above clip sampling: state machines, blend trees, layers,
//   masks, montages, IK, root motion and events, over upstream's clips (.anim and model#clip).
//   The Animation Player stays the simple path; on one hierarchy the Animator runs after the
//   players and owns the bones it binds (D1).
// - The runtime (anim::AnimatorInstance) is driven by AnimatorSystem; scripts go through the
//   Animator object (object/Animator.h); the editor plumbing (reflection tables, Inspector, MCP)
//   covers the authored fields.
// - Thread-safety: an ECS component (main thread); AnimatorSystem's wave reads the authored
//   fields of the Animators it evaluates and writes only their runtime.
// - Invariants: only the authored fields are serialized, cloned as values and emitted on export;
//   `runtime` and the subscriptions are reset on clone/decode (EntityCloner, script reset,
//   Scene::removeSubscriptionsByTag). Paths are relative to the Animator's entity (ClipBinding
//   grammar) or humanoid slot names ("left_foot", HumanBones.h) where noted.

#ifndef ANIMATOR_COMPONENT_H
#define ANIMATOR_COMPONENT_H

#include "Export.h"
#include "animation/AnimatorTypes.h"
#include "asset/AssetRef.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "util/FunctionSubscribe.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    namespace anim{
        class AnimatorInstance;
    }
    // animation/AnimationEvent.h (callers of onEvent include it)
    struct AnimationEvent;

    // One constraint of the IK stack (R11). `bones` holds paths or humanoid slot names:
    //   TwoBone: [upper, mid, end] (or [end]: its parent and grandparent)
    //   Fabrik: the chain, root first (2..16)
    //   LookAt: up to 4 bones, parents first (boneWeights parallel, missing = 1)
    //   FootIK: [upper leg, lower leg, foot]; `pelvis` is the bone that drops (default: hips)
    //   FootLock: [upper leg, lower leg, foot]
    struct JASPERENGINE_API AnimatorIKConstraint{
        std::string name;
        AnimatorIKType type = AnimatorIKType::TwoBone;
        bool enabled = true;
        float weight = 1.0f;
        std::string weightParameter;        // a Float parameter that multiplies the weight
        std::vector<std::string> bones;
        std::vector<float> boneWeights;     // LookAt
        // the target: an entity (its world position), else `targetPosition` (world)
        Entity target = NULL_ENTITY;
        Vector3 targetPosition = Vector3(0, 0, 0);
        bool useTargetRotation = false;     // TwoBone: the end takes the target entity's rotation
        // TwoBone pole: an entity, else `polePosition` (world) when usePole, else the current bend
        Entity pole = NULL_ENTITY;
        Vector3 polePosition = Vector3(0, 0, 0);
        bool usePole = false;
        int iterations = 10;                // Fabrik
        float tolerance = 0.001f;           // Fabrik, metres
        Vector3 forwardAxis = Vector3(0, 0, 1); // LookAt: the last bone's local forward
        float clampAngle = 60.0f;           // LookAt, degrees
        // FootIK
        std::string pelvis;
        float footHeight = 0.08f;           // the ankle above the sole
        float rayStart = 0.5f;              // the ray starts this far above the foot
        float rayLength = 1.0f;             // ... and reaches this far below it
        uint32_t layerMask = ~0u;
        float maxFootAngle = 30.0f;         // degrees
        float smoothing = 0.08f;            // spring half-life, seconds
        // FootLock
        std::string contactParameter;       // a Float parameter: >= 0.5 is contact; empty: by speed
        float speedThreshold = 0.05f;       // m/s below which the foot is in contact
        float releaseTime = 0.1f;

        bool operator==(const AnimatorIKConstraint& other) const{
            return name == other.name && type == other.type && enabled == other.enabled && weight == other.weight &&
                   weightParameter == other.weightParameter && bones == other.bones && boneWeights == other.boneWeights &&
                   target == other.target && targetPosition == other.targetPosition && useTargetRotation == other.useTargetRotation &&
                   pole == other.pole && polePosition == other.polePosition && usePole == other.usePole &&
                   iterations == other.iterations && tolerance == other.tolerance && forwardAxis == other.forwardAxis &&
                   clampAngle == other.clampAngle && pelvis == other.pelvis && footHeight == other.footHeight &&
                   rayStart == other.rayStart && rayLength == other.rayLength && layerMask == other.layerMask &&
                   maxFootAngle == other.maxFootAngle && smoothing == other.smoothing && contactParameter == other.contactParameter &&
                   speedThreshold == other.speedThreshold && releaseTime == other.releaseTime;
        }
        bool operator!=(const AnimatorIKConstraint& other) const{ return !(*this == other); }
    };

    // clipOverrides: plays `override` wherever the controller plays `original` (R15)
    struct JASPERENGINE_API AnimatorClipOverride{
        std::string original;       // a clip path as the controller references it
        std::string replacement;    // an asset-relative .anim or "<model>#<clip>"

        bool operator==(const AnimatorClipOverride& other) const{
            return original == other.original && replacement == other.replacement;
        }
        bool operator!=(const AnimatorClipOverride& other) const{ return !(*this == other); }
    };

    struct JASPERENGINE_API AnimatorComponent{
        bool enabled = true;
        AssetRef controller;                // the .controller, asset-relative
        float speed = 1.0f;
        bool applyRootMotion = false;
        std::string rootMotionBone;         // path or slot name; empty: the clip's, the hips, the first root bone
        AnimatorUpdateMode updateMode = AnimatorUpdateMode::Normal;
        AnimatorCullingMode cullingMode = AnimatorCullingMode::AlwaysAnimate;
        AnimatorWriteBones writeBones = AnimatorWriteBones::All;
        std::vector<std::string> exposedBones;      // writeBones Exposed: these paths too
        AnimatorEventMode eventMode = AnimatorEventMode::LeaderOnly;
        bool sendEventsToScripts = true;    // also call the scripts' methods named like events
        AnimatorRetarget retarget = AnimatorRetarget::Auto;
        std::vector<AnimatorClipOverride> clipOverrides;
        std::vector<AnimatorIKConstraint> ik;

        // runtime only (not serialised)
        bool needsLoad = true;              // the controller or a setting that rebinds changed
        std::shared_ptr<anim::AnimatorInstance> runtime;

        // an animation event fired (after the frame's pose was written)
        FunctionSubscribe<void(const AnimationEvent&)> onEvent;
        // (layer name, state full path)
        FunctionSubscribe<void(const std::string&, const std::string&)> onStateEnter;
        FunctionSubscribe<void(const std::string&, const std::string&)> onStateExit;
        // (montage path, interrupted)
        FunctionSubscribe<void(const std::string&, bool)> onMontageEnded;
        // (delta position world, delta rotation world): when set, root motion is not applied
        FunctionSubscribe<void(Vector3, Quaternion)> onAnimatorMove;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) {
            f(onEvent); f(onStateEnter); f(onStateExit); f(onMontageEnded); f(onAnimatorMove);
        }
    };

}

#endif //ANIMATOR_COMPONENT_H
