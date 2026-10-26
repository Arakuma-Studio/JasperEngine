// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase 5 viewport posing logic (editor/pose/PoseViewportLogic): handle and ring picking, the
// screen-plane drag, ring axes on the canonical axes, joint limits during a ring drag, the 5
// degree snap, the foot snap, the floor grid, onion ghost indices

#include "TestUtil.h"

#include "editor/pose/PoseViewportLogic.h"
#include "pose/PoseIK.h"
#include "pose/PoseMetrics.h"
#include "pose/PoseSampler.h"

#include <gtest/gtest.h>

using namespace posetest;
using namespace jasperengine::editor;
using H = HumanBone;

namespace {

    float dofValue(const PoseRig& rig, const HumanPose& pose, H bone, int k){
        float angles[DOFS_PER_BONE];
        rig.anglesFromLocal(bone, pose.rotations[boneIndex(bone)], angles);
        return angles[k];
    }

    Vec3d bonePos(const PoseRig& rig, const HumanPose& pose, H bone){
        return worldPositions(rig, pose)[rig.boneIndex(bone)];
    }

}

TEST(PoseViewportLogic, JointDotsAndSides){
    const std::vector<H>& dots = poseview::jointDotBones();
    EXPECT_EQ(dots.size(), 17u);
    for (H bone : {H::Hips, H::LeftHand, H::RightHand, H::LeftFoot, H::RightFoot}){
        EXPECT_EQ(std::count(dots.begin(), dots.end(), bone), 0) << humanBoneName(bone) << " has a handle of its own";
    }
    for (H bone : {H::Spine, H::Chest, H::UpperChest, H::Neck, H::Head, H::LeftShoulder, H::RightToes, H::LeftLowerLeg}){
        EXPECT_EQ(std::count(dots.begin(), dots.end(), bone), 1) << humanBoneName(bone);
    }
    EXPECT_EQ(poseview::sideColor(H::LeftHand), poseview::LEFT_COLOR);
    EXPECT_EQ(poseview::sideColor(H::RightUpperLeg), poseview::RIGHT_COLOR);
    EXPECT_EQ(poseview::sideColor(H::Neck), poseview::CENTRE_COLOR);
    Limb limb;
    ASSERT_TRUE(poseview::limbOfEnd(H::RightFoot, limb));
    EXPECT_EQ(limb, Limb::RightLeg);
    EXPECT_FALSE(poseview::limbOfEnd(H::Head, limb));
    H bone;
    ASSERT_TRUE(poseview::boneFromName("left_upper_arm", bone));
    EXPECT_EQ(bone, H::LeftUpperArm);
    EXPECT_FALSE(poseview::boneFromName("tail", bone));
}

TEST(PoseViewportLogic, HandlePickingNearestToTheCameraWins){
    const Vector3 origin(0, 0, 10);
    const Vector3 dir(0, 0, -1);
    EXPECT_NEAR(poseview::raySphere(origin, dir, Vector3(0, 0, 0), 1.0f), 9.0f, 1e-5);
    EXPECT_LT(poseview::raySphere(origin, dir, Vector3(2.5f, 0, 0), 1.0f), 0.0f);
    EXPECT_NEAR(poseview::rayBox(origin, dir, Vector3(0, 0, 0), 0.5f), 9.5f, 1e-5);
    EXPECT_LT(poseview::rayBox(origin, dir, Vector3(0, 0.6f, 0), 0.5f), 0.0f);
    EXPECT_LT(poseview::raySphere(origin, Vector3(0, 0, 1), Vector3(0, 0, 0), 1.0f), 0.0f) << "behind the ray";

    std::vector<poseview::Handle> handles(3);
    handles[0] = {poseview::HandleKind::Hand, H::LeftHand, Vector3(0, 0, -2), 0.5f, false, Quaternion()};
    handles[1] = {poseview::HandleKind::Hips, H::Hips, Vector3(0, 0, 1), 0.5f, true, Quaternion()};     // in front
    handles[2] = {poseview::HandleKind::Joint, H::Spine, Vector3(3, 0, 3), 0.5f, false, Quaternion()};  // off the ray
    float t = 0.0f;
    EXPECT_EQ(poseview::pickHandle(origin, dir, handles, &t), 1);
    EXPECT_NEAR(t, 8.5f, 1e-5);
    handles[1].kind = poseview::HandleKind::None;   // hidden handles are not picked
    EXPECT_EQ(poseview::pickHandle(origin, dir, handles), 0);
    EXPECT_EQ(poseview::pickHandle(Vector3(10, 10, 10), dir, handles), -1);
}

