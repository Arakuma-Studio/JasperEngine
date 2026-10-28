// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseRealisation.h"

namespace jasperengine::pose {

    std::shared_ptr<PoseRealisation> realisePoseAnimation(const PoseAnimationData& data, const std::shared_ptr<const PoseRig>& rig){
        if (!rig || !rig->valid()){
            return nullptr;
        }
        auto out = std::make_shared<PoseRealisation>();
        out->rig = rig;
        out->keys = realiseKeys(data, *rig, rig->skin());
        for (size_t i = 0; i < data.poses.size() && i < out->keys.size(); i++){
            out->metrics.push_back(computeMetrics(*rig, out->keys[i], data.poses[i].body));
        }
        const SamplerInput input = makeSamplerInput(data, out->keys, rig.get());
        out->timeline = input.timeline;
        out->frames = frames(input, data.settings.frameRate);
        return out;
    }

}
