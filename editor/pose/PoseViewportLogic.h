// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The maths of posing in the Scene view (spec: pose animation phase 5), without the GPU, ImGui
// or editor state, so jasperengine-pose-tests can test it: handle and ring hit testing, the
// screen-plane drag (Shift: the horizontal plane), the rotation rings on the active joint's
// canonical axes, joint limits applied during a ring drag (the ring stops at the limit), the
// 5 degree snap, the foot snap to the floor, the floor grid and the canonical -> model mapping
// of the phase 2 metrics.
//
// Spaces: "model" is the Model entity's local space (the rig's); the caller maps it to world.
// Rays, handles and rings are in whatever space the caller gives (the viewport uses world).

#pragma once

#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "pose/HumanBones.h"
#include "pose/PoseDofs.h"
#include "pose/PoseIK.h"
#include "pose/PoseRig.h"
#include "pose/PoseSampler.h"
#include "pose/PoseTypes.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace jasperengine::editor::poseview{

    // ---- the values of the spec (phase 5 sections 3 and 4) ----
    constexpr uint32_t LEFT_COLOR = 0xFF7A45;        // hands, feet, joints and contact rings, left side
    constexpr uint32_t RIGHT_COLOR = 0x36B3FF;       // right side
    constexpr uint32_t HIPS_COLOR = 0xFFD54F;        // the hips cube
    constexpr uint32_t CENTRE_COLOR = 0xE6E6E6;      // centre joints (spine, chest, neck, head)
    constexpr uint32_t ACTIVE_COLOR = 0xFFFFFF;      // the handle being dragged / the active joint
    constexpr uint32_t RING_L_COLOR = 0xF05050;      // red ring: canonical L (pitch)
    constexpr uint32_t RING_U_COLOR = 0x5CD65C;      // green ring: canonical U (twist)
    constexpr uint32_t RING_F_COLOR = 0x4D8CFF;      // blue ring: canonical F (roll)
    constexpr uint32_t RING_FREE_COLOR = 0xD0D0D0;   // the outer ring (about the view direction)
    constexpr uint32_t LIMIT_TICK_COLOR = 0xFF3B30;  // a joint limit on a ring
    constexpr uint32_t BALANCED_COLOR = 0x4CD964;    // centre of mass over the feet
    constexpr uint32_t UNBALANCED_COLOR = 0xFF9500;
    constexpr uint32_t ONION_PREVIOUS_COLOR = 0x6FB3FF;
    constexpr uint32_t ONION_NEXT_COLOR = 0x7CE38B;
    constexpr uint32_t GLOBAL_EDIT_COLOR = 0xFF9F43;

    constexpr float FOOT_SNAP_DISTANCE = 0.02f;      // humanScale units: foot targets snap to the floor
    constexpr float CONTACT_RING_HEIGHT = 0.01f;     // humanScale units: a foot this low gets a ring
    constexpr float FLOOR_RADIUS = 1.5f;             // humanScale units
    constexpr float FLOOR_FILL_ALPHA = 0.05f;
    constexpr float FLOOR_GRID_SPACING = 0.25f;      // metres (world units)
    constexpr float RING_SNAP_DEGREES = 5.0f;        // Ctrl/Cmd while rotating

    // screen sizes, logical pixels
    constexpr float HANDLE_RADIUS_PX = 7.0f;         // hand / foot spheres
    constexpr float HIPS_HALF_SIZE_PX = 7.0f;        // hips cube
    constexpr float JOINT_RADIUS_PX = 4.5f;          // joint dots
    constexpr float JOINT_PICK_RADIUS_PX = 7.0f;     // joint dots are picked a little wider than drawn
    constexpr float RING_RADIUS_PX = 64.0f;          // the three axis rings
    constexpr float FREE_RING_RADIUS_PX = 78.0f;     // the outer ring
    constexpr float RING_PICK_PX = 6.0f;             // ring hit tolerance
    constexpr int RING_SEGMENTS = 64;

    Vector4 color(uint32_t rgb, float alpha = 1.0f);
    // `rgb` moved towards white by `amount` (hover highlight)
    Vector4 lighten(const Vector4& color, float amount);

    // ---- handles ----

    enum class HandleKind : uint8_t { None, Hand, Foot, Hips, Joint, Ring };

    // the joints that get a dot (spec 3): spine, chest, upper chest, neck, head, shoulders,
    // upper/lower arms and legs, toes; the rig's missing slots are skipped by the caller
    const std::vector<pose::HumanBone>& jointDotBones();
    // left side 1, right side -1, centre 0
    int boneSide(pose::HumanBone bone);
    uint32_t sideColor(pose::HumanBone bone);
    // the limb an end handle drives
    bool limbOfEnd(pose::HumanBone end, pose::Limb& limb);
    // "left_hand" -> HumanBone::LeftHand (false for an unknown name)
    bool boneFromName(const char* name, pose::HumanBone& bone);

    struct Handle{
        HandleKind kind = HandleKind::None;
        pose::HumanBone bone = pose::HumanBone::Count;
        Vector3 center;
        float radius = 0.0f;           // sphere radius / cube half size (pick size)
        bool box = false;              // picked as an axis-aligned cube, else a sphere
    };

    // distance along the (unit) ray to the first hit, < 0 for a miss
    float raySphere(const Vector3& origin, const Vector3& direction, const Vector3& center, float radius);
    float rayBox(const Vector3& origin, const Vector3& direction, const Vector3& center, float halfSize);
    // the handle the ray hits nearest to the camera, -1 for none (`distance`: along the ray)
    int pickHandle(const Vector3& origin, const Vector3& direction, const std::vector<Handle>& handles, float* distance = nullptr);

    // ---- rings ----

    struct Ring{
        Vector3 center;
        Vector3 axis;                  // unit
        float radius = 0.0f;
        bool frontHalfOnly = true;     // the axis rings; the outer ring is whole
    };

    // two unit vectors spanning the ring's plane (u x v = axis)
    void ringBasis(const Vector3& axis, Vector3& u, Vector3& v);
    // whether the ring point p faces the camera (toCamera: unit, from the ring centre, or the
    // reverse view direction of an orthographic camera)
    bool ringPointVisible(const Ring& ring, const Vector3& p, const Vector3& toCamera);
    // the ring's segments as drawn (front half only for the axis rings)
    void ringSegments(const Ring& ring, const Vector3& toCamera, int segments, std::vector<std::pair<Vector3, Vector3>>& out);
    // closest distance between a ray and a segment, and the ray parameter there
    float raySegmentDistance(const Vector3& origin, const Vector3& direction, const Vector3& a, const Vector3& b, float* rayT = nullptr);
    // the ring whose drawn part passes within tolerance of the ray (the closest one), -1 for none
    int pickRing(const Vector3& origin, const Vector3& direction, const std::vector<Ring>& rings, const Vector3& cameraPosition,
                 bool orthographic, const Vector3& viewDirection, float tolerance);

    // The point of the ring's plane under the ray, as a direction from the centre in that plane
    // (unit). An edge-on ring (the ray nearly in its plane) takes the hit on the plane facing the
    // camera through the centre, projected onto the ring's plane. False when nothing usable.
    bool ringPlaneDirection(const Ring& ring, const Vector3& origin, const Vector3& direction, const Vector3& viewDirection, Vector3& out);
    // signed angle (degrees) from `from` to `to` about `axis` (both in the plane)
    float signedAngle(const Vector3& from, const Vector3& to, const Vector3& axis);
    // `current` (-180..180) unwrapped next to `previous` (any value)
    float unwrapDegrees(float previous, float current);
    float snapDegrees(float degrees, float step);

    // ---- drags ----

    // Where a dragged point goes: the ray on the plane through `start` facing the camera
    // (normal = viewDirection), plus `offset` (the grab offset); `horizontal` (Shift) uses the
    // horizontal plane through `start` instead (normal `up`), falling back to the camera plane
    // with the height kept when the ray is nearly parallel to it. False when the ray misses.
    bool dragPoint(const Vector3& origin, const Vector3& direction, const Vector3& start, const Vector3& offset,
                   const Vector3& viewDirection, bool horizontal, const Vector3& up, Vector3& out);
    // the grab offset of a drag starting at `point` (handle centre minus the ray's hit on its plane)
    Vector3 grabOffset(const Vector3& origin, const Vector3& direction, const Vector3& point, const Vector3& viewDirection);

    // ---- joints (rig maths, model space) ----

    // a canonical axis of the active joint as its DOFs see it (spec 4.5: the effective parent's
    // frame as seen in the neutral pose; the character frame for the hips), in model space
    Vector3 jointAxisModel(const pose::PoseRig& rig, const pose::HumanPose& pose, pose::HumanBone bone, pose::CanonicalAxis axis);
    // the pose with the joint's world rotation turned by `degrees` about `axisModel` (its
    // children follow); the hips turn about their own position
    pose::HumanPose rotateJoint(const pose::PoseRig& rig, const pose::HumanPose& pose, pose::HumanBone bone,
                                const Vector3& axisModel, float degrees);
    // how far a local rotation of `bone` is outside the joint limits (degrees summed over its DOFs,
    // plus the swing beyond MAX_SWING_DEGREES), 0 within them
    float limitViolation(const pose::PoseRig& rig, pose::HumanBone bone, const Quaternion& local);

    // The angles (degrees, relative to the pose) a ring drag can turn the joint about `axisModel`
    // without going further outside its limits than it starts: the ring stops there. Unlimited
    // ends are +-180 with `limited` false.
    struct AngleRange{
        float low = -180.0f;
        float high = 180.0f;
        bool lowLimited = false;
        bool highLimited = false;
        float clamp(float degrees) const;
    };
    AngleRange ringLimitRange(const pose::PoseRig& rig, const pose::HumanPose& pose, pose::HumanBone bone, const Vector3& axisModel);
    // A ring drag: the start pose turned by `degrees` (snapped to RING_SNAP_DEGREES when `snap`,
    // then clamped to `range` when limits are on); `applied` gets the angle used
    pose::HumanPose ringDrag(const pose::PoseRig& rig, const pose::HumanPose& start, pose::HumanBone bone, const Vector3& axisModel,
                             float degrees, bool snap, const AngleRange* range, float* applied = nullptr);
    // a unit vector of the joint's frame most perpendicular to `axisModel`, projected onto the
    // ring's plane: it turns with the joint, so ring angles are measured from it (limit ticks)
    Vector3 ringReference(const pose::PoseRig& rig, const pose::HumanPose& pose, pose::HumanBone bone, const Vector3& axisModel);

    // the slots clamped to their limits (only the given ones, from their local rotations, the
    // other slots untouched); true when one changed
    bool clampBonesToLimits(const pose::PoseRig& rig, pose::HumanPose& pose, const std::vector<pose::HumanBone>& bones);
    // the bones an IK drag of `limb` changes (upper, lower, end)
    std::vector<pose::HumanBone> limbChain(pose::Limb limb);

    // How high the foot bone sits above the foot's lowest point (skin, else the sole estimate)
    // as the foot is turned in `pose` (model units): feet keep their world rotation while
    // dragged, so this is the height of the bone of a foot standing on the floor
    float footSoleHeight(const pose::PoseRig& rig, const pose::HumanPose& pose, pose::HumanBone foot);
    // A foot target (model space, the foot bone) on a grounded pose: its height snaps to the
    // floor + `soleHeight` when within FOOT_SNAP_DISTANCE * humanScale
    Vector3 snapFootTarget(const pose::PoseRig& rig, const Vector3& targetModel, float soleHeight, bool* snapped = nullptr);

    // What every handle change ends with (spec 3): limits on the touched bones (if on), then
    // grounding when the pose is grounded (the lowest point stays on the floor)
    void finishHandleEdit(const pose::PoseRig& rig, pose::HumanPose& pose, const std::vector<pose::HumanBone>& touched,
                          bool jointLimits, bool grounded);

    // An IK drag of a hand or foot to `targetModel` (feet keep their world rotation and snap to
    // the floor on a grounded pose), finished as above
    pose::HumanPose dragLimb(const pose::PoseRig& rig, const pose::HumanPose& start, pose::HumanBone end, const Vector3& targetModel,
                             bool jointLimits, bool grounded);
    // A hips drag: grounded poses keep their feet planted (moveHipsKeepFeet), others move the
    // whole body
    pose::HumanPose dragHips(const pose::PoseRig& rig, const pose::HumanPose& start, const Vector3& hipsModel,
                             bool jointLimits, bool grounded);

    // ---- floor and metrics ----

    // a phase 2 metrics position (canonical, humanScale units above the floor) in model space
    Vector3 canonicalToModel(const pose::PoseRig& rig, const Vector3& canonical);
    // The grid lines of a disc on a plane: lines parallel to `axisA` and `axisB` (unit, in the
    // plane) every `spacing` from `origin` (any point of the plane), clipped to the disc
    void discGrid(const Vector3& center, float radius, const Vector3& axisA, const Vector3& axisB, const Vector3& origin,
                  float spacing, std::vector<std::pair<Vector3, Vector3>>& out);
    // a circle as segments
    void circleSegments(const Vector3& center, const Vector3& axis, float radius, int segments,
                        std::vector<std::pair<Vector3, Vector3>>& out);

    // ---- views ----

    // a world point in view pixels (top-left origin); false behind the camera
    bool projectToView(const Matrix4& viewProjection, const Vector3& point, float viewWidth, float viewHeight, Vector2& out);
    // world units per view pixel at `point`
    float worldPerPixel(const Matrix4& viewProjection, const Vector3& point, const Vector3& viewDirection, float viewWidth,
                        float viewHeight);

    // the key pose index each onion ghost shows (-1: none): previous wraps to the last pose and
    // next to the first when looping
    int onionPrevious(int primary, int count, bool loop);
    int onionNext(int primary, int count, bool loop);
    // the key pose a preview at time t is leaving: the last key whose time (the holds and
    // transitions before it) is <= t; -1 without poses
    int keyLeaving(const pose::PoseTimeline& timeline, double t);

}
