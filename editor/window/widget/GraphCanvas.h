// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md D14)
// - Purpose: a node-graph canvas for ImGui (the Animator window's state machines; reusable by
//   other graph editors): pan (middle mouse, or Alt + left drag), zoom at the mouse (wheel; text
//   stays crisp with the dynamic font sizes), a grid, boxes with labels, straight arrows between
//   box edges (a pair in both directions drawn side by side, a multiplicity mark when several
//   share a direction), click / Shift / Ctrl selection, box select, node dragging with grid
//   snap, a "link" mode (drag a new arrow from a node to another), and a context-menu request.
//   Esc while the mouse is down cancels a node drag or a box select: positions and selection as
//   they were before the press, no move reported (docs/undo.md, Drags).
// - Immediate mode: the caller passes its nodes and links every frame and reads one Result back;
//   the canvas keeps only its view (pan, zoom) and the gesture in progress.
// - Thread-safety: main thread (ImGui).

#pragma once

#include "imgui.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class GraphCanvas{
    public:
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
        };
        struct Link{
            uint64_t id = 0;
            uint64_t from = 0;
            uint64_t to = 0;            // == from: a loop drawn on the box
            int count = 1;              // links sharing this direction (drawn once, marked)
            bool selected = false;
            float progress = -1.0f;     // 0..1 along the arrow (a transition in progress)
            ImU32 color = IM_COL32(200, 200, 200, 255);
        };
        struct Result{
            uint64_t clickedNode = 0;
            uint64_t clickedLink = 0;
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
            // right click: on a node, a link, or the background (canvas position)
            bool contextRequested = false;
            uint64_t contextNode = 0;
            uint64_t contextLink = 0;
            ImVec2 contextPosition;
            // a drag-and-drop payload released over the canvas (the caller accepts it)
            bool hovered = false;
            ImVec2 mouseCanvas;
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
        bool framed = false;
    };

}
