// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The node canvas' gestures, driven with ImGui's input events on a frame loop of its own: Esc
// during a node drag, a box select or a wire drag (docs/undo.md, Drags), what a click on a member
// of a multi-selection does, and the dataflow parts (S90 R26): ports, wires dragged to a port or
// dropped on the canvas, thumbnails, the zoom fade, groups and frame selected. GraphCanvasCompat pins
// what a graph without ports draws: every vertex of a fixed Animator-like graph through each gesture,
// against a table (fixtures/widget/graph_canvas_nopins.txt; JASPER_TEST_UPDATE_GOLDEN=1 writes it again,
// which is only right when a change of the Animator's drawing is meant). GraphCanvasPins drives what the
// Noise Graph adds (T2 R53, R56, R58): footer lines, an error's message, a port's error ring, refused
// ports dimmed during a drag, and a wire picked along its curve.

#include <gtest/gtest.h>

#include "window/widget/GraphCanvas.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
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
    node.size = ImVec2(0, 0);       // no width asked for: the canvas' least
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
    node.size = ImVec2(240, 0);
    EXPECT_EQ(GraphCanvas::nodeSize(node).x, 240.0f) << "a wider node keeps its width";
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

// S90 review M3: zoomed out, a point between two rows goes to the nearer port; a node on top hides
// the ports under it
TEST(GraphCanvas, APortPressGoesToTheNearestPortOfTheTopNode) {
    Rig rig;
    rig.dataflow();
    const ImVec2 at = rig.centre(1);
    rig.moveTo(at);
    for (int i = 0; i < 5; i++) {
        ImGui::GetIO().AddMouseWheelEvent(0.0f, -1.0f);
        rig.frame();
    }
    ASSERT_LT(rig.canvas.getZoom(), 0.7f);
    ASSERT_GT(GraphCanvas::controlsAlpha(rig.canvas.getZoom()), 0.0f);
    const ImVec2 a = rig.port(1, "a", false), b = rig.port(1, "b", false);
    // a third of the way from b to a: nearer b
    const ImVec2 nearB(b.x, b.y + (a.y - b.y) / 3.0f);
    const GraphCanvas::Result joined = rig.dragTo(rig.port(0, "out", true), nearB);
    EXPECT_EQ(joined.connectTo.port, "b");

    // a box laid over Blend: its ports take no press
    rig.nodes[2].position = rig.nodes[1].position;
    rig.nodes[2].size = ImVec2(400, 300);
    rig.frame();
    rig.moveTo(rig.port(1, "a", false));
    rig.frame();
    rig.press();
    EXPECT_FALSE(rig.canvas.isWiring()) << "the box on top takes the press";
    rig.release();
}

// S90 review M9: a wire to a port a node does not show ends at the middle of that side
TEST(GraphCanvas, APortANodeDoesNotShowIsTheMiddleOfItsSide) {
    GraphCanvas::Node node;
    node.position = ImVec2(10, 20);
    node.size = ImVec2(120, 40);
    const ImVec2 in = GraphCanvas::portPosition(node, "gone", false);
    const ImVec2 out = GraphCanvas::portPosition(node, "gone", true);
    EXPECT_EQ(in.x, 10.0f);
    EXPECT_EQ(out.x, 130.0f);
    EXPECT_EQ(in.y, 40.0f);
}

namespace {

    // An ImGui context of its own, one borderless window at the origin, and the mouse under the test's control
    class CanvasHarness {
    public:
        CanvasHarness() {
            previous = ImGui::GetCurrentContext();
            context = ImGui::CreateContext();
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(800, 500);
            io.DeltaTime = 1.0f / 60.0f;
            io.ConfigInputTrickleEventQueue = false;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }
        ~CanvasHarness() {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        }

        void mouse(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
        void button(int which, bool down) { ImGui::GetIO().AddMouseButtonEvent(which, down); }
        void wheel(float notches) { ImGui::GetIO().AddMouseWheelEvent(0.0f, notches); }

        // One frame; `after` runs inside the window, once the canvas drew
        GraphCanvas::Result frame(GraphCanvas& canvas, std::vector<GraphCanvas::Node>& nodes, std::vector<GraphCanvas::Link>& links,
                                  const std::function<void(const GraphCanvas::Result&, ImDrawList*, int)>& after = nullptr) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleVar();
            ImDrawList* list = ImGui::GetWindowDrawList();
            const int firstVertex = list->VtxBuffer.Size;
            GraphCanvas::Result result = canvas.draw("##graph", nodes, links);
            if (after) after(result, list, firstVertex);
            ImGui::End();
            ImGui::EndFrame();
            return result;
        }

