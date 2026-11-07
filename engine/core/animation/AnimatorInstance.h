// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R1-R15, D3, D5-D12)
// - Purpose: the runtime of one Animator (AnimatorComponent::runtime): parameter values, the
//   state machine of every layer, montage slots, IK state, the bound rig and clips, and the
//   evaluation that turns them into a pose, events and root motion each update.
// - Lifecycle: AnimatorSystem fills and binds it on the main thread (setController, bind, the
//   clip and montage loads), then calls advance() + evaluate() from its parallel wave (one
//   instance per job, no shared mutable state), then reads `output` on the main thread. Scripts
//   and tools call the control functions (setFloat, play, playMontage ...) on the main thread
//   between updates.
// - State machine (D5): Any State transitions first, then the current state's, in order; at
//   most one transition starts per layer and update; conditions ANDed; triggers stay set until
//   a transition that tests them is taken (or resetTrigger); exit time below 1 is tested every
//   loop at that fraction, from 1 once; an interrupted transition freezes the layer's last pose
//   as a snapshot it blends from; sub-state machines enter through their entries then their
//   default; Exit continues with the parent's transitions out of that machine, else the
//   parent's entry. A seek (play(state, time)) fires no event.
// - Allocation: after the first evaluation with a given controller, rig and montage set, an
//   update allocates nothing (scratch, poses and output lists are sized at bind and reused).
// - Thread-safety: see Lifecycle.

#ifndef ANIMATOR_INSTANCE_H
#define ANIMATOR_INSTANCE_H

