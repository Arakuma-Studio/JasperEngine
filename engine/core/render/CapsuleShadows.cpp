// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CapsuleShadows.h"

#include "pose/HumanBones.h"
#include "pose/HumanoidMap.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {
    Vector3 capsuleAxisOf(CapsuleDirection direction){
        switch (direction){
            case CapsuleDirection::X_AXIS: return Vector3(1.0f, 0.0f, 0.0f);
            case CapsuleDirection::Z_AXIS: return Vector3(0.0f, 0.0f, 1.0f);
            default: return Vector3(0.0f, 1.0f, 0.0f);
        }
    }

    float capsuleMaxScale(const Vector3& scale){
        return std::max(std::abs(scale.x), std::max(std::abs(scale.y), std::abs(scale.z)));
    }

    float capsuleSmoothstep(float edge0, float edge1, float x){
        if (edge1 <= edge0)
            return x < edge0 ? 0.0f : 1.0f;
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // the penumbra's half width at the receiver's start (a hard capsule still blends one tenth
    // of its radius) and the shadows' reach when there is no softness
    constexpr float CAPSULE_PENUMBRA_BASE = 0.1f;
    constexpr float CAPSULE_HARD_REACH = 100.0f;
}

CapsuleShadows::World CapsuleShadows::toWorld(const CapsuleShadowShape& shape, const Matrix4& boneModel){
    World world;
    const Vector4 center = boneModel * Vector4(shape.offset.x, shape.offset.y, shape.offset.z, 1.0f);
    const Vector3 axis = capsuleAxisOf(shape.direction) * (0.5f * std::max(shape.length, 0.0f));
    const Vector4 half = boneModel * Vector4(axis.x, axis.y, axis.z, 0.0f);
    const Vector3 c(center.x, center.y, center.z);
    const Vector3 h(half.x, half.y, half.z);
    world.a = c - h;
    world.b = c + h;
    // the radius takes the largest scale of the bone's axes (a collider's rule)
    const float sx = Vector3(boneModel.get(0, 0), boneModel.get(0, 1), boneModel.get(0, 2)).length();
    const float sy = Vector3(boneModel.get(1, 0), boneModel.get(1, 1), boneModel.get(1, 2)).length();
    const float sz = Vector3(boneModel.get(2, 0), boneModel.get(2, 1), boneModel.get(2, 2)).length();
    world.radius = std::max(shape.radius, 0.0f) * std::max(sx, std::max(sy, sz));
    return world;
}

CapsuleShadowShape CapsuleShadows::fromWorld(const std::string& boneName, const Bone& bone, const Vector3& a, const Vector3& b, float radius){
    CapsuleShadowShape shape;
    shape.bone = boneName;
    const Quaternion inverse = bone.rotation.inverse();
    auto toLocal = [&](const Vector3& v){
        const Vector3 rotated = inverse * v;
        return Vector3(bone.scale.x != 0.0f ? rotated.x / bone.scale.x : 0.0f,
                       bone.scale.y != 0.0f ? rotated.y / bone.scale.y : 0.0f,
                       bone.scale.z != 0.0f ? rotated.z / bone.scale.z : 0.0f);
    };
    shape.offset = toLocal((a + b) * 0.5f - bone.position);
    const Vector3 axis = toLocal(b - a);
    const float ax = std::abs(axis.x), ay = std::abs(axis.y), az = std::abs(axis.z);
    if (ax >= ay && ax >= az){
        shape.direction = CapsuleDirection::X_AXIS;
    }else if (az > ay){
        shape.direction = CapsuleDirection::Z_AXIS;
    }else{
        shape.direction = CapsuleDirection::Y_AXIS;
    }
    shape.length = axis.length();
    const float scale = capsuleMaxScale(bone.scale);
    shape.radius = scale > 0.0f ? radius / scale : radius;
    return shape;
}

