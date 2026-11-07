// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's clip mode (animation clips spec 4.4, 9.R4-R7/R31/R39/R54): the UI of
// the AnimationClipController (which owns the bound root, the open clip, the preview and every
// edit). The window (AnimationWindow) decides the mode from the selection and draws this when it
// is in clip mode; the entity mode (AnimationComponent sequences) stays in AnimationWindow.cpp.
// - Files: AnimationWindowClipSheet.cpp (toolbar, dropdown, empty state, banners, binding rows,
//   value column, keys, Add Property, Rebind, shortcuts), AnimationWindowClipEvents.cpp (events
//   lane and event editor), AnimationWindowClipMode.cpp (the AnimationWindow side: mode switch, hooks),
//   AnimationWindowClipCurves.cpp (the curves view, T3c), AnimationWindowClipRecord.cpp (Record and
//   the window's saved settings, T3c).
// - Curves view (View::Curves, the Dopesheet / Curves toggle at the bottom left): per bound channel
//   component the curve the runtime sampler gives (AnimationWindowClipCurves.h), key points dragged
//   in time and value through the controller's key drag API (one merge key per drag).
// - Record (the red button after Preview, R): the record sources (Scene view gizmo, Inspector rows,
//   lattice drags; ClipRecordHooks) key edits of the bound hierarchy at the playhead; leaving Record
//   stops the preview (the scene goes back to its snapshot).
// - Every clip edit is one AnimationClipDocument commit on the project history (the controller);
//   scene steps (Create New Clip, Extract switching the player) are CreateClipCmd composites. The
//   window never writes a scene's modified flag. Main thread.
// - Scale: key selection is resolved once per document revision / selection change into a per-track
//   mask (O(1) lookups); rows draw only the keys in the visible time range (binary search on the
//   sorted keys); script methods are cached by a stamp of the root's scripts (no file read per frame).