TEST(PoseViewportLogic, OrientedHipsCube){
    // a cube turned 45 degrees about y: its corner sticks out along x
    const Quaternion turned = pose::axisAngleRad(Vector3(0, 1, 0), static_cast<float>(POSE_PI / 4));
    const Vector3 dir(0, 0, -1);
    const float corner = std::sqrt(2.0f) * 0.5f;   // half the diagonal of a unit-half-size... face
    EXPECT_GT(poseview::rayOrientedBox(Vector3(0.6f, 0, 10), dir, Vector3(0, 0, 0), turned, 0.5f), 0.0f) << "inside the corner";
    EXPECT_LT(poseview::rayBox(Vector3(0.6f, 0, 10), dir, Vector3(0, 0, 0), 0.5f), 0.0f) << "outside the unturned cube";
    EXPECT_LT(poseview::rayOrientedBox(Vector3(0.75f, 0, 10), dir, Vector3(0, 0, 0), turned, 0.5f), 0.0f);
    // straight on, the turned cube's near edge is at z = half diagonal
    EXPECT_NEAR(poseview::rayOrientedBox(Vector3(0, 0, 10), dir, Vector3(0, 0, 0), turned, 0.5f), 10.0f - corner, 1e-4);
    std::vector<poseview::Handle> handles(1);
    handles[0] = {poseview::HandleKind::Hips, H::Hips, Vector3(0, 0, 0), 0.5f, true, turned};
    EXPECT_EQ(poseview::pickHandle(Vector3(0.6f, 0, 10), dir, handles), 0);
}

TEST(PoseViewportLogic, SceneViewInputRouting){
    // flying the camera (right mouse + W A S D Q E) or orbiting: the pose keys (E: Global Edit) wait
    EXPECT_TRUE(poseview::sceneViewTakesPoseKeys(false, false, false));
    EXPECT_FALSE(poseview::sceneViewTakesPoseKeys(true, false, false));
    EXPECT_FALSE(poseview::sceneViewTakesPoseKeys(false, true, false));
    EXPECT_FALSE(poseview::sceneViewTakesPoseKeys(false, false, true));
    // a click on empty space or another object leaves posing; on the model, a handle or a drag it does not
    EXPECT_TRUE(poseview::clickLeavesPosing(false, false, false, false));
    EXPECT_TRUE(poseview::clickLeavesPosing(false, false, true, false));
    EXPECT_FALSE(poseview::clickLeavesPosing(false, false, true, true));
    EXPECT_FALSE(poseview::clickLeavesPosing(true, false, false, false));
    EXPECT_FALSE(poseview::clickLeavesPosing(false, true, false, false));
}

