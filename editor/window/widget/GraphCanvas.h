// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md D14)
// - Purpose: a node-graph canvas for ImGui (the Animator window's state machines; reusable by
//   other graph editors): pan (middle mouse, or Alt + left drag), zoom at the mouse (wheel; text
//   stays crisp with the dynamic font sizes), a grid, boxes with labels, straight arrows between
//   box edges (a pair in both directions drawn side by side, a multiplicity mark when several
//   share a direction), click / Shift / Ctrl selection, box select, node dragging with grid
//   snap, a "link" mode (drag a new arrow from a node to another), and a context-menu request.
// - A node with pins, a thumbnail, footer lines or an error line, and a link with pin ids, draw
//   as a dataflow graph (T2 R53, N16): pins on the box's sides, cubic wires between them, a drag
//   from a pin to a compatible pin, a drag to empty space for the caller's node search. Everything
//   else takes the code path it always took (R54, GraphCanvasCompat pins its vertices).
// - Immediate mode: the caller passes its nodes and links every frame and reads one Result back;
//   the canvas keeps only its view (pan, zoom) and the gesture in progress.
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
        // T2 R53: a port of a node. An input sits on the box's left side, an output on its right.
        struct Pin{
            uint64_t id = 0;
            std::string label;
            ImU32 color = 0;            // by port type
            bool connected = false;     // drawn filled
            bool error = false;         // an issue names this port: a red ring
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
            // A dataflow node (T2 N16). draw() grows `size` so all of it fits.
            std::vector<Pin> inputs;
            std::vector<Pin> outputs;
            ImTextureID thumbnail{};    // drawn when set, and at zoom 0.5 or above (R56)
            ImVec2 thumbnailSize;       // canvas units; keeps its place in the box while the texture is not there yet
            std::vector<std::string> footer;    // dim lines under the thumbnail
            std::string errorText;      // a red outline and the message, wrapped, at the bottom
        };
        struct Link{
            uint64_t id = 0;
            uint64_t from = 0;
            uint64_t to = 0;            // == from: a loop drawn on the box
            int count = 1;              // links sharing this direction (drawn once, marked)
            bool selected = false;
            float progress = -1.0f;     // 0..1 along the arrow (a transition in progress)
            ImU32 color = IM_COL32(200, 200, 200, 255);
            // both set: a cubic wire between these pins; 0: box to box, an arrow (T2 N16)
            uint64_t fromPin = 0;
            uint64_t toPin = 0;
        };
        struct Result{
            uint64_t clickedNode = 0;
            uint64_t clickedLink = 0;
            uint64_t doubleClickedNode = 0;
            bool clickedBackground = false;
            bool selectionChanged = false;
            std::vector<uint64_t> selectedNodes;    // after this frame (selectionChanged)
            // the nodes a drag moved, with their new positions (when the drag ends)
            std::vector<std::pair<uint64_t, ImVec2>> moved;
            // link mode finished on a node
            uint64_t linkFrom = 0;
            uint64_t linkTo = 0;
            // right click: on a node, a link, or the background (canvas position)
            bool contextRequested = false;
            uint64_t contextNode = 0;
            uint64_t contextLink = 0;
            ImVec2 contextPosition;
            // a drag-and-drop payload released over the canvas (the caller accepts it)
            bool hovered = false;
            ImVec2 mouseCanvas;
            // a wire dropped on a compatible pin: wireFrom is always the output, wireTo the input
            uint64_t wireFrom = 0;
            uint64_t wireTo = 0;
            // a wire dropped on empty space, from this pin (either side): the caller opens its node search
            uint64_t wireDroppedFrom = 0;
            ImVec2 wireDropPosition;    // canvas units
            uint64_t hoveredPin = 0;
        };

        // Draws the graph in the remaining content region
        Result draw(const char* id, std::vector<Node>& nodes, std::vector<Link>& links);

        // a link drag starting at `from` (the "Make Transition" menu item); the next click on a
        // node ends it (Result::linkFrom/linkTo), Esc or a click on the background cancels it
        void beginLink(uint64_t from);
        bool isLinking() const { return linkSource != 0; }
        // centres the view on the nodes, zoom fitted (at most 1)
        void frame(const std::vector<Node>& nodes);
        ImVec2 toCanvas(const ImVec2& screen) const;
        ImVec2 toScreen(const ImVec2& canvas) const;

        float gridSize = 20.0f;
        bool snap = true;
        // May this output feed this input? Pins it refuses dim during a wire drag and refuse the
        // drop. Null: any output connects to any input. Two pins of one side never connect.
        std::function<bool(uint64_t fromPin, uint64_t toPin)> canConnect;
        bool isWiring() const { return wireSource != 0; }

    private:
        ImVec2 pan = ImVec2(0, 0);      // canvas units at the region's top-left
        float zoom = 1.0f;
        ImVec2 origin;                  // the region's screen top-left (this frame)
        ImVec2 regionSize;
        uint64_t linkSource = 0;
        uint64_t wireSource = 0;        // the pin a wire is being dragged from
        // gestures
        bool draggingNodes = false;
        ImVec2 dragStartMouse;
        std::vector<std::pair<uint64_t, ImVec2>> dragStart;
        bool boxSelecting = false;
        ImVec2 boxStart;
        bool framed = false;
    };

}
