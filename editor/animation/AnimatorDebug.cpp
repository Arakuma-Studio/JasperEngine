// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorDebug.h"

#include "Scene.h"
#include "animation/AnimatorInstance.h"
#include "component/AnimatorComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace jasperengine::anim{

namespace{

    constexpr float kDebugRadToDeg = 57.29577951308232f;

    bool debugIsLeg(AnimatorIKType type){
        return type == AnimatorIKType::FootIK || type == AnimatorIKType::FootLock;
    }

    bool debugValidBone(int bone, size_t count){
        return bone >= 0 && (size_t)bone < count;
    }

    nlohmann::json debugVector(const Vector3& v){
        auto r = [](float x){ return std::round((double)x * 10000.0) / 10000.0 + 0.0; };
        return nlohmann::json::array({r(v.x), r(v.y), r(v.z)});
    }

    void overlayAddLine(std::vector<AnimatorOverlayLine>& out, const Vector3& a, const Vector3& b, const Vector4& color){
        out.push_back({a, b, color});
    }

    void overlayAddCross(std::vector<AnimatorOverlayLine>& out, const Vector3& at, float size, const Vector4& color){
        overlayAddLine(out, at - Vector3(size, 0, 0), at + Vector3(size, 0, 0), color);
        overlayAddLine(out, at - Vector3(0, size, 0), at + Vector3(0, size, 0), color);
        overlayAddLine(out, at - Vector3(0, 0, size), at + Vector3(0, 0, size), color);
    }

    void overlayAddDiamond(std::vector<AnimatorOverlayLine>& out, const Vector3& at, float size, const Vector4& color){
        const Vector3 points[6] = {at + Vector3(size, 0, 0), at + Vector3(0, 0, size), at - Vector3(size, 0, 0), at - Vector3(0, 0, size),
                                   at + Vector3(0, size, 0), at - Vector3(0, size, 0)};
        for (int i = 0; i < 4; i++){
            overlayAddLine(out, points[i], points[(i + 1) % 4], color);
            overlayAddLine(out, points[i], points[4], color);
            overlayAddLine(out, points[i], points[5], color);
        }
    }

    void overlayAddDashed(std::vector<AnimatorOverlayLine>& out, const Vector3& a, const Vector3& b, const Vector4& color){
        constexpr int kDashes = 8;
        for (int i = 0; i < kDashes; i++){
            const float t0 = (float)(2 * i) / (float)(2 * kDashes - 1);
            const float t1 = (float)(2 * i + 1) / (float)(2 * kDashes - 1);
            overlayAddLine(out, a + (b - a) * t0, a + (b - a) * t1, color);
        }
    }

    Vector4 overlayFaded(const Vector4& color, float alpha){
        return Vector4(color.x, color.y, color.z, color.w * alpha);
    }

    void overlayAddPath(std::vector<AnimatorOverlayLine>& out, const std::vector<AnimatorOverlayHistory::Sample>& samples, const Vector4* color){
        for (size_t i = 1; i < samples.size(); i++){
            overlayAddLine(out, samples[i - 1].position, samples[i].position, color ? *color : samples[i].color);
        }
    }

    void overlayDropOld(std::vector<AnimatorOverlayHistory::Sample>& samples, float now){
        size_t keep = 0;
        while (keep < samples.size() && samples[keep].time < now - kAnimatorTrailSeconds) keep++;
        if (keep > 0) samples.erase(samples.begin(), samples.begin() + (std::ptrdiff_t)keep);
    }

}

