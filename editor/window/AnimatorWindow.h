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
//   (animatorEditorHooks().drawSelection), with the blend tree editor for a tree state. A box or
//   Shift / Ctrl click selects several nodes: they drag and delete as one undo step, and the
//   Inspector and the node menu act on one of them: the one clicked last, after a box the one
//   that was shown if it is still selected, else the first state.
// - It follows the selected GameObject's Animator (or a pinned one, or a controller opened from
//   the Project window). In play mode it shows the live state (the active state's progress, the
//   transition's on its arrow, parameter values editable, triggers as buttons, layer weights and
//   the playing montage); in edit mode Preview evaluates the controller on the Animator (the
//   scene is put back when it stops), seeking the Particle Systems under it (S20) unless the
//   preference scene_view.preview_particles is off.
// - Every edit is one named project undo step (AnimatorAssetDocument::commit); a drag (a value,
//   nodes) is staged while it lasts and committed once when it ends. Renames follow their
//   references (a state: the transitions and the default; a parameter: the conditions, the
//   trees and the state parameters); deleting a parameter takes the conditions that read it;
//   typed clip and mask paths are checked before they are stored. Refusals show in the toolbar.
// - The current layer follows its name across layer list changes (a delete, a move, their undo
//   and redo); a selection whose item is gone or moved to another layer is dropped, so Delete
//   never reaches another layer's item. The Problems strip lists validateController's findings;
//   a click selects the item of the finding's JSON path.
// - Thread-safety: main thread.

#pragma once

