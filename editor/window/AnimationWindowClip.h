// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's clip mode (animation clips spec 4.4, 9.R4-R7/R31/R39/R54): the UI of
// the AnimationClipController (which owns the bound root, the open clip, the preview and every
// edit). The window (AnimationWindow) decides the mode from the selection and draws this when it
// is in clip mode; the entity mode (AnimationComponent sequences) stays in AnimationWindow.cpp.
// - Files: AnimationWindowClipSheet.cpp (toolbar, dropdown, empty state, banners, binding rows,
//   value column, keys, Add Property, Rebind, shortcuts), AnimationWindowClipEvents.cpp (events
//   lane and event editor), AnimationWindowClipTestHook.cpp (focus_window `animation` hook),
//   AnimationWindowClipMode.cpp (the AnimationWindow side: mode switch, hooks).
// - Extension points (T3c): View::Curves (the Dopesheet / Curves toggle at the bottom left; drawCurves
//   in AnimationWindowClipCurves.cpp) and the Record button after Preview (reserved, disabled).
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
#include "json.hpp"
#include "math/Vector3.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::editor{

    class Project;
    struct SceneProject;

    class AnimationWindowClip{
    public:
        // Dopesheet now; Curves is T3c's (AnimationWindowClipCurves.cpp)
        enum class View{ Dopesheet, Curves };

        // a key by identity (stable across commits)
        struct KeyId{
            std::string path;
            ClipChannel channel = ClipChannel::Position;
            float time = 0.0f;
            bool operator==(const KeyId& other) const;
        };

        // an event by identity (time, function, n-th of the events with both equal)
        struct EventId{
            float time = 0.0f;
            std::string function;
            int ordinal = 0;
        };

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
        // something to show without a selection: a clip opened target-less
        bool hasTargetlessClip() const;
        // every frame (also while the window is hidden or closed): the notice timeout and the
        // controller's takesKeys (its update runs from the App's per-frame hook)
        void update(double dt, bool clipModeShown, bool focused);
        // the window closed or left clip mode: the preview restores
        void stopPreview();
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
        // the entries drawEntityModeEntries shows for `selected` (test hook)
        std::vector<std::string> entityModeEntries(SceneProject* sceneProject, Entity selected) const;

        // MCP focus_window {window: "animation", animation: {...}} (AnimationWindowClipTestHook.cpp)
        bool testHook(SceneProject* sceneProject, const nlohmann::json& hook, nlohmann::json& out, std::string& error);
        // `brief`: no key times or selected keys listed (counts only: large clips)
        nlohmann::json stateJson(SceneProject* sceneProject, bool brief = false);
        // the hook's keys are known ones; `clipActions` false (entity mode): only the window's
        static bool checkHookKeys(const nlohmann::json& hook, bool clipActions, std::string& error);

    private:
        struct Row{
            enum class Kind{ Summary, Track, Component };
            Kind kind = Kind::Track;
            int track = -1;
            int component = -1;     // Component rows: the value component (x/y/z, morph target)
            std::string label;      // the full label "Path : Channel (Missing!)"
            std::string leaf;       // what the row shows: "Leaf : Channel"
            bool missing = false;
            std::string message;    // the binding problem (tooltip)
        };

        struct CopiedKey{
            std::string path;
            ClipChannel channel = ClipChannel::Position;
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

        enum class Drag{ None, PendingKeys, Keys, Box, Scrub, PendingEvent, Event, Splitter };
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
        int openEventEditorDelay = 0;       // frames until the test hook's open (after its focus)
        float eventDragBase = 0.0f;
        uint64_t eventDragSerial = 0;
        std::string eventMergeKey;

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

        // the CPU time of draw() (test hook: large clips stay responsive)
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
        void handleShortcuts();

        // ---- actions (UI and test hook) ----
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
        bool addKeysAt(int track, float time, std::string* error);
        bool keyAt(bool modifiedOnly, std::string* error);
        bool snapKeysToFrames(std::string* error);
        bool setFrameRate(float rate, std::string* error);
        bool setComponentValue(size_t track, int component, float value, bool newEdit, std::string* error);
        bool setPreviewModel(const std::string& model, std::string* error);
        void frameKeys(bool selectedOnly, float width);

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
    };

}