AnimatorDebugSnapshot makeSnapshot(const AnimatorInstance& instance, const AnimatorComponent& component, const AnimatorRoot& root){
    AnimatorDebugSnapshot snapshot;
    if (!instance.bound || !instance.controller || instance.evaluations == 0) return snapshot;
    snapshot.valid = true;
    snapshot.rootPosition = root.position;
    snapshot.rootRotation = root.rotation;
    const Matrix4& world = root.matrix;
    const std::vector<Vector3>& positions = instance.rigPositions();
    const Vector3 up = (root.rotation * Vector3(0, 1, 0)).normalized();
    const float upScale = std::fabs(root.scale.y);

    // Foot IK's pelvis drop: the lowest weighted foot of the stack, when it has a pelvis bone
    // (AnimatorInstance::runIK); rig metres along the rig's up
    float pelvisOffset = 0.0f;
    bool pelvisFound = false;
    for (size_t i = 0; i < component.ik.size() && i < instance.ik.size(); i++){
        const AnimatorIKRun& run = instance.ik[i];
        if (component.ik[i].type != AnimatorIKType::FootIK || run.boneCount < 3 || run.bones[2] < 0) continue;
        pelvisOffset = std::min(pelvisOffset, run.offset * run.solvedWeight);
        if (run.pelvis >= 0) pelvisFound = true;
    }
    if (!pelvisFound) pelvisOffset = 0.0f;

    // the root's velocity from this update's root motion, as AnimatorSystem applies it
    const AnimatorOutput& output = instance.output;
    Vector3 rootVelocity(0, 0, 0);
    if (output.hasRootMotion){
        AnimatorDebugRootMotion& rm = snapshot.rootMotion;
        rm.active = true;
        const Vector3 worldDelta = root.rotation * (root.scale * output.rootDeltaPosition);
        if (output.deltaTime > 0.0f){
            rootVelocity = worldDelta * (1.0f / output.deltaTime);
            rm.yawDegS = output.rootDeltaYaw / output.deltaTime * kDebugRadToDeg;
        }
        rm.velocity = rootVelocity;
        rm.speedMps = rootVelocity.length();
        rm.accumulatedPosition = instance.rootMotionTotal;
        rm.accumulatedYawDeg = instance.rootMotionTotalYaw * kDebugRadToDeg;
    }

    float slideFoot = -1.0f;
    snapshot.ik.reserve(component.ik.size());
    for (size_t i = 0; i < component.ik.size() && i < instance.ik.size(); i++){
        const AnimatorIKConstraint& c = component.ik[i];
        const AnimatorIKRun& run = instance.ik[i];
        AnimatorDebugIK ik;
        ik.name = c.name;
        ik.type = c.type;
        ik.weight = run.solvedWeight;
        ik.reached = run.reached;
        ik.reachErrorMm = run.reachError * 1000.0f;
        for (int k = 0; k < run.boneCount && k < 16; k++){
            if (debugValidBone(run.bones[k], positions.size())) ik.chain.push_back(world * positions[(size_t)run.bones[k]]);
        }
        if (run.solvedWeight > 0.0f){
            ik.hasTarget = true;
            ik.target = run.solvedTargetWorld;
        }else if (!debugIsLeg(c.type) && (run.scriptTarget || run.hasTarget)){
            ik.hasTarget = true;
            ik.target = run.scriptTarget ? run.scriptTargetWorld : run.targetWorld;
        }
        if (c.type == AnimatorIKType::TwoBone && run.hasPole){
            ik.hasPole = true;
            ik.pole = run.poleWorld;
        }
        const bool leg = debugIsLeg(c.type) && run.boneCount >= 3 && debugValidBone(run.bones[2], positions.size());
        if (leg){
            ik.leg = true;
            ik.foot = world * positions[(size_t)run.bones[2]];
            ik.contact = run.contact;
            ik.footVelocity = run.footVelocity;
            ik.slideMmS = run.contact ? run.footVelocity.length() * 1000.0f : 0.0f;
            snapshot.contactSource = "foot_goals";
            if (run.contact && ik.slideMmS > slideFoot){
                slideFoot = ik.slideMmS;
                snapshot.slideMmS = ik.slideMmS;
                if (snapshot.rootMotion.active) snapshot.footSpeedMps = (run.footVelocity - rootVelocity).length();
            }
        }
        if (c.type == AnimatorIKType::FootIK){
            if (run.hasFoot){
                ik.hasRay = true;
                ik.rayFrom = run.footWorld + up * (c.rayStart * upScale);
                ik.rayTo = run.footWorld - up * (c.rayLength * upScale);
            }
            ik.grounded = run.hit;
            if (run.hit){
                ik.hitPoint = world * run.hitPoint;
                ik.hitNormal = (root.rotation * run.hitNormal).normalized();
            }
            ik.pelvisOffsetMm = pelvisOffset * upScale * 1000.0f;
        }
        if (c.type == AnimatorIKType::FootLock){
            ik.locked = run.locked;
            ik.lockPoint = run.lockWorld;
            ik.animatedFoot = run.previousWorld;
            if (run.locked) ik.lockDriftMm = (run.previousWorld - run.lockWorld).length() * 1000.0f;
        }
        snapshot.ik.push_back(std::move(ik));
    }
    snapshot.sliding = snapshot.slideMmS > kAnimatorSlideWarningMmS;
    return snapshot;
}

