// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S48's game-feel rows (docs/specs/S48-pixel-fx.md R42): Pose Frame Rate on the Animator,
// Animation Player and Pose Animator (a combo of Smooth, 8, 12, 24 and Custom), and the Camera
// Shaker's Pixel Snap and Tick Rate (disabled, with the reason, on a perspective camera). Every
// edit is one scene undo step.

#include "InspectorWindow.h"

#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/CameraComponent.h"
#include "component/CameraShakerComponent.h"
#include "util/UIProbe.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;

namespace {

    constexpr float kPoseFrameRatePresets[] = {8.0f, 12.0f, 24.0f};

    // the combo entry of a value: 0 Smooth, 1..3 the presets, 4 Custom
    int poseFrameRateChoice(float fps){
        if (!(fps > 0.0f)) return 0;
        for (int i = 0; i < 3; i++){
            if (fps == kPoseFrameRatePresets[i]) return i + 1;
        }
        return 4;
    }

}

void editor::InspectorWindow::drawPoseFrameRateRow(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities, float current){
    const uint32_t sceneId = sceneProject->id;
    auto set = [&](float fps){
        MultiPropertyCmd* multi = new MultiPropertyCmd();
        for (Entity entity : entities){
            multi->addPropertyCmd<float>(project, sceneId, entity, cpType, "poseFrameRate", fps);
        }
        CommandHandle::get(sceneId)->addCommandNoMerge(multi);
    };

    prefabRowProperty = "poseFrameRate";
    if (propertyHeader("Pose Frame Rate", -1, current != 0.0f)){
        set(0.0f);
    }
    const int choice = poseFrameRateChoice(current);
    static const char* const labels[] = {"Smooth", "8 fps", "12 fps", "24 fps", "Custom"};
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(choice == 4 ? width * 0.5f : width);
    if (ImGui::BeginCombo("##pose_frame_rate", labels[choice])){
        for (int i = 0; i < 5; i++){
            if (ImGui::Selectable(labels[i], i == choice) && i != choice){
                // Custom starts from the value it shows (6 fps from Smooth: a typical held count)
                set(i == 0 ? 0.0f : i == 4 ? (current > 0.0f ? current : 6.0f) : kPoseFrameRatePresets[i - 1]);
            }
        }
        ImGui::EndCombo();
    }
    UIProbe::item("pose_frame_rate");
    ImGui::SetItemTooltip("Frames per second the poses hold on (Smooth: every frame). Each track steps from its previous key, "
                          "so every key's pose shows; blends step too. Root motion and events stay at the true time.");
    if (choice == 4){
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        float value = current;
        if (ImGui::DragFloat("##pose_frame_rate_custom", &value, 0.25f, 0.5f, 120.0f, "%.2f fps") && std::isfinite(value)){
            set(std::max(0.5f, std::min(120.0f, value)));
        }
        UIProbe::item("pose_frame_rate_custom");
    }
}

void editor::InspectorWindow::drawShakerPixelRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    Scene* scene = sceneProject->scene;
    // a perspective camera has no pixel grid: the rows say why they do nothing
    bool perspective = false;
    for (Entity entity : entities){
        const CameraComponent* camera = scene->findComponent<CameraComponent>(entity);
        const CameraShakerComponent* shaker = scene->findComponent<CameraShakerComponent>(entity);
        const bool flat = (shaker && shaker->mode == CameraShakeMode::Mode2D) ||
                          (camera && (camera->type == CameraType::CAMERA_ORTHO || camera->type == CameraType::CAMERA_UI));
        if (camera && !flat) perspective = true;
        if (!camera && shaker && shaker->mode == CameraShakeMode::Mode3D) perspective = true;
    }
    RowSettings snapSettings;
    snapSettings.help = "Rounds the shake to whole art pixels (the Pixel Perfect Camera's grid, else screen pixels) and drops the roll.";
    RowSettings tickSettings;
    tickSettings.help = "The shake moves only on ticks of 1 / Tick Rate seconds (0: every frame), counted on the shake's clock.";
    tickSettings.secondColSize = 6 * ImGui::GetFontSize();
    if (perspective){
        const char* reason = "A perspective (3D) camera has no pixel grid: Pixel Snap and Tick Rate act on 2D shakers only.";
        snapSettings.help = reason;
        tickSettings.help = reason;
        ImGui::BeginDisabled();
    }
    propertyRow(RowPropertyType::Bool, cpType, "pixelSnap", "Pixel Snap", sceneProject, entities, snapSettings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "tickRate", "Tick Rate", sceneProject, entities, tickSettings);
    if (perspective) ImGui::EndDisabled();
}