bool CapsuleShadows::fitToSkeleton(const std::vector<Bone>& bones, std::vector<CapsuleShadowShape>& capsules, std::string* error){
    using pose::HumanBone;

    // the skeleton as the humanoid auto-detect takes it (rest local transforms)
    pose::Skeleton skeleton;
    skeleton.bones.reserve(bones.size());
    for (const Bone& bone : bones){
        pose::SkeletonBone entry;
        entry.name = bone.name;
        entry.parent = bone.parent;
        if (bone.parent >= 0 && bone.parent < (int)bones.size()){
            const Bone& parent = bones[(size_t)bone.parent];
            const Quaternion inverse = parent.rotation.inverse();
            entry.position = inverse * (bone.position - parent.position);
            entry.rotation = inverse * bone.rotation;
        }else{
            entry.position = bone.position;
            entry.rotation = bone.rotation;
        }
        skeleton.bones.push_back(entry);
    }
    const pose::HumanoidMap map = pose::autoDetectHumanoid(skeleton);
    auto find = [&](HumanBone slot){
        const std::string& name = map.bones[(size_t)pose::boneIndex(slot)];
        return name.empty() ? -1 : pose::findSkeletonBone(skeleton, name);
    };

    const int hips = find(HumanBone::Hips);
    const int leftUpperLeg = find(HumanBone::LeftUpperLeg);
    const int rightUpperLeg = find(HumanBone::RightUpperLeg);
    if (hips < 0 || leftUpperLeg < 0 || rightUpperLeg < 0){
        if (error)
            *error = "the bones are not a humanoid skeleton (no hips or upper legs found)";
        return false;
    }

    auto position = [&](int index){ return bones[(size_t)index].position; };
    // the first bone of the skeleton parented to index that no humanoid slot holds (a rig's end
    // bone, such as HeadTop_End), -1 for none
    auto endBone = [&](int index){
        for (int i = 0; i < (int)bones.size(); i++){
            if (bones[(size_t)i].parent != index)
                continue;
            bool mapped = false;
            for (const std::string& name : map.bones)
                mapped = mapped || (!name.empty() && name == bones[(size_t)i].name);
            if (!mapped)
                return i;
        }
        return -1;
    };

    const int neck = find(HumanBone::Neck);
    const int head = find(HumanBone::Head);
    int chest = find(HumanBone::UpperChest);
    if (chest < 0) chest = find(HumanBone::Chest);
    if (chest < 0) chest = find(HumanBone::Spine);
    const int legs[2][4] = {
        {leftUpperLeg, find(HumanBone::LeftLowerLeg), find(HumanBone::LeftFoot), find(HumanBone::LeftToes)},
        {rightUpperLeg, find(HumanBone::RightLowerLeg), find(HumanBone::RightFoot), find(HumanBone::RightToes)},
    };

    // the body's height: the head (or the upper chest) above the hips, plus a leg
    const int top = head >= 0 ? head : (neck >= 0 ? neck : chest);
    const int ground = legs[0][2] >= 0 ? legs[0][2] : (legs[0][1] >= 0 ? legs[0][1] : leftUpperLeg);
    float height = (position(ground) - position(hips)).length();
    if (top >= 0)
        height += (position(top) - position(hips)).length();
    if (height <= 0.0f){
        if (error)
            *error = "the skeleton has no height (its bones share a position)";
        return false;
    }

    capsules.clear();
    auto add = [&](int boneIndex, const Vector3& a, const Vector3& b, float radiusShare){
        if (boneIndex < 0 || (int)capsules.size() >= CapsuleShadowCasterComponent::MAX_CAPSULES)
            return;
        if ((b - a).length() <= 1e-6f)
            return;
        capsules.push_back(fromWorld(bones[(size_t)boneIndex].name, bones[(size_t)boneIndex], a, b, radiusShare * height));
    };

    // the head: from the neck (or the head) to the head's end bone, else a tenth of the body
    // above the head along the neck
    if (head >= 0){
        const int from = neck >= 0 ? neck : head;
        const int end = endBone(head);
        Vector3 headTop;
        if (end >= 0){
            headTop = position(end);
        }else{
            const int below = neck >= 0 ? neck : (chest >= 0 ? chest : hips);
            Vector3 up = position(head) - position(below);
            if (up.length() <= 1e-6f)
                up = Vector3(0.0f, 1.0f, 0.0f);
            headTop = position(head) + up.normalized() * (0.1f * height);
        }
        add(from, position(from), headTop, 0.065f);
    }
    // the spine: the hips to the upper chest
    if (chest >= 0)
        add(hips, position(hips), position(chest), 0.09f);
    // each leg: upper leg, lower leg, foot
    for (const auto& leg : legs){
        if (leg[1] >= 0)
            add(leg[0], position(leg[0]), position(leg[1]), 0.055f);
        if (leg[1] >= 0 && leg[2] >= 0)
            add(leg[1], position(leg[1]), position(leg[2]), 0.04f);
        if (leg[2] >= 0 && leg[3] >= 0)
            add(leg[2], position(leg[2]), position(leg[3]), 0.035f);
    }
    return true;
}

