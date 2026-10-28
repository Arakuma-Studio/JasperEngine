// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R22-R24, R26, R28)
// - Purpose: Window > Animation > Animator: the editor of an Animator Controller (.controller).
//   Left: the Layers and Parameters tabs. Right: the graph of the layer's state machine (or a
//   sub-state machine, with a breadcrumb back): Entry (green), Any State (cyan), Exit (red), the
//   states (the default orange) and sub-state machines as boxes, transitions as arrows. Right
//   click creates states (empty, from a clip, from a new blend tree), sub-state machines and
//   transitions, sets the default state and deletes; clips dropped from the Project window become
//   states. The selection (state, transition, layer) shows in the Inspector
//   (animatorEditorHooks().drawSelection), with the blend tree editor for a tree state.
// - It follows the selected GameObject's Animator (or a pinned one, or a controller opened from
//   the Project window). In play mode it shows the live state (the active state's progress, the
//   transition's on its arrow, parameter values editable, triggers as buttons, layer weights and
//   the playing montage); in edit mode Preview evaluates the controller on the Animator (the
//   scene is put back when it stops), seeking the Particle Systems under it (S20) unless the
//   preference scene_view.preview_particles is off.
// - Every edit is one named project undo step (AnimatorAssetDocument::commit); a drag (a value,
//   nodes) is staged while it lasts and committed once when it ends.
// - Thread-safety: main thread.

#pragma once

#include "animation/AnimatorAssetDocument.h"
#include "animation/PreviewOwner.h"
#include "ecs/Entity.h"
#include "window/widget/GraphCanvas.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::anim{
    class AnimatorInstance;
}

namespace jasperengine::editor{

    class Project;
    class PoseAnimationWindow;

    class AnimatorWindow{
    public:
        static constexpr const char* WINDOW_NAME = "Animator";

        AnimatorWindow(Project* project, PoseAnimationWindow* poseWindow);
        ~AnimatorWindow();

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return windowFocused; }
        void requestFocus();

        // the window on a controller (and an Animator using it when entity is set)
        void openController(const std::string& path, uint32_t sceneId, Entity entity);
        // the Inspector's drawing of the selection; true when it drew
        bool drawSelectionInspector();
        // a scene closes / the project changes: its preview and target go
        void clearSceneState(uint32_t sceneId);
        void prepareForProjectSwitch();
        void stopPreview();
        bool isPreviewing() const { return previewToken != 0; }
        // the Inspector drew the window's selection this frame or the last (its undo is the project's)
        bool inspectorShowsSelection() const;

        // the window's state as tests and MCP read it
        const std::shared_ptr<AnimatorAssetDocument>& getDocument() const { return document; }
        int getLayer() const { return layer; }
        const std::vector<std::string>& getMachinePath() const { return machinePath; }

        // edits the window makes (also the context menus' actions; public for the editor tests)
        bool addState(const std::string& name, const ImVec2& position, const anim::MotionDef& motion);
        bool addSubStateMachine(const std::string& name, const ImVec2& position);
        bool addTransition(const std::string& fromState, const std::string& toNode, bool fromAnyState, bool fromEntry);
        bool setDefaultState(const std::string& name);
        bool removeSelection();
        bool addParameter(const std::string& name, AnimatorParameterType type);
        bool addLayer(const std::string& name);
        void selectState(const std::string& name);

    private:
        enum class SelectionKind { None, State, SubMachine, Transition, Layer };
        // which list a transition is in
        enum class ListKind { State, AnyState, Entry, Exit };
        struct Selection{
            SelectionKind kind = SelectionKind::None;
            std::string name;       // state or sub-machine (in the current machine)
            ListKind list = ListKind::State;
            std::string owner;      // ListKind::State: the state; Exit: the sub-machine
            int index = -1;         // transition index in its list
            bool operator==(const Selection& o) const {
                return kind == o.kind && name == o.name && list == o.list && owner == o.owner && index == o.index;
            }
        };

        Project* project;
        PoseAnimationWindow* poseWindow;
        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequest = false;

        std::shared_ptr<AnimatorAssetDocument> document;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        bool pinned = false;
        int layer = 0;
        std::vector<std::string> machinePath;
        Selection selection;
        // the Inspector shows the selection from a click in the window until the scene selection
        // or the inspected asset changes (the last selection wins, as in Unity)
        bool inspectorClaim = false;
        uint32_t claimScene = 0;
        std::vector<Entity> claimEntities;
        std::string claimAsset;
        int inspectorFrame = -10;
        GraphCanvas canvas;
        int leftTab = 0;
        std::string status;
        uint64_t documentRevision = 0;
        ImVec2 contextPosition;
        // a model dropped on the canvas: its clips offered in a popup
        std::string droppedModel;
        ImVec2 dropPosition;
        std::string renameBuffer;
        // the edit-mode preview
        PreviewOwner::Token previewToken = 0;
        uint32_t previewScene = 0;
        Entity previewEntity = NULL_ENTITY;
        double previewClock = 0.0;
        double lastFrameTime = 0.0;
        bool previewPaused = false;
        float transitionScrub = 0.0f;
        // an edit in progress (a drag): shown at once, committed as one step when no item is active
        std::optional<anim::ControllerDef> pending;
        std::string pendingUndo;

        void followSelection();
        void claimInspector();
        bool claimHolds();
        void setDocument(const std::string& path);
        anim::ControllerDef working() const;
        bool commit(const anim::ControllerDef& def, const std::string& undoName, const std::string& mergeKey = std::string());
        void stage(const anim::ControllerDef& def, const std::string& undoName);
        void flush();
        anim::StateMachineDef* machineOf(anim::ControllerDef& def);
        anim::AnimatorInstance* liveInstance() const;
        bool editable() const;

        void drawToolbar();
        void drawLayers();
        void drawParameters();
        void drawGraph();
        void drawContextMenus();
        void startPreview();
        void updatePreview();
        void seekParticles(double time, bool restart);

        // Inspector panels
        void drawStateInspector(anim::ControllerDef& def, anim::StateDef& state);
        void drawTransitionInspector(anim::ControllerDef& def, anim::TransitionDef& transition, const std::string& from);
        void drawLayerInspector(anim::ControllerDef& def);
        bool drawBlendTree(anim::ControllerDef& def, anim::BlendTreeDef& tree, int depth);
    };

}
