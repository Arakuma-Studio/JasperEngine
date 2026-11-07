// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The node canvas' gestures, driven with ImGui's input events on a frame loop of its own: Esc
// during a node drag, a box select or a wire drag (docs/undo.md, Drags), what a click on a member
// of a multi-selection does, and the dataflow parts (S90 R26): ports, wires dragged to a port or
// dropped on the canvas, thumbnails, the zoom fade, groups and frame selected.

#include <gtest/gtest.h>

#include "window/widget/GraphCanvas.h"

#include "imgui.h"

#include <vector>

using namespace jasperengine::editor;

namespace {

    struct Rig {
        ImGuiContext* previous = nullptr;
        ImGuiContext* context = nullptr;
        GraphCanvas canvas;
        std::vector<GraphCanvas::Node> nodes;
        std::vector<GraphCanvas::Link> links;
        std::vector<GraphCanvas::Group> groups;

        Rig() {
            previous = ImGui::GetCurrentContext();
            context = ImGui::CreateContext();
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1200, 800);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            nodes.resize(3);
            nodes[0].id = 1;
            nodes[0].position = ImVec2(100, 100);
            nodes[1].id = 2;
            nodes[1].position = ImVec2(400, 100);
            nodes[2].id = 3;
            nodes[2].position = ImVec2(100, 400);
            for (int i = 0; i < 3; i++) frame();
        }
        ~Rig() {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        }

        GraphCanvas::Result frame() {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::Begin("canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
            const GraphCanvas::Result result = canvas.draw("##canvas", nodes, links, groups);
            ImGui::End();
            ImGui::EndFrame();
            return result;
        }

        ImVec2 centre(size_t node) const {
            const ImVec2 a = canvas.toScreen(nodes[node].position);
            const ImVec2 b = canvas.toScreen(ImVec2(nodes[node].position.x + nodes[node].size.x, nodes[node].position.y + nodes[node].size.y));
            return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        }

        GraphCanvas::Result moveTo(const ImVec2& at) {
            ImGui::GetIO().AddMousePosEvent(at.x, at.y);
            return frame();
        }
        GraphCanvas::Result press() {
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            return frame();
        }
        GraphCanvas::Result release() {
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            return frame();
        }
        // a press at `a`, a move to `b` in steps, and the release there
        GraphCanvas::Result dragTo(const ImVec2& a, const ImVec2& b) {
            moveTo(a);
            frame();
            press();
            for (int i = 1; i <= 4; i++) moveTo(ImVec2(a.x + (b.x - a.x) * i / 4.0f, a.y + (b.y - a.y) * i / 4.0f));
            frame();
            return release();
        }
        ImVec2 port(size_t node, const char* id, bool output) const {
            return canvas.toScreen(GraphCanvas::portPosition(nodes[node], id, output));
        }
        // nodes 0 and 1 as dataflow nodes: 0 has an output "out", 1 the inputs "a" and "b"
        void dataflow() {
            nodes[0].outputs = {{"out", "Out", IM_COL32(255, 136, 0, 255)}};
            nodes[1].inputs = {{"a", "A"}, {"b", "B"}};
            nodes[1].outputs = {{"out", "Out"}};
            for (int i = 0; i < 2; i++) frame();
        }
        GraphCanvas::Result esc() {
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
            const GraphCanvas::Result result = frame();
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
            frame();
            return result;
        }
    };

}

TEST(GraphCanvas, EscCancelsANodeDrag) {
    Rig rig;
    const ImVec2 start = rig.nodes[0].position;
    const ImVec2 at = rig.centre(0);
    rig.moveTo(at);
    rig.frame();
    rig.press();
    rig.moveTo(ImVec2(at.x + 120, at.y + 80));
    rig.frame();
    ASSERT_NE(rig.nodes[0].position.x, start.x) << "the drag moves the node";

    const GraphCanvas::Result cancelled = rig.esc();
    EXPECT_TRUE(cancelled.moved.empty());
    EXPECT_EQ(rig.nodes[0].position.x, start.x) << "Esc restores the drag's start";
    EXPECT_EQ(rig.nodes[0].position.y, start.y);

    // the mouse is still down: moving it does nothing more, and the release reports no move
    rig.moveTo(ImVec2(at.x + 200, at.y + 200));
    EXPECT_EQ(rig.nodes[0].position.x, start.x);
    const GraphCanvas::Result released = rig.release();
    EXPECT_TRUE(released.moved.empty()) << "no move, so the caller adds no undo step";
    EXPECT_EQ(rig.nodes[0].position.x, start.x);
    EXPECT_FALSE(rig.nodes[0].selected) << "the selection as it was before the press";
}

