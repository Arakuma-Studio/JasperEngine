// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The node-graph canvas (T2 R53, R54, AC41). GraphCanvasCompat pins what a graph without pins draws:
// every vertex of a fixed Animator-like graph through each gesture, against a table captured from the
// code before T2 touched the canvas (fixtures/widget/graph_canvas_nopins.txt; JASPER_TEST_UPDATE_GOLDEN=1
// writes it again, which is only right when a change of the Animator's drawing is meant).

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
