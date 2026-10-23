// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Timeline, per-bone sampling (overlap, looseness, choppiness, moving holds, life), the baked
// frames (in-betweens, follow-through, joint limits) and auto timing (spec: pose animation
// phase 1, section 4.8).
//
// Decisions the spec leaves open:
// - Joint limits on the frames follow humanize.jointLimits alone (like the key poses, phases 2
//   and 4); in-betweens, moving holds, life and follow-through need humanize.enabled.
// - Follow-through, joint limits and the skipping of unmapped slots need SamplerInput::rig; with
//   rig == nullptr they are skipped.
// - lifeRotation = Rx * Ry * Rz (bone-local axes, post-multiplied in that order); hash01 packs
//   (seed << 32) | (bone << 16) | (axis << 8) | k into splitmix64 and takes the top 24 bits.
// - A one-pose animation is static: every sample is the key (no life). An empty one leaves
//   `out` untouched and has no frames.
// - Looping frames always end with a copy of the first frame. In a loop, a time less than 1e-6 s
//   below a multiple of the length samples that boundary: the two-cycle sum of 4.8 step 4 is
//   only seamless up to the springs' residual after one cycle (~1e-5 rad), and float times such
//   as float(length) land just below it.
// - In-betweens on a loop: the warm-up pass ends at frame `count`, the same instant as frame 0,
//   so the write pass starts from that state at frame 0.
// - autoTiming: the mean bone angle is over all 22 slots; a non-looping animation's first pose
//   (no incoming motion) gets the base hold and its last pose (no outgoing segment) the
//   animation's duration; holds clamp to 0..5 s; the step-grid snap is per value (the total
//   length may move by up to half a step per value).

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
        const PoseRig* rig = nullptr;           // optional: follow-through, joint limits, unmapped slots
    };

    JASPERENGINE_API SamplerInput makeSamplerInput(const PoseAnimationData& data, std::vector<HumanPose> keys, const PoseRig* rig = nullptr);

    // Realises every key: joint limits first when humanize.jointLimits, then
    // fromBodyPose(body, skin, grounded, lift)
    JASPERENGINE_API std::vector<HumanPose> realiseKeys(const PoseAnimationData& data, const PoseRig& rig, const PoseSkin* skin = nullptr);

    JASPERENGINE_API void sample(const SamplerInput& input, float t, HumanPose& out);

    // count = max(1, round(length * fps)) + 1 frames, frame f at t = f * length / count
    JASPERENGINE_API std::vector<HumanPose> frames(const SamplerInput& input, int fps);

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