TEST(GraphCanvas, EscEndsABoxSelect) {
    Rig rig;
    rig.nodes[2].selected = true;   // the press on the background clears it: Esc gives it back
    const ImVec2 empty = rig.canvas.toScreen(ImVec2(60, 60));
    rig.moveTo(empty);
    rig.frame();
    rig.press();
    rig.moveTo(rig.centre(1));
    rig.frame();

    rig.esc();
    const GraphCanvas::Result released = rig.release();
    EXPECT_FALSE(released.selectionChanged) << "the box ended at Esc: its release selects nothing";
    EXPECT_FALSE(rig.nodes[0].selected);
    EXPECT_FALSE(rig.nodes[1].selected);
    EXPECT_TRUE(rig.nodes[2].selected) << "the selection as it was before the press";
}

// BF23 F18: a plain click on a member of a multi-selection leaves that one; its drag moves them all
TEST(GraphCanvas, APlainClickOnASelectedNodeCollapsesTheSelection) {
    Rig rig;
    rig.nodes[0].selected = true;
    rig.nodes[1].selected = true;
    const ImVec2 at = rig.centre(1);
    rig.moveTo(at);
    rig.frame();
    const GraphCanvas::Result pressed = rig.press();
    EXPECT_EQ(pressed.selectedNodes.size(), 2u) << "still both at the press: a drag would move them";
    const GraphCanvas::Result released = rig.release();
    EXPECT_TRUE(released.selectionChanged);
    EXPECT_FALSE(rig.nodes[0].selected);
    EXPECT_TRUE(rig.nodes[1].selected);
}

// a press that does not move is a click: an off-grid node keeps its place and reports no move
TEST(GraphCanvas, AClickOnAnOffGridNodeDoesNotMoveIt) {
    Rig rig;
    rig.nodes[0].position = ImVec2(107, 93);
    rig.nodes[1].position = ImVec2(413, 93);
    rig.nodes[0].selected = true;
    rig.nodes[1].selected = true;
    const ImVec2 at = rig.centre(1);
    rig.moveTo(at);
    rig.frame();
    rig.press();
    rig.frame();
    const GraphCanvas::Result released = rig.release();
    EXPECT_TRUE(released.moved.empty()) << "no move, no undo step";
    EXPECT_EQ(rig.nodes[0].position.x, 107.0f);
    EXPECT_EQ(rig.nodes[1].position.y, 93.0f);
    EXPECT_FALSE(rig.nodes[0].selected) << "the click narrows the selection to the node clicked";
    EXPECT_TRUE(rig.nodes[1].selected);
}

// the gesture still ends normally without Esc
TEST(GraphCanvas, ADragWithoutEscReportsItsMove) {
    Rig rig;
    const ImVec2 at = rig.centre(0);
    rig.moveTo(at);
    rig.frame();
    rig.press();
    rig.moveTo(ImVec2(at.x + 120, at.y + 80));
    rig.frame();
    const GraphCanvas::Result released = rig.release();
    ASSERT_EQ(released.moved.size(), 1u);
    EXPECT_EQ(released.moved[0].first, 1u);
}

TEST(GraphCanvas, ADataflowNodeIsSizedByItsPortsAndThumbnail) {
    GraphCanvas::Node node;
    node.position = ImVec2(10, 20);
    node.inputs = {{"a"}, {"b"}, {"c"}};
    node.outputs = {{"out"}};
    const ImVec2 size = GraphCanvas::nodeSize(node);
    EXPECT_EQ(size.x, GraphCanvas::MIN_WIDTH);
    EXPECT_EQ(size.y, GraphCanvas::HEADER_HEIGHT + 3 * GraphCanvas::PORT_ROW + GraphCanvas::PADDING);
    node.thumbnail = true;
    EXPECT_EQ(GraphCanvas::nodeSize(node).y, size.y + GraphCanvas::THUMBNAIL_SLOT + GraphCanvas::PADDING);
    const ImVec2 b = GraphCanvas::portPosition(node, "b", false);
    EXPECT_EQ(b.x, 10.0f) << "an input on the left edge";
    EXPECT_EQ(b.y, 20.0f + GraphCanvas::HEADER_HEIGHT + 1.5f * GraphCanvas::PORT_ROW);
    EXPECT_EQ(GraphCanvas::portPosition(node, "out", true).x, 10.0f + GraphCanvas::MIN_WIDTH) << "an output on the right";
    GraphCanvas::Node box;
    box.size = ImVec2(120, 34);
    EXPECT_EQ(GraphCanvas::nodeSize(box).x, 120.0f) << "a state machine's box keeps its size";
}

