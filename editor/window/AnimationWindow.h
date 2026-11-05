// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/CommandHistory.h"
#include "Project.h"
#include "imgui.h"
#include "yaml-cpp/yaml.h"

#include <memory>
#include <unordered_set>

namespace jasperengine{
    struct ActionFrame;
    struct AnimationComponent;
}

namespace jasperengine::editor{

    class AnimationWindowClip;
    class TimelineAudioPreview;
    enum class EntityCreationType;

    // editor/tests/AnimationWindowClipDriver.h: the editor tests' driver of the window (the
    // selection, entity Animation, clip, drop and entity preview actions, the explicit lane move)
    struct AnimationWindowTestAccess;

    class AnimationWindow{
        friend struct AnimationWindowTestAccess;
    private:

        Project* project;

        // Clip mode (animation clips spec 4.4, 9.R31; AnimationWindowClip*.cpp): used when the
        // selected GameObject or an ancestor has an Animation Player, the selection has neither
        // a player nor an entity Animation (empty state), or a clip was opened from the Project
        // window / Inspector. Entity mode (the code below) is for AnimationComponent sequences.
        std::unique_ptr<AnimationWindowClip> clipMode;
        bool clipModeActive = false;
        // the scene selection last seen (the mode follows its changes, per scene)
        uint32_t lastSelectionScene = 0;
        std::vector<Entity> lastSelection;
        // entity mode's preview owns every root it writes (PreviewOwner, R7): the previewed
        // Animations', their targets' and their tracks' (inspectorclips::actionPreviewEntities)
        std::vector<int> previewOwnerTokens;
        // owns the roots `animations` write (the previous tokens released first): the owners in
        // the way stop before this preview snapshots anything
        void ownPreviewRoots(Scene* scene, const std::vector<Entity>& animations);
        void releasePreviewOwners();
        // R31: view-only lanes of the displayed Animation's frames (never written to
        // ActionFrame::track except by an explicit move); the dragged frame's lane
        std::vector<uint32_t> viewLanes;
        int draggingLane = -1;
        // lanes laid out before, by a signature of the frames and the zoom (no O(n^2) per frame)
        struct LaneCache {
            uint64_t signature = 0;
            std::vector<uint32_t> lanes;
        };
        std::vector<LaneCache> laneCache;
        uint32_t laneOf(const AnimationComponent& anim, size_t frameIndex) const;

        // Playback state
        bool isPlaying;
        float currentTime;
        float playbackSpeed;

        // Timeline view state
        float pixelsPerSecond;
        float scrollX;
        float scrollY;
        float minPixelsPerSecond;
        float maxPixelsPerSecond;
        // Manual scrollbar input during playback temporarily takes precedence
        // over automatic playhead following until playback is started again.
        bool playbackFollowSuspended;

        // Selection
        int selectedFrameIndex;
        int prePlaySelectedFrameIndex;
        bool isDraggingPlayhead;
        bool isDraggingFrame;
        int draggingFrameIndex;
        float dragStartTime;
        uint32_t dragStartTrack;

        // Keyframe marker selection and horizontal drag preview
        int selectedKeyFrameIndex;
        int selectedKeyIndex;
        bool isDraggingKey;
        float keyDragStartTime;
        float keyDragTime;

        // Resize
        bool isResizingFrame;
        int resizingFrameIndex;
        int resizeSide; // -1 = left edge, 1 = right edge
        float resizeStartTime;
        float resizeStartDuration;
        // A key, block or block-edge drag as one undo step: Esc (or Ctrl+Z) while the mouse is
        // held puts the timing back (S24 R12)
        InteractiveEdit timelineEdit;
        bool timelineEditStarted = false;
        bool timelineDragWasModified = false;

        // Snap
        bool snapToGrid;
        float snapInterval;

        // Transition preview: currently active top-level animations blended together,
        // the target clip chosen for the next crossfade, and the clip whose time drives
        // the playhead.
        std::vector<Entity> previewAnimations;
        Entity transitionTarget;
        Entity previewPrimary;

        // Cached entity
        Entity selectedEntity;
        uint32_t selectedSceneId;