TEST(PoseViewportLogic, RingsShowAndPickTheirFrontHalf){
    poseview::Ring ring;
    ring.center = Vector3(0, 0, 0);
    ring.axis = Vector3(1, 0, 0);     // a ring in the YZ plane, seen from +Z: its front half has z >= 0
    ring.radius = 1.0f;
    const Vector3 camera(0, 0, 10);
    std::vector<std::pair<Vector3, Vector3>> segments;
    poseview::ringSegments(ring, Vector3(0, 0, 1), 64, segments);
    ASSERT_FALSE(segments.empty());
    EXPECT_LE(segments.size(), 33u);
    for (const auto& s : segments){
        EXPECT_GE(s.first.z, -1e-4f);
        EXPECT_GE(s.second.z, -1e-4f);
        EXPECT_NEAR(s.first.length(), 1.0f, 1e-4);
    }
    // the top of the ring (0, 1, 0) is on the visible rim; a ray grazing the front point hits
    const std::vector<poseview::Ring> rings = {ring};
    const Vector3 down(0, 0, -1);
    EXPECT_EQ(poseview::pickRing(Vector3(0, 0.99f, 10), down, rings, camera, false, down, 0.05f), 0);
    // the ray through the ring's plane far from the circle misses
    EXPECT_EQ(poseview::pickRing(Vector3(0.5f, 0.5f, 10), down, rings, camera, false, down, 0.05f), -1);
    // the whole (outer) ring includes its back
    poseview::Ring whole = ring;
    whole.frontHalfOnly = false;
    segments.clear();
    poseview::ringSegments(whole, Vector3(0, 0, 1), 64, segments);
    EXPECT_EQ(segments.size(), 64u);

    // of two rings the closer to the ray wins
    poseview::Ring big = ring;
    big.radius = 1.2f;
    EXPECT_EQ(poseview::pickRing(Vector3(0, 1.18f, 10), down, {ring, big}, camera, false, down, 0.1f), 1);
}

TEST(PoseViewportLogic, RingAnglesFollowTheCursor){
    poseview::Ring ring;
    ring.axis = Vector3(0, 0, 1);    // facing the camera
    ring.radius = 1.0f;
    const Vector3 view(0, 0, -1);
    Vector3 start, now;
    ASSERT_TRUE(poseview::ringPlaneDirection(ring, Vector3(1, 0, 10), view, view, start));
    ASSERT_TRUE(poseview::ringPlaneDirection(ring, Vector3(0, 2, 10), view, view, now));
    EXPECT_NEAR(poseview::signedAngle(start, now, ring.axis), 90.0f, 1e-3);
    EXPECT_NEAR(poseview::signedAngle(now, start, ring.axis), -90.0f, 1e-3);
    // edge-on: still a direction in the ring's plane
    poseview::Ring edge;
    edge.axis = Vector3(1, 0, 0);
    edge.radius = 1.0f;
    Vector3 edgeDir;
    ASSERT_TRUE(poseview::ringPlaneDirection(edge, Vector3(0.3f, 0.5f, 10), view, view, edgeDir));
    EXPECT_NEAR(edgeDir.dotProduct(edge.axis), 0.0f, 1e-5);
    EXPECT_NEAR(edgeDir.length(), 1.0f, 1e-5);
    // unwrapping keeps a drag continuous past 180
    float total = 170.0f;
    total = poseview::unwrapDegrees(total, -175.0f);
    EXPECT_NEAR(total, 185.0f, 1e-4);
    total = poseview::unwrapDegrees(total, -100.0f);
    EXPECT_NEAR(total, 260.0f, 1e-4);
    EXPECT_FLOAT_EQ(poseview::snapDegrees(7.4f, 5.0f), 5.0f);
    EXPECT_FLOAT_EQ(poseview::snapDegrees(8.0f, 5.0f), 10.0f);
    EXPECT_FLOAT_EQ(poseview::snapDegrees(-12.6f, 5.0f), -15.0f);
}

