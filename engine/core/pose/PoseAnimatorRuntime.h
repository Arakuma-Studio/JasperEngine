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
#include <memory>
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

    class JASPERENGINE_API PoseAnimatorRuntime {
    public:
        // the asset; `failed` (with `error` logged once) disables the animator until the path changes
        std::string path;
        PoseAnimationData data;
        bool dataLoaded = false;
        bool failed = false;
        std::string error;
        // made for the pending load (PoseAnimatorComponent::needsLoad): parsed, waiting for the model
        bool loading = false;

        // the model it plays on (filled by PoseAnimatorSystem)
        Entity model = NULL_ENTITY;
        std::string modelFile;
        std::shared_ptr<const PoseRig> rig;
        std::array<Entity, HUMAN_BONE_COUNT> slotEntities{};   // NULL_ENTITY: slot unmapped
        // skeleton local = bridge * Transform local (ModelRig bridges: non-bone nodes folded into
        // the skeleton): what is written to a bone transform is bridgeRotationInverse * pose
        std::array<Quaternion, HUMAN_BONE_COUNT> bridgeRotationInverse;
        Matrix4 hipsBridgeInverse;

        // the realised key poses and the frames played (frames[f] at f * length / (frames.size() - 1))
        std::vector<HumanPose> keys;
        std::vector<HumanPose> frames;
        float length = 0.0f;

        // the pose shown last (before the weight blend): where a live transition starts
        HumanPose displayed;
        bool hasDisplayed = false;

        // live transition (goToPose); a request made before the rig is ready waits in `pending`
        bool transitionActive = false;
        bool transitionArrived = false;   // settled: holds the key pose exactly
        float transitionTime = 0.0f;
        float transitionSettle = 0.0f;    // transitionSettleTime() of the current transition
        int transitionKey = -1;
        Transition transition;
        HumanPose transitionFrom;
        PoseGoTo pending;

        std::array<PoseBlendBone, HUMAN_BONE_COUNT> blend;

        // Realises the keys on `rig` (joint limits first when humanize.jointLimits, grounded with
        // the rig's skin) and computes the frames at the animation's frame rate. Uses `data`.
        bool build(std::shared_ptr<const PoseRig> rig, std::string* error);

        bool isLoaded() const { return !failed && rig && !frames.empty(); }
        int poseCount() const { return dataLoaded ? (int)data.poses.size() : 0; }
        int findPose(const std::string& name) const;
        bool stepped() const { return data.settings.stepRate > 0.0f; }

        // the playback pose at `time`
        void samplePlayback(float time, HumanPose& out) const;

        // Starts a live transition from the displayed pose to key `request.key`; false when the
        // key does not exist (or nothing is loaded yet). Every bone eases with the phase 1 ease
        // (spec 4.8) of the transition, delayed by overlap * group weight * depth and loosened by
        // looseness * group weight * depth, as in playback; the step rate quantises its time.
        bool startTransition(const PoseGoTo& request);
        // advances the live transition by dt and writes its pose: the key pose exactly once
        // every bone's spring has settled (|ease - 1| < 1e-4 from then on)
        void sampleTransition(float dt, HumanPose& out);
        // the eased pose `time` seconds into the current transition
        void transitionPose(float time, HumanPose& out) const;
        // seconds until the current transition has settled
        float transitionSettleTime() const;
    };

}

#endif // POSE_ANIMATOR_RUNTIME_H