const AnimatorDebugField* findAnimatorDebugField(std::string_view key){
    for (const AnimatorDebugField& field : kAnimatorDebugFields){
        if (key == field.key) return &field;
    }
    return nullptr;
}

double animatorDebugValue(double value){
    if (!std::isfinite(value)) return 0.0;
    // + 0.0: a tiny negative rounds to 0, not -0
    return std::round(value * 100.0) / 100.0 + 0.0;
}

std::string formatAnimatorDebugField(std::string_view key, double value){
    const AnimatorDebugField* field = findAnimatorDebugField(key);
    const double v = animatorDebugValue(value);
    char text[64] = {0};
    switch (field ? field->format : AnimatorDebugFormat::Ratio){
        case AnimatorDebugFormat::Ratio: std::snprintf(text, sizeof(text), "%.2f", v); break;
        case AnimatorDebugFormat::Millimetres: std::snprintf(text, sizeof(text), "%.1f mm", v); break;
        case AnimatorDebugFormat::MetresPerSecond: std::snprintf(text, sizeof(text), "%.2f m/s", v); break;
        case AnimatorDebugFormat::DegreesPerSecond: std::snprintf(text, sizeof(text), "%.1f deg/s", v); break;
        case AnimatorDebugFormat::Degrees: std::snprintf(text, sizeof(text), "%.1f deg", v); break;
        case AnimatorDebugFormat::MillimetresPerSecond: std::snprintf(text, sizeof(text), "%.0f mm/s", v); break;
    }
    return text;
}

const char* animatorIKTypeLabel(AnimatorIKType type){
    switch (type){
        case AnimatorIKType::TwoBone: return "Two Bone";
        case AnimatorIKType::Fabrik: return "FABRIK";
        case AnimatorIKType::LookAt: return "Look At";
        case AnimatorIKType::FootIK: return "Foot IK";
        case AnimatorIKType::FootLock: return "Foot Lock";
    }
    return "";
}

std::string animatorIKStateLabel(const AnimatorDebugIK& ik){
    switch (ik.type){
        case AnimatorIKType::FootIK:
            return ik.grounded ? "Grounded" : "No Ground";
        case AnimatorIKType::FootLock:
            return ik.locked ? "Locked" : "Free";
        case AnimatorIKType::LookAt:
            if (!(ik.weight > 0.0f)) return "Off";
            return ik.reached ? "On Target" : "Clamped";
        case AnimatorIKType::TwoBone:
        case AnimatorIKType::Fabrik:
            if (!(ik.weight > 0.0f)) return "Off";
            return ik.reached ? "Reached" : "Out of Reach";
    }
    return "";
}

