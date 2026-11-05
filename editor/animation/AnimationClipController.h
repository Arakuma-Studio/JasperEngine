// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 4.4, 5 preview_animation, 9.R2/R4-R7/R21/R31)
// - Purpose: the clip mode of the Animation window without its UI: which clip is edited and on
//   which root, the edit-mode preview (snapshot of bound channels, PreviewOwner, rest updates
//   through ClipRecordHooks' channel write listeners), transport, keying and Record. The window
//   (T3b/T3c) draws it; MCP preview_animation drives it and reads sampledBoundTransforms().
// - Owner: T0 (API); T3a implements it (gtested); T3b/T3c/T4 use it.
// - Thread-safety: main thread.
// - Invariants:
//   * preview never changes saved scene state: bound channels are snapshotted on their first
//     write and restored by stopPreview (also on save, play, scene switch/close, window close:
//     stopTransientPreviews); the scene's modified flag is never written by the preview;
//   * scene commands / undo / redo that write a previewed channel update its snapshot (R6);
//   * every key edit is one AnimationClipDocument::commit (project history), drags merge with a
//     merge key; edits of a read-only (sub-clip) document fail with an "Extract to .anim" error;
//   * key indices (KeyRef) are only valid for the document revision they were read from: a commit
//     re-sorts tracks and keys. Every edit that can insert or reorder reports where the keys it
//     touched are now; findKey maps a stable (path, channel, time) identity back to a KeyRef.
//   * drags: beginKeyDrag keeps the clip as it was (the base); every updateKeyDrag commits
//     base + the TOTAL delta with one merge key, so a key passed over is never merged or lost
//     mid-drag; keys that end the drag on the same time collapse then (with a warning).
//   * one controller per project for the window and MCP (shared()); release() restores any
//     preview and drops it (project close, shutdown: AnimationClipAppHooks::projectClosed). The
//     destructor never touches scenes.

#pragma once

#include "audio/TimelineAudioPreview.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipRecordHooks.h"
#include "animation/ClipTangents.h"
#include "animation/ClipValidation.h"
#include "animation/PreviewOwner.h"
#include "animation/PreviewParticles.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
}

namespace jasperengine::editor{

    class Project;
    struct SceneProject;

    class AnimationClipController{
    public:
        // a key of the open clip
        struct KeyRef{
            size_t track = 0;
            size_t key = 0;
        };

        struct BoundTransform{
            std::string path;
            Entity entity = NULL_ENTITY;
            Vector3 position = Vector3(0, 0, 0);
            Quaternion rotation;
            Vector3 scale = Vector3(1, 1, 1);
        };

        explicit AnimationClipController(Project* project);
        ~AnimationClipController();
        AnimationClipController(const AnimationClipController&) = delete;
        AnimationClipController& operator=(const AnimationClipController&) = delete;

        // the editor's controller (the Animation window's and MCP's), created on first use
        static AnimationClipController& shared(Project* project);
        // stops its preview (restoring the scene while it still exists), closes its document and
        // deletes it; the next shared() makes a new one
        static void release(Project* project);

        // ---- binding and clip ----

        // `root`: a GameObject (with or without an Animation Player); NULL_ENTITY unbinds
        bool bindRoot(uint32_t sceneId, Entity root, std::string* error);
        uint32_t getSceneId() const;
        Entity getRoot() const;
        bool hasPlayer() const;
        // the bound player's state names, in clip list order
        std::vector<std::string> getStateNames() const;

        // a .anim path or "<model>#<clip>" (read-only document)
        bool openClip(const std::string& path, std::string* error);
        // the bound player's state `name`
        bool openState(const std::string& name, std::string* error);
        void closeClip();
        std::shared_ptr<AnimationClipDocument> getDocument() const;
        const std::string& getClipPath() const;
        const std::string& getStateName() const;
        bool isReadOnly() const;
        // the model or prefab a target-less clip previews on (preview.model), empty when none
        std::string getPreviewModel() const;
        // the binding report of the open clip on the root (empty report without one)
        ClipValidationReport getBindings() const;

        // ---- layers (anim layers spec 9) ----

        // the layer the open state plays on in the bound player (0 without one); its name
        int getStateLayer() const;
        std::string getLayerName(int layer) const;
        // the bound player's state names grouped by layer: (layer name, states), in layer order
        std::vector<std::pair<std::string, std::vector<std::string>>> getStatesByLayer() const;
        // Preview over: a base layer state composed under the open state (on its layer, with its
        // mask) at the playhead (ActionSystem::previewPlayer, layered); "" = none. Record still
        // keys the open clip only. Kept per bound root.
        void setPreviewOver(const std::string& baseState);
        const std::string& getPreviewOver() const;
        // the base layer's states (what Preview over can choose)
        std::vector<std::string> previewOverChoices() const;
        // per track of the open clip: why its target is masked out on the state's layer
        // ("masked out by Arm.mask on layer Holding"), "" when it is not
        std::vector<std::string> maskedTracks() const;

