// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseViewportLogic.h"

#include "pose/PoseMath.h"
#include "pose/PoseMetrics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using pose::HumanBone;
    using pose::Quatd;
    using pose::Vec3d;

    constexpr float EPS = 1e-6f;

    Vec3d d(const Vector3& v){ return Vec3d(v); }

    Vector3 rejectAxis(const Vector3& v, const Vector3& axis){
        return v - axis * v.dotProduct(axis);
    }

    // a unit vector perpendicular to `axis`
    Vector3 anyPerpendicular(const Vector3& axis){
        const Vector3 helper = std::fabs(axis.x) < 0.9f ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
        return rejectAxis(helper, axis).normalized();
    }

    bool rayPlane(const Vector3& origin, const Vector3& direction, const Vector3& point, const Vector3& normal, Vector3& hit,
                  float minCos = 1e-4f){
        const float denominator = direction.dotProduct(normal);
        if (std::fabs(denominator) < minCos) return false;
        const float t = (point - origin).dotProduct(normal) / denominator;
        if (!std::isfinite(t)) return false;
        hit = origin + direction * t;
        return true;
    }

    // the joint's world rotation in the canonical frame, and its effective parent's
    void slotWorld(const pose::PoseRig& rig, const pose::HumanPose& pose, HumanBone bone, Quatd& world, Quatd& parentWorld){
        std::array<Quatd, pose::HUMAN_BONE_COUNT> slots;
        rig.slotWorldCanonical(pose, slots);
        world = slots[pose::boneIndex(bone)];
        const HumanBone parent = rig.effectiveParent(bone);
        parentWorld = parent == HumanBone::Count ? Quatd() : slots[pose::boneIndex(parent)];
    }

}

// ---- colours ----

Vector4 poseview::color(uint32_t rgb, float alpha){
    return Vector4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

Vector4 poseview::lighten(const Vector4& c, float amount){
    return Vector4(c.x + (1.0f - c.x) * amount, c.y + (1.0f - c.y) * amount, c.z + (1.0f - c.z) * amount, c.w);
}

// ---- handles ----

const std::vector<HumanBone>& poseview::jointDotBones(){
    static const std::vector<HumanBone> bones = {
        HumanBone::Spine, HumanBone::Chest, HumanBone::UpperChest, HumanBone::Neck, HumanBone::Head,
        HumanBone::LeftShoulder, HumanBone::RightShoulder,
        HumanBone::LeftUpperArm, HumanBone::RightUpperArm, HumanBone::LeftLowerArm, HumanBone::RightLowerArm,
        HumanBone::LeftUpperLeg, HumanBone::RightUpperLeg, HumanBone::LeftLowerLeg, HumanBone::RightLowerLeg,
        HumanBone::LeftToes, HumanBone::RightToes,
    };
    return bones;
}

int poseview::boneSide(HumanBone bone){
    const char* name = pose::humanBoneName(bone);
    if (std::strncmp(name, "left_", 5) == 0) return 1;
    if (std::strncmp(name, "right_", 6) == 0) return -1;
    return 0;
}

uint32_t poseview::sideColor(HumanBone bone){
    const int side = boneSide(bone);
    return side > 0 ? LEFT_COLOR : (side < 0 ? RIGHT_COLOR : CENTRE_COLOR);
}

bool poseview::limbOfEnd(HumanBone end, pose::Limb& limb){
    switch (end){
        case HumanBone::LeftHand: limb = pose::Limb::LeftArm; return true;
        case HumanBone::RightHand: limb = pose::Limb::RightArm; return true;
        case HumanBone::LeftFoot: limb = pose::Limb::LeftLeg; return true;
        case HumanBone::RightFoot: limb = pose::Limb::RightLeg; return true;
        default: return false;
    }
}

bool poseview::boneFromName(const char* name, HumanBone& bone){
    return name && pose::humanBoneFromName(name, bone);
}

float poseview::raySphere(const Vector3& origin, const Vector3& direction, const Vector3& center, float radius){
    const Vector3 oc = origin - center;
    const float b = oc.dotProduct(direction);
    const float c = oc.dotProduct(oc) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) return -1.0f;
    const float root = std::sqrt(discriminant);
    float t = -b - root;
    if (t < 0.0f) t = -b + root;     // the ray starts inside
    return t >= 0.0f ? t : -1.0f;
}

