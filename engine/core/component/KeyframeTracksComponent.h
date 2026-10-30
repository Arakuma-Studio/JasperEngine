// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef KEYFRAMETRACKS_COMPONENT_H
#define KEYFRAMETRACKS_COMPONENT_H

#include "Engine.h"
#include "action/Ease.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"

namespace jasperengine{

    struct JASPERENGINE_API KeyframeTracksComponent{
        std::vector<float> times;
        // Per-segment easing: easings[i] shapes the interpolation from key i to
        // key i+1. Empty or shorter than the segment count means linear there.
        // GLTF LINEAR clips leave this empty; GLTF STEP clips fill it with
        // EaseType::STEP. On tracks with Hermite tangents (GLTF CUBICSPLINE)
        // the eased factor feeds the cubic curve, so leave this empty for
        // spec-faithful playback.
        std::vector<EaseType> easings;
        bool loop = false; // starts over after the last key
        // Transform tracks only: values add to the target's pose when the track starts,
        // position plus value, rotation times value and scale times value
        bool relative = false;
        int index = 0;
        float interpolation = 0;

        // Runtime (not serialized): the pose relative values add to, taken when the track starts
        bool hasBase = false;
        Vector3 basePosition;
        Quaternion baseRotation;
        Vector3 baseScale = Vector3(1, 1, 1);

        void setTimes(const std::vector<float>& values){
            times = values;
            // Discard easings for segments that no longer exist.
            size_t segments = times.size() > 1 ? times.size() - 1 : 0;
            if (easings.size() > segments){
                easings.resize(segments);
            }
        }

        void setEasings(const std::vector<EaseType>& values){
            easings = values;
            // CUSTOM has no per-segment function storage; treat it as linear.
            for (EaseType& ease : easings){
                if (ease == EaseType::CUSTOM || ease == EaseType::CURVE) ease = EaseType::LINEAR;
            }
        }

        void setEasing(unsigned int segment, EaseType ease){
            if (easings.size() <= segment){
                easings.resize(segment + 1, EaseType::LINEAR);
            }
            easings[segment] = ease == EaseType::CUSTOM || ease == EaseType::CURVE ? EaseType::LINEAR : ease;
        }
    };

}

#endif //KEYFRAMETRACKS_COMPONENT_H