float CapsuleShadows::reach(const World& capsule, float softness){
    // the penumbra grows by softness a metre: past eight radii the occlusion is under 1/64
    if (softness <= 1e-4f)
        return CAPSULE_HARD_REACH;
    return std::min(CAPSULE_HARD_REACH, (8.0f - CAPSULE_PENUMBRA_BASE) * capsule.radius / softness);
}

bool CapsuleShadows::reaches(const World& capsule, const Vector3& towardsLight, float softness, const AABB& receiver){
    if (receiver.isNull())
        return false;
    if (receiver.isInfinite())
        return true;
    // the capsule's bounding sphere swept away from the light over its reach, its penumbra at
    // the far end included: the box of that swept sphere against the receiver's
    const float distance = reach(capsule, softness);
    const Vector3 center = (capsule.a + capsule.b) * 0.5f;
    const float radius = 0.5f * (capsule.b - capsule.a).length() + capsule.radius +
                         CAPSULE_PENUMBRA_BASE * capsule.radius + softness * distance;
    const Vector3 farCenter = center - towardsLight * distance;
    const Vector3 extent(radius, radius, radius);
    AABB swept;
    swept.setExtents(Vector3(std::min(center.x, farCenter.x), std::min(center.y, farCenter.y), std::min(center.z, farCenter.z)) - extent,
                     Vector3(std::max(center.x, farCenter.x), std::max(center.y, farCenter.y), std::max(center.z, farCenter.z)) + extent);
    return swept.intersects(receiver);
}

float CapsuleShadows::occlusion(const World& capsule, const Vector3& point, const Vector3& towardsLight, float softness){
    const Vector3 d = capsule.b - capsule.a;
    const Vector3 w = point - capsule.a;
    const float dd = d.dotProduct(d);
    const float ld = towardsLight.dotProduct(d);
    const float lw = towardsLight.dotProduct(w);
    const float dw = d.dotProduct(w);

    // the closest approach of the ray point + t * towardsLight (t >= 0) and the segment
    // a + s * d (0 <= s <= 1): the lines' closest points, s clamped, then t for that s
    const float denominator = dd - ld * ld;
    float s = denominator > 1e-8f ? std::clamp((dw - lw * ld) / denominator, 0.0f, 1.0f) : 0.0f;
    const float along = s * ld - lw;
    // a capsule behind the point (the ray leaves it): no shadow, so a surface facing the
    // light never shadows itself
    if (along <= 0.0f)
        return 1.0f;
    s = dd > 1e-8f ? std::clamp((dw + along * ld) / dd, 0.0f, 1.0f) : 0.0f;
    const float t = std::max(s * ld - lw, 0.0f);
    const float distance = (w + towardsLight * t - d * s).length();

    const float r = capsule.radius;
    const float penumbra = CAPSULE_PENUMBRA_BASE * r + std::max(softness, 0.0f) * t;
    // the share of the light the capsule can hide at that distance: all of it while the
    // penumbra is narrower than the capsule, then the capsule's share of the light's disc
    const float hidden = std::min(1.0f, (r * r) / std::max(penumbra * penumbra, 1e-12f));
    const float occluded = hidden * (1.0f - capsuleSmoothstep(std::max(r - penumbra, 0.0f), r + penumbra, distance)) *
                           capsuleSmoothstep(0.0f, r, along);
    return 1.0f - occluded;
}
