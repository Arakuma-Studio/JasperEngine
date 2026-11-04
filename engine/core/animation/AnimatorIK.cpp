// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorIK.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace{

    constexpr float kEpsilon = 1e-6f;
    constexpr int kMaxChain = 16;

    Quaternion normalizedOr(const Quaternion& q, const Quaternion& fallback){
        const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (!(len > 1e-12f) || !std::isfinite(len)) return fallback;
        return Quaternion(q.w / len, q.x / len, q.y / len, q.z / len);
    }

    Quaternion nlerpShortest(const Quaternion& a, const Quaternion& b, float t){
        Quaternion target = a.dot(b) < 0.0f ? -b : b;
        return normalizedOr(a + t * (target - a), a);
    }

    Quaternion parentRotation(const IKContext& context, int bone){
        const int parent = context.parents[bone];
        return parent >= 0 ? context.space.rotation[parent] : Quaternion();
    }

    // rig rotation -> local rotation of `bone`
    void setRigRotation(IKContext& context, int bone, const Quaternion& rig){
        const Quaternion local = normalizedOr(parentRotation(context, bone).inverse() * rig, context.local->rotation((size_t)bone));
        context.local->setRotation((size_t)bone, local);
    }

    float angleOf(const Quaternion& q){
        const float w = std::min(1.0f, std::fabs(q.w));
        return 2.0f * std::acos(w);
    }

    // `q` scaled to at most `maxAngle` radians about its own axis
    Quaternion clampRotation(const Quaternion& q, float maxAngle){
        const Quaternion r = q.w < 0.0f ? -q : q;
        const float angle = angleOf(r);
        if (angle <= maxAngle || angle < kEpsilon) return r;
        return normalizedOr(Quaternion::slerp(maxAngle / angle, Quaternion(), r), Quaternion());
    }

    Quaternion powRotation(const Quaternion& q, float fraction){
        const Quaternion r = q.w < 0.0f ? -q : q;
        if (fraction >= 1.0f) return r;
        if (fraction <= 0.0f) return Quaternion();
        return normalizedOr(Quaternion::slerp(fraction, Quaternion(), r), Quaternion());
    }

    // radians about a unit axis (the engine's angle convention does not apply here)
    Quaternion axisAngle(const Vector3& axis, float radians){
        const float half = radians * 0.5f;
        const float sine = std::sin(half);
        return Quaternion(std::cos(half), axis.x * sine, axis.y * sine, axis.z * sine);
    }

    // the eased distance of a soft-limited reach: untouched up to `reach - ds`, then
    // 1 - (4 / (a + 4))^4 of the rest, which has slope 1 where it starts, so the end does not
    // change speed there (ozz's 3 has slope 4/3), and approaches `reach` without arriving
    float softened(float distance, float reach, float soften){
        const float ds = reach * soften;
        const float da = reach - ds;
        if (!(ds > kEpsilon) || !(distance > da)) return distance;
        const float r = 4.0f / ((distance - da) / ds + 4.0f);
        const float r2 = r * r;
        return da + ds * (1.0f - r2 * r2);
    }

    // The joint between two bones (`toUpper` and `toEnd` from it) as it turns about `axis`: the end
    // draws a circle, so the distance from upper to end follows a cosine of the angle between the
    // bones' parts across the axis. It holds with a bone off the plane across the axis.
    struct Hinge{
        Vector3 axis;
        Vector3 upperAcross;
        Vector3 endAcross;
        float fixedSquared = 0.0f;      // the squared distance with the two parts across the axis at 90 degrees
        float product = 0.0f;           // the two parts' lengths, times 2
        bool valid = false;             // an axis along a bone turns nothing

        Hinge() = default;
        Hinge(const Vector3& unitAxis, const Vector3& toUpper, const Vector3& toEnd): axis(unitAxis){
            const float upperAlong = toUpper.dotProduct(axis);
            const float endAlong = toEnd.dotProduct(axis);
            upperAcross = toUpper - axis * upperAlong;
            endAcross = toEnd - axis * endAlong;
            fixedSquared = toUpper.squaredLength() + toEnd.squaredLength() - 2.0f * upperAlong * endAlong;
            product = 2.0f * std::sqrt(upperAcross.squaredLength() * endAcross.squaredLength());
            valid = product > 1e-8f;
        }
        float farthest() const{ return std::sqrt(std::max(0.0f, fixedSquared + product)); }
        float nearest() const{ return std::sqrt(std::max(0.0f, fixedSquared - product)); }
        // the turn about the axis that puts the end `distance` from the upper joint, on the side
        // where a positive turn opens the joint (a chain bent backward comes back to that side)
        float turnTo(float distance) const{
            const float cosine = (fixedSquared - distance * distance) / product;
            const float wanted = std::acos(std::min(1.0f, std::max(-1.0f, cosine)));
            const float current = std::atan2(axis.dotProduct(upperAcross.crossProduct(endAcross)), upperAcross.dotProduct(endAcross));
            return wanted - current;
        }
    };

    // a distance between rig-space points in the units IKResult reports
    float worldDistance(const IKContext& context, const Vector3& a, const Vector3& b){
        if (!context.rigToWorld) return (a - b).length();
        return ((*context.rigToWorld * a) - (*context.rigToWorld * b)).length();
    }

    IKResult reachOf(const IKContext& context, const Vector3& end, const Vector3& target){
        const float error = worldDistance(context, end, target);
        IKResult result;
        result.reached = error <= kIKReachTolerance;
        result.error = std::isfinite(error) ? error : 0.0f;
        return result;
    }

    // From a joint, the point of the aim ray `eye + s * forward` as far away as the target is:
    // turning the joint so that this point lands on the way to the target puts the target on the
    // ray. No such point (the target closer than the ray ever passes, or behind the eye): the
    // nearest the ray gets, the best turn there is.
    Vector3 rayPointAtTargetDistance(const Vector3& eye, const Vector3& forward, const Vector3& toTarget){
        const float along = eye.dotProduct(forward);
        const float discriminant = along * along - eye.squaredLength() + toTarget.squaredLength();
        if (!(discriminant >= 0.0f)) return eye - forward * along;
        const float s = std::sqrt(discriminant) - along;
        return s > 0.0f ? eye + forward * s : forward;
    }

    // locals of `bones` saved, then blended back with the solved ones by `weight`
    struct Saved{
        int bones[kMaxChain];
        Quaternion locals[kMaxChain];
        int count = 0;

        void save(const IKContext& context, int bone){
            if (count >= kMaxChain) return;
            bones[count] = bone;
            locals[count] = context.local->rotation((size_t)bone);
            count++;
        }
        void blend(IKContext& context, float weight){
            if (weight >= 1.0f) return;
            int first = -1;
            for (int i = 0; i < count; i++){
                const Quaternion solved = context.local->rotation((size_t)bones[i]);
                context.local->setRotation((size_t)bones[i], weight <= 0.0f ? locals[i] : nlerpShortest(locals[i], solved, weight));
                if (first < 0 || bones[i] < first) first = bones[i];
            }
            if (first >= 0) refreshRigSpace(context, first);
        }
        // the same at an even speed along the arc: a look-at's weight is a part of its turn
        void blendAlongArc(IKContext& context, float weight){
            if (weight >= 1.0f) return;
            int first = -1;
            for (int i = 0; i < count; i++){
                const Quaternion solved = context.local->rotation((size_t)bones[i]);
                context.local->setRotation((size_t)bones[i], normalizedOr(Quaternion::slerp(weight, locals[i], solved), locals[i]));
                if (first < 0 || bones[i] < first) first = bones[i];
            }
            if (first >= 0) refreshRigSpace(context, first);
        }
    };

}

