// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RagdollWizardDialog.h"
#include "component/Transform.h"

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "util/RagdollWizard.h"
#include "component/AnimationPlayerComponent.h"
#include "imgui.h"

using namespace jasperengine;

namespace {
    // Unity's labels, in the wizard's order
    const std::vector<std::pair<const char*, const char*>>& slotLabels(){
        static const std::vector<std::pair<const char*, const char*>> labels = {
            {"pelvis", "Pelvis"}, {"left_hips", "Left Hips"}, {"left_knee", "Left Knee"}, {"left_foot", "Left Foot"},
            {"right_hips", "Right Hips"}, {"right_knee", "Right Knee"}, {"right_foot", "Right Foot"},
            {"left_arm", "Left Arm"}, {"left_elbow", "Left Elbow"}, {"right_arm", "Right Arm"}, {"right_elbow", "Right Elbow"},
            {"middle_spine", "Middle Spine"}, {"head", "Head"}};
        return labels;
    }
}

editor::RagdollWizardDialog& editor::RagdollWizardDialog::instance(){
    static RagdollWizardDialog dialog;
    return dialog;
}

void editor::RagdollWizardDialog::open(Project* project, uint32_t sceneId, Entity root){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(root)){
        return;
    }
    Scene* scene = sceneProject->scene;
    this->project = project;
    this->sceneId = sceneId;
    this->root = root;
    notes.clear();
    error.clear();
    bones = RagdollBuilder::guessBones(scene, root, &notes);
    settings = RagdollBuildSettings();
    // Start As: animated when the character has an Animation Player, else simulated
    bool hasAnimator = false;
    for (Entity e = root; e != NULL_ENTITY && !hasAnimator;){
        hasAnimator = scene->findComponent<AnimationPlayerComponent>(e) != nullptr;
        Transform* t = scene->findComponent<Transform>(e);
        e = t ? t->parent : NULL_ENTITY;
    }
    candidates.clear();
    auto transforms = scene->getComponentArray<Transform>();
    if (transforms->hasEntity(root)){
        const size_t first = transforms->getIndex(root);
        const size_t last = scene->findBranchLastIndex(root);
        for (size_t i = first; i <= last && i < transforms->size(); i++){
            const Entity e = transforms->getEntity(i);
            candidates.push_back(e);
            if (!hasAnimator && scene->findComponent<AnimationPlayerComponent>(e)) hasAnimator = true;
        }
    }
    settings.startSimulated = !hasAnimator;
    isOpen = true;
    justOpened = true;
}

void editor::RagdollWizardDialog::show(){
    if (!isOpen || !project){
        return;
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(root)){
        isOpen = false;
        return;
    }
    Scene* scene = sceneProject->scene;

    const char* popupName = "Create Ragdoll##RagdollWizardModal";
    if (justOpened){
        ImGui::OpenPopup(popupName);
        justOpened = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(popupName, &isOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)){
        return;
    }

    ImGui::Text("Ragdoll of \"%s\"", scene->getEntityName(root).c_str());
    ImGui::TextDisabled("A Rigidbody, collider and joint on each bone, and a Ragdoll on this GameObject");
    ImGui::Separator();

    auto nameOf = [scene](Entity e){
        return e == NULL_ENTITY ? std::string("None") : scene->getEntityName(e);
    };
    if (ImGui::BeginTable("##ragdoll_bones", 2, ImGuiTableFlags_SizingFixedFit)){
        for (const auto& [slot, label] : slotLabels()){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            Entity& bone = bones.slot(slot);
            ImGui::SetNextItemWidth(16.0f * ImGui::GetFontSize());
            if (ImGui::BeginCombo((std::string("##bone_") + slot).c_str(), nameOf(bone).c_str())){
                if (ImGui::Selectable("None", bone == NULL_ENTITY)) bone = NULL_ENTITY;
                for (Entity candidate : candidates){
                    if (ImGui::Selectable((nameOf(candidate) + "##" + std::to_string(candidate)).c_str(), bone == candidate)){
                        bone = candidate;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(8.0f * ImGui::GetFontSize());
    ImGui::DragFloat("Total Mass (kg)", &settings.totalMass, 0.1f, 0.01f, 100000.0f, "%.2f");
    ImGui::Checkbox("Flip Forward", &settings.flipForward);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The character faces the other way (its knees bend backwards with the default)");
    int start = settings.startSimulated ? 1 : 0;
    ImGui::SetNextItemWidth(8.0f * ImGui::GetFontSize());
    if (ImGui::Combo("Start As", &start, "Animated\0Simulated\0")) settings.startSimulated = start == 1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Animated: kinematic parts follow the animated bones until ragdoll.simulated = true");
    ImGui::Checkbox("Self Collision", &settings.selfCollision);

    for (const std::string& note : notes){
        ImGui::TextDisabled("%s", note.c_str());
    }
    for (const char* optional : {"middle_spine", "head", "left_arm", "right_arm", "left_elbow", "right_elbow"}){
        if (bones.slot(optional) == NULL_ENTITY){
            ImGui::TextDisabled("%s: none given, left out", optional);
        }
    }
    if (!error.empty()){
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", error.c_str());
    }

    if (ImGui::Button("Create", ImVec2(8.0f * ImGui::GetFontSize(), 0))){
        RagdollBuildResult result;
        if (applyRagdollWizard(project, sceneId, root, bones, settings, result, error)){
            isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(8.0f * ImGui::GetFontSize(), 0))){
        isOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
