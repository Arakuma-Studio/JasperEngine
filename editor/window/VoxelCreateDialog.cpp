// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelCreateDialog.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "object/Camera.h"
#include "render/SceneRender.h"
#include "util/FileDialogs.h"
#include "voxel/VoxelProjectSettings.h"
#include "window/VoxelEditWindow.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr const char* VOXEL_CREATE_TITLE = "Create Voxel Volume";

    std::string dialogMegabytes(int64_t bytes) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
    }

    // "in view": the camera's pivot over the terrain's square, which is what a framed terrain has
    Entity voxelTerrainInView(SceneProject& sceneProject) {
        Scene* scene = sceneProject.scene;
        auto array = scene->getComponentArray<TerrainComponent>();
        if (array->size() == 0) return NULL_ENTITY;
        Vector3 pivot(0, 0, 0);
        if (sceneProject.sceneRender && sceneProject.sceneRender->getCamera()) pivot = sceneProject.sceneRender->getCamera()->getTarget();
        for (size_t i = 0; i < array->size(); i++) {
            const Entity entity = array->getEntity(i);
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform) continue;
            const float half = array->getComponentFromIndex(i).terrainSize * 0.5f;
            const Vector3 local = pivot - transform->worldPosition;
            if (local.x >= -half && local.x <= half && local.z >= -half && local.z <= half) return entity;
        }
        return array->getEntity(0);
    }

}

VoxelCreateDialog& VoxelCreateDialog::instance() {
    static VoxelCreateDialog dialog;
    return dialog;
}

void VoxelCreateDialog::open(Project* targetProject, uint32_t targetScene, Entity parent) {
    project = targetProject;
    sceneId = targetScene;
    options = VoxelCreateOptions{};
    options.parent = parent;
    error.clear();
    isOpen = true;
    justOpened = true;
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject && sceneProject->scene) {
        options.terrain = voxelTerrainInView(*sceneProject);
        if (sceneProject->sceneRender && sceneProject->sceneRender->getCamera()) {
            // centred on the view's pivot, where the person is looking
            const voxel::Int3 size = voxelCreateDimensions(options);
            const Vector3 pivot = sceneProject->sceneRender->getCamera()->getTarget();
            options.position = Vector3(pivot.x - static_cast<float>(size.x) * options.voxelSize * 0.5f, pivot.y - static_cast<float>(size.y) * options.voxelSize * 0.5f,
                                       pivot.z - static_cast<float>(size.z) * options.voxelSize * 0.5f);
        }
    }
}