TEST(PoseViewportLogic, ScreenPlaneDragAndShiftHorizontal){
    const Vector3 view = Vector3(0, -0.5f, -1).normalized();
    const Vector3 start(0, 1, 0);
    const Vector3 up(0, 1, 0);
    const Vector3 cameraPos = start - view * 5.0f;
    // grabbing at the centre: no offset; the point moves on the camera plane through the start
    const Vector3 offset = poseview::grabOffset(cameraPos, view, start, view);
    EXPECT_LT(offset.length(), 1e-4f);
    const Vector3 ray = (start + Vector3(0.4f, 0.3f, 0) - cameraPos).normalized();
    Vector3 moved;
    ASSERT_TRUE(poseview::dragPoint(cameraPos, ray, start, offset, view, false, up, moved));
    EXPECT_NEAR((moved - start).dotProduct(view), 0.0f, 1e-4) << "stays on the camera plane";
    EXPECT_GT(moved.x, 0.3f);
    // Shift: the horizontal plane through the start (the height is kept)
    ASSERT_TRUE(poseview::dragPoint(cameraPos, ray, start, offset, view, true, up, moved));
    EXPECT_NEAR(moved.y, start.y, 1e-4);
    // a camera looking horizontally: the camera plane, height kept
    const Vector3 flat(0, 0, -1);
    ASSERT_TRUE(poseview::dragPoint(Vector3(0, 1.4f, 5), Vector3(0.1f, 0.0f, -1).normalized(), start, Vector3(0, 0, 0), flat, true, up, moved));
    EXPECT_NEAR(moved.y, start.y, 1e-4);
    EXPECT_GT(moved.x, 0.0f);
    // the grab offset is kept
    const Vector3 grabRay = (start + Vector3(0.1f, 0, 0) - cameraPos).normalized();
    const Vector3 grab = poseview::grabOffset(cameraPos, grabRay, start, view);
    ASSERT_TRUE(poseview::dragPoint(cameraPos, grabRay, start, grab, view, false, up, moved));
    EXPECT_LT((moved - start).length(), 1e-4f);
}

TEST(PoseViewportLogic, RingAxesAreTheCanonicalAxesOfTheJointsFrame){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const HumanPose neutral = rig.neutralPose();
        // in the neutral pose every frame is the character frame
        for (H bone : {H::Hips, H::Spine, H::LeftUpperArm, H::RightLowerLeg}){
            EXPECT_LT((poseview::jointAxisModel(rig, neutral, bone, CanonicalAxis::L) - rig.characterLeft()).length(), 1e-4f) << t->label;
            EXPECT_LT((poseview::jointAxisModel(rig, neutral, bone, CanonicalAxis::U) - rig.characterUp()).length(), 1e-4f) << t->label;
            EXPECT_LT((poseview::jointAxisModel(rig, neutral, bone, CanonicalAxis::F) - rig.characterForward()).length(), 1e-4f) << t->label;
        }
        // a ring turn changes the DOFs about that axis: spine bend (L+), elbow flex (L-), hips turn (U+)
        HumanPose bent = poseview::rotateJoint(rig, neutral, H::Spine, poseview::jointAxisModel(rig, neutral, H::Spine, CanonicalAxis::L), 20.0f);
        EXPECT_NEAR(dofValue(rig, bent, H::Spine, 0), 20.0f, 1e-2) << t->label;
        EXPECT_NEAR(dofValue(rig, bent, H::Spine, 1), 0.0f, 1e-2) << t->label;
        HumanPose elbow = poseview::rotateJoint(rig, neutral, H::LeftLowerArm,
                                                poseview::jointAxisModel(rig, neutral, H::LeftLowerArm, CanonicalAxis::L), -40.0f);
        EXPECT_NEAR(dofValue(rig, elbow, H::LeftLowerArm, 0), 40.0f, 1e-2) << t->label;
        HumanPose turned = poseview::rotateJoint(rig, neutral, H::Hips, poseview::jointAxisModel(rig, neutral, H::Hips, CanonicalAxis::U), 30.0f);
        EXPECT_NEAR(dofValue(rig, turned, H::Hips, 2), 30.0f, 1e-2) << t->label;
        // the chest's axes follow the bent spine: its L axis is the spine's turned one, and a
        // chest ring turn about it is a chest bend
        if (rig.has(H::Chest)){
            const Vector3 chestUp = poseview::jointAxisModel(rig, bent, H::Chest, CanonicalAxis::U);
            EXPECT_NEAR(chestUp.dotProduct(rig.characterUp()), std::cos(degToRad(20.0)), 1e-3) << t->label;
            const HumanPose chest = poseview::rotateJoint(rig, bent, H::Chest, poseview::jointAxisModel(rig, bent, H::Chest, CanonicalAxis::L), 10.0f);
            EXPECT_NEAR(dofValue(rig, chest, H::Chest, 0), 10.0f, 1e-2) << t->label;
            EXPECT_NEAR(dofValue(rig, chest, H::Spine, 0), 20.0f, 1e-2) << t->label << ": the parent is untouched";
        }
        // the hips turn about their own position
        EXPECT_LT((Vec3d(rig.hipsWorldPosition(turned)) - Vec3d(rig.hipsWorldPosition(neutral))).length(), 1e-6);
    }
}

