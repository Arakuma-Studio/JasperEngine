// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A selected .ies file (spec S99 R37): its photometric metadata and a polar plot of its vertical
// profile, as render/IesProfile.h reads it for the renderer.

#include "InspectorWindow.h"

#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "render/IesProfile.h"
#include "util/ProjectChecks.h"
#include "util/UIProbe.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {
    struct IesInspectorState{
        fs::path asset;
        std::string text;
        double checkedTime = -1.0;
        bool parsed = false;
        std::string error;
        IesProfile profile;
    };

    IesInspectorState& iesInspectorState(){
        static IesInspectorState state;
        return state;
    }

    // the half-plane of horizontal angle h and its opposite, h + 180, drawn left and right of the
    // nadir (down) as candela relative to the peak
    void drawIesPolarPlot(const IesProfile& profile, float size){
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 center(origin.x + size * 0.5f, origin.y + size * 0.5f);
        const float radius = size * 0.46f;
        const ImU32 grid = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.35f);
        const ImU32 axis = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.7f);

        drawList->AddRectFilled(origin, ImVec2(origin.x + size, origin.y + size), ImGui::GetColorU32(ImGuiCol_FrameBg));
        for (int ring = 1; ring <= 4; ring++){
            drawList->AddCircle(center, radius * (float)ring / 4.0f, grid, 48);
        }
        for (int spoke = 0; spoke < 12; spoke++){
            const float angle = (float)spoke * 3.14159265f / 6.0f;
            drawList->AddLine(center, ImVec2(center.x + std::sin(angle) * radius, center.y + std::cos(angle) * radius), grid);
        }
        drawList->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), axis);
        drawList->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), axis);

        // C0-C180 solid, C90-C270 lighter: vertical angle 0 points down (the luminaire's nadir)
        struct Plane { float horizontal; ImU32 color; };
        const Plane planes[2] = {{0.0f, IM_COL32(255, 196, 64, 255)}, {90.0f, IM_COL32(110, 170, 255, 220)}};
        const int steps = 90;
        for (const Plane& plane : planes){
            for (int side = 0; side < 2; side++){
                const float horizontal = plane.horizontal + (side ? 180.0f : 0.0f);
                const float sign = side ? -1.0f : 1.0f;
                ImVec2 previous;
                for (int i = 0; i <= steps; i++){
                    const float vertical = 180.0f * (float)i / (float)steps;
                    const float value = profile.sample(vertical, horizontal);
                    const float theta = vertical * 3.14159265f / 180.0f;
                    const ImVec2 point(center.x + sign * std::sin(theta) * value * radius, center.y + std::cos(theta) * value * radius);
                    if (i > 0) drawList->AddLine(previous, point, plane.color, 2.0f);
                    previous = point;
                }
            }
        }
        ImGui::Dummy(ImVec2(size, size));
        editor::UIProbe::item("ies.polar_plot");
    }

    void iesRow(const char* label, const std::string& value){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextWrapped("%s", value.empty() ? "-" : value.c_str());
    }
}

void editor::InspectorWindow::drawIesProfileAsset(const fs::path& asset){
    IesInspectorState& state = iesInspectorState();
    drawAssetHeader(ICON_FA_LIGHTBULB, asset, "IES Profile");

    // read again when another file is inspected or the file changed
    const double now = ImGui::GetTime();
    if (state.asset != asset || now - state.checkedTime >= 0.5){
        state.checkedTime = now;
        std::string text;
        const bool read = ProjectChecks::readFile(asset, text);
        if (state.asset != asset || !read || text != state.text){
            state.asset = asset;
            state.text = read ? text : std::string();
            state.error.clear();
            state.parsed = read && IesProfile::parse(text, state.profile, state.error);
            if (!read) state.error = "cannot read the file";
        }
    }
    if (!state.parsed){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", state.error.c_str());
        ImGui::PopStyleColor();
        return;
    }

    const IesProfile& profile = state.profile;
    const float plotSize = std::min(ImGui::GetContentRegionAvail().x, 18.0f * ImGui::GetFontSize());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - plotSize) * 0.5f));
    drawIesPolarPlot(profile, plotSize);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Candela by vertical angle (0 down the light's forward axis), relative to the peak: C0-C180 orange, C90-C270 blue.");
    ImGui::PopStyleColor();

    if (ImGui::BeginTable("##ies_metadata", 2, ImGuiTableFlags_SizingStretchProp)){
        char number[64];
        iesRow("Format", profile.format);
        iesRow("Manufacturer", profile.manufacturer);
        iesRow("Luminaire", profile.luminaire);
        iesRow("Lamp", profile.lamp);
        std::snprintf(number, sizeof(number), "%.0f cd", profile.peakCandela);
        iesRow("Peak", number);
        std::snprintf(number, sizeof(number), profile.lumensPerLamp > 0.0f ? "%.0f lm" : "%.0f lm (from the candela)", profile.lumens);
        iesRow("Lumens", number);
        std::snprintf(number, sizeof(number), "%i", profile.lamps);
        iesRow("Lamps", number);
        iesRow("Symmetry", IesProfile::symmetryName(profile.symmetry));
        std::snprintf(number, sizeof(number), "%zu vertical x %zu horizontal", profile.verticalAngles.size(), profile.horizontalAngles.size());
        iesRow("Angles", number);
        ImGui::EndTable();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("A Spot or Point Light's IES Profile shapes it; its Intensity stays the peak.");
    ImGui::PopStyleColor();
}
