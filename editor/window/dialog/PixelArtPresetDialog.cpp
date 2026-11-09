// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PixelArtPresetDialog.h"

#include "Project.h"
#include "Theme.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace px = jasperengine::editor::pixelart;

void PixelArtPresetDialog::open(){
    requested = true;
    dirty = true;
    result.clear();
    pixelsPerUnit = project->getPixelsPerUnit() == 1 ? 1 : project->getPixelsPerUnit();
}

px::PixelArtPresetOptions PixelArtPresetDialog::options() const{
    px::PixelArtPresetOptions o;
    const std::vector<px::PixelArtScreen>& sizes = px::presetBaseResolutions();
    if (baseIndex >= 0 && baseIndex < (int)sizes.size()){
        o.baseWidth = sizes[(size_t)baseIndex].width;
        o.baseHeight = sizes[(size_t)baseIndex].height;
    }else{
        o.baseWidth = std::clamp(customWidth, 16, 4096);
        o.baseHeight = std::clamp(customHeight, 16, 4096);
    }
    o.integerExpand = scalingIndex == 1;
    o.pixelsPerUnit = std::clamp(pixelsPerUnit, 1, 256);
    o.windowScale = std::clamp(windowScale, 2, 8);
    o.addCameras = addCameras;
    o.convertTextures = convertTextures;
    o.pointReferences = pointReferences;
    return o;
}

void PixelArtPresetDialog::draw(){
    if (requested){
        ImGui::OpenPopup(POPUP_NAME);
        requested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(720.0f), Theme::dpi(520.0f)), ImGuiCond_Appearing);
    visible = EditorModal::begin(POPUP_NAME, nullptr, 0);
    if (!visible)
        return;

    // the options
    std::vector<std::string> bases;
    for (const px::PixelArtScreen& size : px::presetBaseResolutions())
        bases.push_back(std::to_string(size.width) + "x" + std::to_string(size.height));
    bases.push_back("Custom");
    if (EditorModal::combo("Base Resolution", "base_resolution", &baseIndex, bases)) dirty = true;
    if (baseIndex == (int)bases.size() - 1){
        if (EditorModal::inputInt("Width##PresetCustomWidth", "custom_width", &customWidth)) dirty = true;
        if (EditorModal::inputInt("Height##PresetCustomHeight", "custom_height", &customHeight)) dirty = true;
    }
    if (EditorModal::combo("Scaling", "scaling", &scalingIndex, {"Integer (Letterbox)", "Integer (Expand)"})) dirty = true;
    if (EditorModal::inputInt("Pixels Per Unit", "pixels_per_unit", &pixelsPerUnit)) dirty = true;
    std::vector<std::string> scales;
    for (int k = 2; k <= 8; k++) scales.push_back(std::to_string(k) + "x");
    int scaleIndex = std::clamp(windowScale, 2, 8) - 2;
    if (EditorModal::combo("Window Size", "window_scale", &scaleIndex, scales)){
        windowScale = scaleIndex + 2;
        dirty = true;
    }
    if (EditorModal::checkbox("Add Pixel Perfect Cameras", "add_cameras", &addCameras)) dirty = true;
    if (EditorModal::checkbox("Import Textures as Pixel Art", "convert_textures", &convertTextures)) dirty = true;
    if (EditorModal::checkbox("Point-Filter Their References", "point_references", &pointReferences)) dirty = true;

    if (dirty){
        plan = px::planPixelArtPreset(*project, options());
        dirty = false;
    }
    const px::PresetCounts counts = px::countPresetChanges(plan);
    const std::string summary = std::to_string(plan.size()) + " change(s): " + std::to_string(counts.settings) + " setting(s), " +
                                std::to_string(counts.scenes) + " scene(s), " + std::to_string(counts.textures) + " texture(s), " +
                                std::to_string(counts.references) + " reference(s).";
    EditorModal::message(summary);
    ImGui::Separator();
    ImGui::TextUnformatted(summary.c_str());
    ImGui::BeginChild("##PresetChanges", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.0f), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##PresetTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)){
        ImGui::TableSetupColumn("Change");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("New");
        ImGui::TableHeadersRow();
        for (const px::FixChange& change : plan){
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted((change.target == "project" ? change.field : change.target + "  " + change.field).c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", change.from.dump().c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", change.to.dump().c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (!result.empty()){
        ImGui::TextColored(Theme::Colors::ErrorText, "%s", result.c_str());
        EditorModal::error(result);
    }
    ImGui::BeginDisabled(plan.empty());
    if (EditorModal::button("Apply", "ok")){
        px::FixApplyReport report;
        std::string error;
        if (px::applyPixelArtPreset(*project, plan, report, error)){
            applied = true;
            ImGui::CloseCurrentPopup();
            visible = false;
        }else{
            result = "Nothing was changed: " + error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel")){
        ImGui::CloseCurrentPopup();
        visible = false;
    }
    ImGui::EndPopup();
}
