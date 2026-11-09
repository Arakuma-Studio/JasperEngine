// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The live state of one Pose Animator (spec: pose animation phase 2, section 4.2): the loaded
// asset, the frames it plays, the playback clock, the live goToPose transition and the weight
// blend over the bones. No scene dependency: PoseAnimatorSystem feeds it the bone transforms and
// writes the results back, so the pose unit tests drive it directly.

#ifndef POSE_ANIMATOR_RUNTIME_H
#define POSE_ANIMATOR_RUNTIME_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "pose/PoseTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::pose {

    class PoseRig;

    // Advances a playback clock by dt * speed. A loop wraps into [0, length); a one-shot clamps
    // at the end it runs into (length going forward, 0 going backward) and stops (`playing` =
    // false). Returns true when a one-shot reached its end in this step.
    JASPERENGINE_API bool advancePlayback(float& time, bool& playing, float dt, float speed, float length, bool loop);

    // `time` inside the animation: wrapped when looping, clamped to [0, length] otherwise
    JASPERENGINE_API float playbackTime(float time, float length, bool loop);

    // The pose at `time` from frames sampled at f * length / (frames.size() - 1): the slerp of
    // the two frames around it (hips position lerped), or with `stepped` the frame before it.
    JASPERENGINE_API void sampleFrames(const std::vector<HumanPose>& frames, float length, bool loop, bool stepped,
                                       float time, HumanPose& out);

    // Shortest-path slerp that is safe for (nearly) equal rotations
    JASPERENGINE_API Quaternion slerpShortest(const Quaternion& a, const Quaternion& b, float t);

    // The weight blend of one bone (spec 4.2 "Apply": local = slerp(current, pose, weight)).
    // `current` is read from the bone transform every frame. When it still holds what the
    // animator wrote the frame before, nobody else (no clip) wrote the bone, and the pose under
    // the animator is the base remembered then: the blend stays put instead of creeping toward
    // the pose frame after frame, and weight 0 gives the rest or clip pose back.
    struct JASPERENGINE_API PoseBlendBone {
        bool rotationActive = false;
        Quaternion baseRotation;
        Quaternion writtenRotation;
        bool positionActive = false;
        Vector3 basePosition = Vector3(0, 0, 0);
        Vector3 writtenPosition = Vector3(0, 0, 0);

        // the value to write over `current`
        Quaternion blendRotation(const Quaternion& current, const Quaternion& pose, float weight);
        Vector3 blendPosition(const Vector3& current, const Vector3& pose, float weight);
        void reset() { rotationActive = false; positionActive = false; }
    };

    // A goToPose request: the key and its transition (fields < 0 take the animation's)
    struct JASPERENGINE_API PoseGoTo {
        int key = -1;
        float duration = -1.0f;
        float overshoot = -1.0f;
        float anticipation = -1.0f;
    };

    // The realised key poses and the frames of one animation on one rig (frames[f] at
    // f * length / (frames.size() - 1)): immutable, shared by every animator playing it
    struct JASPERENGINE_API PoseAnimationBuild {
        std::vector<HumanPose> keys;
        std::vector<HumanPose> frames;
        float length = 0.0f;
    };

    // Builds shared by animation file and rig (like PoseRigCache): get() returns the build of
    // `path` whose file text was `source` on `rig` while one is alive, else builds it (outside
    // the lock) and remembers it. Entries hold the build and the rig weakly: they go with the
    // last animator playing them (pruned on every get and by prune(); cleared when a project
    // closes). Thread safe.
    class JASPERENGINE_API PoseBuildCache {
    public:
        static PoseBuildCache& shared();

        // nullptr with `error` when the animation does not build (no poses, no frames)
        std::shared_ptr<const PoseAnimationBuild> get(const std::string& path, const std::string& source,
                                                      const std::shared_ptr<const PoseRig>& rig, const PoseAnimationData& data,
                                                      std::string* error);
        void prune();
        void clear();

        // the build of `data` on `rig`, not cached
        static std::shared_ptr<const PoseAnimationBuild> build(const PoseAnimationData& data, const PoseRig& rig, std::string* error);

    private:
        struct Entry {
            std::string path;
            std::string source;
            std::weak_ptr<const PoseRig> rig;
            std::weak_ptr<const PoseAnimationBuild> build;
        };
        // under the lock
        void pruneLocked();
        std::shared_ptr<const PoseAnimationBuild> findLocked(const std::string& path, const std::string& source,
                                                             const std::shared_ptr<const PoseRig>& rig) const;
        mutable std::mutex mutex;
        std::vector<Entry> entries;
    };

    class JASPERENGINE_API PoseAnimatorRuntime {
    public:
        // the asset; `failed` (with `error` logged once) disables the animator until the path changes
        std::string path;
        std::string source;   // the file text `data` was parsed from ("" = set directly): the shared build's key
        PoseAnimationData data;
        bool dataLoaded = false;
        bool failed = false;
        std::string error;
        // made for the pending load (PoseAnimatorComponent::needsLoad): parsed, waiting for the model
        bool loading = false;
        // a failed animator retries every second; the error last logged (not logged twice)
        float retryTimer = 0.0f;
        std::string loggedError;

        // the model it plays on (filled by PoseAnimatorSystem)
        Entity model = NULL_ENTITY;
        std::string modelFile;
        std::shared_ptr<const PoseRig> rig;
        std::array<Entity, HUMAN_BONE_COUNT> slotEntities{};   // NULL_ENTITY: slot unmapped
        // skeleton local = bridge * Transform local (ModelRig bridges: non-bone nodes folded into
        // the skeleton): what is written to a bone transform is bridgeRotationInverse * pose
        std::array<Quaternion, HUMAN_BONE_COUNT> bridgeRotationInverse;
        Matrix4 hipsBridgeInverse;

        // the realised key poses and the frames played (shared: PoseBuildCache)
        std::shared_ptr<const PoseAnimationBuild> built;
        float length = 0.0f;
        const std::vector<HumanPose>& keys() const;
        const std::vector<HumanPose>& frames() const;

        // the pose shown last (before the weight blend): where a live transition starts
        HumanPose displayed;
        bool hasDisplayed = false;

        // live transition (goToPose); a request made before the rig is ready waits in `pending`
        bool transitionActive = false;
        bool transitionArrived = false;   // settled: holds the key pose exactly
        bool transitionPaused = false;    // pause() during a transition freezes it
        float transitionTime = 0.0f;
        float transitionSettle = 0.0f;    // transitionSettleTime() of the current transition
        int transitionKey = -1;
        Transition transition;
        HumanPose transitionFrom;
        bool transitionFromGrounded = true;   // the start was on the floor (the ground correction's weight)
        PoseGoTo pending;
        bool pendingArrived = false;      // `pending` had settled before a rebuild: it holds the key at once

        // the weight blend per slot, and the bone entity each blend state was written to (the
        // bones to give their base pose back when the animator stops driving them)
        std::array<PoseBlendBone, HUMAN_BONE_COUNT> blend;
        std::array<Entity, HUMAN_BONE_COUNT> blendEntities{};

        // Realises the keys on `rig` (joint limits first when humanize.jointLimits, grounded with
        // the rig's skin) and computes the frames at the animation's frame rate. Uses `data`;
        // with `cache` and a `source` the build is shared (the same file text on the same rig).
        bool build(std::shared_ptr<const PoseRig> rig, std::string* error, PoseBuildCache* cache = nullptr);

        bool isLoaded() const { return !failed && rig && built && !built->frames.empty(); }
        int poseCount() const { return dataLoaded ? (int)data.poses.size() : 0; }
        int findPose(const std::string& name) const;
        bool stepped() const { return data.settings.stepRate > 0.0f; }
        // the key boundaries of the timeline (each transition's start and arrival), sorted:
        // Pose Frame Rate counts its steps from them (built on first use)
        mutable std::vector<float> keyBoundaries;
        // the component's Pose Frame Rate (PoseAnimatorSystem sets it each update): a live
        // transition steps on it too when the animation has no step rate of its own
        float poseFrameRate = 0.0f;

        // the playback pose at `time`; `poseFrameRate` > 0 holds it on whole steps counted
        // from the last key boundary (a transition's start or arrival, S48 R23)
        void samplePlayback(float time, HumanPose& out, float poseFrameRate = 0.0f) const;

        // Starts a live transition from the displayed pose to key `request.key`; false when the
        // key does not exist (or nothing is loaded yet). Every bone eases with the phase 1 ease
        // (spec 4.8) of the transition, delayed by overlap * group weight * depth and loosened by
        // looseness * group weight * depth, as in playback; the step rate quantises its time.
        bool startTransition(const PoseGoTo& request);
        // advances the live transition by dt and writes its pose: the key pose exactly once
        // every bone's spring has settled (|ease - 1| < 1e-4 from then on)
        void sampleTransition(float dt, HumanPose& out);
        // the eased pose `time` seconds into the current transition, with the frames' joint
        // limits (humanize.jointLimits) and ground correction (4.8 frames: g = 1 from a start on
        // the floor to a grounded key, else faded with smoothstep(clamp01(e_hips)))
        void transitionPose(float time, HumanPose& out) const;
        // seconds until the current transition has settled
        float transitionSettleTime() const;

        // A rebuild (a new runtime for the same file): the goToPose target of `previous` (its
        // pending request or its live transition, settled or not) becomes this one's `pending`
        void keepGoTo(const PoseAnimatorRuntime& previous);
        // starts `pending` once loaded (settled at once when it had settled before a rebuild)
        bool startPending();
    };

}

#endif // POSE_ANIMATOR_RUNTIME_H