    private:
        ImGuiContext* previous = nullptr;
        ImGuiContext* context = nullptr;
    };

    // Everything the Animator asks of the canvas: straight and paired arrows, a mark for several in
    // one direction, a loop, a progress on an arrow and on a box, a highlight, a selection, a subtitle
    void animatorLikeGraph(std::vector<GraphCanvas::Node>& nodes, std::vector<GraphCanvas::Link>& links) {
        nodes.clear();
        links.clear();
        auto node = [&](uint64_t id, float x, float y, const char* label, ImU32 fill) -> GraphCanvas::Node& {
            GraphCanvas::Node n;
            n.id = id;
            n.position = ImVec2(x, y);
            n.label = label;
            n.fill = fill;
            nodes.push_back(n);
            return nodes.back();
        };
        node(1, 0, 0, "Entry", IM_COL32(60, 130, 70, 255));
        node(2, 0, 120, "Any State", IM_COL32(60, 130, 140, 255));
        node(3, 480, 0, "Exit", IM_COL32(150, 60, 60, 255));
        node(4, 220, 0, "Idle", IM_COL32(190, 110, 40, 255)).subtitle = "idle_clip";
        GraphCanvas::Node& run = node(5, 220, 120, "Run", IM_COL32(70, 70, 75, 255));
        run.subtitle = "run_clip";
        run.highlighted = true;
        run.progress = 0.4f;
        GraphCanvas::Node& jump = node(6, 460, 120, "Jump", IM_COL32(70, 70, 75, 255));
        jump.size = ImVec2(140, 60);
        jump.rounded = false;
        auto link = [&](uint64_t id, uint64_t from, uint64_t to) -> GraphCanvas::Link& {
            GraphCanvas::Link l;
            l.id = id;
            l.from = from;
            l.to = to;
            links.push_back(l);
            return links.back();
        };
        link(11, 1, 4);
        link(12, 4, 5);
        link(13, 5, 4);
        link(14, 5, 6).count = 3;
        link(15, 5, 5);
        link(16, 2, 6).progress = 0.5f;
        link(17, 6, 3).color = IM_COL32(220, 200, 90, 255);
    }

    std::string describeResult(const GraphCanvas::Result& r) {
        std::ostringstream out;
        out << "clickedNode=" << r.clickedNode << " clickedLink=" << r.clickedLink << " doubleClickedNode=" << r.doubleClickedNode
            << " clickedBackground=" << r.clickedBackground << " selectionChanged=" << r.selectionChanged << " linkFrom=" << r.linkFrom
            << " linkTo=" << r.linkTo << " contextRequested=" << r.contextRequested << " contextNode=" << r.contextNode
            << " contextLink=" << r.contextLink << " hovered=" << r.hovered << " selected=[";
        for (uint64_t id : r.selectedNodes) out << id << ",";
        out << "] moved=[";
        char text[64];
        for (const auto& moved : r.moved) {
            std::snprintf(text, sizeof(text), "%llu:%.2f:%.2f,", static_cast<unsigned long long>(moved.first), moved.second.x, moved.second.y);
            out << text;
        }
        out << "]";
        return out.str();
    }

    // The table: a scene's name, its result, its vertices
    struct Capture {
        std::ostringstream text;

        void scene(const std::string& name, const GraphCanvas::Result& result, ImDrawList* list, int firstVertex, bool vertices) {
            text << "scene " << name << " " << (list->VtxBuffer.Size - firstVertex) << "\n";
            text << "result " << describeResult(result) << "\n";
            if (!vertices) return;
            char line[64];
            for (int i = firstVertex; i < list->VtxBuffer.Size; i++) {
                const ImDrawVert& v = list->VtxBuffer[i];
                std::snprintf(line, sizeof(line), "%.3f %.3f %08x\n", v.pos.x, v.pos.y, v.col);
                text << line;
            }
        }
    };

