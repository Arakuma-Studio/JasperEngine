// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// R35 / AC5: an inspector with 20 visible Curve fields costs at most 0.2 ms of CPU a frame, because
// each field's polyline is cached until its curve changes. An offscreen ImGui context draws the
// 20 fields for 1,000 frames; the budget is enforced with the benchmark slack
// (perfSlack(), docs/testing.md) and printed otherwise. A changed curve redraws its new shape (the cache
// follows the value, not the field).

#include "widget/CurveField.h"

#include "support/EditorTestSupport.h"

#include "imgui.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    struct Offscreen {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGuiContext* context = ImGui::CreateContext();

        Offscreen() {
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1280, 900);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }
        ~Offscreen() {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        }

        // one frame of an inspector with these fields; a checksum of the vertices the window drew
        double frame(std::vector<Curve>& curves) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(400, 880));
            ImGui::Begin("Inspector");
            for (size_t i = 0; i < curves.size(); i++) {
                const std::string id = "curve" + std::to_string(i);
                ui::curveField(id.c_str(), curves[i]);
            }
            const ImVector<ImDrawVert>& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
            double sum = 0.0;
            for (int v = 0; v < vertices.Size; v++) sum += (double)vertices[v].pos.y * (double)(v % 97 + 1);
            ImGui::End();
            ImGui::Render();
            return sum;
        }
    };

    Curve wave(int i) {
        Curve curve;
        for (int k = 0; k < 12; k++) {
            const float t = (float)k / 11.0f;
            curve.addKey(t, 0.5f + 0.5f * std::sin(t * 6.2831853f * (1.0f + (float)i * 0.1f)));
        }
        curve.updateAllTangents();
        return curve;
    }

}

TEST(CurveFieldPerf, TwentyFieldsAFrame) {
    Offscreen gui;
    std::vector<Curve> curves;
    for (int i = 0; i < 20; i++) curves.push_back(wave(i));
    gui.frame(curves);   // the first frame builds the polylines
    constexpr int kFrames = 1000;
    const auto start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; f++) gui.frame(curves);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
    std::printf("[ CurveFieldPerf ] 20 curve fields: %.4f ms a frame (budget 0.2 ms)\n", ms);
    if (test::perfSlack() > 0.0) {
        EXPECT_LE(ms, 0.2 * jasperengine::editor::test::perfSlack());
    }
}

TEST(CurveFieldPerf, AChangedCurveRedraws) {
    Offscreen gui;
    std::vector<Curve> curves = {Curve::linear(0.0f, 0.0f, 1.0f, 1.0f)};
    gui.frame(curves);
    const double straight = gui.frame(curves);
    EXPECT_EQ(gui.frame(curves), straight);   // the same curve draws the same
    curves[0] = wave(3);
    gui.frame(curves);
    const double wavy = gui.frame(curves);
    // the thumbnail follows the new value, and back
    EXPECT_NE(wavy, straight);
    curves[0] = Curve::linear(0.0f, 0.0f, 1.0f, 1.0f);
    gui.frame(curves);
    EXPECT_EQ(gui.frame(curves), straight);
}