        // Tab notification
        bool hasNotification;
        bool windowOpen;
        bool focusRequested;
        bool isWindowVisible;

        // Animation preview
        bool isPreviewing;
        struct PreviewEntityState {
            Entity entity = NULL_ENTITY;
            Entity parent = NULL_ENTITY;
            YAML::Node components;
        };
        std::vector<PreviewEntityState> previewState;

        // Keying: right-click a track block or the empty tracks area to store the
        // target's current position/rotation/scale as a keyframe at the clicked
        // time; the toolbar Snapshot button keys every track at the playhead.
        int keyPopupFrameIndex;  // -1 = empty-area popup keying the animation's target
        float keyPopupTime;
        // Set by the Add Property button under the property list; the toolbar
        // opens the add popup on the next frame.
        bool addPropertyRequested = false;

        // transient toolbar warning (e.g. key skipped), cleared after a few seconds
        std::string keyNotice;
        double keyNoticeAt;

        // channel: 0 = position, 1 = rotation, 2 = scale, 3 = lattice offsets (lattice targets)
        void keyTargetChannels(Scene* scene, SceneProject* sceneProject, AnimationComponent& anim, Entity target,
                               float time, const std::vector<int>& channels);
        void keyTrackChannel(Scene* scene, SceneProject* sceneProject, Entity trackEntity,
                             float localTime, float explicitDuration, int channel);
        bool hasSnapshotTracks(Scene* scene, const AnimationComponent& anim) const;
        // a lattice track's block was selected: select its lattice and points in the Scene view
        void selectLatticeTrackPoints(SceneProject* sceneProject, Entity trackEntity);
        void snapshotTracks(Scene* scene, SceneProject* sceneProject, AnimationComponent& anim);
        // Commit any active key/frame drag and clear its transient state. Playback
        // and preview transitions call this before changing authoring permissions.
        void finishTimelineDrag(Scene* scene, SceneProject* sceneProject, bool refreshPreview);
        void beginTimelineDrag(SceneProject* sceneProject);
        // The drag's edit was cancelled: the live timing goes back, the drag ends (true)
        bool cancelledTimelineDrag(Scene* scene, SceneProject* sceneProject);

        // Helpers
        void drawToolbar(float width, AnimationComponent& anim, Scene* scene, SceneProject* sceneProject);
        void drawTimeRuler(ImVec2 canvasPos, ImVec2 canvasSize, float timeStart, float timeEnd);
        bool drawTracks(ImVec2 canvasPos, ImVec2 canvasSize, float timeStart,
                AnimationComponent& anim, SceneProject* sceneProject, bool allowSelection);
        bool drawPlayhead(ImVec2 canvasPos, ImVec2 canvasSize, float timeStart);

        float snapTime(float time) const;
        float timeToX(float time, float timeStart, ImVec2 canvasPos) const;
        float xToTime(float x, float timeStart, ImVec2 canvasPos) const;

        std::string getActionLabel(Entity actionEntity, Scene* scene) const;
        std::string getAnimationEntityLabel(Entity entity, Scene* scene) const;
        bool canPreviewEntity(Entity entity, Scene* scene) const;
        void collectPreviewEntitiesRecursive(Scene* scene, Entity entity, std::vector<Entity>& entities,
                             std::unordered_set<Entity>& visitedAnimations,
                             std::unordered_set<Entity>& collectedEntities) const;
        PreviewEntityState buildPreviewEntityState(Scene* scene, Entity entity) const;
        void restorePreviewState(Scene* scene) const;
        void applyPreviewModelBindPose(Scene* scene) const;
        float getAnimationDuration(const AnimationComponent& anim, Scene* scene) const;
        // Frame duration <= 0 means auto: resolved from the action's own duration.
        float effectiveFrameDuration(const ActionFrame& frame, Scene* scene) const;
        // Lanes the frames are drawn on (viewLanes): their tracks, re-laned when overlaps are
        // found on one track (engine-created frames all default to track 0). View only: never
        // writes ActionFrame::track nor marks the scene modified (spec 9.R31).
        void autoAssignTracks(const AnimationComponent& anim, SceneProject* sceneProject);
        // Seek clipEntity (NULL_ENTITY = the current timeline clip) to `time`,
        // collapsing any active transition preview to that single clip.
        void seekPreview(Scene* scene, SceneProject* sceneProject, float time, Entity clipEntity = NULL_ENTITY);
        // Clip the timeline displays: the blend target while a transition preview is
        // active, otherwise selectedEntity.
        Entity timelineClip(Scene* scene) const;