    // Every gesture, on the same graph and canvas, each frame's vertices kept
    std::string captureNoPins() {
        CanvasHarness h;
        GraphCanvas canvas;
        std::vector<GraphCanvas::Node> nodes;
        std::vector<GraphCanvas::Link> links;
        animatorLikeGraph(nodes, links);
        Capture capture;
        // A scene's result always; its vertices when `vertices`, for the scenes that draw something the others do not
        auto capturedStep = [&](const char* name, bool vertices) {
            // inside the frame: the next one rebuilds the window's list
            h.frame(canvas, nodes, links, [&](const GraphCanvas::Result& result, ImDrawList* list, int firstVertex) {
                if (name) capture.scene(name, result, list, firstVertex, vertices);
            });
        };
        auto idle = [&]() { capturedStep(nullptr, false); };
        auto drawn = [&](const char* name) { capturedStep(name, true); };
        auto result = [&](const char* name) { capturedStep(name, false); };

        h.mouse(790, 490);
        idle();
        drawn("framed");                    // the first draw frames the view
        h.mouse(300, 200);
        idle();
        h.wheel(2.0f);
        drawn("wheel_in");
        h.wheel(-1.0f);
        result("wheel_out");

        h.mouse(700, 450);
        idle();
        h.button(2, true);
        idle();
        h.mouse(680, 430);
        idle();
        h.mouse(660, 420);
        drawn("middle_pan");
        h.button(2, false);
        idle();

        canvas.frame(nodes);
        result("framed_again");

        // a click on a node selects it; a drag moves it, snapped, and reports on release
        const ImVec2 jumpBox = canvas.toScreen(ImVec2(530, 150));
        h.mouse(jumpBox.x, jumpBox.y);
        idle();
        h.button(0, true);
        drawn("node_press");
        h.mouse(jumpBox.x + 33, jumpBox.y + 27);
        drawn("node_drag");
        h.button(0, false);
        result("node_release");

        // a click on an arrow selects it
        const ImVec2 arrow = canvas.toScreen(ImVec2(200, 20));
        h.mouse(arrow.x, arrow.y);
        idle();
        h.button(0, true);
        drawn("link_press");
        h.button(0, false);
        idle();

        // box select
        h.mouse(750, 480);
        idle();
        h.button(0, true);
        result("box_press");
        h.mouse(500, 300);
        drawn("box_drag");
        h.button(0, false);
        drawn("box_release");

        // link mode: the preview follows the mouse, a click on a node ends it
        canvas.beginLink(4);
        h.mouse(760, 470);
        drawn("link_mode");
        const ImVec2 runBox = canvas.toScreen(ImVec2(300, 140));
        h.mouse(runBox.x, runBox.y);
        idle();
        h.button(0, true);
        result("link_mode_end");
        h.button(0, false);
        idle();

        // right click on a node, an arrow, the background
        const ImVec2 idleBox = canvas.toScreen(ImVec2(300, 20));
        h.mouse(idleBox.x, idleBox.y);
        idle();
        h.button(1, true);
        idle();
        h.button(1, false);
        result("context_node");
        h.mouse(arrow.x, arrow.y);
        idle();
        h.button(1, true);
        idle();
        h.button(1, false);
        result("context_link");
        h.mouse(760, 470);
        idle();
        h.button(1, true);
        idle();
        h.button(1, false);
        result("context_background");

        // a double click
        const ImVec2 exitBox = canvas.toScreen(ImVec2(560, 20));
        h.mouse(exitBox.x, exitBox.y);
        idle();
        h.button(0, true);
        idle();
        h.button(0, false);
        idle();
        h.button(0, true);
        result("double_click");
        h.button(0, false);
        idle();
        return capture.text.str();
    }

    std::vector<std::string> splitLines(const std::string& text) {
        std::vector<std::string> lines;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
        return lines;
    }