void addAnimatorDebugIKJson(const AnimatorDebugIK& ik, nlohmann::json& item){
    item["weight"] = animatorDebugValue(ik.weight);
    item["reached"] = ik.reached;
    item["reach_error_mm"] = animatorDebugValue(ik.reachErrorMm);
    if (ik.type == AnimatorIKType::FootIK){
        if (ik.grounded){
            item["hit_point"] = debugVector(ik.hitPoint);
            item["hit_normal"] = debugVector(ik.hitNormal);
        }
        item["pelvis_offset_mm"] = animatorDebugValue(ik.pelvisOffsetMm);
    }
    if (ik.type == AnimatorIKType::FootLock && ik.locked) item["lock_drift_mm"] = animatorDebugValue(ik.lockDriftMm);
}

void addAnimatorDebugRootMotionJson(const AnimatorDebugSnapshot& snapshot, nlohmann::json& rootMotion){
    const AnimatorDebugRootMotion& rm = snapshot.rootMotion;
    rootMotion["speed_mps"] = animatorDebugValue(rm.speedMps);
    rootMotion["yaw_deg_s"] = animatorDebugValue(rm.yawDegS);
    rootMotion["accumulated_position"] = debugVector(rm.accumulatedPosition);
    rootMotion["accumulated_yaw"] = animatorDebugValue(rm.accumulatedYawDeg);
    rootMotion["contact_source"] = snapshot.contactSource;
    if (std::string_view(snapshot.contactSource) != "none"){
        rootMotion["slide_mm_s"] = animatorDebugValue(snapshot.slideMmS);
        rootMotion["foot_speed_mps"] = animatorDebugValue(snapshot.footSpeedMps);
    }
}

Vector4 footTrailColor(bool contact, float slideMmS){
    if (!contact) return Vector4(1.0f, 1.0f, 1.0f, 0.35f);
    if (slideMmS <= 0.5f * kAnimatorSlideWarningMmS) return overlaycolor::planted;
    // yellow at half the warning speed to red at five times it
    const float t = std::clamp((slideMmS - 0.5f * kAnimatorSlideWarningMmS) / (4.5f * kAnimatorSlideWarningMmS), 0.0f, 1.0f);
    return Vector4(1.0f, 0.9f * (1.0f - t) + 0.2f * t, 0.2f * (1.0f - t) + 0.15f * t, 1.0f);
}