float poseview::rayBox(const Vector3& origin, const Vector3& direction, const Vector3& center, float halfSize){
    float tNear = -std::numeric_limits<float>::infinity();
    float tFar = std::numeric_limits<float>::infinity();
    const float o[3] = {origin.x - center.x, origin.y - center.y, origin.z - center.z};
    const float dir[3] = {direction.x, direction.y, direction.z};
    for (int i = 0; i < 3; i++){
        if (std::fabs(dir[i]) < EPS){
            if (o[i] < -halfSize || o[i] > halfSize) return -1.0f;
            continue;
        }
        float t1 = (-halfSize - o[i]) / dir[i];
        float t2 = (halfSize - o[i]) / dir[i];
        if (t1 > t2) std::swap(t1, t2);
        tNear = std::max(tNear, t1);
        tFar = std::min(tFar, t2);
        if (tNear > tFar) return -1.0f;
    }
    if (tFar < 0.0f) return -1.0f;
    return tNear >= 0.0f ? tNear : tFar;
}

float poseview::rayOrientedBox(const Vector3& origin, const Vector3& direction, const Vector3& center, const Quaternion& orientation,
                               float halfSize){
    // the ray in the cube's frame (a rotation keeps distances: t is the same)
    const Quaternion inverse = orientation.inverse();
    const Vector3 localOrigin = inverse * (origin - center);
    const Vector3 localDirection = inverse * direction;
    return rayBox(localOrigin, localDirection, Vector3(0, 0, 0), halfSize);
}

bool poseview::sceneViewTakesPoseKeys(bool rightMouseDown, bool middleMouseDown, bool cameraNavigating){
    return !rightMouseDown && !middleMouseDown && !cameraNavigating;
}

bool poseview::clickLeavesPosing(bool handleTookIt, bool dragged, bool pickedSomething, bool pickedPosedModel){
    if (handleTookIt || dragged) return false;
    return !pickedSomething || !pickedPosedModel;
}

int poseview::pickHandle(const Vector3& origin, const Vector3& direction, const std::vector<Handle>& handles, float* distance){
    int best = -1;
    float bestT = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < handles.size(); i++){
        const Handle& handle = handles[i];
        if (handle.kind == HandleKind::None || handle.radius <= 0.0f) continue;
        const float t = handle.box ? rayOrientedBox(origin, direction, handle.center, handle.orientation, handle.radius)
                                   : raySphere(origin, direction, handle.center, handle.radius);
        if (t >= 0.0f && t < bestT){
            bestT = t;
            best = static_cast<int>(i);
        }
    }
    if (distance && best >= 0) *distance = bestT;
    return best;
}

// ---- rings ----

void poseview::ringBasis(const Vector3& axis, Vector3& u, Vector3& v){
    u = anyPerpendicular(axis);
    v = axis.crossProduct(u).normalized();
}

bool poseview::ringPointVisible(const Ring& ring, const Vector3& p, const Vector3& toCamera){
    if (!ring.frontHalfOnly) return true;
    return (p - ring.center).dotProduct(toCamera) >= -1e-4f * std::max(ring.radius, 1e-6f);
}

void poseview::ringSegments(const Ring& ring, const Vector3& toCamera, int segments, std::vector<std::pair<Vector3, Vector3>>& out){
    Vector3 u, v;
    ringBasis(ring.axis, u, v);
    segments = std::max(segments, 3);
    Vector3 previous;
    bool previousVisible = false;
    for (int i = 0; i <= segments; i++){
        const float a = static_cast<float>(2.0 * pose::POSE_PI * i / segments);
        const Vector3 p = ring.center + (u * std::cos(a) + v * std::sin(a)) * ring.radius;
        const bool visible = ringPointVisible(ring, p, toCamera);
        if (i > 0 && visible && previousVisible){
            out.emplace_back(previous, p);
        }
        previous = p;
        previousVisible = visible;
    }
}

float poseview::raySegmentDistance(const Vector3& origin, const Vector3& direction, const Vector3& a, const Vector3& b, float* rayT){
    // closest points of the line origin + s * direction (s >= 0) and the segment a + t * (b - a)
    const Vector3 e = b - a;
    const Vector3 w = origin - a;
    const float aa = direction.dotProduct(direction);
    const float bb = direction.dotProduct(e);
    const float cc = e.dotProduct(e);
    const float dd = direction.dotProduct(w);
    const float ee = e.dotProduct(w);
    const float denominator = aa * cc - bb * bb;
    float s = 0.0f;
    float t = 0.0f;
    if (denominator > EPS * aa * cc){
        s = (bb * ee - cc * dd) / denominator;
        t = (aa * ee - bb * dd) / denominator;
    }else{
        t = cc > EPS ? ee / cc : 0.0f;   // parallel
    }
    t = std::clamp(t, 0.0f, 1.0f);
    // the best ray parameter for that segment point
    s = std::max(0.0f, (a + e * t - origin).dotProduct(direction) / std::max(aa, EPS));
    const Vector3 closestRay = origin + direction * s;
    // and the best segment point for that ray point
    t = cc > EPS ? std::clamp((closestRay - a).dotProduct(e) / cc, 0.0f, 1.0f) : 0.0f;
    if (rayT) *rayT = s;
    return (closestRay - (a + e * t)).length();
}

