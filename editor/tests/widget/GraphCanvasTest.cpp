// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The node-graph canvas (T2 R53, R54, AC41). GraphCanvasCompat pins what a graph without pins draws:
// every vertex of a fixed Animator-like graph through each gesture, against a table captured from the
// code before T2 touched the canvas (fixtures/widget/graph_canvas_nopins.txt; JASPER_TEST_UPDATE_GOLDEN=1
// writes it again, which is only right when a change of the Animator's drawing is meant). GraphCanvasPins
// drives the dataflow additions: a wire dragged to a pin, to empty space and to a pin that refuses it,
// a wire's hit test, a box that fits its thumbnail and footer.

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

namespace {

    // perlin [out] -> [in] output, and a third node whose input takes another type
    const uint64_t kPerlinOut = 101, kOutputIn = 201, kCoordIn = 301;

    void pinGraph(std::vector<GraphCanvas::Node>& nodes, std::vector<GraphCanvas::Link>& links) {
        nodes.clear();
        links.clear();
        auto node = [&](uint64_t id, float x, float y, const char* label) -> GraphCanvas::Node& {
            GraphCanvas::Node n;
            n.id = id;
            n.position = ImVec2(x, y);
            n.label = label;
            nodes.push_back(n);
            return nodes.back();
        };
        node(1, 0, 0, "Perlin").outputs.push_back({kPerlinOut, "Out", IM_COL32(120, 200, 255, 255), false, false});
        node(2, 400, 0, "Output").inputs.push_back({kOutputIn, "In", IM_COL32(120, 200, 255, 255), false, false});
        node(3, 400, 200, "Warp").inputs.push_back({kCoordIn, "Coord", IM_COL32(255, 200, 90, 255), false, false});
    }

    // A pin's place on screen: on the box's side, its row's centre (GraphCanvas.cpp's layout)
    ImVec2 pinOnScreen(const GraphCanvas& canvas, const GraphCanvas::Node& node, bool input, size_t row = 0) {
        return canvas.toScreen(ImVec2(node.position.x + (input ? 0.0f : node.size.x), node.position.y + 24.0f + 18.0f * (static_cast<float>(row) + 0.5f)));
    }

