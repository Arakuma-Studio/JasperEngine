// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md D14, docs/specs/S90-procedural-material.md R26)
// - Purpose: a node-graph canvas for ImGui (the Animator's state machines, the Graph Editor's
//   dataflow graphs): pan (middle mouse, or Alt + left drag), zoom at the mouse (wheel; text
//   stays crisp with the dynamic font sizes), a grid, click / Shift / Ctrl selection, box
//   select, node dragging with grid snap, frame all or the selection, and a context-menu request.
//   - State machines: boxes with labels, straight arrows between box edges (a pair in both
//     directions drawn side by side, a multiplicity mark when several share a direction), and a
//     "link" mode (the next click on a node ends an arrow from the given one).
//   - Dataflow: a node with ports gets a header (label, cost), its inputs down the left edge and
//     its outputs down the right, and an optional thumbnail slot the caller draws into
//     (drawThumbnail). Wires are curves from an output to an input in the colour of their type.
//     A press on a port drags a wire: released on a port canConnect accepts it reports a
//     connection, released on empty canvas reports a drop (the caller opens its node search
//     there), anywhere else it does nothing. Zoomed out below 0.6 the ports, their labels and the
//     cost fade (gone at 0.35, where ports take no press); labels and thumbnails stay. Groups are
//     drawn behind the nodes, around their members; a press on a group's title selects them and
//     drags them. Under the thumbnail, a node may have dim footer lines (the Noise Graph's range and
//     time, T2 R56) and its error's message, wrapped (R58); a port an issue names has a red ring.
//   Esc while the mouse is down cancels a node drag, a box select or a wire drag: positions and
//   selection as they were before the press, nothing reported but `cancelled` (docs/undo.md,
//   Drags). Links outside the view are not drawn.
// - Immediate mode: the caller passes its nodes, links and groups every frame and reads one Result
//   back; the canvas keeps only its view (pan, zoom) and the gesture in progress. A dataflow node's
//   size is the canvas' own (nodeSize): draw() writes it into the node.
// - Thread-safety: main thread (ImGui).

