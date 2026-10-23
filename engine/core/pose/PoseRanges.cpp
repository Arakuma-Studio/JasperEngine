// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseRanges.h"

#include "pose/PoseDofs.h"

#include <cmath>
#include <cstdio>

namespace jasperengine::pose {

    namespace {

        // Clamps `value` into [lo, hi] and reports the change; NaN becomes `lo`
        void clampValue(float& value, float lo, float hi, const std::string& field,
                        std::vector<ClampReport>& reports){
            float applied = value;
            if (std::isnan(applied) || applied < lo){
                applied = lo;
            }else if (applied > hi){
                applied = hi;
            }
            if (std::isnan(value) || applied != value){
                reports.push_back({field, value, applied});
                value = applied;
            }
        }

    }

    float wrapAngle(float degrees){
        if (!std::isfinite(degrees)){
            return 0.0f;
        }
        float wrapped = std::fmod(degrees, 360.0f);
        if (wrapped <= -180.0f){
            wrapped += 360.0f;
        }else if (wrapped > 180.0f){
            wrapped -= 360.0f;
        }
        return wrapped;
    }

    void clampTransition(Transition& transition, const std::string& path,
                         std::vector<ClampReport>& reports){
        clampValue(transition.duration, ranges::DURATION_MIN, ranges::DURATION_MAX,
                   path + ".duration", reports);
        clampValue(transition.overshoot, ranges::OVERSHOOT_MIN, ranges::OVERSHOOT_MAX,
                   path + ".overshoot", reports);
        clampValue(transition.anticipation, ranges::ANTICIPATION_MIN, ranges::ANTICIPATION_MAX,
                   path + ".anticipation", reports);
    }

    void clampSettings(PoseAnimationSettings& settings, std::vector<ClampReport>& reports){
        clampTransition(settings.transition, "feel.transition", reports);
        clampValue(settings.stepRate, ranges::STEP_RATE_MIN, ranges::STEP_RATE_MAX,
                   "feel.step_rate", reports);
        clampValue(settings.overlap, ranges::OVERLAP_MIN, ranges::OVERLAP_MAX,
                   "feel.overlap", reports);
        clampValue(settings.looseness, ranges::LOOSENESS_MIN, ranges::LOOSENESS_MAX,
                   "feel.looseness", reports);
        for (int group = 0; group < static_cast<int>(BodyGroup::Count); group++){
            clampValue(settings.weights[group], ranges::WEIGHT_MIN, ranges::WEIGHT_MAX,
                       std::string("feel.weights.") + bodyGroupName(static_cast<BodyGroup>(group)),
                       reports);
        }

        HumanizeSettings& humanize = settings.humanize;
        clampValue(humanize.movingHolds, ranges::MOVING_HOLDS_MIN, ranges::MOVING_HOLDS_MAX,
                   "humanize.moving_holds", reports);
        clampValue(humanize.inbetweens, ranges::INBETWEENS_MIN, ranges::INBETWEENS_MAX,
                   "humanize.inbetweens", reports);
        clampValue(humanize.life, ranges::LIFE_MIN, ranges::LIFE_MAX,
                   "humanize.life", reports);
        if (humanize.seed > ranges::SEED_MAX){
            reports.push_back({"humanize.seed", static_cast<float>(humanize.seed),
                               static_cast<float>(ranges::SEED_MAX)});
            humanize.seed = ranges::SEED_MAX;
        }
        clampValue(humanize.headFollow, ranges::FOLLOW_MIN, ranges::FOLLOW_MAX,
                   "humanize.head_follow", reports);
        clampValue(humanize.torsoFollow, ranges::FOLLOW_MIN, ranges::FOLLOW_MAX,
                   "humanize.torso_follow", reports);
        clampValue(humanize.springiness, ranges::SPRINGINESS_MIN, ranges::SPRINGINESS_MAX,
                   "humanize.springiness", reports);

        if (settings.frameRate < ranges::FRAME_RATE_MIN || settings.frameRate > ranges::FRAME_RATE_MAX){
            const int applied = settings.frameRate < ranges::FRAME_RATE_MIN ? ranges::FRAME_RATE_MIN
                                                                            : ranges::FRAME_RATE_MAX;
            reports.push_back({"frame_rate", static_cast<float>(settings.frameRate),
                               static_cast<float>(applied)});
            settings.frameRate = applied;
        }
    }

    void clampKey(PoseKey& key, int index, std::vector<ClampReport>& reports){
        const std::string path = "poses[" + std::to_string(index) + "]";

        clampValue(key.hold, ranges::HOLD_MIN, ranges::HOLD_MAX, path + ".hold", reports);
        if (key.customTransition){
            clampTransition(key.transition, path + ".transition", reports);
        }
        clampValue(key.lift, ranges::LIFT_MIN, ranges::LIFT_MAX, path + ".lift", reports);

        Vector3& offset = key.body.offset;
        clampValue(offset.x, ranges::OFFSET_MIN, ranges::OFFSET_MAX, path + ".offset.left", reports);
        clampValue(offset.z, ranges::OFFSET_MIN, ranges::OFFSET_MAX, path + ".offset.forward", reports);
        if (offset.y != 0.0f){   // the vertical position comes from grounding and lift
            reports.push_back({path + ".offset.y", offset.y, 0.0f});
            offset.y = 0.0f;
        }

        for (int dof = 0; dof < DOF_COUNT; dof++){
            float& angle = key.body.angles[dof];
            const float wrapped = wrapAngle(angle);
            if (std::isnan(angle) || wrapped != angle){
                reports.push_back({path + ".angles." + dofName(dof), angle, wrapped});
                angle = wrapped;
            }
        }
    }

    void clampData(PoseAnimationData& data, std::vector<ClampReport>& reports){
        clampSettings(data.settings, reports);
        for (size_t i = 0; i < data.poses.size(); i++){
            clampKey(data.poses[i], static_cast<int>(i), reports);
        }
    }

    const char* curveName(TransitionCurve curve){
        switch (curve){
            case TransitionCurve::Linear: return "linear";
            case TransitionCurve::Smooth: return "smooth";
            case TransitionCurve::Spring: break;
        }
        return "spring";
    }

    bool curveFromName(const std::string& name, TransitionCurve& curve){
        if (name == "spring"){
            curve = TransitionCurve::Spring;
        }else if (name == "linear"){
            curve = TransitionCurve::Linear;
        }else if (name == "smooth"){
            curve = TransitionCurve::Smooth;
        }else{
            return false;
        }
        return true;
    }

    std::string describeClamp(const ClampReport& report){
        auto number = [](float value){
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(value));
            return std::string(buffer);
        };
        return report.field + ": " + number(report.requested) + " -> " + number(report.applied);
    }

}