    // Frames a wire drag: a press at `from`, a move in steps, a release at `to`. The release frame's result.
    GraphCanvas::Result dragWire(CanvasHarness& h, GraphCanvas& canvas, std::vector<GraphCanvas::Node>& nodes, std::vector<GraphCanvas::Link>& links,
                                 ImVec2 from, ImVec2 to) {
        h.mouse(from.x, from.y);
        h.frame(canvas, nodes, links);
        h.button(0, true);
        h.frame(canvas, nodes, links);
        EXPECT_TRUE(canvas.isWiring());
        h.mouse((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f);
        h.frame(canvas, nodes, links);
        h.mouse(to.x, to.y);
        h.frame(canvas, nodes, links);
        h.button(0, false);
        return h.frame(canvas, nodes, links);
    }

}

// T2 R53, AC41: a drag from an output to a compatible input, and the other way round, gives output then input
TEST(GraphCanvasPins, DragConnects) {
    CanvasHarness h;
    GraphCanvas canvas;
    std::vector<GraphCanvas::Node> nodes;
    std::vector<GraphCanvas::Link> links;
    pinGraph(nodes, links);
    h.mouse(790, 490);
    h.frame(canvas, nodes, links);
    h.frame(canvas, nodes, links);

    GraphCanvas::Result result = dragWire(h, canvas, nodes, links, pinOnScreen(canvas, nodes[0], false), pinOnScreen(canvas, nodes[1], true));
    EXPECT_EQ(result.wireFrom, kPerlinOut);
    EXPECT_EQ(result.wireTo, kOutputIn);
    EXPECT_EQ(result.wireDroppedFrom, 0u);
    EXPECT_FALSE(canvas.isWiring());
    EXPECT_TRUE(result.moved.empty()) << "a wire drag does not move the node under it";
    EXPECT_FALSE(nodes[0].selected);

    result = dragWire(h, canvas, nodes, links, pinOnScreen(canvas, nodes[1], true), pinOnScreen(canvas, nodes[0], false));
    EXPECT_EQ(result.wireFrom, kPerlinOut) << "normalized: the output first";
    EXPECT_EQ(result.wireTo, kOutputIn);

    // The hovered pin is reported
    const ImVec2 pin = pinOnScreen(canvas, nodes[0], false);
    h.mouse(pin.x, pin.y);
    h.frame(canvas, nodes, links);
    EXPECT_EQ(h.frame(canvas, nodes, links).hoveredPin, kPerlinOut);
}

// T2 R53, AC41: dropped on empty space the caller gets the pin and where, for its search popup
TEST(GraphCanvasPins, DropOnEmptyRequestsSearch) {
    CanvasHarness h;
    GraphCanvas canvas;
    std::vector<GraphCanvas::Node> nodes;
    std::vector<GraphCanvas::Link> links;
    pinGraph(nodes, links);
    h.mouse(790, 490);
    h.frame(canvas, nodes, links);
    h.frame(canvas, nodes, links);

    const ImVec2 empty = canvas.toScreen(ImVec2(200, 150));
    GraphCanvas::Result result = dragWire(h, canvas, nodes, links, pinOnScreen(canvas, nodes[0], false), empty);
    EXPECT_EQ(result.wireDroppedFrom, kPerlinOut);
    EXPECT_NEAR(result.wireDropPosition.x, 200.0f, 1.0f);
    EXPECT_NEAR(result.wireDropPosition.y, 150.0f, 1.0f);
    EXPECT_EQ(result.wireFrom, 0u);

    // On a node's body it is no request and no connection
    const ImVec2 body = canvas.toScreen(ImVec2(nodes[1].position.x + nodes[1].size.x * 0.5f, nodes[1].position.y + 10.0f));
    result = dragWire(h, canvas, nodes, links, pinOnScreen(canvas, nodes[0], false), body);
    EXPECT_EQ(result.wireDroppedFrom, 0u);
    EXPECT_EQ(result.wireFrom, 0u);
}

// T2 R53, AC41: canConnect refuses the drop and dims the pin during the drag
TEST(GraphCanvasPins, IncompatibleIsRefused) {
    CanvasHarness h;
    GraphCanvas canvas;
    std::vector<GraphCanvas::Node> nodes;
    std::vector<GraphCanvas::Link> links;
    pinGraph(nodes, links);
    std::vector<std::pair<uint64_t, uint64_t>> asked;
    canvas.canConnect = [&](uint64_t from, uint64_t to) {
        asked.push_back({from, to});
        return to != kCoordIn;
    };
    h.mouse(790, 490);
    h.frame(canvas, nodes, links);
    h.frame(canvas, nodes, links);

    GraphCanvas::Result result = dragWire(h, canvas, nodes, links, pinOnScreen(canvas, nodes[0], false), pinOnScreen(canvas, nodes[2], true));
    EXPECT_EQ(result.wireFrom, 0u);
    EXPECT_EQ(result.wireTo, 0u);
    EXPECT_EQ(result.wireDroppedFrom, 0u) << "a pin that refuses is no empty space";
    ASSERT_FALSE(asked.empty());
    for (const auto& pair : asked) EXPECT_EQ(pair.first, kPerlinOut) << "canConnect is asked output first";

    // Two inputs never connect, whatever canConnect says
    canvas.canConnect = nullptr;
    result = dragWire(h, canvas, nodes, links, pinOnScreen(canvas, nodes[1], true), pinOnScreen(canvas, nodes[2], true));
    EXPECT_EQ(result.wireFrom, 0u);

    // During a drag the refused pin is dimmed, the accepted one is not
    canvas.canConnect = [](uint64_t, uint64_t to) { return to != kCoordIn; };
    const ImVec2 from = pinOnScreen(canvas, nodes[0], false);
    h.mouse(from.x, from.y);
    h.frame(canvas, nodes, links);
    h.button(0, true);
    h.frame(canvas, nodes, links);
    h.mouse(600, 400);
    size_t refused = 0, accepted = 0;
    h.frame(canvas, nodes, links, [&](const GraphCanvas::Result&, ImDrawList* list, int first) {
        for (int i = first; i < list->VtxBuffer.Size; i++) {
            const ImU32 col = list->VtxBuffer[i].col;
            if ((col & 0x00FFFFFF) == (IM_COL32(255, 200, 90, 0) & 0x00FFFFFF) && ((col >> IM_COL32_A_SHIFT) & 0xFF) == 63) refused++;
            if ((col & 0x00FFFFFF) == (IM_COL32(120, 200, 255, 0) & 0x00FFFFFF) && ((col >> IM_COL32_A_SHIFT) & 0xFF) == 255) accepted++;
        }
    });
    EXPECT_GT(refused, 0u) << "the coord pin at a quarter alpha";
    EXPECT_GT(accepted, 0u) << "the field pin at full alpha";
    h.button(0, false);
    h.frame(canvas, nodes, links);
}

// T2 R53, AC41: a wire is selected along its curve, not along the straight line between its pins
TEST(GraphCanvasPins, WireHitTest) {
    CanvasHarness h;
    GraphCanvas canvas;
    std::vector<GraphCanvas::Node> nodes;
    std::vector<GraphCanvas::Link> links;
    pinGraph(nodes, links);
    nodes[0].position = ImVec2(0, 0);
    nodes[2].position = ImVec2(400, 240);
    nodes[2].inputs[0].connected = true;
    GraphCanvas::Link wire;
    wire.id = 77;
    wire.from = 1;
    wire.to = 3;
    wire.fromPin = kPerlinOut;
    wire.toPin = kCoordIn;
    links.push_back(wire);
    h.mouse(790, 490);
    h.frame(canvas, nodes, links);
    h.frame(canvas, nodes, links);

    // The curve's midpoint is on the straight line, by symmetry; a quarter along they part
    const ImVec2 a = pinOnScreen(canvas, nodes[0], false);
    const ImVec2 b = pinOnScreen(canvas, nodes[2], true);
    const float reach = std::fabs(b.x - a.x) * 0.5f;
    const float u = 0.75f, t = 0.25f;
    const ImVec2 onCurve(u * u * u * a.x + 3 * u * u * t * (a.x + reach) + 3 * u * t * t * (b.x - reach) + t * t * t * b.x,
                         u * u * u * a.y + 3 * u * u * t * a.y + 3 * u * t * t * b.y + t * t * t * b.y);
    const ImVec2 onChord(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    ASSERT_GT(std::fabs(onCurve.y - onChord.y), 10.0f);

    h.mouse(onChord.x, onChord.y);
    h.frame(canvas, nodes, links);
    h.button(0, true);
    GraphCanvas::Result result = h.frame(canvas, nodes, links);
    EXPECT_EQ(result.clickedLink, 0u) << "the chord is not the wire";
    EXPECT_TRUE(result.clickedBackground);
    h.button(0, false);
    h.frame(canvas, nodes, links);

    h.mouse(onCurve.x, onCurve.y);
    h.frame(canvas, nodes, links);
    h.button(0, true);
    result = h.frame(canvas, nodes, links);
    EXPECT_EQ(result.clickedLink, 77u);
    EXPECT_TRUE(links[0].selected);
    EXPECT_TRUE(result.selectionChanged);
    h.button(0, false);
    h.frame(canvas, nodes, links);

    // Right click on it reports it too
    h.button(1, true);
    h.frame(canvas, nodes, links);
    h.button(1, false);
    result = h.frame(canvas, nodes, links);
    EXPECT_TRUE(result.contextRequested);
    EXPECT_EQ(result.contextLink, 77u);
}

// T2 R53, R56, AC41: the box grows around its thumbnail, footer and error, and the thumbnail hides below zoom 0.5
TEST(GraphCanvasPins, ThumbnailAndFooterFit) {
    CanvasHarness h;
    GraphCanvas canvas;
    std::vector<GraphCanvas::Node> nodes;
    std::vector<GraphCanvas::Link> links;
    pinGraph(nodes, links);
    GraphCanvas::Node& node = nodes[0];
    node.size = ImVec2(40, 20);
    node.position = ImVec2(100, 40);
    const ImTextureID texture = static_cast<ImTextureID>(4242);
    node.thumbnail = texture;
    node.thumbnailSize = ImVec2(64, 64);
    node.footer = {"-0.62 ... 0.71  <= 1.00", "0.31 ms"};
    node.errorText = "this message is long enough that it has to wrap to stay inside the box of its node";
    nodes.resize(1);
    h.mouse(790, 490);
    h.frame(canvas, nodes, links);

    size_t images = 0;
    const auto measure = [&](const GraphCanvas::Result&, ImDrawList* list, int first) {
        images = 0;
        for (int i = 0; i < list->CmdBuffer.Size; i++) images += list->CmdBuffer[i].TexRef.GetTexID() == texture;
        const ImVec2 a = canvas.toScreen(nodes[0].position);
        const ImVec2 b = canvas.toScreen(ImVec2(nodes[0].position.x + nodes[0].size.x, nodes[0].position.y + nodes[0].size.y));
        size_t footerVertices = 0, errorVertices = 0;
        for (int i = first; i < list->VtxBuffer.Size; i++) {
            const ImDrawVert& v = list->VtxBuffer[i];
            const bool footer = v.col == IM_COL32(170, 170, 170, 255);
            const bool error = v.col == IM_COL32(255, 120, 120, 255);
            footerVertices += footer;
            errorVertices += error;
            if (!footer && !error) continue;
            EXPECT_GE(v.pos.x, a.x);
            EXPECT_LE(v.pos.x, b.x);
            EXPECT_GE(v.pos.y, a.y);
            EXPECT_LE(v.pos.y, b.y);
        }
        EXPECT_GT(footerVertices, 0u);
        EXPECT_GT(errorVertices, 0u);
    };
    h.frame(canvas, nodes, links, measure);
    EXPECT_GE(nodes[0].size.x, 64.0f + 2 * 6.0f);
    EXPECT_GE(nodes[0].size.y, 24.0f + 18.0f + 64.0f + 2 * 15.0f);
    EXPECT_GT(images, 0u) << "the thumbnail draws";

    // Zoomed out below 0.5 it hides, the box keeps its size
    const ImVec2 sizeBefore = nodes[0].size;
    const ImVec2 inside = canvas.toScreen(ImVec2(nodes[0].position.x + 10.0f, nodes[0].position.y + 10.0f));
    h.mouse(inside.x, inside.y);
    h.frame(canvas, nodes, links);
    for (int i = 0; i < 12; i++) {
        h.wheel(-1.0f);
        h.frame(canvas, nodes, links);
    }
    bool sawImage = false;
    h.frame(canvas, nodes, links, [&](const GraphCanvas::Result&, ImDrawList* list, int) {
        for (int i = 0; i < list->CmdBuffer.Size; i++) sawImage = sawImage || list->CmdBuffer[i].TexRef.GetTexID() == texture;
    });
    EXPECT_FALSE(sawImage) << "no thumbnail below zoom 0.5";
    EXPECT_EQ(nodes[0].size.x, sizeBefore.x);
    EXPECT_EQ(nodes[0].size.y, sizeBefore.y);
}