TEST(PoseViewportLogic, RingDragsStopAtTheJointLimits){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        const HumanPose neutral = rig.neutralPose();
        // the elbow flexes 0..155 (bend L-): from neutral one way is blocked at once
        const Vector3 axis = poseview::jointAxisModel(rig, neutral, H::LeftLowerArm, CanonicalAxis::L);
        const poseview::AngleRange range = poseview::ringLimitRange(rig, neutral, H::LeftLowerArm, axis);
        ASSERT_TRUE(range.lowLimited) << t->label;
        ASSERT_TRUE(range.highLimited) << t->label;
        EXPECT_NEAR(range.high, 0.0f, 0.01f) << t->label;
        EXPECT_NEAR(range.low, -155.0f, 0.05f) << t->label;
        float applied = 0.0f;
        const HumanPose stopped = poseview::ringDrag(rig, neutral, H::LeftLowerArm, axis, -170.0f, false, &range, &applied);
        EXPECT_NEAR(applied, -155.0f, 0.05f);
        EXPECT_NEAR(dofValue(rig, stopped, H::LeftLowerArm, 0), 155.0f, 0.05f) << t->label;
        EXPECT_LT(poseview::limitViolation(rig, H::LeftLowerArm, stopped.rotations[boneIndex(H::LeftLowerArm)]), 1e-2f);
        const HumanPose blocked = poseview::ringDrag(rig, neutral, H::LeftLowerArm, axis, 25.0f, false, &range, &applied);
        EXPECT_NEAR(applied, 0.0f, 0.01f);
        EXPECT_NEAR(dofValue(rig, blocked, H::LeftLowerArm, 0), 0.0f, 0.02f);
        // limits off: the drag goes through
        HumanPose free = poseview::ringDrag(rig, neutral, H::LeftLowerArm, axis, 25.0f, false, nullptr, &applied);
        EXPECT_NEAR(dofValue(rig, free, H::LeftLowerArm, 0), -25.0f, 0.02f);
        EXPECT_GT(poseview::limitViolation(rig, H::LeftLowerArm, free.rotations[boneIndex(H::LeftLowerArm)]), 20.0f);
        // snapping: 5 degree steps, then the limit
        poseview::ringDrag(rig, neutral, H::LeftLowerArm, axis, -12.4f, true, &range, &applied);
        EXPECT_FLOAT_EQ(applied, -10.0f);
        // a pose already outside a limit may move, but never further out
        const poseview::AngleRange outside = poseview::ringLimitRange(rig, free, H::LeftLowerArm, axis);
        EXPECT_NEAR(outside.high, 0.0f, 0.05f) << t->label;
        EXPECT_LT(outside.low, -100.0f) << t->label;
        // the hips have no limits
        const poseview::AngleRange hips = poseview::ringLimitRange(rig, neutral, H::Hips, poseview::jointAxisModel(rig, neutral, H::Hips, CanonicalAxis::U));
        EXPECT_FALSE(hips.lowLimited);
        EXPECT_FALSE(hips.highLimited);
    }
}

TEST(PoseViewportLogic, RingReferenceTurnsWithTheJoint){
    const PoseRig& rig = *tposeRig().rig;
    const HumanPose neutral = rig.neutralPose();
    for (CanonicalAxis a : {CanonicalAxis::L, CanonicalAxis::U, CanonicalAxis::F}){
        const Vector3 axis = poseview::jointAxisModel(rig, neutral, H::LeftUpperArm, a);
        const Vector3 before = poseview::ringReference(rig, neutral, H::LeftUpperArm, axis);
        EXPECT_NEAR(before.dotProduct(axis), 0.0f, 1e-5);
        const HumanPose turned = poseview::rotateJoint(rig, neutral, H::LeftUpperArm, axis, 30.0f);
        const Vector3 after = poseview::ringReference(rig, turned, H::LeftUpperArm, axis);
        EXPECT_NEAR(poseview::signedAngle(before, after, axis), 30.0f, 0.05f);
    }
}