#include "animation/AnimatorAssetDocument.h"
#include "animation/PreviewOwner.h"
#include "animation/PreviewParticles.h"
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

        // (the Pose Animation window is no longer needed: Configure Avatar... opens the Avatar window)
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
        // (both follow the document first: a layer list changed by an undo, MCP or a file)
        int getLayer();
        // "state:<name>", "machine:<name>", "transition:<list>:<owner>:<index>" (list: state,
        // any_state, entry, exit), "layer" or "" (nothing selected)
        std::string describeSelection();
        // the selected nodes by name, the Inspector's first ("Entry", "Any State", "Exit" for the
        // special nodes)
        std::vector<std::string> selectedNodes();
        const std::vector<std::string>& getMachinePath() const { return machinePath; }
        uint32_t getSceneId() const { return sceneId; }
        Entity getEntity() const { return entity; }
        // the last refusal or error (shown in the toolbar); empty after a successful edit
        const std::string& getStatus() const { return status; }
        // the graph's view (a test turns a state's position into screen pixels)
        const GraphCanvas& getCanvas() const { return canvas; }

        // edits the window makes (also the context menus' actions; public for the editor tests)
        bool addState(const std::string& name, const ImVec2& position, const anim::MotionDef& motion);
        bool addSubStateMachine(const std::string& name, const ImVec2& position);
        bool addTransition(const std::string& fromState, const std::string& toNode, bool fromAnyState, bool fromEntry);
        bool setDefaultState(const std::string& name);
        bool removeSelection();
        // Delete (the catalog's edit.delete) removes the window's selection: focused, editable,
        // something selected
        bool canDelete();
        bool addParameter(const std::string& name, AnimatorParameterType type);
        bool addLayer(const std::string& name);
        void selectState(const std::string& name);
        void selectTransition(const std::string& fromState, int index, bool fromAnyState = false, bool fromEntry = false);
        // the Layers tab's click
        void setLayer(int index);
        // the fields' and menus' edits
        bool renameState(const std::string& from, const std::string& to);
        bool renameParameter(const std::string& from, const std::string& to);
        bool removeParameter(const std::string& name);
        bool renameLayer(int index, const std::string& name);
        bool moveLayer(int from, int to);
        bool removeLayer(int index);
        // nodes of the current machine moved (by name; "Entry", "Any State", "Exit" for the
        // special nodes): consecutive moves merge into one undo step (a drag)
        bool moveNodes(const std::vector<std::pair<std::string, ImVec2>>& positions);
        // a 1D blend tree child's threshold: the children stay in threshold order, the child
        // keeps its motion; returns its new index (-1: refused)
        int setBlendThreshold(const std::string& state, int child, float threshold);
        // a typed clip (".anim" or "<model>#<clip>") or mask (".mask") path: inside the assets
        // folder, of the right kind, existing (a sub-clip: its model); asset-relative with its GUID
        bool resolveTypedAsset(const std::string& typed, bool clip, anim::AnimatorAssetRef& out);
        // validateController's findings on the open controller, and the selection of one's item
        std::vector<ClipWarning> problems() const;
        bool selectPath(const std::string& path);
        // the Problems strip's click: the item selected and shown in the Inspector
        bool inspect(const std::string& path);
        // Copy / Paste of the selected state (its motion, settings and transitions to itself),
        // pasted under a free name at `position` of the current machine
        bool copySelection();
        bool paste(const ImVec2& position);
        bool canPaste() const { return clipboard.has_value(); }
        // edit-mode preview on the window's Animator (none while it plays); the window steps it by
        // the frame time, tests by stepPreview
        bool startPreview();
        bool stepPreview(float dt);
        // play mode (or a preview): the running Animator's parameters, edited live (not saved)
        bool isLive() const { return liveInstance() != nullptr; }
        bool setLiveParameter(const std::string& name, float value);

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
            // the other selected nodes of a box or Shift / Ctrl selection, by name (a state's or
            // sub-machine's, unique in a machine, or a special node's). `name` is the one the
            // Inspector shows; with only special nodes selected the kind stays None
            std::vector<std::string> others;
            bool operator==(const Selection& o) const {
                return kind == o.kind && name == o.name && list == o.list && owner == o.owner && index == o.index && others == o.others;
            }
        };

        Project* project;
        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequest = false;

        std::shared_ptr<AnimatorAssetDocument> document;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        bool pinned = false;
        int layer = 0;
        std::string layerName;          // the current layer's name: followed across list changes
        std::vector<std::string> machinePath;
        Selection selection;
        Selection selectionAtPress;     // what Esc during a canvas gesture gives back
        bool claimAtPress = false;      // and whether the Inspector was the window's
        bool restoredByEsc = false;     // this frame: the selection's change claims nothing
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
        // the Problems strip: validateController on the document's revision (it reads the clips)
        uint64_t problemsRevision = ~0ull;
        std::string problemsPath;
        std::vector<ClipWarning> problemCache;
        // the edit-mode preview
        PreviewOwner::Token previewToken = 0;
        uint32_t previewScene = 0;
        Entity previewEntity = NULL_ENTITY;
        double previewClock = 0.0;
        double lastFrameTime = 0.0;
        bool previewPaused = false;
        PreviewParticles particles;
        float transitionScrub = 0.0f;
        // an edit in progress (a drag): shown at once, committed as one step when no item is active
        std::optional<anim::ControllerDef> pending;
        std::optional<anim::StateDef> clipboard;
        std::string pendingUndo;

        void followSelection();
        // the document's revision changed: the layer by its name, a stale selection dropped
        void syncToDocument();
        bool selectionExists(const anim::ControllerDef& def);
        bool isNodeSelected(const std::string& name) const;
        void setCurrentLayer(int index);
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
        void drawProblems();
        void drawLayers();
        void drawParameters();
        void drawGraph();
        void drawContextMenus();
        void updatePreview();
        void syncParticles(bool forward);

        // Inspector panels
        void drawStateInspector(anim::ControllerDef& def, anim::StateDef& state);
        void drawTransitionInspector(anim::ControllerDef& def, anim::TransitionDef& transition, const std::string& from);
        void drawLayerInspector(anim::ControllerDef& def);
        bool drawBlendTree(anim::ControllerDef& def, anim::BlendTreeDef& tree, int depth);
    };

}