#pragma once

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class GraphCanvas{
    public:
        struct Port{
            std::string id;
            std::string label;
            ImU32 color = IM_COL32(200, 200, 200, 255);   // its type's
            bool connected = false;                       // drawn filled
            bool error = false;                           // an issue names this port: a red ring (T2 R58)
        };
        struct Node{
            uint64_t id = 0;
            ImVec2 position;            // canvas units, the box's top-left
            ImVec2 size = ImVec2(180, 40);
            std::string label;
            std::string subtitle;       // a second, dimmer line (a motion name)
            ImU32 fill = IM_COL32(70, 70, 75, 255);
            ImU32 text = IM_COL32(230, 230, 230, 255);
            bool selected = false;
            bool rounded = true;
            float progress = -1.0f;     // 0..1: a bar at the bottom (live normalised time); < 0 none
            bool highlighted = false;   // an outline (the active state while playing)
            // dataflow: any port makes the node a dataflow node (header, ports, nodeSize)
            std::vector<Port> inputs;
            std::vector<Port> outputs;
            bool thumbnail = false;     // a square slot under the ports (drawThumbnail fills it)
            std::string cost;           // the header's right corner ("1.2 ms")
            ImU32 header = 0;           // the header bar (a category's colour); 0: the fill, lighter
            bool error = false;         // a red outline (the node has an error)
            std::vector<std::string> footer;    // dim lines under the thumbnail slot (T2 R56)
            std::string errorText;      // the error's message, wrapped at the bottom; draw() grows the box to hold it
        };
        struct Link{
            uint64_t id = 0;
            uint64_t from = 0;
            uint64_t to = 0;            // == from: a loop drawn on the box
            int count = 1;              // links sharing this direction (drawn once, marked)
            bool selected = false;
            float progress = -1.0f;     // 0..1 along the arrow (a transition in progress)
            ImU32 color = IM_COL32(200, 200, 200, 255);
            // both set: a wire from that output of `from` to that input of `to`
            std::string fromPort;
            std::string toPort;
        };
        struct Group{
            uint64_t id = 0;
            std::string title;
            std::vector<uint64_t> nodes;    // drawn around these; none: at position and size
            ImVec2 position;
            ImVec2 size;
            ImU32 color = IM_COL32(90, 110, 140, 60);
        };
        struct PortRef{
            uint64_t node = 0;
            std::string port;
            bool output = false;
            bool operator==(const PortRef& o) const { return node == o.node && port == o.port && output == o.output; }
        };
        struct Result{
            uint64_t clickedNode = 0;
            uint64_t clickedLink = 0;
            uint64_t clickedGroup = 0;
            uint64_t doubleClickedNode = 0;
            bool clickedBackground = false;
            bool selectionChanged = false;
            std::vector<uint64_t> selectedNodes;    // after this frame (selectionChanged)
            bool cancelled = false;                 // Esc ended a drag or a box: the selection is back
            // the nodes a drag moved, with their new positions (when the drag ends)
            std::vector<std::pair<uint64_t, ImVec2>> moved;
            // link mode finished on a node
            uint64_t linkFrom = 0;
            uint64_t linkTo = 0;
            // a wire released on a port canConnect accepts: an output and an input
            PortRef connectFrom;
            PortRef connectTo;
            // a wire released on empty canvas: its port, and where (canvas units)
            bool wireDropped = false;
            PortRef wireSource;
            ImVec2 wireDropCanvas;
            // right click: on a node, a link, or the background (canvas position)
            bool contextRequested = false;
            uint64_t contextNode = 0;
            uint64_t contextLink = 0;
            ImVec2 contextPosition;
            // a drag-and-drop payload released over the canvas (the caller accepts it)
            bool hovered = false;
            ImVec2 mouseCanvas;
        };

        // a dataflow node's layout, in canvas units (zoom 1)
        static constexpr float HEADER_HEIGHT = 26.0f;
        static constexpr float PORT_ROW = 20.0f;
        static constexpr float PADDING = 6.0f;
        static constexpr float THUMBNAIL_SLOT = 112.0f;
        static constexpr float MIN_WIDTH = 170.0f;
        static constexpr float FOOTER_LINE = 15.0f;
        // zoom below which the controls fade, and at which they are gone
        static constexpr float FADE_START = 0.6f;
        static constexpr float FADE_END = 0.35f;

        // Draws the graph in the remaining content region
        Result draw(const char* id, std::vector<Node>& nodes, std::vector<Link>& links);
        Result draw(const char* id, std::vector<Node>& nodes, std::vector<Link>& links, const std::vector<Group>& groups);

        // Whether a wire may join this output to this input (their types); unset: any output to
        // any input of another node
        std::function<bool(const PortRef& output, const PortRef& input)> canConnect;
        // Fills a node's thumbnail slot (screen pixels); drawn whatever the zoom
        std::function<void(ImDrawList* drawList, uint64_t node, ImVec2 min, ImVec2 max)> drawThumbnail;

        // a link drag starting at `from` (the "Make Transition" menu item); the next click on a
        // node ends it (Result::linkFrom/linkTo), Esc or a click on the background cancels it
        void beginLink(uint64_t from);
        bool isLinking() const { return linkSource != 0; }
        bool isWiring() const { return wiring; }
        // centres the view on the nodes, zoom fitted (at most 1)
        void frame(const std::vector<Node>& nodes);
        // on the selected ones (all when none is)
        void frameSelected(const std::vector<Node>& nodes);
        ImVec2 toCanvas(const ImVec2& screen) const;
        ImVec2 toScreen(const ImVec2& canvas) const;
        float getZoom() const { return zoom; }
        // the ports' and labels' opacity at this zoom (FADE_START..FADE_END)
        static float controlsAlpha(float zoom);

        static bool isDataflow(const Node& node) { return !node.inputs.empty() || !node.outputs.empty(); }
        // a dataflow node's size from its ports, slot and footer (its error's lines are draw()'s, which
        // knows the font); a state machine's box keeps its own
        static ImVec2 nodeSize(const Node& node);
        // a port's centre in canvas units (on the node's left edge for an input, right for an
        // output); the middle of that edge when it has no such port
        static ImVec2 portPosition(const Node& node, const std::string& port, bool output);
        // the thumbnail slot's rect in canvas units (min, max)
        static void thumbnailRect(const Node& node, ImVec2& min, ImVec2& max);

        float gridSize = 20.0f;
        bool snap = true;

    private:
        ImVec2 pan = ImVec2(0, 0);      // canvas units at the region's top-left
        float zoom = 1.0f;
        ImVec2 origin;                  // the region's screen top-left (this frame)
        ImVec2 regionSize;
        uint64_t linkSource = 0;
        // gestures
        bool draggingNodes = false;
        bool dragMoved = false;         // past ImGui's drag threshold: before it a press is a click
        uint64_t collapseTo = 0;        // a plain press on a selected node: the only one if no drag follows
        ImVec2 dragStartMouse;
        std::vector<std::pair<uint64_t, ImVec2>> dragStart;
        std::vector<uint64_t> selectionAtPress;     // what Esc gives back
        bool boxSelecting = false;
        ImVec2 boxStart;
        bool wiring = false;            // a wire dragged from wireStart
        PortRef wireStart;
        bool framed = false;

        bool accepts(const PortRef& a, const PortRef& b) const;
    };

}