TEST(PoseViewportLogic, LimitClampTouchesOnlyTheGivenBones){
    const PoseRig& rig = *aposeRig().rig;
    HumanPose pose = rig.fromBodyPose(bodyWith({{"left_lower_arm.bend", -30}, {"right_lower_arm.bend", -30}}), nullptr, true, 0);
    // fromBodyPose does not clamp
    ASSERT_NEAR(dofValue(rig, pose, H::LeftLowerArm, 0), -30.0f, 0.05f);
    const Quaternion right = pose.rotations[boneIndex(H::RightLowerArm)];
    EXPECT_TRUE(poseview::clampBonesToLimits(rig, pose, {H::LeftLowerArm}));
    EXPECT_NEAR(dofValue(rig, pose, H::LeftLowerArm, 0), 0.0f, 0.05f);
    EXPECT_EQ(pose.rotations[boneIndex(H::RightLowerArm)], right);
    EXPECT_FALSE(poseview::clampBonesToLimits(rig, pose, {H::LeftLowerArm}));
}

TEST(PoseViewportLogic, FootTargetsSnapToTheFloor){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->skinned;
        const float hs = rig.humanScale();
        // the neutral feet stand on the floor: their bones are a sole height above it
        const HumanPose neutral = rig.neutralPose();
        const float sole = poseview::footSoleHeight(rig, neutral, H::LeftFoot);
        EXPECT_NEAR(bonePos(rig, neutral, H::LeftFoot).dot(Vec3d(rig.characterUp())), rig.floorHeight() + sole, 1e-4 * hs) << t->label;
        EXPECT_GT(sole, 0.0f);
        const float floorTarget = rig.floorHeight() + sole;
        bool snapped = false;
        Vector3 near = poseview::snapFootTarget(rig, Vector3(0.1f, floorTarget + 0.015f * hs, 0.2f), sole, &snapped);
        EXPECT_TRUE(snapped);
        EXPECT_NEAR(near.y, floorTarget, 1e-5);
        EXPECT_FLOAT_EQ(near.x, 0.1f);
        Vector3 below = poseview::snapFootTarget(rig, Vector3(0, floorTarget - 0.019f * hs, 0), sole, &snapped);
        EXPECT_TRUE(snapped);
        EXPECT_NEAR(below.y, floorTarget, 1e-5);
        Vector3 far = poseview::snapFootTarget(rig, Vector3(0, floorTarget + 0.03f * hs, 0), sole, &snapped);
        EXPECT_FALSE(snapped);
        EXPECT_FLOAT_EQ(far.y, floorTarget + 0.03f * hs);
    }
}

