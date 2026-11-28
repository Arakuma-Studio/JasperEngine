// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RootMotion.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace jasperengine::anim {

namespace{

    constexpr float kRootPi = 3.14159265358979323846f;

    // per slot: a share of the body's mass at its joint (Dempster-like fractions; the joints
    // stand in for the segments, which is what a centre of mass for root motion needs)
    constexpr float kSlotMass[pose::HUMAN_BONE_COUNT] = {
        0.150f, 0.100f, 0.120f, 0.050f, 0.020f, 0.080f,     // hips, spine, chest, upper chest, neck, head
        0.010f, 0.028f, 0.016f, 0.006f,                     // left shoulder, upper arm, lower arm, hand
        0.010f, 0.028f, 0.016f, 0.006f,                     // right
        0.100f, 0.0465f, 0.0145f, 0.003f,                   // left upper leg, lower leg, foot, toes
        0.100f, 0.0465f, 0.0145f, 0.003f,                   // right
    };

    bool has(const HumanBodyPoints& body, pose::HumanBone bone){
        return body.has.test((size_t)bone);
    }

    const Vector3& at(const HumanBodyPoints& body, pose::HumanBone bone){
        return body.at[(size_t)bone];
    }

    bool bodyUsable(const HumanBodyPoints* body){
        return body && body->usable();
    }

}

bool anim::HumanBodyPoints::usable() const{
    return has.test((size_t)pose::HumanBone::Hips) && has.test((size_t)pose::HumanBone::LeftUpperLeg) &&
           has.test((size_t)pose::HumanBone::RightUpperLeg);
}

bool rootMotionNeedsBody(const ClipRootMotion& rootMotion){
    return rootMotion.rotationBasedUpon != RootRotationBasis::Original || rootMotion.positionYBasedUpon != RootPositionYBasis::Original ||
           rootMotion.positionXZBasedUpon != RootPositionXZBasis::Original;
}

float rootWrapAngle(float a){
    while (a > kRootPi) a -= 2.0f * kRootPi;
    while (a <= -kRootPi) a += 2.0f * kRootPi;
    return a;
}

float rootYawOf(const Quaternion& q){
    const Vector3 f = q * Vector3(0, 0, 1);
    return std::atan2(f.x, f.z);
}

