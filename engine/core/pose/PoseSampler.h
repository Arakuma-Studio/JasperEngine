// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Timeline, per-bone sampling (overlap, looseness, choppiness, moving holds, life), the baked
// frames (in-betweens, follow-through, joint limits) and auto timing (spec: pose animation
// phase 1, section 4.8).
//
// Decisions the spec leaves open:
// - Joint limits on the frames follow humanize.jointLimits alone (like the key poses, phases 2
//   and 4); in-betweens, moving holds, life and follow-through need humanize.enabled.
// - Follow-through, joint limits, the skipping of unmapped slots and the frames' ground
//   correction need SamplerInput::rig; with rig == nullptr they are skipped.
// - Ground correction of the frames (always on with a rig; spec 4.8): a ground weight g per
//   frame (1 in grounded holds and between two grounded keys, 0 in lifted holds and between two
//   lifted keys, faded with smoothstep(clamp01(e_hips)) over launch and landing segments and over
//   the hold before one, where moving holds already drift the hips' ease) puts
//   the lowest point at (1 - g) * max(h, 0) for an uncorrected height h. It runs after the
//   in-betweens and before follow-through; frames follow-through or the limits change are
//   corrected again (their uncorrected height = the new lowest point minus the first shift). The
//   frame's time is classified after the choppiness step, as sample() sees it; the loop's last
//   segment leads back to key 0 and the last frame stays a copy of frame 0. Overlap delays of
//   other bones are not considered (g follows the hips, which lead the motion).
// - lifeRotation = Rx * Ry * Rz (bone-local axes, post-multiplied in that order); hash01 packs
//   (seed << 32) | (bone << 16) | (axis << 8) | k into splitmix64 and takes the top 24 bits.
// - A one-pose animation is static: every sample is the key (no life). An empty one leaves
//   `out` untouched and has no frames.
// - Looping frames always end with a copy of the first frame. The previous cycle (c = -1) of
//   4.8 step 4 is blended to fully settled by the end of the loop (e' = e + (1 - e) *
//   smoothstep(0, 1, tau / len)), so sampling is continuous across every bone's wrap time.
// - In-betweens on a loop: the warm-up pass ends at frame `count`, the same instant as frame 0,
//   so the write pass starts from that state at frame 0.
// - autoTiming: the mean bone angle is over all 22 slots; a non-looping animation's first pose
//   (no incoming motion) gets the base hold and its last pose (no outgoing segment) the
//   animation's duration. Holds clamp to 0..5 s, durations to 0.02..1.0 s; what a clamp removes
//   or adds is spread over the values still free, so the total length is kept unless every
//   value is at a bound. With stepRate > 0 every value is a whole number of steps (at least
//   one) and the total is the step count nearest the old length (unavoidable residual: up to
//   half a step, when the old length is not on the grid).

#ifndef POSE_SAMPLER_H
#define POSE_SAMPLER_H

#include "Export.h"
#include "math/Quaternion.h"
#include "pose/PoseRig.h"
#include "pose/PoseTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine::pose {

    // Segment s goes from pose s to pose s + 1 (the last one back to pose 0 only when looping)
    struct PoseTimeline {
        bool loop = false;
        double length = 0.0;                    // seconds: holds + segment durations
        std::vector<double> holds;              // per pose
        std::vector<double> start;              // per segment: the end of pose s's hold
        std::vector<Transition> transitions;    // per segment: transitionOut(s)
        size_t segmentCount() const { return start.size(); }
    };

    JASPERENGINE_API PoseTimeline buildTimeline(const PoseAnimationData& data);

    struct SamplerInput {
        std::vector<HumanPose> keys;            // realised pose per key
        PoseAnimationSettings settings;
        PoseTimeline timeline;
        const PoseRig* rig = nullptr;           // optional: follow-through, joint limits, unmapped slots, ground correction
        std::vector<bool> grounded;             // per key (PoseKey::grounded): the frames' ground correction (empty: none)
    };

    JASPERENGINE_API SamplerInput makeSamplerInput(const PoseAnimationData& data, std::vector<HumanPose> keys, const PoseRig* rig = nullptr);

    // Realises every key: joint limits first when humanize.jointLimits, then
    // fromBodyPose(body, skin, grounded, lift)
    JASPERENGINE_API std::vector<HumanPose> realiseKeys(const PoseAnimationData& data, const PoseRig& rig, const PoseSkin* skin = nullptr);

    JASPERENGINE_API void sample(const SamplerInput& input, float t, HumanPose& out);

    // count = max(1, round(length * fps)) + 1 frames, frame f at t = f * length / count
    JASPERENGINE_API std::vector<HumanPose> frames(const SamplerInput& input, int fps);

    // The frames' ground weight of a motion from a pose (fromGrounded: on the floor) to a key
    // (toGrounded) at the hips' ease `hipsEase`: constant when both agree, else faded with
    // smoothstep(clamp01(hipsEase)) (out on a launch, in on a landing)
    JASPERENGINE_API double transitionGroundWeight(bool fromGrounded, bool toGrounded, double hipsEase);
    // The frames' ground correction of one pose (rig skin): its lowest point, at height h above
    // the floor, ends at floor + (1 - weight) * max(h, 0). Returns the lowest point it ends at.
    JASPERENGINE_API double groundToWeight(const PoseRig& rig, HumanPose& pose, double weight);
    // The frames' joint limits pass (humanize.jointLimits) on one pose: the mapped bones outside
    // their limits clamped (a long-form swing within the limits kept), the others untouched
    JASPERENGINE_API void clampPoseToLimits(const PoseRig& rig, HumanPose& pose);

    // the humanize life sway of one bone at time t (loops seamlessly over `length`)
    JASPERENGINE_API Quaternion lifeRotation(int bone, float t, float length, float lifeDegrees, uint32_t seed);
    JASPERENGINE_API double hash01(uint32_t seed, int bone, int axis, int k);

    // Auto timing: holds and transition durations from the motion between poses, keeping the
    // total length. The plain form takes hips travel from HumanPose::hipsPosition as is; the rig
    // form measures it in model space (use it when the hips' parent is scaled).
    JASPERENGINE_API void autoTiming(PoseAnimationData& data, const std::vector<HumanPose>& realised, float humanScale);
    JASPERENGINE_API void autoTiming(PoseAnimationData& data, const std::vector<HumanPose>& realised, const PoseRig& rig);

}

#endif // POSE_SAMPLER_H
