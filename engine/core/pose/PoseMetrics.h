// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Numeric checks of a realised pose (spec: pose animation phase 2, section 3.2): foot heights
// and contacts, the lowest point, the centre of mass and balance over the feet in contact, and
// the DOFs outside the joint limits. The MCP tools report them per key pose, so an agent can
// check contact and balance without looking at images.
//
// Every position is in the canonical character frame (x = the character's left, y = up,
// z = forward), in humanScale units, with y measured from the rig's floor.
//
// Decisions the spec leaves open:
// - Foot height: the skin vertices weighted more than 0.5 to the foot and toes bones when the
//   rig has a skin with such vertices, else the sole estimate of the foot and toes bones.
//   FootMetrics::position is the foot (ankle) bone, with y = the foot's height.
// - Centre of mass segments (spec 3.2 weights, normalised by their sum): head + neck from the
//   neck base (neck, else head bone) to an estimated head top (0.2 humanScale along the head),
//   the trunk split evenly over the segments of the chain hips .. neck base, upper arm (upper
//   arm -> lower arm), forearm (lower arm -> hand), hand (0.1 humanScale along it), thigh,
//   shank, foot (foot -> toes, else 0.15 humanScale along the foot).
// - Support: each foot in contact gives its heel (the foot bone) and toe (the toes bone, else the
//   foot tip estimated 0.15 humanScale along the foot) points, widened by 0.045 humanScale to
//   each side so one foot alone has an area; the support is their convex hull on the floor.
// - limitHits lists only DOFs of slots the rig has.

#ifndef POSE_METRICS_H
#define POSE_METRICS_H

#include "Export.h"
#include "math/Vector3.h"
#include "pose/PoseRig.h"
#include "pose/PoseTypes.h"

#include <vector>

namespace jasperengine::pose {

    constexpr float FOOT_CONTACT_HEIGHT = 0.02f;   // humanScale units

    struct FootMetrics {
        float height = 0.0f;        // lowest sole point above the floor
        bool contact = false;       // height <= FOOT_CONTACT_HEIGHT
        Vector3 position;           // the foot bone (x, z), y = height
    };

    struct PoseMetrics {
        FootMetrics left, right;
        float lowest = 0.0f;        // lowest point of the body above the floor
        Vector3 centerOfMass;
        bool balanced = false;      // the centre of mass is above the support of the feet in contact
        float balanceMargin = 0.0f; // signed distance to the support's edge (negative outside)
        std::vector<int> limitHits; // DOF indices outside the joint limits (before any clamping)
    };

    // `pose` realised from `body` on `rig` (body gives limitHits; it is not clamped here)
    JASPERENGINE_API PoseMetrics computeMetrics(const PoseRig& rig, const HumanPose& pose, const BodyPose& body);

}

#endif // POSE_METRICS_H