Quaternion anim::shortestArc(const Vector3& from, const Vector3& to){
    const float lf = from.length();
    const float lt = to.length();
    if (lf < kEpsilon || lt < kEpsilon) return Quaternion();
    const Vector3 a = from * (1.0f / lf);
    const Vector3 b = to * (1.0f / lt);
    const float d = a.dotProduct(b);
    if (d < -1.0f + 1e-6f){
        // opposite: half a turn about any perpendicular axis
        Vector3 axis = Vector3(1, 0, 0).crossProduct(a);
        if (axis.squaredLength() < 1e-6f) axis = Vector3(0, 1, 0).crossProduct(a);
        axis.normalize();
        return Quaternion(0.0f, axis.x, axis.y, axis.z);
    }
    const Vector3 c = a.crossProduct(b);
    return normalizedOr(Quaternion(1.0f + d, c.x, c.y, c.z), Quaternion());
}

void anim::refreshRigSpace(IKContext& context, int bone){
    // the bone and its descendants: the others did not move
    if (bone <= 0) poseToRigSpaceFrom(*context.local, context.parents, 0, context.space);
    else poseToRigSpaceSubtree(*context.local, context.parents, (size_t)bone, context.space);
}

void anim::rotateBoneInRigSpace(IKContext& context, int bone, const Quaternion& rotation){
    if (bone < 0) return;
    setRigRotation(context, bone, rotation * context.space.rotation[bone]);
    refreshRigSpace(context, bone);
}