        // ---- preview and transport ----

        bool startPreview(std::string* error);
        void stopPreview();
        bool isPreviewing() const;
        void play();
        void pause();
        bool isPlaying() const;
        void setPlaybackSpeed(float speed);
        float getPlaybackSpeed() const;
        // seconds; starts the preview when needed
        void seek(float time);
        float getTime() const;
        float getLength() const;
        // per frame while the window shows (advances a playing preview, re-applies after edits)
        void update(double dt);
        // the clip's audio cues heard while the preview plays (S40 R11): the preview voices now and
        // the starts since the preview began (tests, preview_animation)
        int previewAudioVoices();
        uint64_t previewAudioStarts() const;
        // update(dt) at most once per editor frame `frame` (ImGui::GetFrameCount()): the App's
        // per-frame hook (AnimationClipAppHooks::frame) and the Animation window may both call it
        void update(double dt, int frame);
        // the bound entities' local transforms as the preview shows them (up to `max`)
        std::vector<BoundTransform> sampledBoundTransforms(size_t max = 8) const;
        // the open clip's bound properties as the preview shows them, in key space (sRGB
        // colours, bools 0/1; a swizzle track its component), up to `max`
        struct BoundProperty{
            std::string path;
            Entity entity = NULL_ENTITY;
            std::string component;
            std::string property;
            PropertyKind kind = PropertyKind::Float;
            std::vector<float> value;
        };
        std::vector<BoundProperty> sampledBoundProperties(size_t max = 16) const;

        // ---- keying (each one commit; `mergeKey` "" = its own undo step) ----

        // a track for `path` : `channel` with a key at the playhead holding the current value
        // (a humanoid channel: `path` is the bone name, the value from the bound humanoid rig);
        // `newTrack` (optional) receives its index after the commit
        bool addProperty(const std::string& path, ClipChannel channel, size_t* newTrack, std::string* error);
        // the same for an animatable property (anim-properties spec 10, Add Property): `component`
        // the Catalog key, "gameobject" or "script:<Class>", `property` its name (a swizzle
        // "color.a" keys that component alone)
        bool addPropertyTrack(const std::string& path, const std::string& component, const std::string& property,
                              size_t* newTrack, std::string* error);
        // keys `tracks` (all tracks when empty) at `time` with the bound entities' current values
        bool keyTracks(const std::vector<size_t>& tracks, float time, const std::string& mergeKey, std::string* error);
        // K: every bound channel; Shift+K: the channels whose value differs from the clip
        bool keyAll(float time, std::string* error);
        bool keyModified(float time, std::string* error);
        bool setKeyValue(const KeyRef& key, const std::vector<float>& value, const std::string& mergeKey, std::string* error);
        // a discrete move (arrow keys, snap): `moved` (optional) receives the keys' refs afterwards
        bool moveKeys(const std::vector<KeyRef>& keys, float deltaTime, const std::string& mergeKey,
                      std::vector<KeyRef>* moved, std::string* error);

