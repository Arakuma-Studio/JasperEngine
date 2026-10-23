// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Head and torso follow-through from the hips acceleration (spec: pose animation phase 1,
// section 4.9). Applied to baked frames only, never to the stored key poses.
//
// Decisions the spec leaves open:
// - Looping frames are periodic with period count = frames.size() - 1 (the last frame repeats
//   the first): the differences wrap over that period.
// - The spring state recorded for frame f is the state after that frame's four substeps.
// - A missing chain bone's share goes to the next present bone of its chain; a share left at the
//   end of the chain goes to its last present bone.

#ifndef POSE_SECONDARY_H
#define POSE_SECONDARY_H

#include "Export.h"
#include "math/Vector3.h"
#include "pose/PoseRig.h"
#include "pose/PoseTypes.h"

#include <vector>

namespace jasperengine::pose {

    // degrees; lean + = forward (about canonical L), side + = toward the character's left
    struct FollowTilt {
        float torsoLean = 0, torsoSide = 0;
        float headLean = 0, headSide = 0;
    };

    // The tilts per frame from hips positions in the canonical frame, in humanScale units
    JASPERENGINE_API std::vector<FollowTilt> followThroughTilts(const std::vector<Vector3>& hipsCanonical, int fps, bool loop,
                                                                const HumanizeSettings& humanize);

    // Applies the tilts to the spine chain (Spine 0.3, Chest 0.35, UpperChest 0.35) and the head
    // chain (Neck 0.4, Head 0.6) as canonical world rotations, parent before child
    JASPERENGINE_API void applyFollowThrough(std::vector<HumanPose>& frames, const PoseRig& rig, int fps, bool loop,
                                             const HumanizeSettings& humanize);

}

#endif // POSE_SECONDARY_H