void anim::aimBoneInRigSpace(IKContext& context, int bone, const Vector3& point, const Vector3& target){
    if (bone < 0) return;
    const Vector3 origin = context.space.position[bone];
    const int parent = context.parents[bone];
    Vector3 current = point - origin;
    Vector3 desired = target - origin;
    if (parent >= 0){
        const Affine inverse = context.space.world[parent].inverse();
        current = inverse.transformVector(current);
        desired = inverse.transformVector(desired);
    }
    const Quaternion arc = shortestArc(current, desired);
    context.local->setRotation((size_t)bone, normalizedOr(arc * context.local->rotation((size_t)bone), context.local->rotation((size_t)bone)));
    refreshRigSpace(context, bone);
}

void anim::translateBoneInRigSpace(IKContext& context, int bone, const Vector3& delta){
    if (bone < 0) return;
    const int parent = context.parents[bone];
    Vector3 local = delta;
    if (parent >= 0){
        // the parent's affine maps local offsets to rig space
        const Affine inverse = context.space.world[parent].inverse();
        local = inverse.transformVector(delta);
    }
    context.local->setPosition((size_t)bone, context.local->position((size_t)bone) + local);
    refreshRigSpace(context, bone);
}

IKResult anim::solveTwoBone(IKContext& context, int upper, int mid, int end, const Vector3& target,
                            const Vector3* pole, const Quaternion* endRotation, float weight, const TwoBoneOptions& options){
    if (!(weight > 0.0f) || upper < 0 || mid < 0 || end < 0) return {};
    Saved saved;
    saved.save(context, upper);
    saved.save(context, mid);
    saved.save(context, end);

    const float soften = options.soften > 0.0f ? std::min(options.soften, 1.0f) : 0.0f;
    const float hingeLength = options.hingeAxis.length();
    const bool hinged = hingeLength > kEpsilon;
    const Vector3 localHinge = hinged ? options.hingeAxis * (1.0f / hingeLength) : Vector3(0, 0, 0);

    // the bend side: toward the pole, else the side the mid joint is on now
    const Vector3 startA = context.space.position[upper];
    const Vector3 bendHint = pole ? (*pole - startA) : (context.space.position[mid] - startA);
    Vector3 line(0, 0, 0);      // the upper joint to where the end goes
    // passes until the end is there: one with uniform scale, a few when non-uniform scale makes
    // the world bone lengths change with the bend
    for (int pass = 0; pass < 10; pass++){
        const Vector3 a = context.space.position[upper];
        const Vector3 b = context.space.position[mid];
        const Vector3 c = context.space.position[end];
        const float lab = (b - a).length();
        const float lbc = (c - b).length();
        if (lab < kEpsilon || lbc < kEpsilon) break;

        Vector3 toTarget = target - a;
        float distance = toTarget.length();
        if (distance < kEpsilon){
            toTarget = c - a;
            distance = toTarget.length();
            if (distance < kEpsilon) break;
        }
        const Vector3 direction = toTarget * (1.0f / distance);
        // The hinge is exact on the first pass, and uniform scale ends there. Under non-uniform
        // scale the bones change length as the joint turns, which makes repeating its closed form
        // oscillate: the later passes bend on from the side it chose, as the auto solve does.
        const Hinge hinge = hinged && pass == 0 ? Hinge(context.space.rotation[mid] * localHinge, a - b, c - b) : Hinge();
        const float reach = (hinge.valid ? hinge.farthest() : lab + lbc) * 0.999f;
        const float minimum = (hinge.valid ? hinge.nearest() : std::fabs(lab - lbc)) * 1.001f + kEpsilon;
        const float clamped = std::min(std::max(softened(distance, reach, soften), minimum), reach);

        if (hinge.valid){
            rotateBoneInRigSpace(context, mid, axisAngle(hinge.axis, hinge.turnTo(clamped)));
            aimBoneInRigSpace(context, upper, context.space.position[end], a + direction * clamped);
            if (pole){
                // the plane through the target line and the pole; none when they line up
                const Vector3 knee = context.space.position[mid] - a;
                const Vector3 kneeAcross = knee - direction * knee.dotProduct(direction);
                const Vector3 poleAcross = bendHint - direction * bendHint.dotProduct(direction);
                if (kneeAcross.squaredLength() > 1e-10f && poleAcross.squaredLength() > 1e-10f){
                    const float roll = std::atan2(direction.dotProduct(kneeAcross.crossProduct(poleAcross)), kneeAcross.dotProduct(poleAcross));
                    rotateBoneInRigSpace(context, upper, axisAngle(direction, roll));
                }
            }
        }else{
            const Vector3 hint = hinged && !pole ? b - a : bendHint;
            Vector3 bend = hint - direction * hint.dotProduct(direction);
            if (bend.squaredLength() < 1e-10f){
                const Vector3 current = (b - a) - direction * (b - a).dotProduct(direction);
                bend = current.squaredLength() > 1e-10f ? current : direction.crossProduct(std::fabs(direction.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0));
            }
            bend.normalize();

            const float cosA = std::min(1.0f, std::max(-1.0f, (lab * lab + clamped * clamped - lbc * lbc) / (2.0f * lab * clamped)));
            const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
            aimBoneInRigSpace(context, upper, b, a + direction * (lab * cosA) + bend * (lab * sinA));
            aimBoneInRigSpace(context, mid, context.space.position[end], a + direction * clamped);
        }
        line = direction * clamped;
        if ((context.space.position[end] - (a + line)).length() < 1e-5f) break;
    }

    const float lineLength = options.twist != 0.0f && std::isfinite(options.twist) ? line.length() : 0.0f;
    if (lineLength > kEpsilon){
        const Vector3 goal = context.space.position[upper] + line;
        rotateBoneInRigSpace(context, upper, axisAngle(line * (1.0f / lineLength), options.twist * 0.017453292519943295f));
        // a turn in rig space under non-uniform scale moves the end a little
        aimBoneInRigSpace(context, upper, context.space.position[end], goal);
    }

    if (endRotation){
        setRigRotation(context, end, *endRotation);
        refreshRigSpace(context, end);
    }
    saved.blend(context, weight);
    return reachOf(context, context.space.position[end], target);
}