#include "Export.h"
#include "action/Ease.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationEvent.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorPose.h"
#include "animation/AnimatorRetarget.h"
#include "animation/AnimatorRig.h"
#include "animation/BlendTree.h"
#include "animation/CompiledController.h"
#include "component/AnimatorComponent.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::anim{

    // One clip the Animator plays (a controller clip slot after overrides, or a montage's clip)
    struct JASPERENGINE_API AnimatorClipSlot{
        AnimatorAssetRef ref;               // as referenced
        std::string path;                   // what is loaded (after clipOverrides)
        std::shared_ptr<const AnimationClip> clip;
        uint64_t generation = 0;
        bool failed = false;
        std::string error;
        float retry = 0.0f;
        bool reported = false;
        bool bound = false;                 // bound to the current rig
        RigClipBinding binding;             // the tracks that play by path (all of a clip that is not retargeted)
        RigClipCursors cursors;
        // the one retarget path (AnimatorRetarget.h): a humanoid clip, or a generic clip of another
        // humanoid model (Retarget Auto), or a clip whose state asks for humanoid playback
        RetargetBinding retarget;
        std::vector<KeyCursor> retargetCursors;     // per clip track
        bool retargeted = false;            // plays through the humanoid path
        bool humanoid = false;              // the controller asks for the humanoid path (a state's Retarget, Mirror, Foot IK)
        ClipRootMotion rootMotion;
        // root motion: the bone, its tracks, the fixed transform of its parent chain
        int rootBone = -1;
        int rootPositionTrack = -1;
        int rootRotationTrack = -1;
        Affine rootParent;
        Affine rootParentInverse;
        Quaternion rootParentRotation;
        Vector3 rootStart = Vector3(0, 0, 0);   // the bone's rig-space position and heading at time 0
        float rootStartYaw = 0.0f;
        // one lap of the root (P and the heading change at 0 and at the clip's length, baked
        // components held): what every looping frame's delta adds, computed at bind
        Vector3 lapStart = Vector3(0, 0, 0);
        Vector3 lapEnd = Vector3(0, 0, 0);
        float lapStartHeading = 0.0f;
        float lapEndHeading = 0.0f;
        // additive reference pose (computed when an additive layer first plays it)
        bool hasReference = false;
        AdditiveReference referenceKind = AdditiveReference::FirstFrame;
        float referenceTime = 0.0f;
        uint8_t referenceFlags = 0;         // the Mirror (1) and Foot IK (2) it was sampled with
        Pose reference;

        float length() const { return clip ? clip->getLength() : 0.0f; }
        bool ready() const { return clip && bound; }
    };

    struct JASPERENGINE_API AnimatorStateRun{
        int state = -1;
        double time = 0.0;                  // normalised, unbounded (loops count up)
        double previous = 0.0;              // the time before this update's advance
        bool entered = false;               // entered during this update (events at its start fire)
        // entered through a transition (or as a default state) and its events from the start not
        // fired yet: they fire, from `start`, once it leads its layer (or every clip fires)
        bool startPending = false;
        double start = 0.0;
        float length = 0.0f;                // seconds of one cycle at speed 1 (the motion's weighted length)
    };

    struct JASPERENGINE_API AnimatorLayerRun{
        AnimatorStateRun current;
        AnimatorStateRun next;
        int transition = -1;                // the active transition, -1 when none
        float elapsed = 0.0f;
        float duration = 0.0f;
        bool snapshot = false;              // current is the frozen pose of an interrupted transition
        // the state a 0 s transition left in this update: its clip events of the step still fire
        // (the step's events first, then the state notes)
        AnimatorStateRun instantSource;
        float weight = 1.0f;
        float targetWeight = 1.0f;          // setLayerWeight with a fade: weight moves toward it
        float weightFadeSpeed = 0.0f;       // per second (0: no fade)
        Pose output;                        // this update's layer pose (a delta for additive layers)
        Pose snapshotPose;
        bool hasOutput = false;
        std::vector<float> mask;            // per bone (pose stride), null when the layer has no mask
        std::vector<float> channelMask;
        bool masked = false;
        float rootMask = 1.0f;              // the mask's Root body part
        // this update's root motion contribution (rig space) and the weight of the clips giving it
        Vector3 rootDelta = Vector3(0, 0, 0);
        float rootYaw = 0.0f;
        float rootWeight = 0.0f;
    };

    struct JASPERENGINE_API AnimatorMontageRun{
        enum class Phase : uint8_t { Waiting, BlendIn, Playing, BlendOut, Ended };
        std::string path;
        std::shared_ptr<const MontageDef> montage;
        int clip = -1;                      // index into AnimatorInstance::clips
        Phase phase = Phase::Waiting;
        float time = 0.0f;                  // seconds in the clip
        float previous = 0.0f;
        int section = 0;
        std::vector<std::string> nextOverride;      // per section: set-next-section (empty: the asset's)
        std::vector<bool> hasNextOverride;
        float rate = 1.0f;
        float weight = 0.0f;                // eased blend weight
        float blendElapsed = 0.0f;
        float blendDuration = 0.0f;
        float blendFrom = 0.0f;
        EaseType ease = EaseType::LINEAR;
        bool interrupted = false;
        bool startedThisUpdate = false;
        int layer = -1;
        Vector3 rootDelta = Vector3(0, 0, 0);
        float rootYaw = 0.0f;
    };

    // The resolved runtime of one IK constraint (parallel to AnimatorComponent::ik)
    struct JASPERENGINE_API AnimatorIKRun{
        int bones[16];
        int boneCount = 0;
        int pelvis = -1;
        float boneWeights[4] = {1, 1, 1, 1};
        // world-space inputs taken on the main thread before the wave
        bool hasTarget = false;
        Vector3 targetWorld = Vector3(0, 0, 0);
        bool hasTargetRotation = false;
        Quaternion targetRotationWorld;
        bool hasPole = false;
        Vector3 poleWorld = Vector3(0, 0, 0);
        // script overrides (setIKTarget / setIKWeight / setLookAt)
        bool scriptTarget = false;
        Vector3 scriptTargetWorld = Vector3(0, 0, 0);
        float scriptWeight = -1.0f;         // < 0: none
        // The outcome of the last solve, in world space, for scripts (isIKReached), get_animator
        // and debug overlays. Cleared while nothing solves (weight 0, disabled, culled).
        bool reached = false;
        float reachError = 0.0f;            // metres (anim::IKResult)
        float solvedWeight = 0.0f;          // the effective weight the solve ran with
        Vector3 solvedTargetWorld = Vector3(0, 0, 0); // what it aimed at: FootIK's spring target, FootLock's lock too
        // FootIK: last update's foot position (world, for the ray), the hit, the spring
        bool hasFoot = false;
        Vector3 footWorld = Vector3(0, 0, 0);
        bool hit = false;
        Vector3 hitPoint = Vector3(0, 0, 0);    // rig space
        Vector3 hitNormal = Vector3(0, 1, 0);   // rig space
        float offset = 0.0f;
        float offsetVelocity = 0.0f;
        // FootLock
        bool locked = false;
        Vector3 lockWorld = Vector3(0, 0, 0);
        float lockWeight = 0.0f;
        bool hadPrevious = false;
        Vector3 previousWorld = Vector3(0, 0, 0);
    };

    struct JASPERENGINE_API AnimatorStateNote{
        bool enter = true;
        int layer = 0;
        int state = -1;
    };

    struct JASPERENGINE_API AnimatorMontageNote{
        int montage = -1;                   // index into AnimatorInstance::montages
        bool interrupted = false;
    };

    // An event that fired (plain data, so the wave allocates nothing; AnimatorSystem builds the
    // AnimationEvent when it dispatches)
    struct JASPERENGINE_API AnimatorPendingEvent{
        int clip = -1;                      // clip slot (its clip's event `index`)
        int index = -1;
        int montage = -1;                   // a montage's own event: the montage and its `montageEvent`
        int montageEvent = -1;
        int state = -1;                     // the state that played it (-1: a montage)
        int layer = 0;
        double position = 0.0;              // where it was crossed (ordering within the layer)
    };

    // An audio cue that a clip crossed (S40 R4), fired by AnimatorSystem on the main thread
    struct JASPERENGINE_API AnimatorPendingCue{
        int clip = -1;                      // clip slot (its clip's cue `index`)
        int index = -1;
        int state = -1;                     // the state that played it (-1: a montage)
        int montage = -1;
        int layer = 0;
        double position = 0.0;              // where it was crossed on the clip's time line
        float fraction = 0.0f;              // where in the update's step (0..1): the start's place in the frame
        float speed = 1.0f;                 // the state's speed (or the montage's rate): follow_speed
    };

    // the voice of a state_exit cue (S40 R7): stopped when its state exits (or its montage ends)
    struct JASPERENGINE_API AnimatorCueVoice{
        int layer = 0;
        int state = -1;
        int montage = -1;
        uint32_t id = 0;
        uint32_t generation = 0;
        Entity source = NULL_ENTITY;
    };

    // What one update produced (read on the main thread after the wave)
    struct JASPERENGINE_API AnimatorOutput{
        std::vector<AnimatorPendingEvent> events;
        std::vector<AnimatorPendingCue> cues;
        std::vector<AnimatorStateNote> states;
        std::vector<AnimatorMontageNote> montages;
        bool hasRootMotion = false;
        Vector3 rootDeltaPosition = Vector3(0, 0, 0);   // rig space (the Animator entity's local space)
        float rootDeltaYaw = 0.0f;                      // radians about the rig's up (+y)
        bool sampled = false;                           // the pose was evaluated and written
    };

    // the main-thread inputs of one update
    struct JASPERENGINE_API AnimatorUpdateContext{
        float dt = 0.0f;
        bool sample = true;                 // false: culled (time, states, events, root motion only)
        Matrix4 rootWorld;                  // the Animator entity's world matrix (last frame)
        Matrix4 rootWorldInverse;
        Quaternion rootWorldRotation;
    };

    class JASPERENGINE_API AnimatorInstance{
    public:
        std::shared_ptr<const CompiledController> controller;
        std::string controllerPath;         // the component's controller path it was loaded for
        uint64_t controllerGeneration = 0;
        std::string controllerError;

        std::vector<float> parameters;
        std::vector<float> velocities;      // setFloat damping state
        // setFloatTarget: per parameter the value damped toward every update (dampTimes 0: none)
        std::vector<float> dampTargets;
        std::vector<float> dampTimes;
        // per parameter: a trigger a transition consumed this update (reset after every layer)
        std::vector<uint8_t> consumedTriggers;
        std::vector<AnimatorLayerRun> layers;
        std::vector<AnimatorClipSlot> clips;            // controller slots, then montage clips
        size_t controllerClipCount = 0;
        std::vector<AnimatorMontageRun> montages;
        std::vector<AnimatorIKRun> ik;

        AnimatorRig rig;
        AnimatorRestMap rest;
        bool bound = false;
        uint64_t hierarchyRevision = 0;
        std::string modelPath;              // the Animator's model (retarget target sidecar)
        // the ModelComponents on or under the root at bind and the file each had loaded: a
        // pending load waits, another file rebinds
        std::vector<std::pair<Entity, std::string>> models;
        float rebindRetry = 0.0f;           // unresolved paths: retried at 4 Hz after hierarchy changes
        bool bindFailed = false;            // another Animator owns one of its entities
        std::vector<uint64_t> maskGenerations;  // per layer: the mask's pool generation at bind
        RetargetTarget humanoid;            // the rig's humanoid (retargeting, slot names, root motion)
        std::vector<bool> writeMask;        // per bone: written to its entity (writeBones)
        std::string bindError;
        // names a call used that the controller lacks (warned once each)
        std::vector<std::string> warnedNames;
        std::vector<uint64_t> warnedKeys;   // warnOnce(key): the per-event warnings, no string built

        AnimatorOutput output;
        // main thread (AnimatorSystem): the state_exit cues' voices
        std::vector<AnimatorCueVoice> cueVoices;
        float clock = 0.0f;                 // seconds the Animator ran (retry throttles)
        uint64_t evaluations = 0;

        // ---- set up (main thread) ----

        // a new or changed controller: parameters to their defaults (values of parameters with the
        // same name and type are kept), layers from their default states (states whose path still
        // exists keep their time), clip slots rebuilt
        void setController(const std::shared_ptr<const CompiledController>& compiled, uint64_t generation,
                           const std::vector<AnimatorClipOverride>& overrides);
        // the clip slots' paths after `overrides` (a slot whose path changes drops its clip)
        void applyOverrides(const std::vector<AnimatorClipOverride>& overrides);
        // every path the rig must hold for the loaded clips, masks and IK of `component`
        void rigRequests(const AnimatorComponent& component, std::vector<std::string>& paths,
                         std::vector<std::pair<std::string, std::pair<ClipChannel, int>>>& channelPaths,
                         std::vector<RigPropertyRequest>* propertyPaths = nullptr) const;
        // builds the rig and binds every loaded clip, the masks and the IK constraints
        void bind(Scene* scene, Entity root, const AnimatorComponent& component,
                  const std::vector<std::shared_ptr<const AvatarMaskDef>>& masks);
        // a clip slot got (another) clip: bind it to the current rig (and its retargeting)
        void bindClip(size_t slot, const AnimatorComponent& component);
        // the bone of a path or humanoid slot name, -1 when none
        int resolveBone(const std::string& pathOrSlot) const;

        // ---- control (main thread, between updates) ----

        int findParameter(const std::string& name) const;
        bool setFloat(const std::string& name, float value);
        bool setFloat(const std::string& name, float value, float dampTime, float deltaTime);
        // a Float parameter SmoothDamps toward `value` once per update until another set
        // (upstream's player setFloat(name, value, dampTime)); others are set at once
        bool setFloatTarget(const std::string& name, float value, float dampTime);
        bool setInteger(const std::string& name, int value);
        bool setBool(const std::string& name, bool value);
        bool setTrigger(const std::string& name);
        bool resetTrigger(const std::string& name);
        float getParameter(const std::string& name) const;     // 0 when unknown
        // the state at once (normalizedTime < 0: from its start, or kept when it is the current one)
        bool play(const std::string& state, int layer, float normalizedTime);
        // a transition to the state (duration in source-state lengths, or seconds when fixed)
        bool crossFade(const std::string& state, float duration, bool fixed, int layer, float offset);
        void setLayerWeight(int layer, float weight);
        // weight reached linearly over `fade` seconds (<= 0: at once)
        void setLayerWeight(int layer, float weight, float fade);
        // every layer back to its default state (transitions cancelled, montages stopped,
        // parameters kept): Unity's Animator enabled again (upstream's player stop() and enable)
        void restart();
        float getLayerWeight(int layer) const;
        int getLayerIndex(const std::string& name) const;

        // a montage (its asset loaded by the caller): blends in on its slot's layer; a montage
        // playing on that slot blends out (interrupted)
        bool playMontage(const std::string& path, const std::shared_ptr<const MontageDef>& montage, size_t clipSlot,
                         float rate, const std::string& section);
        // every playing montage (or those of `path`) blends out over `blendOut` (< 0: its own)
        void stopMontage(const std::string* path, float blendOut);
        bool isMontagePlaying() const;
        const AnimatorMontageRun* activeMontage() const;
        bool montageJumpToSection(const std::string& section);
        bool montageSetNextSection(const std::string& from, const std::string& to);
        // the clip slot of a montage clip path, added when new (the caller loads it)
        size_t montageClipSlot(const AnimatorAssetRef& ref);

        // ---- update (any thread: the system's wave) ----

        // advances the layers' state machines and the montages, collects events and root motion;
        // then, when context.sample, evaluates the pose (pose(), rigWorld()); the caller writes it
        void update(const AnimatorComponent& component, const AnimatorUpdateContext& context);
        // the final pose of the last evaluation (local) and its rig space
        const Pose& pose() const { return finalPose; }
        const std::vector<Affine>& rigWorld() const { return spaceWorld; }
        const std::vector<Vector3>& rigPositions() const { return spacePosition; }
        const std::vector<Quaternion>& rigRotations() const { return spaceRotation; }

        // ---- queries ----

        struct StateInfo{
            int state = -1;
            std::string name;
            std::string fullPath;
            std::string tag;
            float normalizedTime = 0.0f;
            float length = 0.0f;
            bool loop = false;
            float speed = 1.0f;             // the state's speed times its speed parameter
            bool mirror = false;            // the state's humanoid playback flags (§12)
            bool footIK = false;
        };
        StateInfo currentState(int layer) const;
        StateInfo nextState(int layer) const;
        bool isInTransition(int layer) const;
        float transitionProgress(int layer) const;

        // nothing solves this frame (disabled, culled): no constraint reports a stale outcome
        void clearIKResults();

        // true the first time a message is given (the caller logs it): one warning per name
        bool warnOnce(const std::string& message);
        // the same by a key (a hash the caller makes without allocating)
        bool warnOnce(uint64_t key);

        // The bytes of the Animator's runtime state (R20's 8 KB): the instance itself, its
        // parameters, layer, montage and IK runs and the outputs' buffers; not the rig, the poses,
        // the per-bone masks, the clip slots' bindings and cursors nor the humanoid (they grow
        // with the bones and the clips, and are what a pose costs, not the controller's state).
        size_t runtimeStateBytes() const;

    private:
        // scratch (sized at bind)
        Pose temp;
        Pose accumulate;
        Pose source;
        Pose destination;
        Pose montagePose;
        Pose finalPose;
        std::vector<MotionLeaf> leaves;
        std::vector<Affine> spaceWorld;
        std::vector<Vector3> spacePosition;
        std::vector<Quaternion> spaceRotation;
        std::vector<Vector3> spaceScale;

        void resetLayer(size_t layer);
        void enterNode(size_t layer, TransitionTargetKind kind, int target, double offset, AnimatorStateRun& run, int depth);
        bool resolveExit(int machine, TransitionTargetKind& kind, int& target, int depth);
        bool conditionsHold(const CompiledTransition& transition) const;
        void consumeTriggers(const CompiledTransition& transition);
        bool exitTimeReached(const CompiledTransition& transition, const AnimatorStateRun& run) const;
        bool tryTransitions(size_t layer, uint32_t begin, uint32_t count, bool anyState, int stopAt, const AnimatorStateRun& from);
        void startTransition(size_t layer, int transition);
        void advanceLayer(size_t layer, float dt);
        float stateSpeed(int state) const;
        float motionLength(const CompiledMotion& motion);
        bool stateLoops(int state, int clipSlot) const;
        // a looping state plays the clip back and forth (forced ping_pong, or a ping-pong clip
        // under inherit / loop)
        bool statePingPong(int state, int clipSlot) const;
        void collectLayer(size_t layer, const AnimatorComponent& component);
        void collectLeaves(const AnimatorStateRun& run, bool next, float weight, bool leaderOnly, float& leaderWeight, int& leaderIndex);
        void advanceMontages(float dt, const AnimatorComponent& component);
        void evaluateLayer(size_t layer, const AnimatorComponent& component);
        void evaluateMotion(size_t layer, const AnimatorStateRun& run, Pose& out, const AnimatorComponent& component);
        void sampleSlot(size_t slot, float clipTime, Pose& out, bool additive, AdditiveReference reference, float referenceTime,
                        const AnimatorComponent& component, bool mirror = false, bool footIK = false);
        // the humanoid path of a retargeted slot at `clipTime` into `out` (holding the rest elsewhere)
        void sampleHumanoid(AnimatorClipSlot& slot, float clipTime, Pose& out, bool mirror, bool footIK, KeyCursor* cursors) const;
        // the clip slot plays through the humanoid path: its source (null: a humanoid clip), false when it plays by path
        bool humanoidSource(const AnimatorClipSlot& slot, const AnimatorComponent& component,
                            std::shared_ptr<const RetargetSource>& source, std::string* error) const;
        void removeRootMotion(const AnimatorClipSlot& slot, Pose& out) const;
        // P(t) with the baked components held at the start, h(t) the heading change since time 0
        void rootPoint(const AnimatorClipSlot& slot, float clipTime, Vector3& position, float& heading) const;
        bool rootSample(const AnimatorClipSlot& slot, float clipTime, Vector3& position, Quaternion& rotation) const;
        void rootDelta(const AnimatorClipSlot& slot, double from, double to, bool loops, bool pingPong, Vector3& delta, float& yaw) const;
        void applyMontages(size_t layer, Pose& out, const AnimatorComponent& component);
        void runIK(const AnimatorComponent& component, const AnimatorUpdateContext& context);
        void fireEvents(int slot, ClipWrapMode wrap, double from, double to, bool includeStart, int state, int layer);
        struct LeafRun{
            int slot = -1;
            float weight = 0.0f;
            double from = 0.0, to = 0.0;    // unwrapped clip seconds
            bool loops = false, pingPong = false, includeStart = false;
            int state = -1;
            bool next = false;              // a leaf of the layer's next state (else its current)
            double pendingStart = -1.0;     // clip seconds of a start its events did not fire at yet (-1: none)
        };
        std::vector<LeafRun> leafRuns;
        std::vector<CrossedEvent> crossed;
    };

}

#endif //ANIMATOR_INSTANCE_H
