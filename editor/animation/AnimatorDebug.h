// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S77 R36, R49, R53, D2, D3)
// - Purpose: the one producer of an Animator's live debug numbers in the editor. makeSnapshot
//   reads what the last update left in the runtime (AnimatorIKRun, AnimatorOutput) and puts it in
//   world space; the Animator Inspector's IK (live) table, the Scene view's Gizmos > Animation
//   overlays and get_animator / control_animator `live` print it and never calculate. Metres
//   become mm here only. kAnimatorDebugFields names each number once (JSON key, label, unit).
// - Foot contact comes only from the Animator's own legs (Foot IK and Foot Lock constraints, the
//   foot goals it has): an Animator without them reports contact_source "none" and no slide,
//   never a guess from heights or speeds (R37).
// - AnimatorOverlayPass is the Scene view's per-frame step: with every flag off it takes no
//   snapshot and draws no line (R40), and its counters say so.
// - Thread-safety: main thread (the runtime is read between frames, as the Inspector does).
// - Editor only: nothing here compiles into a player.

#pragma once

#include "animation/AnimatorTypes.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include "json.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jasperengine{
    class Scene;
    struct AnimatorComponent;
    struct Transform;
}

namespace jasperengine::anim{

    class AnimatorInstance;

    // One IK constraint as its last solve left it, world space
    struct AnimatorDebugIK{
        std::string name;
        AnimatorIKType type = AnimatorIKType::TwoBone;
        float weight = 0.0f;                // the weight the solve ran with; 0: nothing solved
        bool reached = false;
        float reachErrorMm = 0.0f;          // the end (a look-at: its aim ray) to the target
        bool hasTarget = false;
        Vector3 target = Vector3(0, 0, 0);  // what it aimed at (Foot IK: the ankle's ground target)
        bool hasPole = false;
        Vector3 pole = Vector3(0, 0, 0);
        std::vector<Vector3> chain;         // the chain's bones after the solve, parents first
        // Foot IK
        bool hasRay = false;
        Vector3 rayFrom = Vector3(0, 0, 0);
        Vector3 rayTo = Vector3(0, 0, 0);
        bool grounded = false;
        Vector3 hitPoint = Vector3(0, 0, 0);
        Vector3 hitNormal = Vector3(0, 1, 0);
        float pelvisOffsetMm = 0.0f;        // the pelvis drop of the Foot IK stack (<= 0)
        // Foot Lock
        bool locked = false;
        Vector3 lockPoint = Vector3(0, 0, 0);
        Vector3 animatedFoot = Vector3(0, 0, 0);
        float lockDriftMm = 0.0f;           // the lock point to where the clip has the foot now
        // Foot IK and Foot Lock: the leg's own contact and the animated foot's world speed
        bool leg = false;
        bool contact = false;
        float slideMmS = 0.0f;              // the foot's world speed while in contact, else 0
        Vector3 footVelocity = Vector3(0, 0, 0);    // the animated foot's, world m/s
        Vector3 foot = Vector3(0, 0, 0);    // the foot after the solve
    };

    struct AnimatorDebugRootMotion{
        bool active = false;                // the last update gave root motion
        float speedMps = 0.0f;
        float yawDegS = 0.0f;
        Vector3 velocity = Vector3(0, 0, 0);    // world, m/s
        Vector3 accumulatedPosition = Vector3(0, 0, 0); // metres since the bind, in the rig frame of then
        float accumulatedYawDeg = 0.0f;
    };

    struct AnimatorDebugSnapshot{
        bool valid = false;                 // the Animator is bound and has evaluated
        Vector3 rootPosition = Vector3(0, 0, 0);
        Quaternion rootRotation;
        std::vector<AnimatorDebugIK> ik;    // parallel to AnimatorComponent::ik
        AnimatorDebugRootMotion rootMotion;
        // where foot contact comes from: "foot_goals" (its Foot IK / Foot Lock legs) or "none"
        const char* contactSource = "none";
        float slideMmS = 0.0f;              // the fastest foot in contact
        float footSpeedMps = 0.0f;          // that foot's speed relative to the root (root motion only)
        bool sliding = false;               // slideMmS over kAnimatorSlideWarningMmS
    };

    // a foot in contact moving faster than this shows the Inspector's slide warning
    inline constexpr float kAnimatorSlideWarningMmS = 100.0f;

    // Where the Animator GameObject is in the world: what makeSnapshot puts rig space into
    struct AnimatorRoot{
        Vector3 position = Vector3(0, 0, 0);
        Quaternion rotation;
        Vector3 scale = Vector3(1, 1, 1);
        Matrix4 matrix;
    };

    // A Transform's world placement, and the one of `entity` (false without a Transform). Defined in
    // render/SceneRender3D.cpp, a unit that reads Transforms already, so this one and the clip quality
    // tools stay out of Transform.h's fan-out (tools/check_fanout.py)
    AnimatorRoot animatorRoot(const Transform& transform);
    bool animatorRootOf(Scene* scene, Entity entity, AnimatorRoot& root);

    // `root` is the Animator GameObject's placement. An Animator that never evaluated gives !valid.
    AnimatorDebugSnapshot makeSnapshot(const AnimatorInstance& instance, const AnimatorComponent& component, const AnimatorRoot& root);

    enum class AnimatorDebugFormat{ Ratio, Millimetres, MetresPerSecond, DegreesPerSecond, Degrees, MillimetresPerSecond };

    struct AnimatorDebugField{
        const char* key;        // the JSON key (ik[i] or root_motion)
        const char* label;      // the Inspector's label
        const char* unit;
        AnimatorDebugFormat format;
    };