Vector3 rootRotateYaw(const Vector3& v, float yaw){
    const float c = std::cos(yaw), s = std::sin(yaw);
    return Vector3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

Quaternion rootYawRotation(float yaw){
    return Quaternion(std::cos(0.5f * yaw), 0.0f, std::sin(0.5f * yaw), 0.0f);
}

float bodyHeading(const HumanBodyPoints& body){
    using pose::HumanBone;
    // the body's right: the hips line, plus the shoulders line when both arms are mapped
    Vector3 right = at(body, HumanBone::RightUpperLeg) - at(body, HumanBone::LeftUpperLeg);
    if (has(body, HumanBone::RightUpperArm) && has(body, HumanBone::LeftUpperArm)){
        right = right + (at(body, HumanBone::RightUpperArm) - at(body, HumanBone::LeftUpperArm));
    }
    // forward = up x right
    return std::atan2(right.z, -right.x);
}

Vector3 bodyCenterOfMass(const HumanBodyPoints& body){
    Vector3 sum(0, 0, 0);
    float mass = 0.0f;
    for (size_t s = 0; s < (size_t)pose::HUMAN_BONE_COUNT; s++){
        if (!body.has.test(s)) continue;
        sum = sum + body.at[s] * kSlotMass[s];
        mass += kSlotMass[s];
    }
    return mass > 0.0f ? sum / mass : Vector3(0, 0, 0);
}

float bodyFeetHeight(const HumanBodyPoints& body){
    using pose::HumanBone;
    const bool left = has(body, HumanBone::LeftFoot), right = has(body, HumanBone::RightFoot);
    if (left && right) return std::min(at(body, HumanBone::LeftFoot).y, at(body, HumanBone::RightFoot).y);
    if (left) return at(body, HumanBone::LeftFoot).y;
    if (right) return at(body, HumanBone::RightFoot).y;
    return std::min(at(body, HumanBone::LeftUpperLeg).y, at(body, HumanBone::RightUpperLeg).y);
}

RootBasisSample rootBasis(const ClipRootMotion& rm, const Vector3& rootPosition, const Quaternion& rootRotation,
                                const HumanBodyPoints* body){
    RootBasisSample out;
    out.point = rootPosition;
    out.heading = rootYawOf(rootRotation);
    if (!bodyUsable(body)) return out;
    if (rm.rotationBasedUpon == RootRotationBasis::BodyOrientation) out.heading = bodyHeading(*body);
    const bool massY = rm.positionYBasedUpon == RootPositionYBasis::CenterOfMass;
    const bool massXZ = rm.positionXZBasedUpon == RootPositionXZBasis::CenterOfMass;
    if (massY || massXZ){
        const Vector3 mass = bodyCenterOfMass(*body);
        if (massY) out.point.y = mass.y;
        if (massXZ){
            out.point.x = mass.x;
            out.point.z = mass.z;
        }
    }
    if (rm.positionYBasedUpon == RootPositionYBasis::Feet) out.point.y = bodyFeetHeight(*body);
    return out;
}

RootMotionFrame rootMotionFrame(const ClipRootMotion& rm, const RootBasisSample& start, const HumanBodyPoints* startBody,
                                      const HumanBodyPoints* restBody){
    RootMotionFrame frame;
    frame.startPoint = start.point;
    frame.startHeading = start.heading;
    frame.turn = rm.rotationOffset * kRootPi / 180.0f;
    frame.shift = Vector3(0, rm.positionYOffset, 0);
    if (!bodyUsable(startBody) || !bodyUsable(restBody)) return frame;
    // Based Upon at start: the body faces the root's forward, its feet or centre of mass stand
    // where the model's rest pose has them
    if (rm.rotationBasedUpon == RootRotationBasis::BodyOrientation){
        frame.turn += -rootWrapAngle(bodyHeading(*startBody) - bodyHeading(*restBody));
    }
    if (rm.positionYBasedUpon == RootPositionYBasis::Feet){
        frame.shift.y += bodyFeetHeight(*restBody) - bodyFeetHeight(*startBody);
    }else if (rm.positionYBasedUpon == RootPositionYBasis::CenterOfMass){
        frame.shift.y += bodyCenterOfMass(*restBody).y - bodyCenterOfMass(*startBody).y;
    }
    if (rm.positionXZBasedUpon == RootPositionXZBasis::CenterOfMass){
        const Vector3 rest = bodyCenterOfMass(*restBody), now = bodyCenterOfMass(*startBody);
        frame.shift.x += rest.x - now.x;
        frame.shift.z += rest.z - now.z;
    }
    return frame;
}

void rootMotionPoint(const ClipRootMotion& rm, const RootMotionFrame& frame, const RootBasisSample& now, Vector3& point, float& heading){
    point = now.point;
    heading = rm.bakeRotation ? 0.0f : rootWrapAngle(now.heading - frame.startHeading);
    if (rm.bakePositionY) point.y = frame.startPoint.y;
    if (rm.bakePositionXZ){
        point.x = frame.startPoint.x;
        point.z = frame.startPoint.z;
    }
}

void removeRootMotion(const ClipRootMotion& rm, const RootMotionFrame& frame, const RootBasisSample& now, Vector3& p, Quaternion& q){
    const Vector3 pivot = frame.startPoint;
    Vector3 basis = now.point;
    if (!rm.bakeRotation){
        const float h = rootWrapAngle(now.heading - frame.startHeading);
        q = rootYawRotation(-h) * q;
        p = pivot + rootRotateYaw(p - pivot, -h);
        basis = pivot + rootRotateYaw(basis - pivot, -h);
    }
    // the bone moves the whole body: pinning the basis point moves the bone by as much
    if (!rm.bakePositionXZ){
        p.x += pivot.x - basis.x;
        p.z += pivot.z - basis.z;
    }
    if (!rm.bakePositionY) p.y += pivot.y - basis.y;
    if (frame.turn != 0.0f){
        q = rootYawRotation(frame.turn) * q;
        p = pivot + rootRotateYaw(p - pivot, frame.turn);
    }
    p = p + frame.shift;
}

}  // namespace jasperengine::anim
