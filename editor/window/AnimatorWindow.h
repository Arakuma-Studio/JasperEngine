// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 11.4)
// - Purpose: Window > Animation > Animator (Ctrl+7): Unity's Animator window for an Animator
//   Controller (.controller): the Layers and Parameters tabs, the state graph (states, the Any
//   State, transition arrows; pan, zoom, framing, box selection, drags, context menus, drops),
//   the selection's inspector on the right (state, transition with its conditions and timeline,
//   blend tree with a scrubbable parameter), the validation strip, an edit-mode preview on the
//   bound GameObject, and in play mode the live state, transition and parameters of that
//   GameObject's Animation Player.
// - What it shows: the controller of the selected GameObject's Animation Player (followed while
//   the selection changes to another GameObject with a controller), or a .controller opened by
//   openController (the Project window's double-click, the Inspector's Controller field, MCP,
//   AnimationClipAppHooks::openAnimator).
// - Edits: every edit is one project undo step through AnimatorControllerDocument (the clip undo
//   rules: a drag is one step, merged by a gesture key); the window follows undo, redo and outside
//   changes through the document's listener. The action functions below are what the window's
//   buttons, menus, drags and shortcuts run; the editor tests call the same ones.
// - The selection's inspector is the window's right panel (the Inspector keeps showing the scene
//   selection): there is no hook in the Inspector for another window's selection.
// - Thread-safety: main thread.

#pragma once

