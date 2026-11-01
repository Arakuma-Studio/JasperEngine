// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Model section's Rig foldout (anim-retarget spec 9): Unity's Rig tab. Animation Type
// (Generic / Humanoid, the .humanoid sidecar's animation_type, one project undo step: the model's
// clips reload as humanoid clips), the Avatar's status chip, Configure Avatar... (the Avatar
// Configuration window) and the human scale.

#include "InspectorWindow.h"

#include "App.h"
#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "component/ModelComponent.h"
#include "external/IconsFontAwesome6.h"
#include "imgui.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseRigProvider.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"

#include <algorithm>

using namespace jasperengine;
namespace fs = std::filesystem;

void editor::InspectorWindow::drawModelRig(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    (void)cpType;
    const ModelComponent* model = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<ModelComponent>(entity) : nullptr;
    if (!model || model->filename.empty() || model->bonesIdMapping.empty()) return;
    ImGui::PushID("ModelRig");
    if (!ImGui::CollapsingHeader("Rig", ImGuiTreeNodeFlags_None)){
        ImGui::PopID();
        return;
    }
    const fs::path file = project->resolveAssetPath(FileUtils::pathFromUtf8(model->filename)).lexically_normal();
    const std::shared_ptr<const PoseRigInfo> info = PoseRigProvider::forModelFile(project, file);
    const pose::HumanoidOverrides& overrides = info->overrides;
    const bool humanoid = overrides.isHumanoid();

    // Animation Type: the sidecar as it is, its animation_type changed (one project step)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Animation Type");
    ImGui::SameLine(getLabelSize("Animation Type") + ImGui::GetStyle().ItemSpacing.x * 2.0f);
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    ImGui::BeginDisabled(sceneProject->playState != ScenePlayState::STOPPED);
    if (ImGui::BeginCombo("##AnimationType", humanoid ? "Humanoid" : "Generic")){
        int chosen = -1;
        if (ImGui::Selectable("Generic", !humanoid)) chosen = 0;
        ImGui::SetItemTooltip("The model's clips play as they are: absolute bone rotations, for characters of this rig");
        if (ImGui::Selectable("Humanoid", humanoid)) chosen = 1;
        ImGui::SetItemTooltip("The model's clips play as humanoid clips, retargeted on any humanoid character");
        ImGui::EndCombo();
        if (chosen >= 0 && (chosen == 1) != humanoid){
            // the sidecar as read, its animation_type changed (an unreadable one is refused)
            std::string error;
            if (!PoseRigProvider::setAnimationType(project, file, chosen == 1 ? pose::AnimationType::Humanoid : pose::AnimationType::Generic, error)){
                Out::error("Animation Type: %s", error.c_str());
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Unity's Rig > Animation Type, stored in %s.humanoid", fs::u8path(model->filename).filename().u8string().c_str());

    // the Avatar's status chip (the Pose window's colours)
    int slots = 0;
    for (const std::string& bone : info->humanoid.bones) slots += bone.empty() ? 0 : 1;
    if (info->rig){
        const int fingers = info->extra.mappedCount(0, pose::HUMAN_FINGER_BONE_COUNT);
        const int eyes = info->extra.mappedCount(pose::extraIndex(pose::HumanExtraBone::LeftEye), 2);
        const bool jaw = !info->extra.bones[(size_t)pose::extraIndex(pose::HumanExtraBone::Jaw)].empty();
        ImGui::TextColored(Theme::Colors::SubtleText, ICON_FA_BONE " Humanoid %d/22 · fingers %d/30 · eyes %d%s", slots, fingers, eyes, jaw ? " · jaw" : "");
        std::string tooltip;
        for (const std::string& note : info->notes) tooltip += note + "\n";
        if (!tooltip.empty()) ImGui::SetItemTooltip("%s", tooltip.c_str());
    }else{
        std::string missing;
        for (const std::string& slot : info->missing) missing += (missing.empty() ? "" : ", ") + slot;
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s",
                           missing.empty() ? "Not a humanoid" : ("Missing: " + missing).c_str());
        ImGui::SetItemTooltip("%s", info->error.c_str());
    }
    if (!info->extraError.empty()){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Hands & Head: %s", info->extraError.c_str());
    }
    if (ImGui::Button("Configure Avatar...")){
        Backend::getApp().openAvatarWindow(sceneProject->id, entity);
    }
    ImGui::SetItemTooltip("Map the humanoid bones, fingers, eyes and jaw; twist corrections; preview the neutral pose and a test clip");
    if (info->rig){
        ImGui::SameLine();
        ImGui::TextColored(Theme::Colors::SubtleText, "Human scale %.3f", info->rig->humanScale());
        ImGui::SetItemTooltip("The neutral hips height / 0.95: humanoid clips scale the hips motion and the foot goals by it");
    }
    ImGui::PopID();
}