IKResult anim::solveFabrik(IKContext& context, const int* chain, int count, const Vector3& target, int iterations, float tolerance, float weight){
    if (!(weight > 0.0f) || count < 2) return {};
    count = std::min(count, kMaxChain);
    Saved saved;
    Vector3 points[kMaxChain];
    float lengths[kMaxChain];
    float total = 0.0f;
    for (int k = 0; k < count; k++){
        if (chain[k] < 0) return {};
        saved.save(context, chain[k]);
        points[k] = context.space.position[chain[k]];
    }
    for (int k = 0; k + 1 < count; k++){
        lengths[k] = (points[k + 1] - points[k]).length();
        total += lengths[k];
    }
    if (total < kEpsilon) return {};
    const Vector3 root = points[0];
    if ((target - root).length() >= total){
        // unreachable: a straight chain toward the target
        const Vector3 direction = (target - root).normalized();
        for (int k = 0; k + 1 < count; k++) points[k + 1] = points[k] + direction * lengths[k];
    }else{
        for (int iteration = 0; iteration < std::max(1, iterations); iteration++){
            points[count - 1] = target;
            for (int k = count - 2; k >= 0; k--){
                Vector3 d = points[k] - points[k + 1];
                const float len = d.length();
                points[k] = points[k + 1] + (len > kEpsilon ? d * (lengths[k] / len) : Vector3(0, 0, 0));
            }
            points[0] = root;
            for (int k = 0; k + 1 < count; k++){
                Vector3 d = points[k + 1] - points[k];
                const float len = d.length();
                points[k + 1] = points[k] + (len > kEpsilon ? d * (lengths[k] / len) : Vector3(0, 0, 0));
            }
            if ((points[count - 1] - target).length() <= tolerance) break;
        }
    }
    // rotations back, parents first (shortest arc per joint keeps the twist)
    for (int k = 0; k + 1 < count; k++){
        aimBoneInRigSpace(context, chain[k], context.space.position[chain[k + 1]], points[k + 1]);
    }
    saved.blend(context, weight);
    return reachOf(context, context.space.position[chain[count - 1]], target);
}