void appendAnimatorOverlayLines(const AnimatorDebugSnapshot& snapshot, const AnimatorOverlayFlags& flags,
                                      const AnimatorOverlayHistory* history, std::vector<AnimatorOverlayLine>& out){
    if (!snapshot.valid) return;
    if (flags.ik){
        for (const AnimatorDebugIK& ik : snapshot.ik){
            const bool solved = ik.weight > 0.0f;
            for (size_t k = 1; k < ik.chain.size(); k++) overlayAddLine(out, ik.chain[k - 1], ik.chain[k], overlaycolor::chain);
            if (ik.hasTarget && (solved || !ik.leg)){
                const Vector4& color = ik.reached ? overlaycolor::reached : overlaycolor::unreached;
                overlayAddCross(out, ik.target, 0.04f, color);
                if (!ik.chain.empty()) overlayAddLine(out, ik.chain.back(), ik.target, overlayFaded(color, 0.6f));
            }
            if (ik.hasPole){
                overlayAddDiamond(out, ik.pole, 0.03f, overlaycolor::pole);
                if (ik.chain.size() >= 2) overlayAddDashed(out, ik.pole, ik.chain[1], overlaycolor::pole);
            }
            if (ik.hasRay){
                if (ik.grounded){
                    overlayAddLine(out, ik.rayFrom, ik.hitPoint, overlaycolor::ray);
                    overlayAddCross(out, ik.hitPoint, 0.025f, overlaycolor::hit);
                    overlayAddLine(out, ik.hitPoint, ik.hitPoint + ik.hitNormal * 0.15f, overlaycolor::hit);
                }else{
                    overlayAddLine(out, ik.rayFrom, ik.rayTo, overlaycolor::ray);
                    overlayAddCross(out, ik.rayTo, 0.025f, overlaycolor::miss);
                }
            }
            if (ik.locked){
                overlayAddDiamond(out, ik.lockPoint, 0.025f, overlaycolor::locked);
                overlayAddLine(out, ik.lockPoint, ik.lockPoint + Vector3(0, 0.12f, 0), overlaycolor::locked);
                if (ik.lockDriftMm > 1.0f) overlayAddLine(out, ik.lockPoint, ik.animatedFoot, overlayFaded(overlaycolor::locked, 0.6f));
            }
        }
    }
    if (flags.footTrails && history){
        for (const std::vector<AnimatorOverlayHistory::Sample>& foot : history->feet) overlayAddPath(out, foot, nullptr);
    }
    if (flags.rootMotion){
        if (history) overlayAddPath(out, history->root, &overlaycolor::rootPast);
        const AnimatorDebugRootMotion& rm = snapshot.rootMotion;
        if (rm.active && rm.speedMps > 0.0f){
            // where the root goes if speed and turn stay as they are
            constexpr int kSteps = 12;
            const float step = kAnimatorFutureSeconds / (float)kSteps;
            const float turn = rm.yawDegS / kDebugRadToDeg * step;
            const Quaternion stepTurn(std::cos(0.5f * turn), 0.0f, std::sin(0.5f * turn), 0.0f);
            Vector3 position = snapshot.rootPosition;
            Vector3 velocity = rm.velocity;
            for (int i = 0; i < kSteps; i++){
                const Vector3 next = position + velocity * step;
                overlayAddLine(out, position, next, overlaycolor::rootFuture);
                position = next;
                velocity = stepTurn * velocity;
            }
        }
    }
}

void AnimatorOverlayPass::update(Scene* scene, const std::vector<Entity>& entities, const AnimatorOverlayFlags& flags, const Draw& draw){
    if (!flags.any()){
        // off costs nothing (R40): no snapshot, no line, nothing remembered
        if (!histories.empty()) histories.clear();
        return;
    }
    std::unordered_set<Entity> seen;
    for (Entity entity : entities){
        AnimatorComponent* component = scene->findComponent<AnimatorComponent>(entity);
        AnimatorRoot root;
        if (!component || !component->runtime || !animatorRootOf(scene, entity, root)) continue;
        const AnimatorInstance& instance = *component->runtime;
        if (!instance.bound || instance.evaluations == 0) continue;
        const AnimatorDebugSnapshot snapshot = makeSnapshot(instance, *component, root);
        snapshotCount++;
        if (!snapshot.valid) continue;
        seen.insert(entity);

        AnimatorOverlayHistory& history = histories[entity];
        if (instance.evaluations != history.evaluations){
            const float now = instance.clock;
            // a new run (a rebind, Play again) starts its trails over
            if (!history.root.empty() && history.root.back().time > now) history = AnimatorOverlayHistory();
            history.evaluations = instance.evaluations;
            history.feet.resize(snapshot.ik.size());
            for (size_t i = 0; i < snapshot.ik.size(); i++){
                const AnimatorDebugIK& ik = snapshot.ik[i];
                if (!flags.footTrails || !ik.leg){
                    history.feet[i].clear();
                    continue;
                }
                history.feet[i].push_back({now, ik.foot, footTrailColor(ik.contact, ik.slideMmS)});
                overlayDropOld(history.feet[i], now);
            }
            history.root.push_back({now, snapshot.rootPosition, overlaycolor::rootPast});
            overlayDropOld(history.root, now);
        }

        scratch.clear();
        appendAnimatorOverlayLines(snapshot, flags, &history, scratch);
        lineCount += scratch.size();
        draw(entity, scratch);
    }
    for (auto it = histories.begin(); it != histories.end();){
        if (seen.count(it->first)) ++it;
        else it = histories.erase(it);
    }
}

}