        // A key drag (time and/or value): see the drag invariant above. `valueDelta` (optional)
        // is added to every dragged key's value (ClipKey::value layout, the curves view's
        // vertical drag). `moved` receives where the dragged keys are now.
        bool beginKeyDrag(const std::vector<KeyRef>& keys, std::string* error);
        bool updateKeyDrag(float totalDeltaTime, const std::vector<float>* valueDelta,
                           std::vector<KeyRef>* moved, std::string* error);
        void endKeyDrag();
        bool isDraggingKeys() const;
        // a key's stable identity across commits: (path, channel, time within ANIM_TIME_EPSILON)
        static bool findKey(const ClipData& data, const std::string& path, ClipChannel channel, float time, KeyRef& out);
        // the same by a track's identity (property tracks: path, component and property)
        static bool findKey(const ClipData& data, const ClipTrackData& like, float time, KeyRef& out);
        bool deleteKeys(const std::vector<KeyRef>& keys, std::string* error);
        bool setEase(const std::vector<KeyRef>& keys, EaseType ease, std::string* error);
        // the keys' tangent mode (the curves view's Tangents menu, S21 R27): the track becomes a
        // Hermite one (its eases drop, Constant keys step); Free unifies a key's two sides. Scalar
        // curves and continuous property tracks (anim-properties); `notice`: the eases the edit
        // turned linear
        bool setTangentMode(const std::vector<KeyRef>& keys, ClipTangentMode mode, std::string* error, std::string* notice = nullptr);
        // a dragged tangent handle: `slope` (value units per second) for component `component`
        // on the key's out side (`outSide`) or in side; a Broken key moves that side only, any
        // other mode both sides and turns Free (Unity). `mergeKey`: one undo step per drag
        bool setKeyTangent(const KeyRef& key, size_t component, bool outSide, float slope, const std::string& mergeKey,
                           std::string* error);
        // anim-properties' Left / Right / Both Tangents (ClipTangents.h): one side Free, Linear or
        // Constant on the keys of scalar curves (rotations and discrete properties are skipped); the
        // key becomes Broken (Both Linear: Linear). One commit
        bool setSideTangent(const std::vector<KeyRef>& keys, cliptangents::Side side, cliptangents::SideType type, std::string* error,
                            std::string* notice = nullptr);
        // anim-properties' handle drag, on setKeyTangent (Left: the in side, otherwise the out side)
        bool setTangentSlope(const KeyRef& key, size_t component, cliptangents::Side side, float slope, const std::string& mergeKey,
                             std::string* error, std::string* notice = nullptr);
        bool removeTrack(size_t track, std::string* error);
        // rewrites a track's path (the row's Rebind / Retarget): changes the clip for every user
        bool retargetTrack(size_t track, const std::string& newPath, std::string* error);
        // any other edit of the clip data (events lane, settings): one commit
        bool commit(const ClipData& data, const std::string& undoName, const std::string& mergeKey, std::string* error);
        // keyTracks / keyAll / keyModified reporting where the keys they wrote are now (`keyed`
        // optional, one ref per keyed track)
        bool keyTracks(const std::vector<size_t>& tracks, float time, const std::string& mergeKey,
                       std::vector<KeyRef>* keyed, std::string* error);
        bool keyAll(float time, std::vector<KeyRef>* keyed, std::string* error);
        bool keyModified(float time, std::vector<KeyRef>* keyed, std::string* error);
        // what normalize changed in the last commit (keys collapsed by a drag, ignored eases...)
        const std::vector<ClipWarning>& getLastWarnings() const;
        // Unity's pink field: an unkeyed value of a track's channel shown on its bound entity
        // (the scene value layout, ClipKey::value width; relative tracks too: the entity's value,
        // not the offset) until the playhead moves, the clip changes or the preview stops; K
        // (keyTracks / keyAll / keyModified) keys it. Starts the preview when needed.
        bool setPendingValue(size_t track, const std::vector<float>& value, std::string* error);
        bool hasPendingValue(size_t track) const;
        void clearPendingValues();
        // the time key edits use (the playhead), in seconds
        void setTime(float time);

        // A key drag giving each dragged key an ABSOLUTE value (ClipKey::value layout, the drag
        // base's width; rotations are normalized): `dragIndex` is the key's position in the list
        // given to beginKeyDrag; an empty value (or a key not listed) keeps the base value. The
        // time offset applies to every dragged key as in the other overload; one merge key per
        // drag. The curves view computes the values from dragBaseData().
        struct KeyDragValue{
            size_t dragIndex = 0;
            std::vector<float> value;
        };
        bool updateKeyDrag(float totalDeltaTime, const std::vector<KeyDragValue>& values,
                           std::vector<KeyRef>* moved, std::string* error);
        // the clip as it was when the drag began (valid while isDraggingKeys())
        const ClipData& dragBaseData() const;

        // ---- record ----

        void setRecording(bool recording);
        bool isRecording() const;
        // A save, Play or Prefab Mode Auto Save stopped the preview and with it Record (Unity's
        // Record keys into the preview): true once after that, for the window's notice
        bool takeRecordStoppedNotice();

        // ---- binding and preview root ----

        // `entity` or its nearest ancestor with an Animation Player (the root clip mode binds),
        // NULL_ENTITY when none
        static Entity findPlayerRoot(EntityRegistry* registry, Entity entity);
        // What the preview writes: the bound root, or for a clip opened without one the instance
        // of its preview.model found in the selected scene when the preview started (a GameObject
        // whose Model is that file, or an instance root of that prefab; the selected GameObject
        // first). NULL_ENTITY / 0 while not previewing.
        Entity getPreviewRoot() const;
        uint32_t getPreviewSceneId() const;
        // paths of the open clip's tracks that did not resolve in the last preview write
        const std::vector<std::string>& getUnresolvedPaths() const;

