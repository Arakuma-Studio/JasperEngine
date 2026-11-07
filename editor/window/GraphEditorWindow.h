// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S90-procedural-material.md R26, R27's window parts, R30, D7)
// - Purpose: Window > Assets > Graph Editor: the editor of a graph file of any registered kind
//   (engine/core/graph), and the base of a kind's own window (Procedural Material). A toolbar
//   (the file, Open..., Arrange, Frame), the Problems strip (graph::validate's issues and the
//   kind's own: a click selects the node, Fix applies a one-edit fix), and the canvas: nodes with
//   typed ports, wires coloured by type, a thumbnail and a cost on nodes when the kind's window
//   gives them. Dragging a wire from a port to a compatible port connects them; dropping it on
//   empty canvas opens the node search filtered to the nodes that can take it, and the node
//   picked is connected with that port under the cursor. Right click (or a double click on empty
//   canvas) opens the search at the mouse. The search ranks by SearchScorer over the type's name,
//   category and aliases, and lists each enum value as an entry of its own ("Blend: Multiply").
// - The selected node's parameters show in the Inspector (graphEditorHooks().drawSelection) as
//   rows from its type's ParamDesc: colours, gradients (up to 32 keys) and curves through
//   colorField, gradientField and curveField, never a modal dialog. Right click on a row copies or
//   pastes its value as JSON text; the canvas' Copy, Paste and Duplicate (the catalog's edit verbs)
//   carry nodes and the wires between them as JSON text on the clipboard.
// - Every edit is one named project undo step (GraphDocument::apply); a field's drag folds into
//   one step through the field's session. Esc during any canvas drag puts it back and adds no
//   step. Nodes the file gives no position are drawn where the automatic layout puts them (R30);
//   Arrange writes a layout of every node as one step.
// - Thread-safety: main thread.
//
// What a kind's window adds (override the protected virtuals; each has a no-op default):
//   nodeHasThumbnail / drawNodeThumbnail  the node's image in its slot, screen pixels; called for
//                                         visible nodes only, at every zoom, so a renderer can
//                                         budget one new image per frame (S90 R24)
//   nodeCost                              the node's cost label ("1.2 ms"); "" for none
//   hasPreviewPane / drawPreviewPane      a column right of the canvas (the preview, R27)
//   drawToolbarExtras                     controls after the toolbar's own (seed, Bake, the tier)
//   collectProblems                       the kind's checks, as Problems strip rows with a fix
//   documentChanged / selectionChanged    a new revision of the graph (hand getDocument()->graph() to a
//                                         ProceduralSession::setGraph); the selected nodes changed
//   frameStarted                          once a frame while the window is open, before it draws (a
//                                         session's update(): its evaluator's slice and the debounce)
//   documentClosed                        the graph goes (another one opens, the project switches; not
//                                         from the base destructor, so a subclass's own does the same):
//                                         cancel what was started for it and drop its images;
//                                         getDocument() is still the old one
// App calls prepareForProjectSwitch on every graph window, a kind's too, so none keeps a document of the
// project that closes.

#pragma once

#include "ecs/Entity.h"
#include "graph/GraphDocument.h"
#include "window/widget/GraphCanvas.h"