    // Vertices within 0.02 px (a compiler's float rounding), the rest to the letter
    bool sameLine(const std::string& a, const std::string& b) {
        if (a == b) return true;
        float ax, ay, bx, by;
        unsigned ac, bc;
        if (std::sscanf(a.c_str(), "%f %f %x", &ax, &ay, &ac) != 3 || std::sscanf(b.c_str(), "%f %f %x", &bx, &by, &bc) != 3) return false;
        return ac == bc && std::fabs(ax - bx) <= 0.02f && std::fabs(ay - by) <= 0.02f;
    }

}

// T2 R54, AC41: nothing changes for a graph without pins
TEST(GraphCanvasCompat, NoPinsDrawsAsBefore) {
    const std::filesystem::path table = std::filesystem::path(JASPER_TEST_FIXTURES_DIR) / "widget" / "graph_canvas_nopins.txt";
    const std::string now = captureNoPins();
    if (std::getenv("JASPER_TEST_UPDATE_GOLDEN")) {
        std::filesystem::create_directories(table.parent_path());
        std::ofstream(table, std::ios::binary) << now;
    }
    std::ifstream in(table, std::ios::binary);
    ASSERT_TRUE(in.good()) << "missing " << table;
    std::ostringstream expected;
    expected << in.rdbuf();

    const std::vector<std::string> want = splitLines(expected.str());
    const std::vector<std::string> got = splitLines(now);
    std::string scene;
    size_t differences = 0;
    for (size_t i = 0; i < want.size() && i < got.size(); i++) {
        if (want[i].rfind("scene ", 0) == 0) scene = want[i];
        if (!sameLine(want[i], got[i]) && differences++ < 5) {
            ADD_FAILURE() << "in " << scene << ", line " << (i + 1) << ": table says \"" << want[i] << "\", the canvas gave \"" << got[i] << "\"";
        }
    }
    EXPECT_EQ(want.size(), got.size());
    EXPECT_EQ(differences, 0u);
}

// T2 R56, R58, AC41: the box grows around its footer and its error's wrapped message, both drawn inside it
TEST(GraphCanvasPins, FooterAndErrorFitTheBox) {
    Rig rig;
    rig.dataflow();
    GraphCanvas::Node& node = rig.nodes[0];
    node.thumbnail = true;
    node.footer = {"-0.62 ... 0.71  <= 1.00", "unused"};
    node.error = true;
    node.errorText = "this message is long enough that it has to wrap to stay inside the box of its node";
    rig.frame();
    const ImVec2 bare = GraphCanvas::nodeSize(node);
    EXPECT_EQ(bare.y, GraphCanvas::HEADER_HEIGHT + GraphCanvas::PORT_ROW + 2 * GraphCanvas::PADDING + GraphCanvas::THUMBNAIL_SLOT +
                          2 * GraphCanvas::FOOTER_LINE);
    EXPECT_GT(rig.nodes[0].size.y, bare.y + 2 * ImGui::GetFontSize() * 0.85f) << "at least two wrapped lines more";

    size_t footerVertices = 0, errorVertices = 0, outside = 0;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const int first = list->VtxBuffer.Size;
    rig.canvas.draw("##canvas", rig.nodes, rig.links, rig.groups);
    const ImVec2 a = rig.canvas.toScreen(rig.nodes[0].position);
    const ImVec2 b = rig.canvas.toScreen(ImVec2(rig.nodes[0].position.x + rig.nodes[0].size.x, rig.nodes[0].position.y + rig.nodes[0].size.y));
    for (int i = first; i < list->VtxBuffer.Size; i++) {
        const ImDrawVert& v = list->VtxBuffer[i];
        const bool footer = v.col == IM_COL32(170, 170, 170, 255);
        const bool error = v.col == IM_COL32(255, 120, 120, 255);
        footerVertices += footer;
        errorVertices += error;
        if ((footer || error) && (v.pos.x < a.x || v.pos.x > b.x || v.pos.y < a.y || v.pos.y > b.y)) outside++;
    }
    ImGui::End();
    ImGui::EndFrame();
    EXPECT_GT(footerVertices, 0u);
    EXPECT_GT(errorVertices, 0u);
    EXPECT_EQ(outside, 0u) << "every footer and error glyph inside the box";
}

namespace {