        // ---- App integration (AnimationClipAppHooks) ----

        void stopTransientPreviews();
        void sceneClosed(uint32_t sceneId);
        // the window sets it every frame: it is focused in clip mode and handles its keys
        // (AnimationClipAppHooks::takesKeys)
        void setTakesKeys(bool takes);
        bool takesKeys() const;

        // ---- undo routing (spec 9.R5b / R5c) ----
        // Ctrl+Z / Ctrl+Y and Edit > Undo / Redo take S24's timeline step: the newest undoable
        // (for redo: the one undone last) step across the project history and the selected
        // scene's, a clip commit included, whatever window has focus. These say whether that step
        // is a clip commit made through the editor UI (AnimationClipAppHooks::ownsUndo: the
        // Animation window keeps its keys then); Record never steals a scene undo.
        // Clip commits: this controller's edits (the window, Record, Retarget) and the ones the
        // UI marks with markClipCommit (the Inspector's .anim asset section, Extract / Create
        // file writes). MCP tools never mark theirs.

        bool ownsUndo() const;
        bool newestStepIsClipEdit() const;
        bool newestRedoIsClipEdit() const;
        // the same for `project` (null: the timeline's), with or without a controller
        static bool newestUndoIsClipCommit(Project* project);
        static bool newestRedoIsClipCommit(Project* project);
        // A clip commit made outside the controller: take the clock before it, mark after it.
        // The project step made (or merged into) since `clockBefore` is a clip commit.
        static uint64_t clipCommitClock(Project* project);
        static void markClipCommit(uint64_t clockBefore);
        // App::engineInit, right after it sets CommandHistory's listeners: the history timeline
        // (and the clip references re-resolved after a scene undo / redo) chained after them
        static void installHistoryTimeline(Project* project);

    private:
        struct Snapshot;
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity root = NULL_ENTITY;
        std::shared_ptr<AnimationClipDocument> document;
        std::string clipPath;
        std::string stateName;
        std::string previewOver;
        // maskedTracks' cache: what it was computed for
        mutable std::vector<std::string> maskedCache;
        mutable std::string maskedKey;
        mutable double maskedAt = -1.0;
        float time = 0.0f;
        float playbackSpeed = 1.0f;
        bool previewing = false;
        bool playing = false;
        bool recording = false;
        bool recordStoppedNotice = false;
        // the drag's base clip and keys (beginKeyDrag)
        bool dragging = false;
        ClipData dragBase;
        std::vector<KeyRef> dragKeys;
        std::string dragMergeKey;

        // ---- T3a ----
        uint32_t previewSceneId = 0;
        Entity previewRoot = NULL_ENTITY;
        anim::ClipPreviewSession session;
        // the derived state (world matrices, skinning) of the previewed hierarchy at start
        std::unique_ptr<Snapshot> snapshot;
        PreviewOwner::Token ownerToken = 0;
        // the Particle Systems under the previewed root follow the playhead (S21 R28)
        PreviewParticles particles;
        float particleTime = -1.0f;
        // the audio cues of a playing preview (S40 R11): crossed as the playhead moves by playback,
        // never by scrubbing; state_exit cues stop when the preview stops, loops or is sought
        TimelineAudioPreview audioPreview;
        void previewCues(float from, float delta, double dt);
        int documentListener = 0;
        ClipRecordHooks::Id beforeWriteListener = 0;
        ClipRecordHooks::Id afterWriteListener = 0;
        ClipRecordHooks::Id previewQuery = 0;
        ClipRecordHooks::Id recordListener = 0;
        // a scene write changed a previewed channel's rest: the preview is applied again on update
        bool needsApply = false;
        // stopPreview() while the scene was being saved: update() finishes it
        bool stopPending = false;
        // the running stop came from stopTransientPreviews (a save, Play, Auto Save)
        bool transientStop = false;
        bool windowKeys = false;
        int lastUpdateFrame = -1;
        std::vector<std::string> unresolved;
        // the drag's dragged keys by identity (path, channel, time in dragBase)
        struct DragIdentity{
            std::string path;
            ClipChannel channel = ClipChannel::Position;
            float time = 0.0f;
            std::string component;      // property tracks
            std::string property;
        };
        std::vector<DragIdentity> dragIdentities;
        uint64_t dragRevision = 0;
        uint64_t dragSerial = 0;
        // the previewed value of a written channel a scene write is about to replace (only a
        // different value becomes the rest)
        std::vector<std::pair<std::pair<Entity, ClipChannel>, std::vector<float>>> shownValues;
        struct PendingValue{
            std::string path;
            ClipChannel channel = ClipChannel::Position;
            std::vector<int> points;
            std::vector<float> value;
            std::string component;      // property tracks (value in key space)
            std::string property;
        };
        std::vector<PendingValue> pending;
        float pendingTime = 0.0f;
        std::vector<ClipWarning> lastWarnings;
        // the merge key of the last Record edit that could not be committed (warned once)
        std::string failedRecordKey;
        void writePending();