        void startPreview(Scene* scene);
        void stopPreview(Scene* scene, SceneProject* sceneProject, bool applyBindPose = false);

        // Snapshot + reset one animation's action tree so it can be previewed (and later
        // restored). Used for both the initial clip and any clip a transition blends in.
        void addAnimationToPreview(Scene* scene, Entity animEntity);
        // Crossfade the running preview clip(s) into transitionTarget over its fade time.
        void triggerTransitionPreview(Scene* scene, SceneProject* sceneProject);

        // ---- clip mode (AnimationWindowClipMode.cpp) ----
        // the mode for this frame from the selection of `sceneProject` (true: clip mode)
        bool updateMode(SceneProject* sceneProject);
        // the selection `selected` of `sceneProject` decides the mode
        void applySelection(SceneProject* sceneProject, const std::vector<Entity>& selected);
        void drawClipMode(SceneProject* sceneProject);
        // .anim files dropped anywhere on the window (spec 4.1): they join the bound or selected
        // GameObject's Animation Player (one scene step) and the first opens in clip mode
        bool dropClipFiles(SceneProject* sceneProject, const std::vector<std::string>& paths, std::string* error);
        void acceptClipDrop(SceneProject* sceneProject);
        void installClipHooks();

        // ---- timeline audio of the entity mode (S40 R22, R23) ----
        // the playing preview's audio actions (preview voices outside the mixer)
        std::unique_ptr<TimelineAudioPreview> actionAudio;
        // a new audio, Sound Event or Mixer Snapshot action (type) with its frame at `time`, one
        // undo step; `content` is the sound (assets-relative) or the snapshot name ("" none)
        bool addAudioAction(SceneProject* sceneProject, Entity animEntity, EntityCreationType type, const std::string& content, float time,
                            uint32_t track, std::string* error);
        // the commands the entity mode shares with clip mode (Align, Add ... action): run, or
        // whether they can run
        bool runAudioActionCommand(std::string_view id, bool run);
        // where the selected audio action's frame starts after `align` ("" when it can; else why)
        std::string alignActionProblem(int align, float* time) const;
        // the audio actions the playhead crossed between two times of the playing preview
        void previewAudioActions(Scene* scene, Entity animEntity, float from, float to, float dt);
        // the Add Property menu's Sound submenu (true: an action was added)
        bool drawSoundMenu(SceneProject* sceneProject, AnimationComponent& anim, uint32_t track);

    public:
        static constexpr const char* WINDOW_NAME = "Animation";

        AnimationWindow(Project* project);
        ~AnimationWindow();

        // the Project window / Inspector open a clip (root NULL_ENTITY: target-less) or an
        // entity Animation in the window (AnimationClipInspectorHooks::openClip /
        // openEntityAnimation)
        void openClip(uint32_t sceneId, Entity root, const std::string& clip);
        void openEntityAnimation(uint32_t sceneId, Entity animation);
        bool isClipMode() const;
        // its clip mode (the catalog's animation.* commands run on it)
        AnimationWindowClip* getClipMode() const { return clipMode.get(); }

        void show();
        void setOpen(bool open);
        bool isOpen() const;

        void selectEntity(Entity entity, uint32_t sceneId);
        void clearSceneState(uint32_t sceneId);

        bool isPreviewingEntity(Entity entity, uint32_t sceneId) const;
        bool getIsPlaying() const;

        void externalPlay(Entity entity, uint32_t sceneId);
        void externalStop();
        void externalPause();
        void seekPreviewExternal(Scene* scene, SceneProject* sceneProject, float time);
        // the preview voices the playing entity preview started (tests)
        uint64_t previewAudioStarts() const;

        // Restores the entity-mode preview's snapshot before a save, play, scene switch or close
        // (animation clips spec 9.R6); App::stopTransientPreviews calls it.
        void stopTransientPreviews();
    };

}