    // the vertices of one frame of the rig, with the mouse where it is
    std::vector<ImDrawVert> drawnVertices(Rig& rig) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImDrawList* list = ImGui::GetWindowDrawList();
        const int first = list->VtxBuffer.Size;
        rig.canvas.draw("##canvas", rig.nodes, rig.links, rig.groups);
        std::vector<ImDrawVert> vertices(list->VtxBuffer.Data + first, list->VtxBuffer.Data + list->VtxBuffer.Size);
        ImGui::End();
        ImGui::EndFrame();
        return vertices;
    }

    size_t countColour(const std::vector<ImDrawVert>& vertices, ImU32 rgb, unsigned alpha) {
        size_t count = 0;
        for (const ImDrawVert& v : vertices) {
            if ((v.col & ~IM_COL32_A_MASK) == (rgb & ~IM_COL32_A_MASK) && ((v.col >> IM_COL32_A_SHIFT) & 0xFF) == alpha) count++;
        }
        return count;
    }

}

// T2 R53, R58, AC41: a port an issue names has a red ring; during a wire drag a port canConnect refuses dims
TEST(GraphCanvasPins, ErrorRingAndRefusedPortsDim) {
    Rig rig;
    rig.dataflow();
    const ImU32 coord = IM_COL32(255, 200, 90, 255), field = IM_COL32(120, 200, 255, 255);
    rig.nodes[1].inputs[0].color = field;
    rig.nodes[1].inputs[1].color = coord;
    rig.frame();
    EXPECT_EQ(countColour(drawnVertices(rig), IM_COL32(235, 70, 70, 255), 255), 0u);
    rig.nodes[1].inputs[1].error = true;
    EXPECT_GT(countColour(drawnVertices(rig), IM_COL32(235, 70, 70, 255), 255), 0u) << "the ring";
    rig.nodes[1].inputs[1].error = false;

    rig.canvas.canConnect = [](const GraphCanvas::PortRef&, const GraphCanvas::PortRef& input) { return input.port != "b"; };
    const ImVec2 from = rig.port(0, "out", true);
    rig.moveTo(from);
    rig.frame();
    rig.press();
    rig.moveTo(rig.canvas.toScreen(ImVec2(700, 500)));
    ASSERT_TRUE(rig.canvas.isWiring());
    const std::vector<ImDrawVert> dragging = drawnVertices(rig);
    EXPECT_GT(countColour(dragging, coord, static_cast<unsigned>(255 * 0.3f)), 0u) << "the refused port at 30 % alpha";
    EXPECT_GT(countColour(dragging, field, 255), 0u) << "the accepted one at full alpha";
    rig.release();
}

// T2 R53, AC41: a wire is picked along its curve, not along the straight line between its ports
TEST(GraphCanvasPins, AWireIsPickedAlongItsCurve) {
    Rig rig;
    rig.dataflow();
    rig.nodes[1].position = ImVec2(400, 340);
    GraphCanvas::Link wire;
    wire.id = 77;
    wire.from = 1;
    wire.to = 2;
    wire.fromPort = "out";
    wire.toPort = "a";
    rig.links.push_back(wire);
    rig.frame();

    // the curve's midpoint is on the chord, by symmetry; a quarter along they part
    const ImVec2 a = rig.port(0, "out", true), b = rig.port(1, "a", false);
    const float reach = std::max(40.0f * rig.canvas.getZoom(), std::fabs(b.x - a.x) * 0.5f);
    const float u = 0.75f, t = 0.25f;
    const ImVec2 onCurve(u * u * u * a.x + 3 * u * u * t * (a.x + reach) + 3 * u * t * t * (b.x - reach) + t * t * t * b.x,
                         u * u * u * a.y + 3 * u * u * t * a.y + 3 * u * t * t * b.y + t * t * t * b.y);
    const ImVec2 onChord(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    ASSERT_GT(std::fabs(onCurve.y - onChord.y), 10.0f);

    rig.moveTo(onChord);
    rig.frame();
    GraphCanvas::Result pressed = rig.press();
    rig.release();
    EXPECT_EQ(pressed.clickedLink, 0u) << "the chord is not the wire";

    rig.moveTo(onCurve);
    rig.frame();
    pressed = rig.press();
    rig.release();
    EXPECT_EQ(pressed.clickedLink, 77u);
    EXPECT_TRUE(rig.links[0].selected);
}