    inline constexpr AnimatorDebugField kAnimatorDebugFields[] = {
        {"weight", "Weight", "ratio", AnimatorDebugFormat::Ratio},
        {"reach_error_mm", "Reach Error", "mm", AnimatorDebugFormat::Millimetres},
        {"pelvis_offset_mm", "Pelvis Offset", "mm", AnimatorDebugFormat::Millimetres},
        {"lock_drift_mm", "Lock Drift", "mm", AnimatorDebugFormat::Millimetres},
        {"speed_mps", "Speed", "m/s", AnimatorDebugFormat::MetresPerSecond},
        {"yaw_deg_s", "Yaw", "deg/s", AnimatorDebugFormat::DegreesPerSecond},
        {"foot_speed_mps", "Foot Speed", "m/s", AnimatorDebugFormat::MetresPerSecond},
        {"slide_mm_s", "Slide", "mm/s", AnimatorDebugFormat::MillimetresPerSecond},
        {"accumulated_yaw", "Total Yaw", "deg", AnimatorDebugFormat::Degrees},
    };

    const AnimatorDebugField* findAnimatorDebugField(std::string_view key);
    // the number as the JSON holds it (rounded to 0.01 of its unit)
    double animatorDebugValue(double value);
    // for people: "0.80", "4.2 mm", "3.12 m/s", "4.1 deg/s", "280 mm/s", from animatorDebugValue
    std::string formatAnimatorDebugField(std::string_view key, double value);
    // "Two Bone", "Foot IK", ...
    const char* animatorIKTypeLabel(AnimatorIKType type);
    // the State column: "Reached", "Out of Reach", "Grounded", "No Ground", "Locked", "Free", "Off"
    std::string animatorIKStateLabel(const AnimatorDebugIK& ik);

    // The new live fields (R53) of ik[i] and of root_motion, merged into get_animator's objects
    void addAnimatorDebugIKJson(const AnimatorDebugIK& ik, nlohmann::json& item);
    void addAnimatorDebugRootMotionJson(const AnimatorDebugSnapshot& snapshot, nlohmann::json& rootMotion);

    // ---- the Scene view's Gizmos > Animation overlays (R49) ----

    struct AnimatorOverlayFlags{
        bool ik = false;
        bool rootMotion = false;
        bool footTrails = false;

        bool any() const { return ik || rootMotion || footTrails; }
    };

    struct AnimatorOverlayLine{
        Vector3 a = Vector3(0, 0, 0);
        Vector3 b = Vector3(0, 0, 0);
        Vector4 color = Vector4(1, 1, 1, 1);
    };

    // the colours of R49
    namespace overlaycolor{
        inline const Vector4 reached(0.25f, 0.9f, 0.3f, 1.0f);
        inline const Vector4 unreached(0.95f, 0.2f, 0.15f, 1.0f);
        inline const Vector4 chain(0.2f, 0.9f, 0.95f, 1.0f);
        inline const Vector4 pole(0.85f, 0.85f, 0.95f, 1.0f);
        inline const Vector4 ray(1.0f, 0.9f, 0.2f, 1.0f);
        inline const Vector4 hit(0.25f, 0.9f, 0.3f, 1.0f);
        inline const Vector4 miss(1.0f, 1.0f, 1.0f, 1.0f);
        inline const Vector4 locked(1.0f, 0.55f, 0.1f, 1.0f);
        inline const Vector4 planted(0.25f, 0.9f, 0.3f, 1.0f);
        inline const Vector4 rootPast(0.95f, 0.2f, 0.15f, 1.0f);
        inline const Vector4 rootFuture(0.25f, 0.9f, 0.3f, 1.0f);
    }
    // a foot trail's colour: green planted and still, yellow to red with the slide speed, a dim
    // white in the air
    Vector4 footTrailColor(bool contact, float slideMmS);

    // What the overlays remember of one Animator: the feet's and the root's last seconds
    struct AnimatorOverlayHistory{
        struct Sample{
            float time = 0.0f;              // AnimatorInstance::clock
            Vector3 position = Vector3(0, 0, 0);
            Vector4 color = Vector4(1, 1, 1, 1);
        };
        uint64_t evaluations = 0;           // the update the last samples came from
        std::vector<std::vector<Sample>> feet;  // per IK constraint (legs only)
        std::vector<Sample> root;
    };

    // Seconds of foot trail and of the root's past path; seconds of its predicted path
    inline constexpr float kAnimatorTrailSeconds = 2.0f;
    inline constexpr float kAnimatorFutureSeconds = 1.0f;

    // the lines of one Animator's overlays (history: the trails and the past path, may be null)
    void appendAnimatorOverlayLines(const AnimatorDebugSnapshot& snapshot, const AnimatorOverlayFlags& flags,
                                    const AnimatorOverlayHistory* history, std::vector<AnimatorOverlayLine>& out);

    class AnimatorOverlayPass{
    public:
        using Draw = std::function<void(Entity, const std::vector<AnimatorOverlayLine>&)>;

        // Once a frame: the Animators among `entities` that ran get a snapshot and their lines
        // through `draw`. With every flag off nothing happens at all and the history is dropped.
        void update(Scene* scene, const std::vector<Entity>& entities, const AnimatorOverlayFlags& flags, const Draw& draw);

        uint64_t snapshots() const { return snapshotCount; }
        uint64_t lines() const { return lineCount; }

    private:
        std::unordered_map<Entity, AnimatorOverlayHistory> histories;
        std::vector<AnimatorOverlayLine> scratch;
        uint64_t snapshotCount = 0;
        uint64_t lineCount = 0;
    };

}