IKResult anim::solveLookAt(IKContext& context, const int* bones, const float* boneWeights, int count, const Vector3& forward,
                           const Vector3& target, float clampDegrees, float weight, const Vector3& aimOffset){
    if (!(weight > 0.0f) || count <= 0) return {};
    count = std::min(count, 4);
    float remaining = 0.0f;
    int taker = -1;     // the last bone with a weight: it takes what the others left
    for (int i = 0; i < count; i++){
        if (bones[i] < 0) return {};
        if (boneWeights[i] > 0.0f){
            remaining += boneWeights[i];
            taker = i;
        }
    }
    const float forwardLength = forward.length();
    if (taker < 0 || forwardLength < kEpsilon) return {};
    const Vector3 axis = forward * (1.0f / forwardLength);
    const int last = bones[count - 1];
    const auto eyeNow = [&](){ return context.space.position[last] + context.space.rotation[last] * aimOffset; };
    const Vector3 animated = context.space.rotation[last] * axis;
    const Vector3 eyeStart = eyeNow();
    const Vector3 toTarget = target - eyeStart;
    if (toTarget.squaredLength() < 1e-10f) return {};
    const float maxAngle = std::max(0.0f, clampDegrees) * 0.017453292519943295f;

    // past the clamp the bones turn toward the clamp's direction, not the target
    const Quaternion wholeTurn = shortestArc(animated, toTarget);
    const bool limited = angleOf(wholeTurn) > maxAngle;
    const Vector3 aimed = limited ? eyeStart + (clampRotation(wholeTurn, maxAngle) * animated) * toTarget.length() : target;

    Saved saved;
    for (int i = 0; i < count; i++) saved.save(context, bones[i]);
    // Each bone measures the turn again from where the ones before it left the head, which is what
    // keeps the parents' swing of the head out of the aim. A share is a part of the turn the head
    // sees, so an upright yaw stays a pure yaw with the exact shares. The taker hits the target
    // exactly, the lever from it to the eyes included.
    for (int i = 0; i < taker; i++){
        if (!(boneWeights[i] > 0.0f)) continue;
        const Quaternion turn = shortestArc(context.space.rotation[last] * axis, aimed - eyeNow());
        rotateBoneInRigSpace(context, bones[i], powRotation(turn, boneWeights[i] / remaining));
        remaining -= boneWeights[i];
    }
    const Vector3 pivot = context.space.position[bones[taker]];
    const Vector3 toAimed = aimed - pivot;
    aimBoneInRigSpace(context, bones[taker], pivot + rayPointAtTargetDistance(eyeNow() - pivot, context.space.rotation[last] * axis, toAimed), aimed);

    // the parents carried the eyes, so the way to the target changed since the clamp was measured
    const Quaternion turned = shortestArc(animated, target - eyeNow());
    if (limited || angleOf(turned) > maxAngle){
        rotateBoneInRigSpace(context, bones[taker], shortestArc(context.space.rotation[last] * axis, clampRotation(turned, maxAngle) * animated));
    }
    saved.blendAlongArc(context, weight);

    const Vector3 ray = context.space.rotation[last] * axis;
    const Vector3 toTargetNow = target - eyeNow();
    const float along = toTargetNow.dotProduct(ray);
    const Vector3 miss = along > 0.0f ? toTargetNow - ray * along : toTargetNow;
    const float error = worldDistance(context, target - miss, target);
    const float degrees = std::atan2((toTargetNow - ray * along).length(), along) * 57.29577951308232f;
    IKResult result;
    result.reached = degrees <= kIKAimTolerance;
    result.error = std::isfinite(error) ? error : 0.0f;
    return result;
}

float anim::footAnkleHeight(float footHeight, const Vector3& normal, float maxAngleDegrees){
    const float c = normal.y;
    if (!std::isfinite(c)) return footHeight;
    const float clampRadians = std::min(std::max(maxAngleDegrees, 0.0f), 90.0f) * 0.017453292519943295f;
    const float cosClamp = std::max(0.0f, std::cos(clampRadians));
    // no slope correction runs away: past the steeper of 60 degrees and the clamp, it stops growing
    const float lowest = std::max(0.05f, std::min(0.5f, cosClamp));
    // the ankle `H` above the hit, the foot's up `u` turned by min(slope, clamp), on a plane of
    // normal `n` through the hit: (H up - h u) . n = 0, so H = h (u . n) / (up . n), and u . n is
    // 1 up to the clamp, then the cosine of what the clamp leaves of the slope
    const float facing = c >= cosClamp ? 1.0f : c * cosClamp + std::sqrt(std::max(0.0f, 1.0f - c * c)) * std::sin(clampRadians);
    return footHeight * std::max(0.0f, facing) / std::max(c, lowest);
}

float anim::springDamp(float current, float target, float& velocity, float halfLife, float dt){
    if (!(dt > 0.0f)) return current;
    if (!(halfLife > 0.0f)){
        velocity = 0.0f;
        return target;
    }
    // x'' = -w^2 (x - target) - 2 w x', exact over dt; (1 + w h) e^(-w h) = 1/2 at the half-life
    const float omega = 1.6783469900166605f / halfLife;
    const float y0 = current - target;
    const float c2 = velocity + omega * y0;
    const float e = std::exp(-omega * dt);
    const float y = (y0 + c2 * dt) * e;
    velocity = (velocity - omega * c2 * dt) * e;
    return target + y;
}
