// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 3.2-3.3, 9.R1/R2/R9/R10/R16-R19, R43)
// - Purpose: the runtime state of one Animation Player (AnimationPlayerComponent::runtime) and
//   of one edit-mode clip preview (ClipPreviewSession), and the state-name rule. Internal to the
//   engine's player code and the editor, not game-facing API (anim:: is internal; public
//   signatures use jasperengine types: the read-only state view AnimationStateInfo is in
//   object/AnimationPlayer.h).
// - Owner: T0 (shape); T1b fills and drives it (ActionSystem players, ActionSystemPlayers.cpp)
//   and may add fields. Nothing outside the engine's player code writes these structs: the
//   editor and scripts go through AnimationPlayer (object/AnimationPlayer.h) and
//   ActionSystem::previewPlayer.
// - Thread-safety: main thread (owned by an ECS component / the editor preview).
// - Invariants:
//   * one state per clip entry (states[i] is AnimationPlayerComponent::clips[i] while the list
//     is unchanged; needsLoad rebuilds it, keeping time/weight of states whose name survives).
//   * time: once / clamp_forever states keep the plain time; loop and ping_pong states keep
//     time in [0, 2 * length) plus `cycles` (whole 2*length periods), so a long-running loop
//     never loses float precision (Lua `time` stays a float = the time in the clip).
//   * rest values are per (entity, channel), taken the first time the entity is bound and
//     before the player writes it, and kept for the whole run across rebinding, clip reloads
//     and state changes (R2). In a preview session rest is the preview snapshot (the editor
//     updates it when a scene edit changes a previewed channel: setRest).
//   * events and finish notifications are queued while sampling and dispatched after
//     flushPoseBlend, in time order (R16).

#ifndef ANIMATION_PLAYER_RUNTIME_H
#define ANIMATION_PLAYER_RUNTIME_H

#include "Export.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationEvent.h"
#include "animation/KeyframeSampling.h"
#include "component/AnimationPlayerComponent.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine::anim{

    // ---- state names (implemented; the one rule for runtime, Inspector, controller, MCP) ----

    // the entry's state name: `name`, else the sub-clip name ("hero.gltf#Walk" -> "Walk"), else the
    // .anim file stem ("anim/walk.anim" -> "walk")
    JASPERENGINE_API std::string defaultStateName(const AnimationPlayerClip& entry);
    // every entry's state name in list order; a name already taken gets " 2", " 3"... (the first
    // free one; runtime alias only, never written back)
    JASPERENGINE_API std::vector<std::string> playerStateNames(const std::vector<AnimationPlayerClip>& clips);

    // The rest values of one entity's channels (R2): what (1 - W) blends toward. morphWeights:
    // one per morph target of the MeshComponent (MAX_MORPHTARGETS); latticeOffsets: the full cage
    // by flat index. Keyed by Entity (ids are recycled): an entry is dropped when its entity fails
    // the bind validation (ClipBindings::boundNames / boundParents) or is destroyed.
    struct JASPERENGINE_API ChannelRest{
        bool hasTransform = false;
        Vector3 position = Vector3(0, 0, 0);
        Quaternion rotation;
        Vector3 scale = Vector3(1, 1, 1);
        bool hasMorph = false;
        std::vector<float> morphWeights;
        bool hasLattice = false;
        std::vector<Vector3> latticeOffsets;
    };
    using RestMap = std::unordered_map<Entity, ChannelRest>;

    // One clip's tracks resolved on a hierarchy
    struct JASPERENGINE_API ClipBindings{
        std::shared_ptr<const AnimationClip> clip;
        uint64_t clipGeneration = 0;        // AnimationClipPool generation bound to
        uint64_t hierarchyRevision = 0;     // EntityRegistry::hierarchyRevision() bound at
        std::vector<Entity> targets;        // per clip track, NULL_ENTITY = unresolved
        // per target: name and parent at bind (validate-then-re-resolve, R9)
        std::vector<std::string> boundNames;
        std::vector<Entity> boundParents;
        std::vector<KeyCursor> cursors;     // per clip track
        float nextUnresolvedRetry = 0.0f;   // re-resolve throttle for unresolved paths (4 Hz)
    };

    struct JASPERENGINE_API PlayerStateRuntime{
        std::string name;                   // unique (runtime " 2" alias applied)
        size_t entry = 0;                   // index into AnimationPlayerComponent::clips
        ClipBindings bindings;
        // load state (PoseAnimator pattern): failed + retry every 1 s, error logged once
        bool loadFailed = false;
        float retryTimer = 0.0f;
        bool errorLogged = false;
        std::string error;

        float time = 0.0f;
        uint64_t cycles = 0;                // loop / ping_pong: whole 2*length periods (see time)
        float speed = 1.0f;
        float weight = 0.0f;
        float targetWeight = 0.0f;
        float fadeSpeed = 0.0f;
        bool enabled = false;
        bool playing = false;
        bool stopOnFadeOut = false;
        bool startedThisUpdate = false;     // fire events at exactly 0 (R16)
        bool finishNotified = false;        // clamp_forever: onFinish once
        ClipWrapMode wrapOverride = ClipWrapMode::Default;  // Lua state.wrap
    };

    struct JASPERENGINE_API QueuedState{
        size_t state = 0;
        float fade = 0.0f;
    };

    // a callback to run after the pose is written: an event, or a finished state
    struct JASPERENGINE_API PendingPlayerCallback{
        bool finish = false;
        AnimationEvent event;               // finish: event.state is the finished state
    };

    class JASPERENGINE_API AnimationPlayerRuntime{
    public:
        std::vector<PlayerStateRuntime> states;
        std::deque<QueuedState> queue;
        RestMap rest;
        // waiting for a ModelComponent on or under the root to load (R10): no writes
        bool waitingForModel = false;
        // the loadedFilename of each model bound under the root: a change rebinds
        std::unordered_map<Entity, std::string> modelFiles;
        std::vector<PendingPlayerCallback> pending;
        // (clip, function) pairs already warned about having no handler (play mode, once)
        std::vector<std::string> warnedHandlers;

        // index into states, -1 when none
        int findState(const std::string& name) const;
    };

    // One edit-mode clip preview on a root (ActionSystem::previewPlayer): bindings and rest kept
    // between calls; the Animation window / MCP preview own it for the preview's lifetime and end
    // it with ActionSystem::restorePlayerPreview
    class JASPERENGINE_API ClipPreviewSession{
    public:
        Entity root = NULL_ENTITY;
        RestMap rest;
        // per previewed clip (by pointer identity of the AnimationClip); previewPlayer drops the
        // bindings of clips the current call does not name (a drag commits a new clip per frame)
        std::vector<ClipBindings> bindings;
        // the (entity, channel) entries this session wrote: what restorePlayerPreview puts back
        std::vector<std::pair<Entity, ClipChannel>> written;
        // fireEvents: the previous call's time per state (key: the state name, or the clip's
        // address for explicit clips)
        std::unordered_map<std::string, float> previousTimes;

        // a scene edit changed a previewed channel: the new value is its rest (R6)
        void setRest(Entity entity, const ChannelRest& value);
        // forget everything (the caller restored the scene)
        void reset();
    };

}

#endif //ANIMATION_PLAYER_RUNTIME_H