TEST(GraphCanvas, AWireDraggedToAPortConnects) {
    Rig rig;
    rig.dataflow();
    const GraphCanvas::Result released = rig.dragTo(rig.port(0, "out", true), rig.port(1, "b", false));
    EXPECT_EQ(released.connectFrom, (GraphCanvas::PortRef{1, "out", true}));
    EXPECT_EQ(released.connectTo, (GraphCanvas::PortRef{2, "b", false}));
    EXPECT_TRUE(released.moved.empty()) << "a press on a port drags a wire, not the node";
    EXPECT_FALSE(released.wireDropped);

    // from an input back to an output: the same connection
    const GraphCanvas::Result reversed = rig.dragTo(rig.port(1, "a", false), rig.port(0, "out", true));
    EXPECT_EQ(reversed.connectFrom, (GraphCanvas::PortRef{1, "out", true}));
    EXPECT_EQ(reversed.connectTo, (GraphCanvas::PortRef{2, "a", false}));
}

TEST(GraphCanvas, AWireTheCallerRefusesConnectsNothing) {
    Rig rig;
    rig.dataflow();
    rig.canvas.canConnect = [](const GraphCanvas::PortRef&, const GraphCanvas::PortRef& input) { return input.port == "a"; };
    const GraphCanvas::Result refused = rig.dragTo(rig.port(0, "out", true), rig.port(1, "b", false));
    EXPECT_EQ(refused.connectTo.node, 0u);
    EXPECT_FALSE(refused.wireDropped) << "released on a port, not on empty canvas";
    const GraphCanvas::Result accepted = rig.dragTo(rig.port(0, "out", true), rig.port(1, "a", false));
    EXPECT_EQ(accepted.connectTo.port, "a");
    // an output to an output, and a node to itself, never
    const GraphCanvas::Result same = rig.dragTo(rig.port(0, "out", true), rig.port(1, "out", true));
    EXPECT_EQ(same.connectTo.node, 0u);
}

TEST(GraphCanvas, AWireDroppedOnEmptyCanvasReportsWhere) {
    Rig rig;
    rig.dataflow();
    const ImVec2 empty = rig.canvas.toScreen(ImVec2(700, 500));
    const GraphCanvas::Result dropped = rig.dragTo(rig.port(0, "out", true), empty);
    ASSERT_TRUE(dropped.wireDropped);
    EXPECT_EQ(dropped.wireSource, (GraphCanvas::PortRef{1, "out", true}));
    EXPECT_NEAR(dropped.wireDropCanvas.x, 700.0f, 0.5f);
    EXPECT_NEAR(dropped.wireDropCanvas.y, 500.0f, 0.5f);
    // released on a node's body: neither
    const GraphCanvas::Result onNode = rig.dragTo(rig.port(0, "out", true), rig.centre(2));
    EXPECT_FALSE(onNode.wireDropped);
    EXPECT_EQ(onNode.connectTo.node, 0u);
}

TEST(GraphCanvas, EscCancelsAWireDrag) {
    Rig rig;
    rig.dataflow();
    const ImVec2 from = rig.port(0, "out", true);
    rig.moveTo(from);
    rig.frame();
    rig.press();
    rig.moveTo(rig.port(1, "a", false));
    rig.frame();
    ASSERT_TRUE(rig.canvas.isWiring());
    const GraphCanvas::Result cancelled = rig.esc();
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_FALSE(rig.canvas.isWiring());
    const GraphCanvas::Result released = rig.release();
    EXPECT_EQ(released.connectTo.node, 0u) << "released over a port after Esc: no connection";
    EXPECT_FALSE(released.wireDropped);
}

