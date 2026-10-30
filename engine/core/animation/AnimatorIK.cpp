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

void anim::solveTwoBone(IKContext& context, int upper, int mid, int end, const Vector3& target,
                        const Vector3* pole, const Quaternion* endRotation, float weight){
    if (!(weight > 0.0f) || upper < 0 || mid < 0 || end < 0) return;
    Saved saved;
    saved.save(context, upper);
    saved.save(context, mid);
    saved.save(context, end);

    // the bend side: toward the pole, else the side the mid joint is on now
    const Vector3 startA = context.space.position[upper];
    const Vector3 bendHint = pole ? (*pole - startA) : (context.space.position[mid] - startA);
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
        const float reach = (lab + lbc) * 0.999f;
        const float minimum = std::fabs(lab - lbc) * 1.001f + kEpsilon;
        const float clamped = std::min(std::max(distance, minimum), reach);

        Vector3 bend = bendHint - direction * bendHint.dotProduct(direction);
        if (bend.squaredLength() < 1e-10f){
            const Vector3 current = (b - a) - direction * (b - a).dotProduct(direction);
            bend = current.squaredLength() > 1e-10f ? current : direction.crossProduct(std::fabs(direction.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0));
        }
        bend.normalize();

        const float cosA = std::min(1.0f, std::max(-1.0f, (lab * lab + clamped * clamped - lbc * lbc) / (2.0f * lab * clamped)));
        const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
        aimBoneInRigSpace(context, upper, b, a + direction * (lab * cosA) + bend * (lab * sinA));
        aimBoneInRigSpace(context, mid, context.space.position[end], a + direction * clamped);
        if ((context.space.position[end] - (a + direction * clamped)).length() < 1e-5f) break;
    }

    if (endRotation){
        setRigRotation(context, end, *endRotation);
        refreshRigSpace(context, end);
    }
    saved.blend(context, weight);
}

void anim::solveFabrik(IKContext& context, const int* chain, int count, const Vector3& target, int iterations, float tolerance, float weight){
    if (!(weight > 0.0f) || count < 2) return;
    count = std::min(count, kMaxChain);
    Saved saved;
    Vector3 points[kMaxChain];
    float lengths[kMaxChain];
    float total = 0.0f;
    for (int k = 0; k < count; k++){
        if (chain[k] < 0) return;
        saved.save(context, chain[k]);
        points[k] = context.space.position[chain[k]];
    }
    for (int k = 0; k + 1 < count; k++){
        lengths[k] = (points[k + 1] - points[k]).length();
        total += lengths[k];
    }
    if (total < kEpsilon) return;
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
}

void anim::solveLookAt(IKContext& context, const int* bones, const float* boneWeights, int count, const Vector3& forward,
                       const Vector3& target, float clampDegrees, float weight){
    if (!(weight > 0.0f) || count <= 0) return;
    count = std::min(count, 4);
    float sum = 0.0f;
    for (int i = 0; i < count; i++){
        if (bones[i] < 0) return;
        sum += std::max(0.0f, boneWeights[i]);
    }
    if (!(sum > 0.0f)) return;
    const int last = bones[count - 1];
    const Vector3 lastForward = context.space.rotation[last] * forward;
    const Vector3 toTarget = target - context.space.position[last];
    if (toTarget.squaredLength() < 1e-10f) return;
    const float maxAngle = std::max(0.0f, clampDegrees) * 0.017453292519943295f;
    // the whole turn of the last bone, clamped, then shared out (all about one axis, so the
    // shares compose to exactly the clamped turn), scaled by the weight
    const Quaternion turn = powRotation(clampRotation(shortestArc(lastForward, toTarget), maxAngle), std::min(1.0f, weight));
    for (int i = 0; i < count; i++){
        const Quaternion share = powRotation(turn, std::max(0.0f, boneWeights[i]) / sum);
        rotateBoneInRigSpace(context, bones[i], share);
    }
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