void VoxelCreateDialog::draw() {
    if (!isOpen || !project) return;
    if (justOpened) {
        ImGui::OpenPopup(VOXEL_CREATE_TITLE);
        justOpened = false;
    }
    bool keepOpen = true;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(VOXEL_CREATE_TITLE, &keepOpen)) {
        if (!keepOpen || !ImGui::IsPopupOpen(VOXEL_CREATE_TITLE)) isOpen = false;
        return;
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        isOpen = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    char name[128];
    std::snprintf(name, sizeof(name), "%s", options.name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name))) options.name = name;

    int preset = static_cast<int>(options.preset);
    if (ImGui::BeginCombo("Size", voxelCreatePresetLabel(options.preset))) {
        for (int i = 0; i <= static_cast<int>(VoxelCreatePreset::Custom); i++) {
            if (ImGui::Selectable(voxelCreatePresetLabel(static_cast<VoxelCreatePreset>(i)), i == preset)) options.preset = static_cast<VoxelCreatePreset>(i);
        }
        ImGui::EndCombo();
    }
    if (options.preset == VoxelCreatePreset::Custom) {
        int size[3] = {options.dimensions.x, options.dimensions.y, options.dimensions.z};
        if (ImGui::DragInt3("Dimensions", size, 16.0f, 16, 1024)) options.dimensions = voxel::Int3{size[0], size[1], size[2]};
        ImGui::SetItemTooltip("Voxels per axis, rounded up to multiples of 16");
    }
    ImGui::DragFloat("Voxel Size", &options.voxelSize, 0.01f, 0.01f, 64.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);

    if (ImGui::BeginCombo("Base", voxelCreateBaseLabel(options.base))) {
        for (int i = 0; i <= static_cast<int>(VoxelCreateBase::FromMesh); i++) {
            const VoxelCreateBase candidate = static_cast<VoxelCreateBase>(i);
            if (ImGui::Selectable(voxelCreateBaseLabel(candidate), candidate == options.base)) options.base = candidate;
        }
        ImGui::EndCombo();
    }
    if (options.base == VoxelCreateBase::NoiseGraph) {
        char text[256];
        std::snprintf(text, sizeof(text), "%s", options.graphPath.c_str());
        ImGui::InputTextWithHint("Graph", "None", text, sizeof(text), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##source")) {
            const std::string chosen = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_ALL);
            if (!chosen.empty() && project->isInsideAssetsPath(chosen)) options.graphPath = project->normalizeToAssetsRelative(chosen).generic_string();
        }
    }
    if (options.base == VoxelCreateBase::FromMesh) {
        // R36 voxelizes a GameObject's mesh; a model file comes in as GameObjects first
        const std::string current = options.mesh == NULL_ENTITY ? std::string("None") : sceneProject->scene->getEntityName(options.mesh);
        if (ImGui::BeginCombo("Mesh", current.c_str())) {
            auto meshes = sceneProject->scene->getComponentArray<MeshComponent>();
            for (size_t i = 0; i < meshes->size(); i++) {
                const Entity entity = meshes->getEntity(i);
                if (std::find(sceneProject->entities.begin(), sceneProject->entities.end(), entity) == sceneProject->entities.end()) continue;
                ImGui::PushID(static_cast<int>(entity));
                if (ImGui::Selectable(sceneProject->scene->getEntityName(entity).c_str(), entity == options.mesh)) options.mesh = entity;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    int seed = static_cast<int>(options.seed);
    if (ImGui::InputInt("Seed", &seed)) options.seed = static_cast<uint64_t>(seed < 0 ? 0 : seed);

    // the terrains of the scene; what is in view comes selected
    auto terrains = sceneProject->scene->getComponentArray<TerrainComponent>();
    if (terrains->size() > 0) {
        const std::string current = options.terrain == NULL_ENTITY ? std::string("None") : sceneProject->scene->getEntityName(options.terrain);
        if (ImGui::BeginCombo("Terrain", current.c_str())) {
            if (ImGui::Selectable("None", options.terrain == NULL_ENTITY)) options.terrain = NULL_ENTITY;
            for (size_t i = 0; i < terrains->size(); i++) {
                const Entity entity = terrains->getEntity(i);
                if (ImGui::Selectable(sceneProject->scene->getEntityName(entity).c_str(), entity == options.terrain)) options.terrain = entity;
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();
    const VoxelCreateEstimate estimate = estimateVoxelCreate(options, project->getVoxelSettings());
    const voxel::Int3 size = voxelCreateDimensions(options);
    ImGui::Text("%d x %d x %d voxels, about %s", size.x, size.y, size.z, dialogMegabytes(estimate.bytes).c_str());
    if (!estimate.warning.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION "  %s", estimate.warning.c_str());
        ImGui::PopTextWrapPos();
    }
    const std::string problem = voxelCreateProblem(options);
    if (!problem.empty()) ImGui::TextDisabled("%s", problem.c_str());
    if (!error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", error.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::BeginDisabled(!problem.empty());
    if (ImGui::Button("Create", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) {
        const Entity entity = createVoxelVolume(project, sceneId, options, &error);
        if (entity != NULL_ENTITY) {
            // the slow bases run as a job the panel shows; the rest resolved already
            if (voxelCreateNeedsGeneration(options)) {
                if (VoxelEditWindow* window = Backend::getApp().getVoxelEditWindow()) {
                    window->openForEntity(entity, sceneId);
                    // part of the step that made the volume (R59): undoing that takes the GameObject away
                    window->startGenerate(sceneId, entity, voxelCreateRequest(options), false);
                }
            }
            isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) {
        isOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
