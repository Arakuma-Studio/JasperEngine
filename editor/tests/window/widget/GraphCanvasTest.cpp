// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The node canvas' gestures, driven with ImGui's input events on a frame loop of its own: Esc
// during a node drag or a box select (docs/undo.md, Drags), and what a click on a member of a
// multi-selection does.

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
            const GraphCanvas::Result result = canvas.draw("##canvas", nodes, links);
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