#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationClipController.h"
#include "animation/ClipValidation.h"
#include "ecs/Entity.h"
#include "imgui.h"
#include "math/Vector3.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::editor{

    class Project;
    class CommandCatalog;
    struct SceneProject;

    // editor/tests/AnimationWindowClipDriver.h: the editor tests' driver of the clip mode (its
    // actions by name, its state as JSON)
    struct AnimationWindowClipTestAccess;

    class AnimationWindowClip{
        friend struct AnimationWindowClipTestAccess;
    public:
        // The catalog's animation.* commands (shortcuts/DefaultCommands.cpp) run on the clip
        // window target() returns, while it takes keys (App; the editor tests' own catalog)
        static void bindCommands(CommandCatalog& catalog, std::function<AnimationWindowClip*()> target);

        // the key rows or the curves (AnimationWindowClipCurves.cpp)
        enum class View{ Dopesheet, Curves };

        // a key by identity (stable across commits)
        struct KeyId{
            std::string path;
            ClipChannel channel = ClipChannel::Position;
            float time = 0.0f;
            std::string property;       // property tracks: "component.property" (propertyId)
            bool operator==(const KeyId& other) const;
        };
        // the identity of the key of `track` at `time`
        static KeyId keyIdOf(const ClipTrackData& track, float time);

        // an event by identity (time, function, n-th of the events with both equal)
        struct EventId{
            float time = 0.0f;
            std::string function;
            int ordinal = 0;
        };
        // an audio cue by identity (S40: time, kind, n-th of the cues with both equal)
        struct CueId{
            float time = 0.0f;
            ClipCueKind kind = ClipCueKind::Sound;
            int ordinal = 0;
        };
        // R10's alignments of a cue (or an audio action) to the playhead
        enum class CueAlign{ Start, Peak, End };
        // the start time an alignment gives a sound of `length` seconds (and its peak, peakMs < 0:
        // none) for the playhead at `playhead`; false and why when it would start before 0 or the
        // sound has no peak. Shared with the entity mode's audio actions.
        static bool alignedStart(CueAlign align, float playhead, double length, double peakMs, float& time, std::string* why);
        // the action mode's handler of the commands both modes share (Align, Add ... action): set by
        // AnimationWindow; returns whether it ran (enabled: whether it can)
        std::function<bool(std::string_view id, bool run)> entityCommands;
        // the window shows the entity mode and has focus: its commands take keys
        bool entityKeys = false;

        explicit AnimationWindowClip(Project* project);

        // ---- the window's mode switch ----

        // a GameObject was selected (its nearest Animation Player root binds; without one the
        // empty state shows). NULL_ENTITY unbinds.
        void bind(uint32_t sceneId, Entity selected);
        // the Project window / Inspector opened `clip` (.anim or "<model>#<clip>"); `root`
        // NULL_ENTITY: target-less (edits the file, previews on preview.model)
        bool open(uint32_t sceneId, Entity root, const std::string& clip, std::string* error);
        // a state of the bound player (the entity mode's dropdown)
        bool openState(const std::string& state, std::string* error);
        // .anim files dropped on the window (spec 4.1): with a GameObject bound in `sceneProject`
        // (clip mode's root, else `fallback`) they join its Animation Player (added when it has
        // none; one scene undo step, clips it lists already are reused) and the first opens as
        // its state; with none the first opens target-less
        bool dropClips(SceneProject* sceneProject, Entity fallback, const std::vector<std::string>& paths, std::string* error);
        // something to show without a selection: a clip opened target-less
        bool hasTargetlessClip() const;
        // every frame (also while the window is hidden or closed): the notice timeout and the
        // controller's takesKeys (its update runs from the App's per-frame hook)
        void update(double dt, bool clipModeShown, bool focused);
        // the window closed or left clip mode: the preview restores
        void stopPreview();
        // Record is on (the window keeps its binding while recording, Unity's lock)
        bool isRecording() const;
        // a scene is gone
        void sceneClosed(uint32_t sceneId);

        // draws clip mode (the window's content region)
        void draw(SceneProject* sceneProject, bool focused);

        // the dropdown picked an entity Animation (the window switches to entity mode on it)
        bool takeEntityAnimationRequest(uint32_t& sceneId, Entity& animation);
        // the entity mode's clip dropdown entries for `selected` (R31): its player's states and
        // Create New Clip...; true when one was picked: the window restores its entity preview,
        // then runEntityModeRequest acts on it and the window switches to clip mode
        bool drawEntityModeEntries(SceneProject* sceneProject, Entity selected);
        void runEntityModeRequest(SceneProject* sceneProject, Entity selected);
        // the entries drawEntityModeEntries shows for `selected`
        std::vector<std::string> entityModeEntries(SceneProject* sceneProject, Entity selected) const;

        // the ImGui ini handler of the window's saved settings (label width, Dopesheet / Curves):
        // the App registers it right after creating the ImGui context (AnimationWindowClipRecord.cpp)
        static void registerSettingsHandler();

    private:
        struct Row{
            enum class Kind{ Summary, Track, Component };
            Kind kind = Kind::Track;
            int track = -1;
            int component = -1;     // Component rows: the value component (x/y/z, morph target)
            std::string label;      // the full label "Path : Channel (Missing!)"
            std::string leaf;       // what the row shows: "Leaf : Channel"
            std::string channelText;    // what follows " : " ("Position", a property's "Image.Color")
            bool missing = false;
            std::string message;    // the binding problem (tooltip)
            // the layer's mask hides the track's target: "masked out by Arm.mask on layer Holding"
            std::string masked;
        };

        struct CopiedKey{
            std::string path;
            ClipChannel channel = ClipChannel::Position;
            std::string component;      // property tracks
            std::string property;
            PropertyKind kind = PropertyKind::None;     // property tracks: the copied track's kind
            std::vector<int> points;
            float offset = 0.0f;    // from the earliest copied key
            ClipKey key;
        };

        struct ScriptCache{
            std::vector<std::string> functions;
            uint32_t sceneId = 0;
            Entity root = NULL_ENTITY;
            std::string stamp;
            double checkedAt = -10.0;
            bool hasCpp = false;
            uint64_t version = 0;
        };

        enum class Drag{ None, PendingKeys, Keys, Box, Scrub, PendingEvent, Event, Splitter, PendingCurve, CurveKeys, CurveBox, CurvePan, CurveTangent,
                         PendingCue, Cue };
        enum class EntityModeRequest{ None, State, Create };

        Project* project = nullptr;
        View view = View::Dopesheet;
        // the shared controller is made on first use only (after App::engineInit: its undo
        // timeline chains onto the App's history listeners), never by a frame that doesn't use it
        mutable bool controllerUsed = false;

        // view state
        float pixelsPerSecond = 160.0f;
        float scrollX = 0.0f;
        bool showFrames = true;
        float labelWidth = 320.0f;          // unscaled; the splitter resizes it
        std::set<std::string> expanded;     // "path|channel" of unfolded rows

        // key selection by identity, resolved into refs and a per-track mask when the document
        // revision or the selection changes
        std::vector<KeyId> selection;
        std::vector<KeyId> resolvedSelection;
        std::weak_ptr<AnimationClipDocument> resolvedDocument;
        uint64_t selectionRevision = 0;
        std::vector<AnimationClipController::KeyRef> selectedRefs;
        std::vector<std::vector<char>> selectedMask;
        std::unordered_map<std::string, int> trackIndex;
        std::weak_ptr<AnimationClipDocument> indexDocument;
        uint64_t indexRevision = 0;

        // interaction
        Drag drag = Drag::None;
        ImVec2 dragStart = ImVec2(0, 0);
        float dragGrabTime = 0.0f;
        float dragAppliedDelta = 0.0f;
        float dragMinTime = 0.0f;           // the earliest dragged key's time (it stops at 0)
        float dragStartLabelWidth = 0.0f;
        bool dragAdditive = false;
        int contextRow = -1;
        float contextTime = 0.0f;
        int rowMenuTrack = -1;
        // a row label was right-clicked: drawRowMenu opens the menu in its own ID scope (the
        // label's is its row's)
        bool rowMenuRequested = false;
        // the keys under the mouse, by identity (valid after a commit in the same frame)
        std::vector<KeyId> hoveredKeyIds;
        float hoveredTime = 0.0f;

        // events: the selection by identity, its index resolved per revision
        int selectedEvent = -1;
        bool eventSelected = false;
        EventId selectedEventId;
        uint64_t eventRevision = 0;
        std::weak_ptr<AnimationClipDocument> eventDocument;
        bool openEventEditor = false;
        float eventDragBase = 0.0f;
        uint64_t eventDragSerial = 0;
        std::string eventMergeKey;

        // audio cues (S40): the selection by identity, its index resolved per revision
        int selectedCue = -1;
        bool cueSelected = false;
        CueId selectedCueId;
        uint64_t cueRevision = 0;
        std::weak_ptr<AnimationClipDocument> cueDocument;
        bool openCueEditor = false;
        float cueDragBase = 0.0f;
        uint64_t cueDragSerial = 0;
        std::string cueMergeKey;
        // Add Property opened by an Add ... Cue command: that section unfolded
        int addPropertyCueKind = -1;

        // popups
        bool openAddProperty = false;
        char addPropertySearch[128] = {};
        int rebindTrack = -1;
        bool openRebind = false;
        char rebindSearch[128] = {};
        // computed when the Rebind popup opens
        std::vector<std::string> rebindCandidates;
        std::vector<std::string> rebindReasons;     // why a candidate can't take the track ("" = it can)
        size_t rebindNearest = 0;
        size_t rebindSceneUsers = 0;
        size_t rebindPrefabUsers = 0;
        bool rebindMissing = false;
        uint64_t valueEditSerial = 0;
        std::string valueMergeKey;
        // the Euler triple of the rotation field being dragged (kept while active, M13)
        int eulerEditTrack = -1;
        Vector3 eulerEdit = Vector3(0, 0, 0);

        // a read-only or target-less clip's "Preview on" for this session (never written)
        std::string previewOverride;

        // the clipboard of Ctrl+C / Ctrl+V
        std::vector<CopiedKey> clipboard;

        // a transient status line (warnings in WarningText)
        std::string notice;
        bool noticeWarning = false;
        double noticeAt = 0.0;

        // the binding report, rebuilt when the clip, the root, the hierarchy or the scripts change
        ClipValidationReport report;
        uint64_t reportRevision = ~uint64_t(0);
        Entity reportRoot = NULL_ENTITY;
        uint64_t reportHierarchy = 0;
        uint64_t reportScripts = 0;
        double reportAt = -1.0;
        std::string reportClip;
        mutable ScriptCache scriptCache;

        // what the dropdowns asked the window to do
        bool entityAnimationRequested = false;
        uint32_t requestedScene = 0;
        Entity requestedAnimation = NULL_ENTITY;
        EntityModeRequest entityModeRequest = EntityModeRequest::None;
        std::string entityModeState;

        // ---- curves view (T3c) ----
        // a curve point: a key and one of its value components
        struct CurveKeyId{
            KeyId key;
            int component = 0;
        };
        // One track's curves, cached per (document revision, track content, visible time range,
        // zoom, visibility): sample times, values (times x components) and key values (keys x
        // components; rotations as continuous Euler degrees)
        struct CurveTrackCache{
            size_t components = 0;
            uint64_t contentHash = 0;
            std::vector<char> visible;          // per component
            bool anyVisible = false;
            std::vector<float> times;
            std::vector<float> values;
            std::vector<float> keyValues;
            std::vector<Vector3> eulers;        // rotation: keyEulers
        };
        struct CurveCache{
            std::weak_ptr<AnimationClipDocument> document;
            uint64_t revision = ~uint64_t(0);
            float from = -1.0f, to = -1.0f, pixelsPerSecond = 0.0f;
            uint64_t hiddenVersion = ~uint64_t(0);
            std::vector<CurveTrackCache> tracks;
        };
        // hidden curves: "path|channel#component"
        std::set<std::string> hiddenCurves;
        uint64_t hiddenVersion = 0;
        CurveCache curveCache;
        // the curve points selected in the curves view: a selected key (the shared KeyId selection)
        // with entries here has only those components selected, without entries all of them
        // (the dopesheet's selection); selectKeys clears it
        std::vector<CurveKeyId> curveSelection;
        uint64_t curveSelectionVersion = 0;
        // curveSelection resolved per revision: (track, key, component) and the keys with entries
        std::set<std::tuple<size_t, size_t, int>> curveSelected;
        std::set<std::pair<size_t, size_t>> curveSelectedKeys;
        uint64_t curveSelectedRevision = ~uint64_t(0);
        uint64_t curveSelectedVersion = ~uint64_t(0);
        std::weak_ptr<AnimationClipDocument> curveSelectedDocument;
        // the value range shown (value units: Euler degrees for rotations), framed on a new clip
        float curveMin = -1.0f;
        float curveMax = 1.0f;
        std::string curveRangeClip;
        // the key point grabbed (its track and component) and the drag's state: the dragged keys,
        // per key the components the value moves and a rotation key's base Euler
        int curveGrabTrack = -1;
        int curveGrabComponent = -1;
        // a tangent handle being dragged (S21 R27): its key, component and side; one undo step
        AnimationClipController::KeyRef tangentRef;
        int tangentComponent = -1;
        bool tangentOutSide = true;
        float tangentKeyTime = 0.0f;
        std::string tangentMergeKey;
        int tangentDrags = 0;
        float curveGrabValue = 0.0f;
        float curveDragTime = 0.0f;
        float curveDragValue = 0.0f;
        std::vector<AnimationClipController::KeyRef> curveDragRefs;
        std::vector<std::vector<int>> curveDragComponents;
        std::vector<Vector3> curveDragEulers;
        std::vector<CurveKeyId> curveDragSelection;
        // the rotation tracks' continuous key Eulers, per document revision (trackEulers)
        mutable std::vector<std::vector<Vector3>> curveEulerCache;
        mutable std::vector<char> curveEulerValid;
        mutable uint64_t curveEulerRevision = ~uint64_t(0);
        mutable std::weak_ptr<AnimationClipDocument> curveEulerDocument;
        float curvePixelsPerValue = 1.0f;   // the graph's scale in the last frame
        float curveGraphHeight = 1.0f;

        // ---- Record and the saved settings (T3c, AnimationWindowClipRecord.cpp) ----
        bool settingsLoaded = false;
        // the preview was on when Record started (leaving Record keeps it, Unity)
        bool previewBeforeRecord = false;

        // the CPU time of draw(): a moving average, the worst and the frames drawn (large clips stay
        // responsive: AnimationWindowClipTest)
        double drawMs = 0.0;
        double drawMsMax = 0.0;
        uint64_t drawFrames = 0;

        AnimationClipController& controller() const;
        std::shared_ptr<AnimationClipDocument> document() const;
        SceneProject* boundScene() const;
        Scene* valueScene() const;
        Entity valueRoot() const;
        void setNotice(const std::string& text, bool warning);
        float scaledLabelWidth() const;
        // a player added to the bound GameObject after its selection opens its default state
        void refreshBinding();

        // ---- the sheet (AnimationWindowClipSheet.cpp) ----
        void refreshReport();
        void refreshSelection();
        void setSelectionFromRefs(const std::vector<AnimationClipController::KeyRef>& refs);
        bool isSelected(size_t track, size_t key) const;
        std::vector<Row> buildRows() const;
        std::vector<float> sampledValue(size_t track, float time) const;
        std::vector<std::string> componentLabels(size_t track) const;
        float frameRate() const;
        float snapToFrame(float time) const;
        float viewLength() const;
        // the model a target-less clip previews on: the override, preview.model, or a sub-clip's
        // own model
        std::string previewModelOf() const;
        // an instance of `model` (a model file or a prefab) in `sceneProject`, selection first
        Entity findModelInstance(SceneProject* sceneProject, const std::string& model) const;
        // why Preview can't start now ("" when it can)
        std::string previewBlocker(SceneProject* sceneProject) const;
        // starts the preview (a target-less clip without preview.model binds an instance of its
        // preview model first)
        bool startPreview(SceneProject* sceneProject, std::string* error);

        void drawToolbar(SceneProject* sceneProject);
        void drawClipCombo(SceneProject* sceneProject);
        void drawBanners(SceneProject* sceneProject);
        void drawEmptyState(SceneProject* sceneProject);
        void drawSheet(SceneProject* sceneProject, bool focused);
        void drawRuler(ImDrawList* drawList, ImVec2 origin, float width, float height);
        void drawRowLabel(const Row& row, size_t index, ImVec2 pos, float width, float height);
        void drawValueField(const Row& row, ImVec2 pos, float width, float height);
        void drawKeyContextMenu();
        void drawRowMenu();
        void drawAddPropertyPopup();
        void openRebindFor(int track);
        void drawRebindPopup();
        // a catalog animation.* command (S25), run by the dispatcher while the window takes keys
        void runKeyCommand(std::string_view id);
        float sheetWidth = 1.0f;     // the key sheet's width at the last draw (F / A frame over it)

        // ---- actions (the buttons, menus and shortcuts) ----
        void seek(float time);
        void togglePreview(SceneProject* sceneProject);
        void togglePlay(SceneProject* sceneProject);
        void stepFrame(int direction);
        void stepKey(int direction);
        bool createNewClip(SceneProject* sceneProject, const std::string& path, std::string* error);
        void askCreateNewClip(SceneProject* sceneProject);
        bool extract(SceneProject* sceneProject, const std::string& path, std::string* error);
        void askExtract(SceneProject* sceneProject);
        bool selectKeys(const std::vector<KeyId>& keys, bool additive);
        bool boxSelect(int firstRow, int lastRow, float fromTime, float toTime, bool additive);
        bool beginDrag(std::string* error);
        bool updateDrag(float rawDelta, std::string* error);
        void endDrag();
        bool deleteSelected(std::string* error);
        void copySelected();
        bool paste(float time, std::string* error);
        bool duplicateSelected(std::string* error);
        bool setEase(EaseType ease, std::string* error);
        // the selected keys' tangent mode (the key menu's Tangents)
        bool setTangentMode(ClipTangentMode mode, std::string* error);
        bool addKeysAt(int track, float time, std::string* error);
        bool keyAt(bool modifiedOnly, std::string* error);
        bool snapKeysToFrames(std::string* error);
        bool setFrameRate(float rate, std::string* error);
        bool setComponentValue(size_t track, int component, float value, bool newEdit, std::string* error);
        // a property track's whole key value (key space) from the value column: previewed (pink)
        // until K, or keyed at the playhead with Record (anim-properties spec 10)
        bool setPropertyValue(size_t track, const std::vector<float>& value, bool newEdit, std::string* error);
        // the value column of a property row (checkbox, int, float, colour, vector components)
        void drawPropertyValueField(const Row& row, ImVec2 pos, float width, float height);
        bool setPreviewModel(const std::string& model, std::string* error);
        void frameKeys(bool selectedOnly, float width);

        // ---- curves view (AnimationWindowClipCurves.cpp) ----
        // draws the curves in place of the key rows: the curve list (labels, visibility) on the
        // left, the graph (value axis, curves, key points, tangents) on the right
        void drawCurves(SceneProject* sceneProject, ImVec2 origin, float labelW, float width, float height);
        // F / A: the time range (frameKeys) and the value range of the selected keys / all curves
        void frameCurveValues(bool selectedOnly);
        bool curveVisible(const ClipTrackData& track, int component) const;
        // a rotation track's continuous key Eulers (clipcurves::keyEulers, cached per revision)
        const std::vector<Vector3>& trackEulers(size_t track) const;
        void setCurveVisible(const ClipTrackData& track, int component, bool visible);
        // a drag of the selected curve points (time frame snapped for their keys; the value delta
        // added to every selected point on its own curve, rotations in Euler degrees: one absolute
        // value per dragged key through the controller's drag API, one merge key)
        bool beginCurveDrag(int track, int component, std::string* error);
        bool updateCurveDrag(float rawTimeDelta, float valueDelta, std::string* error);
        // click / box selection of curve points (their keys join the shared selection)
        void selectCurvePoints(const std::vector<CurveKeyId>& points, bool additive);
        void refreshCurveSelection();
        bool curvePointSelected(size_t track, size_t key, int component) const;
        // the components a selected key's value moves in a curve drag (its selected points, or
        // every visible component when the key was selected as a whole)
        std::vector<int> curveDragComponentsOf(size_t track, size_t key) const;
        // the per-track sample cache for the visible range [from, to] at the current zoom
        void refreshCurveCache(float from, float to, float stepTime);

        // ---- Record and settings (AnimationWindowClipRecord.cpp) ----
        // Record on (starts the preview) / off (stops it: the scene goes back to its snapshot)
        bool setRecording(SceneProject* sceneProject, bool on, std::string* error);
        // what Record does now ("" when not recording): `full` the sentence (the tooltip), else the
        // short form of the bottom status
        std::string recordStatus(bool full = true) const;
        // the label column width and the view, kept in the editor layout ini (an ImGui settings
        // handler): loaded on the first frame, saved when they change
        void loadSettings();
        void saveSettings();

        // ---- events (AnimationWindowClipEvents.cpp) ----
        void drawEventsLane(ImDrawList* drawList, ImVec2 origin, float width, float height, float keyAreaX);
        void drawEventEditor(SceneProject* sceneProject);
        void selectEvent(int index);
        void resolveSelectedEvent();
        bool addEvent(float time, const std::string& function, const std::vector<AnimationEventArg>& args, std::string* error);
        bool editEvent(int index, const ClipEventData& event, const std::string& mergeKey, std::string* error);
        bool deleteEvent(int index, std::string* error);
        bool moveEvent(int index, float time, const std::string& mergeKey, std::string* error);
        std::vector<std::string> scriptFunctions() const;
        // why `event` can't be written ("" when valid): identifier (no Lua keyword), args
        static std::string eventProblem(const ClipEventData& event);

        // ---- audio cues (AnimationWindowClipAudio.cpp, S40 R9-R11) ----
        // the lane's height: a row per cue kind in use, one while sounds are dragged over the window
        float audioLaneHeight(const ClipData& data) const;
        // the kinds the lane shows, in row order
        std::vector<ClipCueKind> audioLaneKinds(const ClipData& data) const;
        void drawAudioLane(ImDrawList* drawList, ImVec2 origin, float width, float height, float timeOriginX);
        void drawAudioLaneLabels(ImDrawList* drawList, ImVec2 origin, float width);
        void drawCueEditor();
        // the Add Property popup's Timeline Audio section (Audio Cue, Snapshot Cue) and an entity's Source Cue
        bool drawAddCueItems(const std::string& needle);
        bool drawSourceCueItem(Entity entity, const std::string& path);
        void selectCue(int index);
        void resolveSelectedCue();
        bool addCue(const ClipAudioCue& cue, const std::string& undoName, std::string* error);
        bool editCue(int index, const ClipAudioCue& cue, const std::string& undoName, const std::string& mergeKey, std::string* error);
        bool deleteCue(int index, std::string* error);
        bool moveCue(int index, float time, const std::string& mergeKey, std::string* error);
        bool alignCue(int index, CueAlign align, std::string* error);
        // why the alignment can't run now ("" when it can; `time` the start it gives)
        std::string alignProblem(int index, CueAlign align, float* time) const;
        // sounds dropped at `time`: one Audio Cue each, one undo step
        bool dropSounds(const std::vector<std::string>& assetPaths, float time, std::string* error);
        // what a cue plays (assets-relative): its sound, or a source cue's AudioSource's clip or event
        std::string cueSound(const ClipAudioCue& cue) const;
        // its block's length in seconds at the preview's speed (0: unknown)
        double cueLength(const ClipAudioCue& cue) const;
    };

}
