// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A pose animation realised on one rig (spec: pose animation phase 2, section 5): the key poses
// as rig poses, their metrics and the frames. What the editor's previews, metrics, baking and
// auto timing use (editor/pose/PoseAnimationDocument caches it per revision and rig).

#ifndef POSE_REALISATION_H
#define POSE_REALISATION_H

#include "Export.h"
#include "pose/PoseMetrics.h"
#include "pose/PoseRig.h"
#include "pose/PoseSampler.h"
#include "pose/PoseTypes.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine::pose {

    struct PoseRealisation {
        std::shared_ptr<const PoseRig> rig;
        uint64_t revision = 0;                  // set by the caller (the document revision)
        std::vector<HumanPose> keys;            // per key pose: realiseKeys (joint limits first when humanize.jointLimits)
        std::vector<PoseMetrics> metrics;       // per key pose; limit hits from the key's own (unclamped) angles
        std::vector<HumanPose> frames;          // pose::frames at frame_rate (f = 0..count)
        PoseTimeline timeline;
    };

    // nullptr when the rig is missing or invalid
    JASPERENGINE_API std::shared_ptr<PoseRealisation> realisePoseAnimation(const PoseAnimationData& data,
                                                                           const std::shared_ptr<const PoseRig>& rig);

}

#endif // POSE_REALISATION_H
