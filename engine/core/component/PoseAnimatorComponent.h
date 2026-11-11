// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef POSE_ANIMATOR_COMPONENT_H
#define POSE_ANIMATOR_COMPONENT_H

#include "Export.h"
#include "asset/AssetRef.h"
#include "util/FunctionSubscribe.h"

#include <memory>
#include <string>

namespace jasperengine{

    namespace pose{
        class PoseAnimatorRuntime;
    }

    // Plays a Pose Animation (.poseanim) live on the humanoid Model of its entity (spec: pose
    // animation phase 2, section 4.1). PoseAnimatorSystem drives it in play mode and in games;
    // in the editor's edit mode it does nothing (the Pose Animation window previews itself).
    struct JASPERENGINE_API PoseAnimatorComponent{
        // the .poseanim, relative to the project's assets folder like ModelComponent::filename
        // (the root an exported game reads its assets from); its GUID is the editor's (S04)
        AssetRef animation;
        bool autoplay = true;
        float speed = 1.0f;
        float weight = 1.0f;     // 0..1 blend over whatever the ActionSystem wrote this frame
        // S48 Pose Frame Rate (fps, 0 = smooth): the pose holds on whole steps of 1 / fps from
        // each key, so the animation reads as held drawings
        float poseFrameRate = 0.0f;

        // runtime only (not serialised)
        bool playing = false;
        float time = 0;
        bool needsLoad = true;   // set again by a new animation path: the next update loads it
        // the initial playing state was decided: autoplay applied, or a script played,
        // paused or stopped before the first update
        bool started = false;
        std::shared_ptr<pose::PoseAnimatorRuntime> runtime;

        // a one-shot animation reached its end
        FunctionSubscribe<void()> onFinish;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onFinish); }
    };

}

#endif //POSE_ANIMATOR_COMPONENT_H
