// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The value ranges of the pose animation data (spec: pose animation phase 1, section 4.2):
// one table, applied everywhere a value enters (file load, UI, MCP tools). The clamp functions
// change the data in place and append one ClampReport per field that changed, so callers can
// echo it (MCP) or turn it into load warnings.
//
// Field paths in reports use the C++ member names:
//   settings.transition.overshoot, settings.stepRate, settings.weights.spine,
//   settings.humanize.movingHolds, settings.frameRate, poses[2].hold, poses[2].lift,
//   poses[2].offset.x, poses[2].transition.duration, poses[2].angles.left_upper_arm.raise.
// A non-finite value (NaN) is replaced by the lower bound of its range (angles: 0).
// Joint limits (the 4.5 table) are not clamped here: angles only wrap to (-180, 180].

#ifndef POSE_RANGES_H
#define POSE_RANGES_H

#include "Export.h"
#include "pose/PoseTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::pose {

    struct ClampReport {
        std::string field;
        float requested;
        float applied;
    };

    // The ranges of the 4.2 table
    namespace ranges {
        constexpr float HOLD_MIN = 0.0f,             HOLD_MAX = 5.0f;
        constexpr float DURATION_MIN = 0.02f,        DURATION_MAX = 1.0f;
        constexpr float OVERSHOOT_MIN = 0.0f,        OVERSHOOT_MAX = 0.9f;
        constexpr float ANTICIPATION_MIN = 0.0f,     ANTICIPATION_MAX = 0.5f;
        constexpr float LIFT_MIN = 0.0f,             LIFT_MAX = 3.0f;
        constexpr float OFFSET_MIN = -5.0f,          OFFSET_MAX = 5.0f;
        constexpr float STEP_RATE_MIN = 0.0f,        STEP_RATE_MAX = 30.0f;
        constexpr float OVERLAP_MIN = 0.0f,          OVERLAP_MAX = 0.3f;
        constexpr float LOOSENESS_MIN = 0.0f,        LOOSENESS_MAX = 1.0f;
        constexpr float WEIGHT_MIN = 0.0f,           WEIGHT_MAX = 2.0f;
        constexpr float MOVING_HOLDS_MIN = 0.0f,     MOVING_HOLDS_MAX = 0.5f;
        constexpr float INBETWEENS_MIN = 0.0f,       INBETWEENS_MAX = 1.0f;
        constexpr float LIFE_MIN = 0.0f,             LIFE_MAX = 5.0f;     // degrees
        constexpr uint32_t SEED_MAX = 2147483647u;                        // 2^31 - 1
        constexpr float FOLLOW_MIN = 0.0f,           FOLLOW_MAX = 1.5f;   // head / torso follow
        constexpr float SPRINGINESS_MIN = 0.0f,      SPRINGINESS_MAX = 1.0f;
        constexpr int FRAME_RATE_MIN = 12,           FRAME_RATE_MAX = 120;
    }

    // Wraps an angle in degrees to (-180, 180]; a non-finite angle becomes 0
    JASPERENGINE_API float wrapAngle(float degrees);

    // `path` is the prefix of the reported fields: "settings.transition", "poses[2].transition"
    JASPERENGINE_API void clampTransition(Transition& transition, const std::string& path,
                                          std::vector<ClampReport>& reports);
    // Clamps the settings, including their default transition and the humanize block
    JASPERENGINE_API void clampSettings(PoseAnimationSettings& settings,
                                        std::vector<ClampReport>& reports);
    // Clamps one key (hold, lift, offset, angles, custom transition); `index` only names the fields
    JASPERENGINE_API void clampKey(PoseKey& key, int index, std::vector<ClampReport>& reports);
    // Settings, then every key
    JASPERENGINE_API void clampData(PoseAnimationData& data, std::vector<ClampReport>& reports);

    // "spring" | "linear" | "smooth"
    JASPERENGINE_API const char* curveName(TransitionCurve curve);
    JASPERENGINE_API bool curveFromName(const std::string& name, TransitionCurve& curve);

    // "poses[2].hold: 9 -> 5" style text of a report (load warnings, tool messages)
    JASPERENGINE_API std::string describeClamp(const ClampReport& report);

}

#endif // POSE_RANGES_H
