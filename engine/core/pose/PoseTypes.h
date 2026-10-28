// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pose animation data model (spec: pose animation phase 1, section 4.2). Shared by the
// engine runtime, the .poseanim file (phase 2), the editor and the MCP tools.

#ifndef POSE_TYPES_H
#define POSE_TYPES_H

#include "Export.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "pose/HumanBones.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::pose {

    enum class TransitionCurve : uint8_t { Spring = 0, Linear, Smooth };

    // How a key pose moves to the next one
    struct Transition {
        TransitionCurve curve = TransitionCurve::Spring;
        float duration = 0.12f;      // snap time, seconds: the target is first reached at this time
        float overshoot = 0.25f;     // spring only: peak overshoot fraction
        float anticipation = 0.06f;  // spring only: wind-up depth before the move
    };

    struct HumanizeSettings {
        bool enabled = false;
        bool jointLimits = true;
        float movingHolds = 0.08f;   // fraction of the next move started during a hold
        float inbetweens = 0.4f;     // 0 = off, 1 = strongest cap on per-frame rotation
        float life = 0.6f;           // sway amplitude, degrees
        uint32_t seed = 1;
        float headFollow = 0.5f;     // follow-through amounts
        float torsoFollow = 0.4f;
        float springiness = 0.5f;    // 0 = damped, 1 = bouncy follow-through
    };

    struct PoseAnimationSettings {
        bool loop = true;
        Transition transition;                 // default transition of every pose
        float stepRate = 0.0f;                 // choppiness: steps per second, 0 = off
        float overlap = 0.05f;                 // seconds of delay for the deepest bones
        float looseness = 0.2f;                // 0..1, deep bones arrive later and swing further
        std::array<float, 4> weights{1.0f, 1.0f, 1.0f, 0.0f};   // BodyGroup order: spine, head, arms, legs
        HumanizeSettings humanize;
        int frameRate = 30;                    // bake / runtime frames per second
    };

    // 22 bones x 3 degrees of freedom; DOF index = bone * 3 + k (k: 0 = dof1, 1 = dof2, 2 = twist)
    constexpr int DOFS_PER_BONE = 3;
    constexpr int DOF_COUNT = HUMAN_BONE_COUNT * DOFS_PER_BONE;

    // Rig-independent pose: joint angles in degrees (0 = neutral standing pose) and the hips
    // horizontal offset in the canonical character frame, in humanScale units (y is always 0:
    // the vertical position comes from grounding and PoseKey::lift)
    struct BodyPose {
        std::array<float, DOF_COUNT> angles{};
        Vector3 offset = Vector3(0, 0, 0);
    };

    // One key pose of a Pose Animation
    struct PoseKey {
        std::string name;
        float hold = 0.1f;                 // seconds held before the transition out
        bool customTransition = false;     // when false the animation's transition is used
        Transition transition;             // the transition out of this pose (when custom)
        bool grounded = true;              // lowest point on the floor
        float lift = 0.0f;                 // humanScale units above the floor when !grounded
        BodyPose body;
    };

    struct PoseAnimationData {
        PoseAnimationSettings settings;
        std::vector<PoseKey> poses;
        std::string previewModel;          // project-relative model path for previews
    };

    // Rig-specific realised pose: local rotations of the 22 slot bones (relative to their actual
    // skeleton parent bone; identity for slots the rig does not have) and the hips local position
    struct HumanPose {
        std::array<Quaternion, HUMAN_BONE_COUNT> rotations;
        Vector3 hipsPosition = Vector3(0, 0, 0);
    };

    // The transition out of pose `index` (its own when custom, else the animation's)
    inline const Transition& transitionOut(const PoseAnimationData& data, size_t index){
        const PoseKey& key = data.poses[index];
        return key.customTransition ? key.transition : data.settings.transition;
    }

}

#endif // POSE_TYPES_H
