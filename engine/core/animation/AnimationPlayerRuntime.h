// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 3.2-3.3, 9.R1/R2/R9/R10/R16-R19, R43)
// - Purpose: the runtime state of one Animation Player (AnimationPlayerComponent::runtime) and
//   of one edit-mode clip preview (ClipPreviewSession), and the state-name rule. Internal to the
//   engine's player code and the editor, not game-facing API (this header and ModelSubClips are
//   internal; ClipBinding and KeyframeSampling are the public anim:: helpers, JasperEngine.h; game
//   signatures use jasperengine types: the read-only state view AnimationStateInfo is in
//   object/AnimationPlayer.h).
// - Filled and driven by ActionSystem's players (ActionSystemPlayers.cpp). Nothing outside the
//   engine's player code writes these structs: the editor and scripts go through
//   AnimationPlayer (object/AnimationPlayer.h) and ActionSystem::previewPlayer.
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
//   * layers (anim layers spec 7.2): one LayerRuntime per effective layer (playerLayers), rebuilt
//     with the states on needsLoad keeping each surviving layer's runtime weight, blend and mask
//     by name (an authored value that changed since is taken instead); each layer has its own
//     queue (R17 per layer). A state's layer is its entry's (clamped to the layers), or the Lua
//     override (`layerOverride`).
//   * the layered evaluator (ActionSystem::evaluateLayeredPlayer) runs for every player that is
//     not isDefaultLayering; the per-track mask weights (`trackMask`) and the slot table of the
//     pose scratch are computed at bind (when a stamp changes), never per frame.

#ifndef ANIMATION_PLAYER_RUNTIME_H
#define ANIMATION_PLAYER_RUNTIME_H

