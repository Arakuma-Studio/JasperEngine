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
// - Extension points (T3c): View::Curves (drawCurves in AnimationWindowClipCurves.cpp) and the
//   Record toggle slot of the toolbar.
// - Every clip edit is one AnimationClipDocument commit on the project history (the controller);
//   scene steps (Create New Clip, Extract switching the player) are CreateClipCmd composites. The
//   window never writes a scene's modified flag. Main thread.

#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationClipController.h"
#include "animation/ClipValidation.h"
#include "ecs/Entity.h"
#include "imgui.h"
#include "json.hpp"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
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

        // a key by identity (stable across commits, AnimationClipController::findKey)
        struct KeyId{
            std::string path;
            ClipChannel channel = ClipChannel::Position;
            float time = 0.0f;
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
        // every frame (also while the window is hidden or closed): the controller's update
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
        // Create New Clip...; true when one was picked (the window switches to clip mode)
        bool drawEntityModeEntries(SceneProject* sceneProject, Entity selected);

        // MCP focus_window {window: "animation", animation: {...}} (AnimationWindowClipTestHook.cpp)
        bool testHook(SceneProject* sceneProject, const nlohmann::json& hook, nlohmann::json& out, std::string& error);
        nlohmann::json stateJson(SceneProject* sceneProject);
        // the hook's keys are known ones; `clipActions` false (entity mode): only the window's
        // (entity, entity_animation, clip, root)
        static bool checkHookKeys(const nlohmann::json& hook, bool clipActions, std::string& error);

    private:
        struct Row{
            enum class Kind{ Summary, Track, Component };
            Kind kind = Kind::Track;
            int track = -1;
            int component = -1;     // Component rows: the value component (x/y/z, morph target)
            std::string label;
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

        enum class Drag{ None, PendingKeys, Keys, Box, Scrub, PendingEvent, Event };

        Project* project = nullptr;
        View view = View::Dopesheet;
        // the shared controller is made on first use only (after App::engineInit: its undo
        // timeline chains onto the App's history listeners), never by a frame that doesn't use it
        mutable bool controllerUsed = false;

        // view state
        float pixelsPerSecond = 160.0f;
        float scrollX = 0.0f;
        bool showFrames = true;
        std::set<std::string> expanded;     // "path|channel" of unfolded rows

        // key selection by identity, and the refs of the current revision
        std::vector<KeyId> selection;
        uint64_t selectionRevision = 0;
        std::vector<AnimationClipController::KeyRef> selectedRefs;

        // interaction
        Drag drag = Drag::None;
        ImVec2 dragStart = ImVec2(0, 0);
        float dragGrabTime = 0.0f;
        float dragAppliedDelta = 0.0f;
        float dragMinTime = 0.0f;           // the earliest dragged key's time (it stops at 0)
        bool dragAdditive = false;
        int contextRow = -1;
        float contextTime = 0.0f;
        int rowMenuTrack = -1;

        // events
        int selectedEvent = -1;
        float eventDragBase = 0.0f;
        uint64_t eventDragSerial = 0;
        std::string eventMergeKey;

        // popups
        bool openAddProperty = false;
        char addPropertySearch[128] = {};
        int rebindTrack = -1;
        bool openRebind = false;
        char rebindSearch[128] = {};
        uint64_t valueEditSerial = 0;
        std::string valueMergeKey;

        // the clipboard of Ctrl+C / Ctrl+V
        std::vector<CopiedKey> clipboard;

        // a transient status line (warnings in WarningText)
        std::string notice;
        bool noticeWarning = false;
        double noticeAt = 0.0;

        // the binding report, rebuilt when the clip, the root or the hierarchy change
        ClipValidationReport report;
        uint64_t reportRevision = ~uint64_t(0);
        Entity reportRoot = NULL_ENTITY;
        uint64_t reportHierarchy = 0;
        double reportAt = -1.0;
        std::string reportClip;

        // what the dropdown asked the window to do
        bool entityAnimationRequested = false;
        uint32_t requestedScene = 0;
        Entity requestedAnimation = NULL_ENTITY;

        AnimationClipController& controller() const;
        std::shared_ptr<AnimationClipDocument> document() const;
        SceneProject* boundScene() const;
        Scene* valueScene() const;
        Entity valueRoot() const;
        void setNotice(const std::string& text, bool warning);

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
        void drawRebindPopup();
        void handleShortcuts();

        // ---- actions (UI and test hook) ----
        void seek(float time);
        void togglePreview();
        void togglePlay();
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
        bool snapKeysToFrames(std::string* error);
        bool setFrameRate(float rate, std::string* error);
        bool setComponentValue(size_t track, int component, float value, bool newEdit, std::string* error);
        bool setPreviewModel(const std::string& model, std::string* error);
        void frameKeys(bool selectedOnly, float width);

        // ---- events (AnimationWindowClipEvents.cpp) ----
        void drawEventsLane(ImDrawList* drawList, ImVec2 origin, float width, float height, float keyAreaX);
        void drawEventEditor(SceneProject* sceneProject);
        float eventEditorHeight() const;
        bool addEvent(float time, const std::string& function, const std::vector<AnimationEventArg>& args, std::string* error);
        bool editEvent(int index, const ClipEventData& event, const std::string& mergeKey, std::string* error);
        bool deleteEvent(int index, std::string* error);
        bool moveEvent(int index, float time, const std::string& mergeKey, std::string* error);
        std::vector<std::string> scriptFunctions() const;
    };

}