TEST(GraphCanvas, AClickOnAWireSelectsItsLink) {
    Rig rig;
    rig.dataflow();
    GraphCanvas::Link wire;
    wire.id = 77;
    wire.from = 1;
    wire.to = 2;
    wire.fromPort = "out";
    wire.toPort = "a";
    rig.links.push_back(wire);
    rig.frame();
    // a wire's curve passes through the midpoint of its ends
    const ImVec2 a = rig.port(0, "out", true), b = rig.port(1, "a", false);
    rig.moveTo(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f));
    rig.frame();
    const GraphCanvas::Result pressed = rig.press();
    rig.release();
    EXPECT_EQ(pressed.clickedLink, 77u);
    EXPECT_TRUE(rig.links[0].selected);
}

TEST(GraphCanvas, ThumbnailsAreDrawnAtEveryZoomAndPortsFadeOut) {
    Rig rig;
    rig.dataflow();
    rig.nodes[0].thumbnail = true;
    rig.nodes[2].position = ImVec2(100000, 100000);   // far outside the view
    rig.nodes[2].thumbnail = true;
    rig.nodes[2].inputs = {{"x"}};
    std::vector<uint64_t> drawn;
    rig.canvas.drawThumbnail = [&](ImDrawList*, uint64_t node, ImVec2 min, ImVec2 max) {
        EXPECT_GT(max.x, min.x);
        drawn.push_back(node);
    };
    rig.frame();
    EXPECT_EQ(drawn, std::vector<uint64_t>{1}) << "a node outside the view is not drawn";
    EXPECT_EQ(GraphCanvas::controlsAlpha(1.0f), 1.0f);
    EXPECT_GT(GraphCanvas::controlsAlpha(0.5f), 0.0f);
    EXPECT_LT(GraphCanvas::controlsAlpha(0.5f), 1.0f);
    EXPECT_EQ(GraphCanvas::controlsAlpha(0.3f), 0.0f);

    // zoomed out past the fade: the thumbnail still draws, and a press on a port drags the node
    const ImVec2 at = rig.centre(0);
    rig.moveTo(at);
    for (int i = 0; i < 12 && rig.canvas.getZoom() > GraphCanvas::FADE_END; i++) {
        ImGui::GetIO().AddMouseWheelEvent(0.0f, -1.0f);
        rig.frame();
    }
    ASSERT_LE(rig.canvas.getZoom(), GraphCanvas::FADE_END);
    drawn.clear();
    rig.frame();
    EXPECT_EQ(drawn, std::vector<uint64_t>{1});
    const ImVec2 start = rig.nodes[0].position;
    const GraphCanvas::Result released = rig.dragTo(rig.port(0, "out", true), ImVec2(rig.port(0, "out", true).x + 60, rig.port(0, "out", true).y + 60));
    EXPECT_FALSE(released.wireDropped);
    ASSERT_EQ(released.moved.size(), 1u) << "no port to press: the node moves";
    EXPECT_NE(rig.nodes[0].position.x, start.x);
}

TEST(GraphCanvas, APressOnAGroupTitleDragsItsNodes) {
    Rig rig;
    GraphCanvas::Group group;
    group.id = 9;
    group.title = "Inputs";
    group.nodes = {1, 3};
    rig.groups.push_back(group);
    rig.frame();
    // the title bar sits above the topmost member
    const ImVec2 title = rig.canvas.toScreen(ImVec2(rig.nodes[0].position.x + 10, rig.nodes[0].position.y - 16 - GraphCanvas::HEADER_HEIGHT + 5));
    const GraphCanvas::Result released = rig.dragTo(title, ImVec2(title.x + 100, title.y + 40));
    EXPECT_TRUE(rig.nodes[0].selected);
    EXPECT_TRUE(rig.nodes[2].selected);
    EXPECT_FALSE(rig.nodes[1].selected);
    EXPECT_EQ(released.moved.size(), 2u) << "both members move as one drag";
}

TEST(GraphCanvas, FrameSelectedCentresTheSelection) {
    Rig rig;
    rig.nodes[1].selected = true;
    rig.canvas.frameSelected(rig.nodes);
    rig.frame();
    const ImVec2 centre = rig.centre(1);
    EXPECT_NEAR(centre.x, ImGui::GetIO().DisplaySize.x * 0.5f, 20.0f);
    EXPECT_NEAR(centre.y, ImGui::GetIO().DisplaySize.y * 0.5f, 20.0f);
}