        SceneProject* sceneProjectOf(uint32_t id) const;
        void setDocument(const std::shared_ptr<AnimationClipDocument>& next);
        std::string stateOfDocument() const;
        Entity findPreviewInstance(SceneProject* sceneProject) const;
        // writes the open clip at the playhead (false when it can't now)
        bool apply();
        // the scene back to its unpreviewed state; `write` false: forget it (the scene is gone)
        void endPreview(bool write);
        void takeSnapshot(SceneProject* sceneProject);
        void restoreSnapshot(SceneProject* sceneProject);
        void writeRest(Entity entity, ClipChannel channel);
        void beforeSceneWrite(uint32_t sceneId, Entity entity, ClipChannel channel);
        void afterSceneWrite(uint32_t sceneId, Entity entity, ClipChannel channel);
        bool recordClaims(uint32_t sceneId, Entity entity, ClipChannel channel) const;
        // recordClaims without its legacy-layout refusal: what Record would take otherwise
        bool recordEligible(uint32_t sceneId, Entity entity, ClipChannel channel) const;
        bool recordAnimates(uint32_t sceneId, Entity entity, ClipChannel channel) const;
        void recordValue(uint32_t sceneId, Entity entity, const ClipRecordValue& value, const std::string& mergeKey);
        // properties (anim-properties spec 10): Record of Inspector rows and checkboxes, scene
        // writes of previewed properties (their rest, 9.R6)
        bool recordClaimsProperty(uint32_t sceneId, Entity entity, const ClipPropertyKey& property) const;
        bool recordAnimatesProperty(uint32_t sceneId, Entity entity, const ClipPropertyKey& property) const;
        void recordPropertyValue(uint32_t sceneId, Entity entity, const ClipRecordValue& value, const std::string& mergeKey);
        void beforePropertySceneWrite(uint32_t sceneId, Entity entity, const ClipPropertyKey& property);
        void afterPropertySceneWrite(uint32_t sceneId, Entity entity, const ClipPropertyKey& property);
        // the previewed property keys of `entity` a write of `property` touches: that one, every
        // one of its component (an empty property: Reset, Paste Component Values), a Lua script
        // row ("scripts" "scripts[N].<field>": the field of that script, "" every script field)
        std::vector<uint32_t> previewedKeysOf(Scene* scene, Entity entity, const ClipPropertyKey& property) const;
        // the previewed property back to its rest (key space) and what its write changed besides
        void writePropertyRest(Entity entity, uint32_t key);
        ClipRecordHooks::Id beforePropertyListener = 0;
        ClipRecordHooks::Id afterPropertyListener = 0;
        // the root key values are read on (the preview root while previewing, else the bound root)
        Entity valueRoot(SceneProject** sceneProject) const;
        // the key value of `track` now: the bound entity's current value (relative tracks: its
        // offset from the preview rest); false when the track's path does not resolve. A humanoid
        // track's (anim-retarget 9): the value of the bound root's humanoid rig's current pose
        // (false without a humanoid rig)
        bool currentTrackValue(const ClipTrackData& track, std::vector<float>& out) const;
    public:
        // the bound root's humanoid rig (Record and Key on humanoid tracks; null without one),
        // looked up again when the hierarchy changes
        std::shared_ptr<const anim::HumanoidSlots> boundHumanoid() const;
    private:
        mutable std::shared_ptr<const anim::HumanoidSlots> humanoidCache;
        mutable Entity humanoidCacheRoot = NULL_ENTITY;
        mutable uint64_t humanoidCacheRevision = 0;
        mutable uint64_t humanoidCachePool = 0;     // AnimationClipPool::revision(): an Avatar edit rebuilds it
        bool editable(std::string* error) const;
        bool commitEdit(const ClipData& data, const std::string& undoName, const std::string& mergeKey, std::string* error);
        bool setKeyTangentPrepared(ClipData& data, const KeyRef& ref, size_t component, bool outSide, float slope,
                                   const std::string& mergeKey, std::string* error);
    };

}