#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    class GraphEditorWindow{
    public:
        static constexpr const char* WINDOW_NAME = "Graph Editor";

        // A row of the Problems strip: an issue of graph::validate or of the kind's window
        struct Problem{
            graph::Severity severity = graph::Severity::Error;
            std::string code;
            std::string message;
            std::string nodeId;                 // selected by a click; "" for the graph
            std::vector<graph::GraphOp> fix;    // a single edit Fix applies (empty: no Fix)
            std::string fixLabel;               // its undo name
        };
        // An entry of the node search: a node type, or one value of one of its enum parameters
        struct SearchEntry{
            std::string type;
            std::string label;                  // "Blend" or "Blend: Multiply"
            std::string category;
            std::string param;                  // the enum parameter set, with value
            graph::Json value;
            std::string port;                   // the new node's port the dropped wire joins
            bool output = false;                // that port's direction
            int score = 0;
        };

        // `windowName`: the ImGui window's (a kind's window passes its own); `kindId`: the kind it
        // opens ("" any kind no other window claims)
        explicit GraphEditorWindow(Project* project, std::string windowName = WINDOW_NAME, std::string kindId = std::string());
        virtual ~GraphEditorWindow();
        GraphEditorWindow(const GraphEditorWindow&) = delete;
        GraphEditorWindow& operator=(const GraphEditorWindow&) = delete;

        void show();
        void setOpen(bool open) { windowOpen = open; }
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return windowFocused; }
        void requestFocus() { focusRequest = true; }
        const std::string& windowName() const { return name; }
        const std::string& kindId() const { return kindFilter; }
        // a project closes: the document and the selection go (documentClosed, selectionChanged)
        void prepareForProjectSwitch();

        // the window on a graph file; false + status when it is not one of its kind or does not load
        bool openGraph(const std::string& path);
        const std::shared_ptr<GraphDocument>& getDocument() const { return document; }
        const std::string& getStatus() const { return status; }
        const GraphCanvas& getCanvas() const { return canvas; }
        // where a node is on screen, after the last frame (tests turn it into clicks)
        bool nodeRect(const std::string& nodeId, ImVec2& min, ImVec2& max) const;
        bool portScreen(const std::string& nodeId, const std::string& port, bool output, ImVec2& out) const;

        // selection
        const std::vector<std::string>& selectedNodes() const { return selection; }
        const std::string& selectedEdge() const { return edgeSelection; }
        void selectNodes(const std::vector<std::string>& ids);

        // edits (the canvas, the menus and the Inspector call these; public for the editor tests)
        // a node of `type` at `position` (its top-left, canvas units); its id, "" when refused
        std::string addNode(const std::string& type, const ImVec2& position, const graph::Json& params = graph::Json::object());
        // an output to an input; an input that takes one wire loses the one it had
        bool connect(const std::string& fromNode, const std::string& fromPort, const std::string& toNode, const std::string& toPort);
        bool canConnect(const std::string& fromNode, const std::string& fromPort, const std::string& toNode, const std::string& toPort) const;
        bool setParam(const std::string& nodeId, const std::string& param, const graph::Json& value);
        bool moveNodes(const std::vector<std::pair<std::string, ImVec2>>& positions);
        // the catalog's edit.delete: the selected nodes (with their wires) or the selected wire
        bool canDelete() const;
        bool removeSelection();
        // Arrange: every node laid out (graph::layoutPositions), one step
        bool arrange();
        void frameSelected();

        // copy and paste as JSON text ({"graph_clipboard": kind, nodes, edges}); paste places the
        // nodes' top-left at `at` (canvas units) with new ids and the wires between them
        std::string copyText() const;
        bool copySelection();
        bool canPaste() const;
        bool paste();
        bool pasteText(const std::string& text, const ImVec2& at);
        bool duplicateSelection();
        // a parameter's value as JSON text, and back
        std::string copyValueText(const std::string& nodeId, const std::string& param) const;
        bool pasteValueText(const std::string& nodeId, const std::string& param, const std::string& text);

        // node search: the entries for `query`, best first; with `source`, only those with a
        // port that can join it (and `port` set to the first such)
        std::vector<SearchEntry> searchNodes(const std::string& query, const GraphCanvas::PortRef* source = nullptr) const;
        // opens the search at `at` (canvas units); a dropped wire's port joins the node picked
        void openSearch(const ImVec2& at, const std::optional<GraphCanvas::PortRef>& source);
        bool isSearchOpen() const { return searchOpen; }
        const std::string& searchQuery() const { return query; }
        // what a click on an entry does: the node at the search's place, joined to the wire's
        // port with that port under the cursor; the node's id, "" when refused
        std::string pickSearchEntry(const SearchEntry& entry);
        void closeSearch();

        // the Problems strip
        std::vector<Problem> problems() const;
        // a click on a row: its node selected and shown
        bool inspectProblem(const Problem& problem);
        bool fixProblem(const Problem& problem);

        // the Inspector's drawing of the selected node; true when it drew
        bool drawSelectionInspector();
        // the Inspector drew the window's selection this frame or the last (its undo is the project's)
        bool inspectorShowsSelection() const;

        // the canvas node of a graph node (ports, colours, thumbnail and cost from the kind's window)
        GraphCanvas::Node canvasNode(const graph::Node& node) const;
        GraphCanvas::Node canvasNodeOfType(const std::string& type, const graph::Json& params) const;

    protected:
        // ---- what a kind's window adds (see the contract above)
        virtual bool nodeHasThumbnail(const graph::Node&) const { return false; }
        virtual void drawNodeThumbnail(const graph::Node&, ImDrawList*, ImVec2, ImVec2) {}
        virtual std::string nodeCost(const graph::Node&) const { return std::string(); }
        virtual bool hasPreviewPane() const { return false; }
        virtual void drawPreviewPane() {}
        virtual void drawToolbarExtras() {}
        virtual void collectProblems(std::vector<Problem>&) const {}
        virtual void documentChanged() {}
        virtual void selectionChanged() {}
        virtual void frameStarted() {}
        virtual void documentClosed() {}

        Project* project;

    private:
        std::string name;
        std::string kindFilter;
        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequest = false;
        std::shared_ptr<GraphDocument> document;
        int listener = 0;
        uint64_t seenRevision = 0;
        std::string status;
        GraphCanvas canvas;
        bool frameRequest = false;
        // canvas ids: node index + 1 in the graph, wires from EDGE_BASE
        std::vector<std::string> nodeIds;
        std::unordered_map<std::string, uint64_t> canvasIds;
        std::vector<GraphCanvas::Node> lastNodes;
        std::vector<std::string> selection;
        std::vector<std::string> selectionAtPress;
        std::string edgeSelection;
        // the Inspector shows the selection from a click in the window until the scene selection
        // or the inspected asset changes (the Animator's rule)
        bool inspectorClaim = false;
        uint32_t claimScene = 0;
        std::string claimAsset;
        std::vector<Entity> claimEntities;
        int inspectorFrame = -10;
        ImVec2 lastMouseCanvas;
        bool canvasHovered = false;
        // the node search
        bool searchOpen = false;
        bool searchFocus = false;
        std::string query;
        ImVec2 searchAt;
        std::optional<GraphCanvas::PortRef> searchSource;
        std::string searchSourceNode;
        int searchCursor = 0;
        ImVec2 contextAt;

        // what a node shows that the graph's edges and issues say
        struct NodeFacts{
            std::set<std::string> connected;    // "i:<port>", "o:<port>"
            bool error = false;
        };
        GraphCanvas::Node canvasNode(const graph::Node& node, const NodeFacts& facts) const;
        void setDocument(std::shared_ptr<GraphDocument> next);
        void syncToDocument();
        bool apply(const std::vector<graph::GraphOp>& ops, const std::string& undoName, std::vector<graph::GraphOp>* inverses = nullptr);
        void setSelection(std::vector<std::string> ids, bool claim);
        void claimInspector();
        bool claimHolds();
        std::string nodeIdOf(uint64_t canvasId) const;
        uint64_t canvasIdOf(const std::string& nodeId) const;
        const graph::Port* portOf(const graph::Node& node, const std::string& port, bool output, std::vector<graph::Port>& storage) const;

        void drawToolbar();
        void drawProblems();
        void drawCanvas();
        void drawSearch();
        void drawNodeInspector(const graph::Node& node);

        static std::vector<GraphEditorWindow*>& instances();
        static bool drawClaimedInspector();
    };

}
