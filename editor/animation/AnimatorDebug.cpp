// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorDebug.h"

#include "Scene.h"
#include "animation/AnimatorInstance.h"
#include "animation/ControllerDef.h"
#include "component/AnimatorComponent.h"
#include "pose/HumanBones.h"
#include "pose/HumanoidMap.h"

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

    // a shaft from `from` along the unit `axis`, and a head at its tip
    void overlayAddArrow(std::vector<AnimatorOverlayLine>& out, const Vector3& from, const Vector3& axis, float length, const Vector4& color){
        const Vector3 tip = from + axis * length;
        Vector3 side = axis.crossProduct(Vector3(0, 1, 0));
        if (side.length() < 1e-3f) side = axis.crossProduct(Vector3(1, 0, 0));
        side = side.normalized();
        const Vector3 back = tip - axis * (0.15f * length);
        overlayAddLine(out, from, tip, color);
        overlayAddLine(out, tip, back + side * (0.08f * length), color);
        overlayAddLine(out, tip, back - side * (0.08f * length), color);
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

void appendTorsoDirectionLines(const AnimatorInstance& instance, const AnimatorRoot& root, std::vector<AnimatorOverlayLine>& out){
    if (!instance.bound || !instance.controller || instance.evaluations == 0) return;
    const size_t bones = instance.rig.bones.size();
    const std::vector<Quaternion>& rotations = instance.rigRotations();
    const std::vector<Vector3>& positions = instance.rigPositions();
    if (rotations.size() < bones || positions.size() < bones) return;
    std::vector<Quaternion> rest;
    const Vector3 front(0, 0, 1);
    for (size_t l = 1; l < instance.layers.size(); l++){
        if (!instance.layerMeshSpaceApplies((int)l)) continue;
        if (rest.empty()){
            // the rest pose's rig rotations: an axis is the front at rest turned as the bone turned since
            rest.resize(bones);
            poseRigRotations(instance.rig.rest, instance.rig.parents.data(), bones, rest.data());
        }
        const std::vector<uint16_t>& boundary = instance.layerBoundaryBones((int)l);
        const std::vector<Quaternion>* unpassed = instance.layerMeshSpaceUnpassed((int)l);
        for (size_t k = 0; k < boundary.size(); k++){
            const size_t b = boundary[k];
            if (b >= bones) continue;
            const Vector3 from = root.matrix * positions[b];
            const Quaternion fromRest = rest[b].inverse();
            const Vector3 axis = (root.rotation * (rotations[b] * (fromRest * front))).normalized();
            overlayAddArrow(out, from, axis, kTorsoAxisLength, overlaycolor::applied);
            if (unpassed && k < unpassed->size()){
                const Vector3 ghost = (root.rotation * ((*unpassed)[k] * (fromRest * front))).normalized();
                overlayAddArrow(out, from, ghost, kTorsoAxisLength, overlaycolor::rejected);
            }
        }
    }
}

void AnimatorOverlayPass::update(Scene* scene, const std::vector<Entity>& entities, const AnimatorOverlayFlags& flags, const Draw& draw,
                                 const std::vector<Entity>& selected){
    // Torso Direction's recording runs only on the Animators it draws (R30): on for the selected
    // ones while it is on, off again for every other one this pass turned on
    if (flags.torsoDirection || !recording.empty()){
        const std::vector<Entity> none;
        const std::vector<Entity>& wanted = flags.torsoDirection ? selected : none;
        for (Entity entity : recording){
            if (std::find(wanted.begin(), wanted.end(), entity) != wanted.end()) continue;
            AnimatorComponent* component = scene->findComponent<AnimatorComponent>(entity);
            if (component && component->runtime && component->runtime->meshSpaceDebug()) component->runtime->setMeshSpaceDebug(false);
        }
        recording.clear();
        for (Entity entity : wanted){
            AnimatorComponent* component = scene->findComponent<AnimatorComponent>(entity);
            if (!component || !component->runtime) continue;
            if (!component->runtime->meshSpaceDebug()) component->runtime->setMeshSpaceDebug(true);
            recording.push_back(entity);
        }
    }
    if (!flags.any()){
        // off costs nothing (R40): no snapshot, no line, nothing remembered
        if (!histories.empty()) histories.clear();
        return;
    }
    const bool live = flags.ik || flags.rootMotion || flags.footTrails;
    if (!live && !histories.empty()) histories.clear();
    std::unordered_set<Entity> seen;
    for (Entity entity : entities){
        AnimatorComponent* component = scene->findComponent<AnimatorComponent>(entity);
        AnimatorRoot root;
        if (!component || !component->runtime || !animatorRootOf(scene, entity, root)) continue;
        const AnimatorInstance& instance = *component->runtime;
        if (!instance.bound || instance.evaluations == 0) continue;
        const bool torso = flags.torsoDirection && std::find(selected.begin(), selected.end(), entity) != selected.end();
        if (!live && !torso) continue;
        scratch.clear();
        bool drawn = torso;
        if (live){
            const AnimatorDebugSnapshot snapshot = makeSnapshot(instance, *component, root);
            snapshotCount++;
            if (snapshot.valid){
                drawn = true;
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
                appendAnimatorOverlayLines(snapshot, flags, &history, scratch);
            }
        }
        if (torso) appendTorsoDirectionLines(instance, root, scratch);
        if (!drawn) continue;
        lineCount += scratch.size();
        draw(entity, scratch);
    }
    for (auto it = histories.begin(); it != histories.end();){
        if (seen.count(it->first)) ++it;
        else it = histories.erase(it);
    }
}

// ---------------------------------------------------------------- Mesh Space Rotation

namespace{

    // the warning's name of a torso slot (R29: "the mask starts at Spine")
    const char* torsoSlotLabel(int slot){
        switch ((pose::HumanBone)slot){
            case pose::HumanBone::Spine: return "Spine";
            case pose::HumanBone::Chest: return "Chest";
            case pose::HumanBone::UpperChest: return "Upper Chest";
            default: return nullptr;
        }
    }

}

std::vector<std::string> meshSpaceBoundaryNames(const AnimatorInstance& instance, int layer){
    std::vector<std::string> names;
    if (!instance.bound) return names;
    for (uint16_t bone : instance.layerBoundaryBones(layer)){
        if (bone < instance.rig.bones.size()) names.push_back(instance.rig.bones[bone].name);
    }
    return names;
}

std::vector<MeshSpaceWarning> meshSpaceWarnings(const ControllerDef& def, const AnimatorInstance& instance){
    std::vector<MeshSpaceWarning> warnings;
    if (!instance.bound || !instance.controller) return warnings;
    // a torso bone: the humanoid's hips, spine, chest or upper chest, else by its name
    auto torso = [&](size_t bone){
        if (!instance.humanoid.valid) return pose::isTorsoBoneName(instance.rig.bones[bone].name);
        const int slot = instance.humanoidSlotOf((int)bone);
        return slot == (int)pose::HumanBone::Hips || torsoSlotLabel(slot) != nullptr;
    };
    for (size_t l = 1; l < def.layers.size(); l++){
        const LayerDef& layer = def.layers[l];
        if (layer.blending != AnimatorLayerBlending::Override || layer.meshSpaceRotation) continue;
        const int run = instance.getLayerIndex(layer.name);
        if (run <= 0 || !(instance.getLayerWeight(run) > 0.0f)) continue;
        // the runtime's value too: a Lua override that turns it on answers the warning
        if (instance.layerMeshSpaceRotation(run)) continue;
        const std::vector<float>& mask = instance.layers[(size_t)run].mask;
        // the first place, parents first, where the mask starts under a torso bone it leaves out
        // and that start is a torso bone too: the torso split between the layers
        std::string where;
        for (size_t bone = 0; bone < instance.rig.bones.size() && bone < mask.size() && where.empty(); bone++){
            const int parent = instance.rig.parents[bone];
            if (parent < 0 || (size_t)parent >= mask.size() || !(mask[bone] > 0.0f) || mask[(size_t)parent] != 0.0f) continue;
            if (!torso((size_t)parent) || !torso(bone)) continue;
            const char* slot = instance.humanoid.valid ? torsoSlotLabel(instance.humanoidSlotOf((int)bone)) : nullptr;
            where = slot ? std::string(slot) : instance.rig.bones[bone].name;
        }
        if (where.empty()) continue;
        warnings.push_back({(int)l, "Layer '" + layer.name + "': the mask starts at " + where +
                                    ", so the torso follows the base layer's hips. Turn on Mesh Space Rotation to keep this layer's direction."});
    }
    return warnings;
}

}
