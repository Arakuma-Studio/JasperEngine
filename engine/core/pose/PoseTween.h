// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The transition ease and the feel presets (spec: pose animation phase 1, section 4.8).

#ifndef POSE_TWEEN_H
#define POSE_TWEEN_H

#include "Export.h"
#include "pose/PoseTypes.h"

#include <string>
#include <vector>

namespace jasperengine::pose {

    // Progress of a transition at x = (t - start) / duration: 0 before the start, 1 at x = 1.
    // Spring: an underdamped step response with a wind-up dip to -anticipation and a first peak of
    // 1 + overshoot * (1 + anticipation); Linear and Smooth ignore overshoot and anticipation.
    JASPERENGINE_API float ease(float x, const Transition& transition);
    JASPERENGINE_API double easePrecise(double x, const Transition& transition);

    // Looseness l (0..1) applied to a transition: duration *= 1 + 1.5 l, overshoot -> lerp(overshoot, 0.7, l)
    JASPERENGINE_API Transition loosen(const Transition& transition, float looseness);

    struct FeelPreset {
        const char* name;          // display name: "Snappy", "Cartoon", ..., "Stop-motion"
        Transition transition;
        float stepRate;
        float overlap;
        float looseness;
    };

    // the preset table (4.8), in display order
    JASPERENGINE_API const std::vector<FeelPreset>& feelPresets();
    // by name, case-insensitive, '-', '_' and ' ' equivalent ("stop_motion" finds "Stop-motion");
    // nullptr when unknown
    JASPERENGINE_API const FeelPreset* findFeelPreset(const std::string& name);
    // every preset value equal within 1e-4 (the curve exactly)
    JASPERENGINE_API bool feelPresetMatches(const FeelPreset& preset, const PoseAnimationSettings& settings);
    // the first matching preset, nullptr when none
    JASPERENGINE_API const FeelPreset* matchingFeelPreset(const PoseAnimationSettings& settings);
    // writes the preset's transition, stepRate, overlap and looseness
    JASPERENGINE_API void applyFeelPreset(const FeelPreset& preset, PoseAnimationSettings& settings);

}

#endif // POSE_TWEEN_H