TEST(PoseViewportLogic, LimbAndHipsDrags){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->skinned;
        const double hs = rig.humanScale();
        const HumanPose neutral = rig.neutralPose();
        // a hand drag reaches its target (ungrounded, no limits)
        const Vec3d hand = bonePos(rig, neutral, H::LeftHand);
        const Vec3d target = hand + toModel(rig, Vec3d(0.0, 0.25, 0.25) * hs);
        HumanPose reached = poseview::dragLimb(rig, neutral, H::LeftHand, target.toVector3(), false, false);
        EXPECT_LT((bonePos(rig, reached, H::LeftHand) - target).length(), 1e-3 * hs) << t->label;
        // grounded: a hand pushed through the floor lifts the body, the lowest point stays on it
        const Vec3d belowFloor = hand + toModel(rig, Vec3d(0.0, -0.5, 0.3) * hs);
        HumanPose grounded = poseview::dragLimb(rig, neutral, H::LeftHand, belowFloor.toVector3(), false, true);
        EXPECT_NEAR(rig.lowestPointPrecise(grounded, rig.skin()), rig.floorHeight(), 1e-4 * hs) << t->label;
        // hips down on a grounded pose: the feet stay planted
        const Vec3d hips(rig.hipsWorldPosition(neutral));
        const Vec3d lower = hips + toModel(rig, Vec3d(0.02, -0.1, 0.03) * hs);
        HumanPose crouch = poseview::dragHips(rig, neutral, lower.toVector3(), true, true);
        for (H f : {H::LeftFoot, H::RightFoot}){
            EXPECT_LT((bonePos(rig, crouch, f) - bonePos(rig, neutral, f)).length(), 2e-3 * hs) << t->label;
        }
        EXPECT_NEAR(Vec3d(rig.hipsWorldPosition(crouch)).dot(Vec3d(rig.characterUp())), lower.dot(Vec3d(rig.characterUp())), 2e-3 * hs);
        // from the crouch, a foot stepped forward just above the floor snaps onto it, grounded
        const Vec3d foot = bonePos(rig, crouch, H::RightFoot);
        const Vec3d lifted = foot + toModel(rig, Vec3d(0.0, 0.01, 0.08) * hs);
        HumanPose stepped = poseview::dragLimb(rig, crouch, H::RightFoot, lifted.toVector3(), false, true);
        EXPECT_NEAR(bonePos(rig, stepped, H::RightFoot).dot(Vec3d(rig.characterUp())),
                    rig.floorHeight() + poseview::footSoleHeight(rig, crouch, H::RightFoot), 1e-4 * hs) << t->label;
        EXPECT_NEAR(rig.lowestPointPrecise(stepped, rig.skin()), rig.floorHeight(), 1e-4 * hs) << t->label;
        EXPECT_GT((bonePos(rig, stepped, H::RightFoot) - foot).dot(toModel(rig, Vec3d(0, 0, 1))), 0.07 * hs) << t->label;
        // ungrounded: the whole body moves
        const Vec3d up = hips + toModel(rig, Vec3d(0.1, 0.3, 0.0) * hs);
        HumanPose jump = poseview::dragHips(rig, neutral, up.toVector3(), true, false);
        EXPECT_LT(((bonePos(rig, jump, H::LeftFoot) - bonePos(rig, neutral, H::LeftFoot)) - (up - hips)).length(), 1e-4 * hs);
    }
}

TEST(PoseViewportLogic, LimitsOnDuringAnIKDrag){
    const PoseRig& rig = *tposeRig().skinned;
    const double hs = rig.humanScale();
    const HumanPose neutral = rig.neutralPose();
    // a target that would bend the knee backwards: with limits the knee stays within 0..155
    const Vec3d foot = bonePos(rig, neutral, H::LeftFoot);
    const Vec3d target = foot + toModel(rig, Vec3d(0.0, 0.3, -0.3) * hs);
    HumanPose limited = poseview::dragLimb(rig, neutral, H::LeftFoot, target.toVector3(), true, false);
    for (H bone : {H::LeftUpperLeg, H::LeftLowerLeg, H::LeftFoot}){
        float angles[DOFS_PER_BONE];
        rig.anglesFromLocal(bone, limited.rotations[boneIndex(bone)], angles);
        EXPECT_TRUE(boneWithinLimits(bone, angles)) << humanBoneName(bone);
    }
}