int poseview::pickRing(const Vector3& origin, const Vector3& direction, const std::vector<Ring>& rings, const Vector3& cameraPosition,
                       bool orthographic, const Vector3& viewDirection, float tolerance){
    int best = -1;
    float bestDistance = tolerance;
    std::vector<std::pair<Vector3, Vector3>> segments;
    for (size_t i = 0; i < rings.size(); i++){
        const Ring& ring = rings[i];
        if (ring.radius <= 0.0f) continue;
        const Vector3 toCamera = orthographic ? -viewDirection.normalized() : (cameraPosition - ring.center).normalized();
        segments.clear();
        ringSegments(ring, toCamera, RING_SEGMENTS, segments);
        for (const auto& segment : segments){
            const float distance = raySegmentDistance(origin, direction, segment.first, segment.second);
            if (distance <= bestDistance){
                bestDistance = distance;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

bool poseview::ringPlaneDirection(const Ring& ring, const Vector3& origin, const Vector3& direction, const Vector3& viewDirection,
                                  Vector3& out){
    Vector3 hit;
    const Vector3 dir = direction.normalized();
    // a ring seen nearly edge-on: the camera plane through the centre, projected onto the ring's
    if (!rayPlane(origin, dir, ring.center, ring.axis, hit, 0.15f)){
        if (!rayPlane(origin, dir, ring.center, viewDirection.normalized(), hit)) return false;
    }
    const Vector3 inPlane = rejectAxis(hit - ring.center, ring.axis);
    if (inPlane.length() < EPS * std::max(ring.radius, 1.0f)) return false;
    out = inPlane.normalized();
    return true;
}

float poseview::signedAngle(const Vector3& from, const Vector3& to, const Vector3& axis){
    const float sine = from.crossProduct(to).dotProduct(axis);
    const float cosine = from.dotProduct(to);
    return static_cast<float>(pose::radToDeg(std::atan2(sine, cosine)));
}

float poseview::unwrapDegrees(float previous, float current){
    float delta = current - std::fmod(previous, 360.0f);
    while (delta > 180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    return previous + delta;
}

float poseview::snapDegrees(float degrees, float step){
    if (step <= 0.0f) return degrees;
    return std::round(degrees / step) * step;
}

// ---- drags ----

Vector3 poseview::grabOffset(const Vector3& origin, const Vector3& direction, const Vector3& point, const Vector3& viewDirection){
    Vector3 hit;
    if (!rayPlane(origin, direction.normalized(), point, viewDirection.normalized(), hit)) return Vector3(0, 0, 0);
    return point - hit;
}

bool poseview::dragPoint(const Vector3& origin, const Vector3& direction, const Vector3& start, const Vector3& offset,
                         const Vector3& viewDirection, bool horizontal, const Vector3& up, Vector3& out){
    const Vector3 dir = direction.normalized();
    Vector3 hit;
    if (horizontal){
        const Vector3 n = up.normalized();
        // the horizontal plane through the start; nearly edge-on, the camera plane with the
        // start's height kept
        if (rayPlane(origin, dir, start, n, hit, 0.05f)){
            out = hit + rejectAxis(offset, n);
            return true;
        }
        if (!rayPlane(origin, dir, start, viewDirection.normalized(), hit)) return false;
        const Vector3 moved = hit + offset;
        out = moved - n * (moved - start).dotProduct(n);
        return true;
    }
    if (!rayPlane(origin, dir, start, viewDirection.normalized(), hit)) return false;
    out = hit + offset;
    return true;
}

// ---- joints ----

Vector3 poseview::jointAxisModel(const pose::PoseRig& rig, const pose::HumanPose& pose, HumanBone bone, pose::CanonicalAxis axis){
    Quatd world, parentWorld;
    slotWorld(rig, pose, bone, world, parentWorld);
    Quatd frame;   // the hips' DOFs are about the character frame itself
    const HumanBone parent = rig.effectiveParent(bone);
    if (bone != HumanBone::Hips && parent != HumanBone::Count){
        frame = parentWorld * rig.neutralWorldCanonical(parent).inverse();
    }
    const Vec3d canonical = frame * Vec3d(pose::canonicalAxis(axis));
    const Quatd character(rig.characterRotation());
    return (character * canonical).normalized().toVector3();
}

pose::HumanPose poseview::rotateJoint(const pose::PoseRig& rig, const pose::HumanPose& pose, HumanBone bone, const Vector3& axisModel,
                                      float degrees){
    pose::HumanPose result = pose;
    if (!rig.has(bone)) return result;
    Quatd world, parentWorld;
    slotWorld(rig, pose, bone, world, parentWorld);
    const Quatd character(rig.characterRotation());
    const Vec3d axisCanonical = character.inverse() * d(axisModel);
    const Quatd turned = (pose::axisAngleRad(axisCanonical, pose::degToRad(degrees)) * world).normalized();
    result.rotations[pose::boneIndex(bone)] = rig.localFromSlotWorld(bone, parentWorld, turned).normalized().toQuaternion();
    return result;
}

float poseview::limitViolation(const pose::PoseRig& rig, HumanBone bone, const Quaternion& local){
    float angles[pose::DOFS_PER_BONE];
    rig.anglesFromLocal(bone, local, angles);
    const pose::BoneDofInfo& info = pose::boneDofInfo(bone);
    float excess = 0.0f;
    for (int k = 0; k < pose::DOFS_PER_BONE; k++){
        const pose::DofInfo& dof = info.dofs[k];
        if (angles[k] < dof.minDegrees) excess += dof.minDegrees - angles[k];
        if (angles[k] > dof.maxDegrees) excess += angles[k] - dof.maxDegrees;
    }
    const float swing = std::sqrt(angles[0] * angles[0] + angles[1] * angles[1]);
    if (swing > pose::MAX_SWING_DEGREES) excess += swing - pose::MAX_SWING_DEGREES;
    return excess;
}

float poseview::AngleRange::clamp(float degrees) const{
    return std::clamp(degrees, low, high);
}

poseview::AngleRange poseview::ringLimitRange(const pose::PoseRig& rig, const pose::HumanPose& pose, HumanBone bone,
                                              const Vector3& axisModel){
    AngleRange range;
    if (!rig.has(bone)) return range;
    const int slot = pose::boneIndex(bone);
    const float startViolation = limitViolation(rig, bone, pose.rotations[slot]);
    auto allowed = [&](float degrees){
        const pose::HumanPose turned = rotateJoint(rig, pose, bone, axisModel, degrees);
        return limitViolation(rig, bone, turned.rotations[slot]) <= startViolation + 1e-3f;
    };
    // outwards from 0 in 1 degree steps, then bisect the first step that goes too far
    auto search = [&](float sign, float& end, bool& limited){
        float ok = 0.0f;
        for (int step = 1; step <= 180; step++){
            const float candidate = sign * static_cast<float>(step);
            if (allowed(candidate)){
                ok = candidate;
                continue;
            }
            float bad = candidate;
            for (int i = 0; i < 16; i++){
                const float middle = 0.5f * (ok + bad);
                if (allowed(middle)) ok = middle; else bad = middle;
            }
            end = ok;
            limited = true;
            return;
        }
        end = sign * 180.0f;
        limited = false;
    };
    search(-1.0f, range.low, range.lowLimited);
    search(1.0f, range.high, range.highLimited);
    return range;
}

pose::HumanPose poseview::ringDrag(const pose::PoseRig& rig, const pose::HumanPose& start, HumanBone bone, const Vector3& axisModel,
                                   float degrees, bool snap, const AngleRange* range, float* applied){
    float angle = snap ? snapDegrees(degrees, RING_SNAP_DEGREES) : degrees;
    if (range) angle = range->clamp(angle);
    if (applied) *applied = angle;
    return rotateJoint(rig, start, bone, axisModel, angle);
}

Vector3 poseview::ringReference(const pose::PoseRig& rig, const pose::HumanPose& pose, HumanBone bone, const Vector3& axisModel){
    Quatd world, parentWorld;
    slotWorld(rig, pose, bone, world, parentWorld);
    // the joint's frame as it sits now: the neutral canonical axes carried by its rotation
    const Quatd frame = world * rig.neutralWorldCanonical(bone).inverse();
    const Quatd character(rig.characterRotation());
    Vector3 best;
    float bestLength = -1.0f;
    for (int a = 0; a < 3; a++){
        const Vector3 axis = (character * (frame * Vec3d(pose::canonicalAxis(static_cast<pose::CanonicalAxis>(a))))).toVector3();
        const Vector3 inPlane = rejectAxis(axis, axisModel);
        const float length = inPlane.length();
        if (length > bestLength){
            bestLength = length;
            best = inPlane;
        }
    }
    return bestLength > EPS ? best.normalized() : anyPerpendicular(axisModel);
}

bool poseview::clampBonesToLimits(const pose::PoseRig& rig, pose::HumanPose& pose, const std::vector<HumanBone>& bones){
    bool changed = false;
    for (HumanBone bone : bones){
        if (!rig.has(bone)) continue;
        const int slot = pose::boneIndex(bone);
        float angles[pose::DOFS_PER_BONE];
        rig.anglesFromLocal(bone, pose.rotations[slot], angles);
        float clamped[pose::DOFS_PER_BONE] = {angles[0], angles[1], angles[2]};
        if (pose::clampBoneToLimits(bone, clamped) == 0) continue;
        // a value a round trip left a hair outside its limit is not worth a change
        float moved = 0.0f;
        for (int k = 0; k < pose::DOFS_PER_BONE; k++) moved = std::max(moved, std::fabs(clamped[k] - angles[k]));
        if (moved > 1e-3f){
            pose.rotations[slot] = rig.localFromAngles(bone, clamped);
            changed = true;
        }
    }
    return changed;
}

std::vector<HumanBone> poseview::limbChain(pose::Limb limb){
    HumanBone upper, lower, end;
    pose::limbBones(limb, upper, lower, end);
    return {upper, lower, end};
}

float poseview::footSoleHeight(const pose::PoseRig& rig, const pose::HumanPose& pose, HumanBone foot){
    if (!rig.has(foot)) return 0.0f;
    const pose::PoseMetrics metrics = pose::computeMetrics(rig, pose, rig.toBodyPose(pose));
    const pose::FootMetrics& feet = foot == HumanBone::RightFoot ? metrics.right : metrics.left;
    std::vector<Quaternion> rotations;
    std::vector<Vector3> positions;
    rig.computeWorld(pose, rotations, positions);
    const float boneHeight = positions[static_cast<size_t>(rig.boneIndex(foot))].dotProduct(rig.characterUp());
    return boneHeight - (rig.floorHeight() + feet.height * rig.humanScale());
}

Vector3 poseview::snapFootTarget(const pose::PoseRig& rig, const Vector3& targetModel, float soleHeight, bool* snapped){
    Vector3 target = targetModel;
    const Vector3 up = rig.characterUp();
    const float floorTarget = rig.floorHeight() + soleHeight;
    const float height = target.dotProduct(up);
    const bool near = std::fabs(height - floorTarget) <= FOOT_SNAP_DISTANCE * rig.humanScale();
    if (near) target = target + up * (floorTarget - height);
    if (snapped) *snapped = near;
    return target;
}

void poseview::finishHandleEdit(const pose::PoseRig& rig, pose::HumanPose& pose, const std::vector<HumanBone>& touched,
                                bool jointLimits, bool grounded){
    if (jointLimits) clampBonesToLimits(rig, pose, touched);
    if (grounded) rig.groundPose(pose, 0.0f, rig.skin());
}

pose::HumanPose poseview::dragLimb(const pose::PoseRig& rig, const pose::HumanPose& start, HumanBone end, const Vector3& targetModel,
                                   bool jointLimits, bool grounded){
    pose::HumanPose result = start;
    pose::Limb limb;
    if (!limbOfEnd(end, limb)) return result;
    const bool foot = end == HumanBone::LeftFoot || end == HumanBone::RightFoot;
    const Vector3 target = foot && grounded ? snapFootTarget(rig, targetModel, footSoleHeight(rig, start, end)) : targetModel;
    pose::solveLimbIK(rig, result, limb, target, foot);
    finishHandleEdit(rig, result, limbChain(limb), jointLimits, grounded);
    return result;
}

pose::HumanPose poseview::dragHips(const pose::PoseRig& rig, const pose::HumanPose& start, const Vector3& hipsModel,
                                   bool jointLimits, bool grounded){
    pose::HumanPose result = start;
    if (grounded){
        pose::moveHipsKeepFeet(rig, result, hipsModel);
        std::vector<HumanBone> legs = limbChain(pose::Limb::LeftLeg);
        const std::vector<HumanBone> right = limbChain(pose::Limb::RightLeg);
        legs.insert(legs.end(), right.begin(), right.end());
        finishHandleEdit(rig, result, legs, jointLimits, true);
    }else{
        rig.setHipsWorldPosition(result, hipsModel);
    }
    return result;
}

// ---- floor ----

Vector3 poseview::canonicalToModel(const pose::PoseRig& rig, const Vector3& canonical){
    const float hs = rig.humanScale();
    return rig.characterLeft() * (canonical.x * hs) + rig.characterUp() * (canonical.y * hs + rig.floorHeight()) +
           rig.characterForward() * (canonical.z * hs);
}

void poseview::discGrid(const Vector3& center, float radius, const Vector3& axisA, const Vector3& axisB, const Vector3& origin,
                        float spacing, std::vector<std::pair<Vector3, Vector3>>& out){
    if (radius <= 0.0f || spacing <= 0.0f) return;
    for (int family = 0; family < 2; family++){
        // lines along `along`, stepping along `across`
        const Vector3& along = family == 0 ? axisA : axisB;
        const Vector3& across = family == 0 ? axisB : axisA;
        const float centerOffset = (center - origin).dotProduct(across);
        const int first = static_cast<int>(std::ceil((centerOffset - radius) / spacing - 1e-4f));
        const int last = static_cast<int>(std::floor((centerOffset + radius) / spacing + 1e-4f));
        for (int k = first; k <= last && k - first < 2000; k++){
            const float offset = k * spacing - centerOffset;   // from the centre, across
            const float half = radius * radius - offset * offset;
            if (half <= 0.0f) continue;
            const float h = std::sqrt(half);
            const Vector3 mid = center + across * offset;
            out.emplace_back(mid - along * h, mid + along * h);
        }
    }
}

void poseview::circleSegments(const Vector3& center, const Vector3& axis, float radius, int segments,
                              std::vector<std::pair<Vector3, Vector3>>& out){
    Ring ring;
    ring.center = center;
    ring.axis = axis.normalized();
    ring.radius = radius;
    ring.frontHalfOnly = false;
    ringSegments(ring, Vector3(0, 0, 0), segments, out);
}

// ---- views ----

bool poseview::projectToView(const Matrix4& viewProjection, const Vector3& point, float viewWidth, float viewHeight, Vector2& out){
    Vector4 clip = viewProjection * Vector4(point.x, point.y, point.z, 1.0f);
    if (clip.w <= 1e-6f) return false;
    const float x = clip.x / clip.w;
    const float y = clip.y / clip.w;
    out = Vector2((x + 1.0f) * 0.5f * viewWidth, (1.0f - y) * 0.5f * viewHeight);
    return true;
}

float poseview::worldPerPixel(const Matrix4& viewProjection, const Vector3& point, const Vector3& viewDirection, float viewWidth,
                              float viewHeight){
    // a world unit across the view at `point`, in pixels (square pixels: any direction across)
    const Vector3 across = anyPerpendicular(viewDirection.normalized());
    Vector2 a, b;
    if (viewWidth <= 0.0f || viewHeight <= 0.0f || !projectToView(viewProjection, point, viewWidth, viewHeight, a) ||
        !projectToView(viewProjection, point + across, viewWidth, viewHeight, b)){
        return 0.0f;
    }
    const float pixels = (b - a).length();
    return pixels > EPS ? 1.0f / pixels : 0.0f;
}

int poseview::onionPrevious(int primary, int count, bool loop){
    if (count < 2 || primary < 0 || primary >= count) return -1;
    if (primary > 0) return primary - 1;
    return loop ? count - 1 : -1;
}

int poseview::onionNext(int primary, int count, bool loop){
    if (count < 2 || primary < 0 || primary >= count) return -1;
    if (primary < count - 1) return primary + 1;
    return loop ? 0 : -1;
}

int poseview::keyLeaving(const pose::PoseTimeline& timeline, double t){
    const int count = static_cast<int>(timeline.holds.size());
    if (count == 0) return -1;
    int leaving = 0;
    double keyTime = 0.0;
    for (int i = 1; i < count; i++){
        const size_t segment = static_cast<size_t>(i - 1);
        if (segment >= timeline.start.size()) break;
        keyTime = timeline.start[segment] + timeline.transitions[segment].duration;
        if (keyTime <= t + 1e-9) leaving = i; else break;
    }
    return leaving;
}