#include "Export.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationEvent.h"
#include "animation/AvatarMaskWeights.h"
#include "animation/ControllerDef.h"
#include "animation/HumanoidClip.h"
#include "animation/KeyframeSampling.h"
#include "animation/PropertyBindings.h"
#include "component/AnimationPlayerComponent.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine::anim{


    // ---- state names (the one rule for runtime, Inspector, controller, MCP) ----

    // the entry's state name: `name`, else the sub-clip name ("hero.gltf#Walk" -> "Walk"), else the
    // .anim file stem ("anim/walk.anim" -> "walk")
    JASPERENGINE_API std::string defaultStateName(const AnimationPlayerClip& entry);
    // every entry's state name in list order; a name already taken gets " 2", " 3"... (the first
    // free one; runtime alias only, never written back)
    JASPERENGINE_API std::vector<std::string> playerStateNames(const std::vector<AnimationPlayerClip>& clips);

    // The rest values of one entity's channels (R2): what (1 - W) blends toward. morphWeights:
    // one per morph target of the MeshComponent (MAX_MORPHTARGETS); latticeOffsets: the full cage
    // by flat index. Keyed by Entity: an entry is dropped when its entity is destroyed (a renamed
    // or reparented entity that fails the bind validation keeps it, R2).
    // The rest of one property of an entity (anim-properties spec 5.8): the whole value when
    // first bound, in key space (what blends use) and as stored (what a preview restore writes
    // back: exact, no key-space round trip), the binding it was read through and the row's
    // snapshot (a sprite's texture rect, an anchor preset)
    struct JASPERENGINE_API PropertyRest{
        bool has = false;
        float value[4] = {0, 0, 0, 0};
        float stored[4] = {0, 0, 0, 0};
        bool hasSnapshot = false;
        float snapshot[4] = {0, 0, 0, 0};
        PropertyBinding binding;
    };
    // reads a bound property's rest now (stored, key space, the row's snapshot); false when it
    // cannot be read (`rest` unchanged then). The binding is not copied
    JASPERENGINE_API bool readPropertyRest(Scene* scene, Entity entity, const PropertyBinding& binding, PropertyRest& rest);
    // writes a rest back exactly: its stored value through its binding, then the row's snapshot
    JASPERENGINE_API bool restorePropertyRest(Scene* scene, Entity entity, const PropertyRest& rest);

    struct JASPERENGINE_API ChannelRest{
        bool hasTransform = false;
        Vector3 position = Vector3(0, 0, 0);
        Quaternion rotation;
        Vector3 scale = Vector3(1, 1, 1);
        bool hasMorph = false;
        std::vector<float> morphWeights;
        bool hasLattice = false;
        std::vector<Vector3> latticeOffsets;
        // by property key (internPropertyKey; nodes stay put on insert)
        std::unordered_map<uint32_t, PropertyRest> properties;
    };
    using RestMap = std::unordered_map<Entity, ChannelRest>;

    // One clip's tracks resolved on a hierarchy
    struct JASPERENGINE_API ClipBindings{
        std::shared_ptr<const AnimationClip> clip;
        uint64_t clipGeneration = 0;        // AnimationClipPool generation bound to
        uint64_t hierarchyRevision = 0;     // EntityRegistry::hierarchyRevision() bound at
        uint64_t checkedFrame = 0;          // the player update that last looked at them
        std::vector<Entity> targets;        // per clip track, NULL_ENTITY = unresolved
        // per clip track, each target's rest resolved at bind (empty when bound without a rest map:
        // a preview's). Nodes of the player's RestMap: stable, and an erased one's target fails
        // validation before anything samples
        std::vector<ChannelRest*> rests;
        std::vector<KeyCursor> cursors;     // per clip track
        float nextUnresolvedRetry = 0.0f;   // re-resolve throttle for unresolved paths (4 Hz)
        // the root's index in the Transform array at bind (SIZE_MAX without a Transform): a
        // hierarchy change that shifts every path entity like the root moved nothing under it (a
        // spawn elsewhere), any other shift re-resolves (R9)
        size_t rootIndex = SIZE_MAX;
        // every entity the resolved paths go through (targets and their ancestors below the
        // root), with its name, parent, Transform index and "#N" (its rank among the earlier
        // same-named siblings) at bind: what the validation checks (R9)
        std::vector<Entity> pathEntities;
        std::vector<std::string> pathNames;
        std::vector<Entity> pathParents;
        std::vector<size_t> pathIndices;
        std::vector<int> pathSiblingIndices;
        bool resolved = false;              // resolved at least once for `clip`
        size_t resolvedCount = 0;           // tracks whose path resolved
        // a process-wide number taken on every resolve: what was computed from the targets
        // (mask weights, slots) is current while it is unchanged
        uint64_t serial = 0;
        // per clip track when the clip has property tracks (empty otherwise): how each property
        // track bound (a track whose binding is not Bound has no target); the property tracks
        // that did not bind (missing components are looked for again like unresolved paths)
        std::vector<PropertyBinding> properties;
        size_t unboundProperties = 0;
        // bound tracks on Lua script fields: their script instance is checked every update (a
        // reload or a removed script binds again, spec P4)
        size_t scriptProperties = 0;
        // a humanoid clip (anim-retarget 7.4): its realisation on the root's humanoid rig (null:
        // a generic clip, or no rig: its humanoid tracks are unresolved); the generic tracks of a
        // humanoid clip that did not resolve by path are bound by unique bone name (`byName`)
        std::shared_ptr<HumanoidBinding> humanoid;
        std::vector<bool> byName;
        // a Retarget entry playing a generic clip: the clip's source model's rig (the one retarget
        // path reads the clip through it, as the Animator does; null: none, or it failed), and
        // `retarget` the entry asked for it (a source that failed binds nothing: `retargetError`)
        bool retarget = false;
        std::shared_ptr<const RetargetSource> source;
        std::string retargetError;
        // the clip plays through the humanoid path (a humanoid clip, or a generic one retargeted)
        bool humanoidPath() const { return clip && (clip->isHumanoid() || retarget); }
    };

    // Legacy AnimationState.AddMixingTransform: the state animates only these paths
    struct JASPERENGINE_API MixingTransform{
        std::string path;
        bool recursive = true;          // the path's descendants too
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

        std::string clip;                   // the entry's clip reference the state was built from
        // what the pool loads: `clip` (a Retarget entry loads its generic clip as it is: the one
        // retarget path reads it through its source model at play time)
        std::string poolKey;
        ClipWrapMode entryWrap = ClipWrapMode::Default;     // the entry's wrap (R14)
        // humanoid playback (anim-retarget 7.1): the entry's (or controller state's) flags and the
        // Lua overrides (-1: none, else 0 / 1)
        bool retarget = false;
        bool mirror = false;
        bool footIK = false;
        int mirrorOverride = -1;
        int footIKOverride = -1;
        // AnimationPlayerRuntime::humanoidRevision the bindings were made with; the clip a "no
        // humanoid rig" warning was given for
        uint64_t humanoidRevision = 0;
        const AnimationClip* humanoidWarned = nullptr;
        bool startAtEnd = false;            // started backwards before its clip loaded: at its length then
        bool stopAtCycleEnd = false;        // a queued state waits for it: stops at its next end (R17)
        // R1b: an ended state (once at its end, or a state a queue stopped at its cycle end) is
        // not playing, does not advance or fire, and keeps contributing its last pose (time =
        // its end) at its weight until a fade brings that to 0 (then it is stopped and rewound)
        bool held = false;
        // this update only: a state that reached its end is sampled there, then held
        bool finishing = false;
        float finishTime = 0.0f;            // the clip time it ended at
        float finishWhen = 0.0f;            // seconds into the update when it ended

        // ---- layers (anim layers spec 7.2) ----
        int layer = 0;                      // the entry's layer, clamped to the player's layers
        int layerOverride = -1;             // Lua state.layer (-1: none)
        std::vector<MixingTransform> mixing;
        uint64_t mixingRevision = 0;        // bumped by every mixing transform change
        // per clip track: the layer mask's weight times the mixing weight, filled at bind
        // (empty until computed; see maskStamp)
        std::vector<float> trackMask;
        // what trackMask was computed for: changes of any of them recompute it
        struct MaskStamp{
            const AvatarMaskDef* mask = nullptr;
            uint64_t maskEntities = 0;      // LayerRuntime::maskEntitiesSerial
            int layer = -1;
            uint64_t bindings = 0;          // AnimationPlayerRuntime::scratchSerial
            uint64_t mixing = 0;
            uint64_t humanoid = 0;          // AnimationPlayerRuntime::humanoidRevision
            bool valid = false;
        } maskStamp;
        // additive layers: every track sampled at the clip's additive_reference (raw values),
        // for the bindings serial `referenceSerial`
        std::vector<TrackSample> reference;
        uint64_t referenceSerial = 0;
        const AnimationClip* referenceClip = nullptr;
        bool referenceWarned = false;       // a zero scale reference was reported
        bool discreteAdditiveWarned = false;    // discrete property tracks skipped on an additive layer
        // per clip track: the slot of the player's PlayerPoseScratch (UINT32_MAX: none)
        std::vector<uint32_t> trackSlots;
        // a humanoid clip, per HumanoidBinding output: its slot, mask weight and additive reference
        // (the realised value at additive_reference, for referenceSerial)
        std::vector<uint32_t> humanoidSlots;
        std::vector<float> humanoidMask;
        std::vector<HumanoidOutput> humanoidReference;
        // the Mirror / Foot IK the reference was realised with (bit 0, bit 1): a change (Lua
        // state.mirror / footIK) realises it again
        uint8_t humanoidReferenceFlags = 0;

    };

    struct JASPERENGINE_API QueuedState{
        size_t state = 0;
        float fade = 0.0f;
    };

    // a callback to run after the pose is written: an event or a finished state
    struct JASPERENGINE_API PendingPlayerCallback{
        bool finish = false;
        AnimationEvent event;               // finish: event.state is the finished state
        float when = 0.0f;                  // seconds into the update it happened (dispatch order)
        std::string clip;                   // the state's clip reference (missing-handler warning)
    };

    // an audio cue a state crossed this update (S40 R4): fired after the advance
    struct JASPERENGINE_API PendingPlayerCue{
        size_t state = 0;                   // index into AnimationPlayerRuntime::states
        size_t cue = 0;                     // into the state's clip getAudioCues()
        double position = 0.0;              // on the state's unwrapped time line
        float when = 0.0f;                  // seconds into the update it was crossed
    };

    // the voice of a state_exit cue (S40 R7), stopped when the state that fired it stops, is left
    // or finishes its crossfade out
    struct JASPERENGINE_API PlayerCueVoice{
        std::string state;
        uint32_t id = 0;                    // a sound cue's free voice
        uint32_t generation = 0;
        Entity source = NULL_ENTITY;        // a source cue's AudioSource
    };

    // The runtime of one layer (anim layers spec 7.2)
    struct JASPERENGINE_API LayerRuntime{
        std::string name;
        // the effective weight λ (layers[0]: always 1), fading linearly toward targetWeight
        float weight = 1.0f;
        float targetWeight = 1.0f;
        float fadeSpeed = 0.0f;
        AnimationLayerBlend blend = AnimationLayerBlend::Override;
        // the mask in use: the authored one, or the Lua override (setLayerMask); "" = none
        std::string maskPath;
        // S21's Avatar Mask (AnimatorAssetPool::mask; the weight rule: AvatarMaskWeights.h)
        std::shared_ptr<const AvatarMaskDef> mask;
        // a mask that is not loaded yet (maskLoading) or failed (maskFailed: logged once, tried
        // again every second) makes the layer contribute nothing (spec 5.7)
        bool maskLoading = false;
        bool maskFailed = false;
        uint64_t maskGeneration = 0;        // AnimatorAssetPool generation of `mask`
        std::string maskError;
        bool maskErrorLogged = false;
        float maskRetry = 0.0f;
        // the mask resolved on the root for the current bindings (mask transform entities)
        // (resolved again with every slot table rebuild, i.e. after any rebind)
        std::vector<Entity> maskEntities;
        uint64_t maskEntitiesSerial = 0;    // bumped on every resolve
        bool maskEntitiesValid = false;
        std::deque<QueuedState> queue;      // this layer's queue (R17 per layer)
        // the authored values the runtime ones came from: an authored change (Inspector, MCP)
        // is applied on the rebuild, a runtime one (Lua) is kept otherwise
        float authoredWeight = 1.0f;
        AnimationLayerBlend authoredBlend = AnimationLayerBlend::Override;
        std::string authoredMask;
    };

    // The layered evaluator's per-player scratch (anim layers spec 7.3), sized at bind and reused
    // every frame: no allocation or hashing per frame once built.
    // - A slot is one (target, channel) a bound track writes, with `width` elements: 1 for
    //   position, rotation and scale, MAX_MORPHTARGETS for morph, the cage's point count for
    //   lattice. Extension point (anim-properties): a Property slot is one (target, property
    //   key) with the property's component count as width (`key` names it); anim-retarget's
    //   humanoid contributions are ordinary Rotation/Position slots of the bone entities.
    // - An element keeps the current layer's sums (as TransformBlendAccum: value sum, weight sum,
    //   rotation reference) and the running composed value R.
    struct JASPERENGINE_API PoseSlot{
        Entity target = NULL_ENTITY;
        ClipChannel channel = ClipChannel::Position;
        uint32_t first = 0;                 // its first element
        uint32_t width = 1;
        uint32_t key = 0;                   // Property: the property key (internPropertyKey)
        // the target's rest (AnimationPlayerRuntime::rest: map nodes stay put on insert; erased
        // only by a rebind, which rebuilds the table)
        ChannelRest* rest = nullptr;
        // Property: one element per component of the whole value; its rest and binding
        PropertyRest* propertyRest = nullptr;
        bool discrete = false;
    };
    struct JASPERENGINE_API PoseElement{
        float sum[4] = {0, 0, 0, 0};        // Σ w·v (rotation: w x y z, hemisphere aligned)
        float weight = 0.0f;                // Σ w
        float ref[4] = {0, 0, 0, 0};        // rotation: the first sample of the layer
        float result[4] = {0, 0, 0, 0};     // R (rotation: w x y z)
        bool hasRef = false;
        bool touched = false;               // in the current layer's touched list
        bool hasResult = false;
        // the highest Override layer that replaces this element entirely this frame (weight 1,
        // states summing to 1 on it): the layers below it do not sample it (-1: none)
        int occluder = -1;
        int coverLayer = -1;                // the layer `coverage` sums (the occlusion pass)
        float coverage = 0.0f;
    };
    // What the evaluator composes in one call (filled per frame into reused vectors)
    struct JASPERENGINE_API PoseLayerInput{
        float weight = 1.0f;                // λ (layer 0: 1)
        AnimationLayerBlend blend = AnimationLayerBlend::Override;
        bool skip = false;                  // contributes nothing (λ = 0 above the base, a mask not loaded)
    };
    struct JASPERENGINE_API PoseStateInput{
        const AnimationClip* clip = nullptr;
        ClipBindings* bindings = nullptr;
        float time = 0.0f;                  // the clip time
        float weight = 0.0f;
        int layer = 0;
        const float* trackMask = nullptr;   // per clip track; null: 1 everywhere
        const uint32_t* trackSlots = nullptr;   // per clip track (PoseSlot index, UINT32_MAX: none)
        const TrackSample* reference = nullptr; // additive layers: per clip track at additive_reference
        bool* referenceWarned = nullptr;    // a zero scale reference warned (per state)
        bool* discreteWarned = nullptr;     // discrete property tracks skipped on an additive layer (per state)
        // humanoid clips (bindings->humanoid): per output its slot, mask (null: 1) and additive
        // reference (anim-retarget 7.3, 7.5)
        const uint32_t* humanoidSlots = nullptr;
        const float* humanoidMask = nullptr;
        const HumanoidOutput* humanoidReference = nullptr;
    };

    struct JASPERENGINE_API PlayerPoseScratch{
        std::vector<PoseLayerInput> layerInputs;
        std::vector<PoseStateInput> stateInputs;
        std::vector<PoseSlot> slots;
        std::vector<PoseElement> elements;
        std::vector<uint32_t> elementSlot;  // element -> slot
        std::vector<uint32_t> touched;      // elements the current layer reached
        std::vector<uint32_t> covered;      // elements the occlusion pass reached
        std::vector<uint32_t> written;      // elements with a result, in first-written order
        // per state: the bindings serial the state's trackSlots were built for (0: none)
        // (any change rebuilds the whole table: rest entries may have gone with a rebind)
        std::vector<uint64_t> builtFor;
        bool dirty = true;
    };

    class JASPERENGINE_API AnimationPlayerRuntime{
    public:
        std::vector<PlayerStateRuntime> states;
        // one per effective layer (playerLayers), never empty after playerRuntime()
        std::vector<LayerRuntime> layers;
        RestMap rest;
        // waiting for a ModelComponent on or under the root to load (R10): no writes
        bool waitingForModel = false;
        // the loadedFilename of each model bound under the root: a change rebinds
        std::unordered_map<Entity, std::string> modelFiles;
        std::vector<PendingPlayerCallback> pending;
        std::vector<PendingPlayerCue> pendingCues;
        std::vector<PlayerCueVoice> cueVoices;
        // state names a call named that the clip list does not have (warned once each)
        std::vector<std::string> warnedStates;
        // the clip list's duplicate state names as aliased (spec 3.2, R40: "'Walk' (clip 3) plays
        // as 'Walk 2'", empty when none) and the last list warned about: one warning per player
        // and clip list
        std::string duplicateStateNames;
        std::string warnedDuplicateStateNames;
        // seconds the player ran (the 4 Hz unresolved-path throttle)
        float clock = 0.0f;
        // EntityRegistry::hierarchyRevision() the models under the root were collected at
        uint64_t modelsRevision = 0;
        bool modelsCollected = false;

        // the layered evaluator's scratch (anim layers spec 7.3); scratchSerial counts its
        // rebuilds (what the states' track masks were computed for)
        PlayerPoseScratch scratch;
        uint64_t scratchSerial = 0;
        // the root's humanoid rig for humanoid masks (spec 6.2 rule 2; anim-retarget's target rig
        // lookup shares it): resolved when a layer's mask needs it, again after a model reload
        std::shared_ptr<HumanoidSlots> humanoid;
        uint64_t humanoidRevision = 0;      // bumped whenever `humanoid` is resolved again
        uint64_t humanoidHierarchy = 0;     // the hierarchy revision its bone entities were checked at
        bool humanoidWarned = false;        // "a humanoid mask on a root without a humanoid rig"
        // warned once: unknown layer names/indices, entries on missing layers, base-layer writes
        std::vector<std::string> warnedLayers;
        std::string warnedEntryLayers;

        // index into states, -1 when none
        int findState(const std::string& name) const;
        // index into layers, -1 when none
        int findLayer(const std::string& name) const;
        // the layer a state plays on (its override, else its entry's), in [0, layers.size())
        int stateLayer(const PlayerStateRuntime& state) const;
        // the effective weight λ of a layer (1 for layer 0)
        float layerWeight(int layer) const;
    };

    // ---- layers (anim layers spec 5.2, 7.1, 7.4) ----

    constexpr const char* BASE_LAYER_NAME = "Base Layer";

    // the effective authored layers: `layers`, or one Base Layer when empty
    JASPERENGINE_API std::vector<AnimationPlayerLayer> playerLayers(const AnimationPlayerComponent& player);
    // one layer without a mask and no mixing transforms: the v1 fast path (spec 5.5).
    // Reads the runtime when there is one (runtime masks and mixing transforms count).
    JASPERENGINE_API bool isDefaultLayering(const AnimationPlayerComponent& player);
    // "override" / "additive"
    JASPERENGINE_API const char* layerBlendName(AnimationLayerBlend blend);
    JASPERENGINE_API bool layerBlendFromName(const std::string& name, AnimationLayerBlend& out);

    // the runtime layer index of a name (-1 when unknown; warned once per name with `who`)
    JASPERENGINE_API int layerIndex(AnimationPlayerComponent& player, const std::string& name, const std::string& who);
    // an index checked against the layers (-1 when out of range; warned once)
    JASPERENGINE_API int layerIndex(AnimationPlayerComponent& player, int index, const std::string& who);
    // runtime: weight clamped to [0, 1], linear fade over `fade` seconds (instant when <= 0);
    // ignored on layer 0 (warned once)
    JASPERENGINE_API void setLayerWeight(AnimationPlayerComponent& player, int layer, float weight, float fade, const std::string& who);
    // runtime mask override ("" = none): its tracks re-mask on the next update
    JASPERENGINE_API void setLayerMask(AnimationPlayerComponent& player, int layer, const std::string& path, const std::string& who);
    // runtime blend (layer 0: always Override, warned once)
    JASPERENGINE_API void setLayerBlend(AnimationPlayerComponent& player, int layer, AnimationLayerBlend blend, const std::string& who);
    // every state of the layer fades out over `fade` (instant when <= 0) and stops; its queue clears
    JASPERENGINE_API void stopLayer(AnimationPlayerComponent& player, int layer, float fade);
    // authored: appends a layer (needsLoad); its index, or -1 when the name is taken or empty
    JASPERENGINE_API int addLayer(AnimationPlayerComponent& player, const std::string& name, float weight,
                                  AnimationLayerBlend blend, const std::string& mask);
    // authored: removes a layer above 0; its entries move to layer 0 (warned), the entries of the
    // layers above it move down one. False for layer 0 or an unknown layer.
    JASPERENGINE_API bool removeLayer(AnimationPlayerComponent& player, int layer, const std::string& who);
    // a runtime move of a state to another layer: it keeps its time and weight, its tracks re-mask
    JASPERENGINE_API void setStateLayer(AnimationPlayerComponent& player, PlayerStateRuntime& state, int layer);
    // mixing transforms (all three invalidate the state's trackMask)
    JASPERENGINE_API void addMixingTransform(PlayerStateRuntime& state, const std::string& path, bool recursive);
    JASPERENGINE_API bool removeMixingTransform(PlayerStateRuntime& state, const std::string& path);
    JASPERENGINE_API void clearMixingTransforms(PlayerStateRuntime& state);

    // ---- playback (Unity legacy Animation semantics, object/AnimationPlayer.h) ----

    // The player's runtime, created when missing; a changed clip list (needsLoad) rebuilds the
    // states first (no file is read here: clips load in the system update)
    JASPERENGINE_API AnimationPlayerRuntime& playerRuntime(AnimationPlayerComponent& player);
    // the wrap a state plays with: Lua override, else the entry's, else the clip's (Once while
    // the clip is not loaded)
    JASPERENGINE_API ClipWrapMode stateWrap(const PlayerStateRuntime& state);
    // the state's clip length, 0 while not loaded
    JASPERENGINE_API float stateLength(const PlayerStateRuntime& state);
    // the state's time in its clip (wrapped for loop / ping-pong)
    JASPERENGINE_API float stateClipTime(const PlayerStateRuntime& state);
    // sets a state's time (the stored form: see PlayerStateRuntime::time)
    JASPERENGINE_API void setStateTime(PlayerStateRuntime& state, float time);

    // a state starts from its beginning (0, or its length when its effective speed is negative,
    // R18) at its current weight; stop rewinds it and keeps the pose (weight 0, not enabled)
    JASPERENGINE_API void startState(const AnimationPlayerComponent& player, PlayerStateRuntime& state);
    JASPERENGINE_API void stopState(PlayerStateRuntime& state);
    // the state's speed times the player's (0 for non-finite values)
    JASPERENGINE_API float effectiveSpeed(const AnimationPlayerComponent& player, const PlayerStateRuntime& state);
    // humanoid playback (anim-retarget 7.1): the Lua override, else the entry's flag
    JASPERENGINE_API bool stateMirror(const PlayerStateRuntime& state);
    JASPERENGINE_API bool stateFootIK(const PlayerStateRuntime& state);
    // the state plays a humanoid clip (one, or a generic clip retargeted)
    JASPERENGINE_API bool stateIsHumanoid(const PlayerStateRuntime& state);
    // a Retarget entry's bindings: the source of their generic clip (`bindings.source`, or
    // `retargetError`), set when the clip is loaded or changed
    JASPERENGINE_API void prepareRetarget(ClipBindings& bindings, const std::string& clipReference, bool retarget);

    // `who` names the player in warnings. False (warned once per name) for an unknown state.
    // play: fade <= 0 is instant (Unity Play), else crossFade. StopSameLayer (Unity's default)
    // stops or fades out the other states of the target's layer and clears that layer's queue;
    // StopAll does it on every layer
    JASPERENGINE_API bool playState(AnimationPlayerComponent& player, const std::string& name, float fade, const std::string& who,
                                    PlayMode mode = PlayMode::StopSameLayer);
    JASPERENGINE_API bool blendState(AnimationPlayerComponent& player, const std::string& name, float weight, float fade, const std::string& who);
    // waits for the states playing on the target's layer, on that layer's queue
    JASPERENGINE_API bool queueState(AnimationPlayerComponent& player, const std::string& name, float fade, const std::string& who);
    // name null: every state (and every queue); a name: that state, and its layer's queue
    JASPERENGINE_API void stopStates(AnimationPlayerComponent& player, const std::string* name, const std::string& who);
    JASPERENGINE_API void rewindStates(AnimationPlayerComponent& player, const std::string* name, const std::string& who);
    // every playing state fades to 0 over `fade` and stops (instantly when fade <= 0); every queue is cleared
    JASPERENGINE_API void fadeOutStates(AnimationPlayerComponent& player, float fade);
    // name null: any state
    JASPERENGINE_API bool isStatePlaying(const AnimationPlayerComponent& player, const std::string* name);

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
        // the (entity, property key) entries it wrote (their rest: rest[entity].properties)
        std::vector<std::pair<Entity, uint32_t>> writtenProperties;
        // fireEvents: the previous call's time per state (key: the state name, or the clip's
        // address for explicit clips)
        std::unordered_map<std::string, float> previousTimes;
        // the layered preview's evaluator state (anim layers spec 7.5), rebuilt per call
        PlayerPoseScratch scratch;
        std::vector<std::vector<uint32_t>> trackSlots;
        std::vector<std::vector<float>> trackMasks;
        std::vector<std::vector<TrackSample>> references;
        std::vector<std::vector<uint32_t>> humanoidSlots;
        std::vector<std::vector<float>> humanoidMasks;
        std::vector<std::vector<HumanoidOutput>> humanoidReferences;
        // the root's humanoid rig (masks and humanoid clips)
        std::shared_ptr<HumanoidSlots> humanoid;
        uint64_t humanoidRevision = 0;      // the hierarchy revision it was resolved at
        uint64_t humanoidPool = 0;          // AnimationClipPool::revision() then (an Avatar edit rebuilds the rig)

        // a scene edit changed a previewed channel: the new value is its rest (R6)
        void setRest(Entity entity, const ChannelRest& value);
        // forget everything (the caller restored the scene)
        void reset();
    };

}

#endif //ANIMATION_PLAYER_RUNTIME_H