TEST(PoseViewportLogic, FloorGridAndMetricsMapping){
    std::vector<std::pair<Vector3, Vector3>> lines;
    poseview::discGrid(Vector3(0, 0, 0), 1.0f, Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(0, 0, 0), 0.25f, lines);
    // offsets -0.75 .. 0.75 in each family (the tangent lines at +-1 have no length)
    EXPECT_EQ(lines.size(), 14u);
    for (const auto& l : lines){
        EXPECT_NEAR(l.first.length(), 1.0f, 1e-4);
        EXPECT_NEAR(l.second.length(), 1.0f, 1e-4);
        EXPECT_NEAR(l.first.y, 0.0f, 1e-6);
    }
    // the grid stays on the world lines when the disc moves (the origin anchors it)
    lines.clear();
    poseview::discGrid(Vector3(0.1f, 0, 0), 1.0f, Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(0, 0, 0), 0.25f, lines);
    for (const auto& l : lines){
        if (std::fabs(l.first.x - l.second.x) < 1e-5f){
            const float k = l.first.x / 0.25f;
            EXPECT_NEAR(k, std::round(k), 1e-4) << "a line along z at x = " << l.first.x;
        }
    }
    lines.clear();
    poseview::circleSegments(Vector3(0, 1, 0), Vector3(0, 1, 0), 0.5f, 24, lines);
    EXPECT_EQ(lines.size(), 24u);

    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->skinned;
        const HumanPose neutral = rig.neutralPose();
        const PoseMetrics metrics = computeMetrics(rig, neutral, rig.toBodyPose(neutral));
        const Vector3 model = poseview::canonicalToModel(rig, metrics.left.position);
        const Vec3d foot = bonePos(rig, neutral, H::LeftFoot);
        EXPECT_NEAR(Vec3d(model).dot(Vec3d(rig.characterLeft())), foot.dot(Vec3d(rig.characterLeft())), 1e-4) << t->label;
        EXPECT_NEAR(Vec3d(model).dot(Vec3d(rig.characterForward())), foot.dot(Vec3d(rig.characterForward())), 1e-4) << t->label;
        // y = the height above the floor
        EXPECT_NEAR(model.y, rig.floorHeight() + metrics.left.height * rig.humanScale(), 1e-4) << t->label;
        EXPECT_LE(metrics.left.height, poseview::CONTACT_RING_HEIGHT) << t->label << ": the neutral feet get contact rings";
    }
}

TEST(PoseViewportLogic, ViewProjection){
    Matrix4 identity;
    Vector2 p;
    ASSERT_TRUE(poseview::projectToView(identity, Vector3(0, 0, 0), 200, 100, p));
    EXPECT_NEAR(p.x, 100.0f, 1e-4);
    EXPECT_NEAR(p.y, 50.0f, 1e-4);
    ASSERT_TRUE(poseview::projectToView(identity, Vector3(1, 1, 0), 200, 100, p));
    EXPECT_NEAR(p.x, 200.0f, 1e-4);
    EXPECT_NEAR(p.y, 0.0f, 1e-4);
    // a square view: one unit = half the view
    EXPECT_NEAR(poseview::worldPerPixel(identity, Vector3(0, 0, 0), Vector3(0, 0, -1), 200, 200), 0.01f, 1e-5);
}

TEST(PoseViewportLogic, OnionGhostsAndPreviewKey){
    EXPECT_EQ(poseview::onionPrevious(2, 4, false), 1);
    EXPECT_EQ(poseview::onionPrevious(0, 4, false), -1);
    EXPECT_EQ(poseview::onionPrevious(0, 4, true), 3);
    EXPECT_EQ(poseview::onionNext(3, 4, true), 0);
    EXPECT_EQ(poseview::onionNext(3, 4, false), -1);
    EXPECT_EQ(poseview::onionNext(0, 1, true), -1) << "one pose has no ghost";
    EXPECT_EQ(poseview::onionPrevious(-1, 4, true), -1);

    PoseAnimationData data = makeAnimation({BodyPose(), BodyPose(), BodyPose()}, true, 0.2f);
    data.settings.transition.duration = 0.1f;
    const PoseTimeline timeline = buildTimeline(data);
    // keys at 0, 0.3, 0.6; the loop back ends at 0.9
    EXPECT_EQ(poseview::keyLeaving(timeline, 0.0), 0);
    EXPECT_EQ(poseview::keyLeaving(timeline, 0.29), 0);
    EXPECT_EQ(poseview::keyLeaving(timeline, 0.31), 1);
    EXPECT_EQ(poseview::keyLeaving(timeline, 0.85), 2);
    EXPECT_EQ(poseview::keyLeaving(PoseTimeline(), 0.5), -1);
}