#include "animation/AnimAssetDocument.h"
#include "animation/AnimatorEdits.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/PreviewOwner.h"
#include "ecs/Entity.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::editor{

    class Project;
    struct SceneProject;

    class AnimatorWindow{
    public:
        static constexpr const char* WINDOW_NAME = "Animator";

        explicit AnimatorWindow(Project* project);
        ~AnimatorWindow();

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return windowFocused; }
        void requestFocus() { focusRequested = true; }

        // Shows `controller` (project-relative or asset-relative), bound to the Animation Player
        // of `entity` in `sceneId` when given (live view, preview root). False + error when it
        // does not open (the window then shows the error).
        bool openController(const std::string& controller, uint32_t sceneId, Entity entity, std::string* error);
        // binds to the selected GameObject when it (or an ancestor) has a player with a controller
        void followSelection();
        // before a save, Play, a scene switch: the preview restores the scene
        void stopTransientPreviews();
        void clearSceneState(uint32_t sceneId);
        void projectClosed();

        // ---- what it shows ----
        const std::shared_ptr<AnimatorControllerDocument>& document() const { return doc; }
        const std::string& openError() const { return error; }
        uint32_t boundScene() const { return sceneId; }
        Entity boundEntity() const { return entity; }
        int currentLayer() const { return layer; }
        void setCurrentLayer(int index);
        const animator::Item& selection() const { return selected; }
        const std::vector<int>& selectedStates() const { return selectedStateList; }
        // the validation of the current revision (anim::validateController, clips through the pool)
        const std::vector<ClipWarning>& warnings();
        const std::string& notice() const { return noticeText; }

        // ---- actions (each edit: one project undo step; false + notice() when refused) ----
        void select(const animator::Item& item, bool add = false);
        void selectStates(const std::vector<int>& states);
        void clearSelection();
        // a warning's JSON path selects its item (and its layer)
        void selectPath(const std::string& jsonPath);

        // Create State: Empty (no clip), From Clip (`clip` asset-relative .anim or "<model>#<clip>"),
        // From New Blend Tree; at a canvas position (null: the next grid cell). Its index, -1 when
        // refused
        int createState(const std::string& clip, bool blendTree, const std::array<float, 2>* at);
        // the dropped files (absolute or project paths): .anim files, a model's clips (one state each)
        int dropClips(const std::vector<std::string>& paths, const std::array<float, 2>& at);
        bool makeTransition(int from, int to);              // from -1: the Any State
        bool setDefaultState(int state);
        bool deleteSelection();
        bool copySelection();
        bool paste(const std::array<float, 2>& at);
        bool renameState(int state, const std::string& name);
        // a state, transition or layer changed by `edit` (the inspector's fields): `mergeKey`
        // merges a drag of one field into one step
        bool editState(int state, const std::function<void(anim::ControllerState&)>& edit, const std::string& mergeKey = "");
        bool editTransition(int transition, const std::function<void(anim::ControllerTransition&)>& edit, const std::string& mergeKey = "");
        bool editLayer(int layer, const std::function<void(anim::ControllerLayer&)>& edit, const std::string& mergeKey = "");

        // a state drag: one undo step from begin to end (the boxes move with dragStates)
        void beginDrag();
        bool dragStates(const std::vector<int>& states, float dx, float dy);
        bool dragAnyState(float dx, float dy);
        void endDrag();

        int addParameter(anim::ControllerParameterType type);
        bool renameParameter(int parameter, const std::string& name);
        bool setParameterDefault(int parameter, float value, const std::string& mergeKey = "");
        bool removeParameter(int parameter);

        int addLayer();
        bool removeLayer(int index);
        bool moveLayer(int from, int to);
        bool renameLayer(int index, const std::string& name);

        // play mode (bound to a playing GameObject): the runtime's parameter, not the file
        bool setLiveParameter(const std::string& name, float value);
        bool isLive() const;

        // F / A: the canvas pan and zoom that frame the selection / every state (`width` x
        // `height`: the canvas in pixels; 0: the last drawn size)
        void frameSelection(float width = 0.0f, float height = 0.0f);
        void frameAll(float width = 0.0f, float height = 0.0f);
        float zoom() const { return viewZoom; }
        std::array<float, 2> pan() const { return {panX, panY}; }

        // edit-mode preview of the selected state (a blend tree at `blendValue`) on the bound
        // GameObject; false (notice) when there is nothing to preview on
        bool startPreview();
        void stopPreview();
        bool isPreviewing() const { return previewing; }
        void setBlendValue(float value) { blendValue = value; }
        float getBlendValue() const { return blendValue; }
        // the preview's next frame (time + dt): the window calls it every frame
        void updatePreview(float dt);

    private:
        Project* project = nullptr;
        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequested = false;

        std::shared_ptr<AnimatorControllerDocument> doc;
        int listener = 0;
        std::string error;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        // the last selection followed (a new selection is followed once)
        uint32_t followedScene = 0;
        std::vector<Entity> followedSelection;
        int layer = 0;
        animator::Item selected;
        std::vector<int> selectedStateList;
        animator::Clipboard clipboard;
        std::string noticeText;
        bool noticeError = false;

        // the validation, per document revision
        std::vector<ClipWarning> validation;
        uint64_t validationRevision = ~uint64_t(0);

        // the canvas
        float panX = 0.0f, panY = 0.0f;
        float viewZoom = 1.0f;
        float canvasWidth = 800.0f, canvasHeight = 400.0f;
        bool framePending = true;
        int dragGesture = 0;
        bool dragging = false;
        int linkFrom = -3;              // Make Transition: the source (-1: Any State), -3: none
        bool boxSelecting = false;
        float boxStartX = 0.0f, boxStartY = 0.0f;
        int pressedState = -3;          // the state under the press of the current left drag
        bool pressMoved = false;
        int parameterRename = -1;       // the parameter whose name is being edited
        char renameBuffer[128] = {0};
        std::string pickerFilter;
        std::array<float, 2> pickerAt = {0.0f, 0.0f};
        std::array<float, 2> contextAt = {0.0f, 0.0f};
        std::vector<std::string> pickerClips;   // asset-relative .anim and model clips of the project
        bool pickerStale = true;
        bool pickerRequested = false;
        int fieldGesture = 0;

        // the preview
        bool previewing = false;
        PreviewOwner::Token previewToken = 0;
        anim::ClipPreviewSession previewSession;
        float previewTime = 0.0f;       // seconds (clip) or normalized time (tree)
        float blendValue = 0.0f;

        void setNotice(const std::string& text, bool isError);
        void close();
        void attach(const std::shared_ptr<AnimatorControllerDocument>& document);
        bool commit(anim::ControllerData data, const std::string& undoName, const std::string& mergeKey = "");
        const anim::ControllerLayer* currentLayerData() const;
        SceneProject* boundSceneProject() const;
        // the bound GameObject's player component (null when none)
        AnimationPlayerComponent* boundPlayer() const;
        std::string newMergeKey(const char* what);
        std::string clipGuidOf(const std::string& clip);

        void drawHeader();
        void drawLeftPanel(float width, float height);
        void drawLayersTab();
        void drawParametersTab();
        void drawCanvas(float width, float height);
        void drawCanvasMenus();
        void drawClipPicker();
        void drawInspector(float width, float height);
        void drawStateInspector(int state);
        void drawTransitionInspector(int transition);
        void drawBlendTreeInspector(int state);
        void drawValidationStrip();
        void refreshPickerClips();
    };

    // the validation the window, MCP and the Inspector show: anim::validateController with clips
    // read through AnimationClipPool (a clip loads; it loops by its own wrap)
    std::vector<ClipWarning> validateControllerClips(const anim::ControllerData& data);

}
