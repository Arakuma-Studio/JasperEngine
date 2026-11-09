// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelEditWindow.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "component/TerrainComponent.h"
#include "component/VoxelVolumeComponent.h"
#include "external/IconsFontAwesome6.h"
#include "shortcuts/CommandCatalog.h"
#include "subsystem/VoxelSystem.h"
#include "util/FileDialogs.h"
#include "util/Util.h"
#include "voxel/VoxelBrushTool.h"
#include "voxel/VoxelMaterials.h"
#include "voxel/VoxelOverlay.h"
#include "voxel/VoxelProblems.h"
#include "voxel/VoxelSceneOverlay.h"
#include "voxel/VoxelSliceRender.h"
#include "voxel/VoxelVolumeOps.h"
#include "window/ProjectWindow.h"
#include "widget/ColorField.h"
#include "widget/ColorPickerWindow.h"
#include "window/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const ImVec4 kVoxelErrorStrip(0.55f, 0.10f, 0.10f, 1.0f);

    std::string panelMegabytes(int64_t bytes) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
    }

    bool voxelToolButton(const char* icon, const char* id, const char* tooltip, bool selected, const ImVec2& size) {
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? Theme::Colors::ButtonActivated : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        const bool clicked = Widgets::iconButton(id, icon, size);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tooltip);
        return clicked;
    }

    // label on the left, field on the right, as the Terrain window's rows
    void voxelPanelRow(const char* label, const char* tooltip = nullptr) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
    }

}

void editor::drawVoxelProblemStrip(Project& project, const SceneProject& sceneProject, Entity owner) {
    const std::vector<VoxelProblem> problems = voxelProblems(project, sceneProject, owner);
    logVoxelProblems(sceneProject, owner, problems);
    if (problems.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kVoxelErrorStrip);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::BeginChild("voxel_problems", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    for (const VoxelProblem& problem : problems) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION "  %s", problem.what.c_str());
        ImGui::TextUnformatted(problem.action.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

VoxelEditWindow::VoxelEditWindow(Project* project): project(project) {
}

void VoxelEditWindow::open() {
    setOpen(true);
    focusRequested = true;
}

void VoxelEditWindow::setOpen(bool value) {
    if (value == windowOpen) return;
    windowOpen = value;
    if (!windowOpen) {
        focusRequested = false;
        VoxelBrushTool::get().disarm();
    }
}

void VoxelEditWindow::openForEntity(Entity entity, uint32_t scene) {
    open();
    sceneId = scene;
    target = entity;
    VoxelBrushTool::get().setTarget(project, scene, entity);
}

void VoxelEditWindow::updateTarget() {
    SceneProject* selected = project->getSelectedScene();
    if (!selected || !selected->scene) return;
    const std::vector<Entity> entities = project->getSelectedEntities(selected->id);
    if (entities.size() == 1 && selected->scene->findComponent<VoxelVolumeComponent>(entities[0])) {
        sceneId = selected->id;
        target = entities[0];
    }
    SceneProject* current = project->getScene(sceneId);
    if (!current || !current->scene || !current->scene->isEntityCreated(target) || !current->scene->findComponent<VoxelVolumeComponent>(target)) {
        target = NULL_ENTITY;
    }
    VoxelBrushTool::get().setTarget(project, sceneId, target);
}

void VoxelEditWindow::setStatus(const std::string& text, bool isError) {
    status = text;
    statusIsError = isError;
}

void VoxelEditWindow::startRegenerate(uint32_t scene, Entity owner) {
    SceneProject* sceneProject = project->getScene(scene);
    std::shared_ptr<VoxelSystem> system = sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<VoxelSystem>() : nullptr;
    const std::shared_ptr<VoxelAsset> asset = system ? system->getAsset(owner) : nullptr;
    if (!asset) {
        setStatus("The volume is not loaded.", true);
        return;
    }
    // the base it already describes, again: the source may have changed (V10)
    const VoxelVolumeComponent& component = sceneProject->scene->getComponent<VoxelVolumeComponent>(owner);
    VoxelGenerateRequest request;
    request.seed = asset->generator.seed;
    if (asset->generator.kind == "noise_graph") {
        request.source = "noise_graph";
        request.graphPath = asset->generator.sourcePath;
    } else if (asset->generator.kind == "terrain") {
        request.source = "terrain";
        request.terrain = component.terrain;
        request.blendWidth = asset->generator.blendWidth;
    } else if (asset->generator.kind == "mesh") {
        // the file keeps the mesh's hash, not the GameObject: it is the panel's Mesh that is read again
        if (meshSource == NULL_ENTITY) {
            setStatus("Choose the GameObject with the mesh in the Brush tab's Mesh shape first.", true);
            return;
        }
        request.source = "mesh";
        request.mesh = meshSource;
    } else {
        request.source = "preset";
        request.preset = asset->generator.kind == "preset" ? asset->generator.preset : asset->generator.kind;
    }
    startGenerate(scene, owner, request, true);
}

void VoxelEditWindow::startGenerate(uint32_t scene, Entity owner, const VoxelGenerateRequest& request, bool recordUndo) {
    std::string error;
    if (generation.start(project, scene, owner, request, recordUndo, error)) setStatus("Generating...", false);
    else setStatus(error, true);
}

void VoxelEditWindow::show() {
    VoxelBrushTool& tool = VoxelBrushTool::get();
    const ImGuiIO& io = ImGui::GetIO();
    VoxelGenerateResult finished;
    if (generation.poll(finished)) {
        if (finished.ok) setStatus("Regenerated: " + std::to_string(finished.blocksNonUniform) + " non-uniform blocks, " + panelMegabytes(finished.memory) + ".", false);
        else setStatus(finished.message.empty() ? finished.error : finished.message, true);
    }
    const bool cameraMoving = ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left));
    tool.tick(VoxelBrushModifiers{io.KeyCtrl, io.KeyShift}, cameraMoving);
    VoxelSceneOverlay::frame(*project);

    if (!windowOpen) return;
    updateTarget();

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 27.0f, ImGui::GetFontSize() * 44.0f), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    bool open = windowOpen;
    if (!ImGui::Begin(WINDOW_NAME, &open)) {
        ImGui::End();
        if (!open) setOpen(false);
        return;
    }

    SceneProject* sceneProject = project->getScene(sceneId);
    if (target == NULL_ENTITY || !sceneProject || !sceneProject->scene) {
        tool.disarm();
        ImGui::TextWrapped("%s", project->isAnyScenePlaying() ? "Stop the scene to edit voxels." : "Select a Voxel Volume in the Hierarchy, or make one with GameObject > 3D Object > Voxel Volume.");
    } else {
        ImGui::TextWrapped(ICON_FA_CUBES "  %s", sceneProject->scene->getEntityName(target).c_str());
        drawVoxelProblemStrip(*project, *sceneProject, target);
        if (!tool.lastError().empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", tool.lastError().c_str());
            ImGui::PopTextWrapPos();
        }
        if (generation.isRunning()) {
            ImGui::ProgressBar(generation.progress(), ImVec2(-ImGui::GetFrameHeight() * 3.0f, 0.0f));
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) generation.cancel();
        }
        if (ImGui::BeginTabBar("voxel_tabs")) {
            if (ImGui::BeginTabItem("Brush")) {
                drawBrushTab(*sceneProject);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Materials")) {
                drawMaterialsTab(*sceneProject);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Overlays & Stats")) {
                drawOverlaysTab(*sceneProject);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Volume")) {
                drawVolumeTab(*sceneProject);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!status.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(0.0f);
            if (statusIsError) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", status.c_str());
            else ImGui::TextUnformatted(status.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::End();
    if (!open) setOpen(false);
}

void VoxelEditWindow::drawBrushTab(SceneProject& sceneProject) {
    VoxelBrushTool& tool = VoxelBrushTool::get();
    VoxelBrushSettings& brush = tool.settings();
    const CommandCatalog& catalog = CommandCatalog::editor();
    const float size = std::round(ImGui::GetFrameHeight() * 1.6f);
    const ImVec2 buttonSize(size, size);

    struct ToolRow {
        VoxelTool tool;
        const char* icon;
        const char* id;
        const char* name;
        const char* command;
    };
    static const ToolRow tools[] = {
        {VoxelTool::Dig, ICON_FA_TROWEL, "##voxel_dig", "Dig", "voxel.tool.dig"},
        {VoxelTool::Add, ICON_FA_CUBES_STACKED, "##voxel_add", "Add", "voxel.tool.add"},
        {VoxelTool::Smooth, ICON_FA_WATER, "##voxel_smooth", "Smooth", "voxel.tool.smooth"},
        {VoxelTool::Paint, ICON_FA_PAINTBRUSH, "##voxel_paint", "Paint", "voxel.tool.paint"},
        {VoxelTool::Pick, ICON_FA_EYE_DROPPER, "##voxel_pick", "Pick", "voxel.tool.pick"},
    };
    for (int i = 0; i < IM_ARRAYSIZE(tools); i++) {
        if (i > 0) ImGui::SameLine();
        const bool selected = tool.isArmed() && brush.tool == tools[i].tool;
        const std::string tooltip = catalog.tooltip(tools[i].command, tools[i].name);
        if (voxelToolButton(tools[i].icon, tools[i].id, tooltip.c_str(), selected, buttonSize)) {
            if (selected) tool.disarm();
            else tool.arm(tools[i].tool);
        }
    }
    ImGui::Text("%s%s", voxelToolName(brush.tool), tool.isArmed() ? "" : " (not armed)");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", voxelBrushLegend(catalog).c_str());
    ImGui::PopTextWrapPos();

    if (!ImGui::BeginTable("voxel_brush", 2, ImGuiTableFlags_SizingStretchProp)) return;
    voxelPanelRow("Shape");
    int shape = static_cast<int>(brush.shape);
    const char* shapes[] = {"Sphere", "Box", "Capsule (drag)", "Mesh", "Stamp"};
    if (ImGui::Combo("##shape", &shape, shapes, IM_ARRAYSIZE(shapes))) brush.shape = static_cast<VoxelBrushShape>(shape);
    voxelPanelRow("Size", "Metres from the centre to the shape's edge ([ and ])");
    if (ImGui::DragFloat("##size", &brush.radius, 0.05f, 0.1f, 256.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp)) tool.adjustSize(1.0f);
    voxelPanelRow("Blend", "Smooth-CSG radius in metres: rounds the join with what is there");
    ImGui::DragFloat("##blend", &brush.blend, 0.02f, 0.0f, 32.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
    if (brush.tool == VoxelTool::Smooth) {
        voxelPanelRow("Amount");
        ImGui::SliderFloat("##amount", &brush.amount, 0.0f, 1.0f, "%.2f");
        voxelPanelRow("Iterations");
        ImGui::SliderInt("##iterations", &brush.iterations, 1, 8);
    }
    if (brush.tool == VoxelTool::Smooth || brush.tool == VoxelTool::Paint) {
        voxelPanelRow("Falloff", "How soft the shape's edge is");
        ImGui::SliderFloat("##falloff", &brush.falloff, 0.0f, 1.0f, "%.2f");
    }
    if (brush.tool == VoxelTool::Paint) {
        voxelPanelRow("Weight");
        ImGui::SliderFloat("##weight", &brush.weight, 0.0f, 1.0f, "%.2f");
    }
    if (brush.tool == VoxelTool::Paint || brush.tool == VoxelTool::Add || brush.tool == VoxelTool::Pick) {
        voxelPanelRow("Material", "The slot Add and Paint write; Alt+Click picks it from the surface");
        const VoxelVolumeComponent& component = sceneProject.scene->getComponent<VoxelVolumeComponent>(target);
        const std::string current = brush.material < static_cast<int>(component.materialSlots.size())
                                        ? std::to_string(brush.material) + ": " + component.materialSlots[static_cast<size_t>(brush.material)].name
                                        : std::to_string(brush.material);
        if (ImGui::BeginCombo("##material", current.c_str())) {
            for (int i = 0; i < static_cast<int>(component.materialSlots.size()); i++) {
                const VoxelMaterialSlot& slot = component.materialSlots[static_cast<size_t>(i)];
                if (isVoxelSlotEmpty(slot)) continue;
                const std::string name = std::to_string(i) + ": " + slot.name;
                if (ImGui::Selectable(name.c_str(), i == brush.material)) brush.material = i;
            }
            ImGui::EndCombo();
        }
    }
    if (brush.shape == VoxelBrushShape::Stamp) {
        voxelPanelRow("Stamp", "A .voxelvolume whose surface is the shape: drop one here, or browse");
        char name[256];
        std::snprintf(name, sizeof(name), "%s", stampPath.c_str());
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##stamp", "None", name, sizeof(name), ImGuiInputTextFlags_ReadOnly);
        std::string chosen;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
                if (!dropped.empty()) chosen = dropped[0];
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##stamp_browse")) chosen = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_ALL);
        if (!chosen.empty()) {
            if (!project->isInsideAssetsPath(chosen)) {
                setStatus("A stamp must be inside the project's assets.", true);
            } else {
                stampPath = project->normalizeToAssetsRelative(chosen).generic_string();
                brush.field = voxelStampFieldFromAsset(stampPath);
                if (!brush.field) setStatus("'" + stampPath + "' did not load as a .voxelvolume.", true);
            }
        }
    }
    if (brush.shape == VoxelBrushShape::Mesh) {
        voxelPanelRow("Mesh", "The mesh of a GameObject, voxelized once");
        const std::string label = meshSource == NULL_ENTITY ? std::string("None") : sceneProject.scene->getEntityName(meshSource);
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Use Selected")) {
            const std::vector<Entity> selected = project->getSelectedEntities(sceneProject.id);
            std::shared_ptr<VoxelSystem> system = sceneProject.scene->findSystem<VoxelSystem>();
            voxel::Store* store = system ? system->getStore(target) : nullptr;
            if (selected.size() == 1 && store) {
                brush.field = voxelMeshField(sceneProject.scene, selected[0], store->desc().voxelSize);
                meshSource = brush.field ? selected[0] : NULL_ENTITY;
                if (!brush.field) setStatus("The selection has no mesh.", true);
            } else {
                setStatus("Select the GameObject with the mesh first.", true);
            }
        }
    }
    if (brush.shape == VoxelBrushShape::Mesh || brush.shape == VoxelBrushShape::Stamp) {
        voxelPanelRow("Scale");
        ImGui::DragFloat("##stamp_scale", &brush.stampScale, 0.01f, 0.01f, 64.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::EndTable();

    ImGui::SeparatorText("Presets");
    bool first = true;
    for (const VoxelBrushPreset& preset : voxelBrushPresets()) {
        if (!first) ImGui::SameLine();
        first = false;
        if (ImGui::SmallButton(preset.name)) {
            applyVoxelBrushPreset(brush, preset);
            tool.arm(preset.tool);
        }
        ImGui::SetItemTooltip("%s", preset.description);
    }

    ImGui::SeparatorText("Info");
    drawInfoStrip();
}

void VoxelEditWindow::drawInfoStrip() {
    const VoxelCursorInfo& info = VoxelBrushTool::get().cursorInfo();
    if (!info.valid) {
        ImGui::TextDisabled("Point at the volume in the Scene view.");
        return;
    }
    ImGui::Text("Position  %.2f, %.2f, %.2f", info.position.x, info.position.y, info.position.z);
    ImGui::Text("Value     %.3f m", info.value);
    ImGui::Text("Material  %d%s", info.material.slot, info.material.weight > 0 ? (" + " + std::to_string(info.material.second) + " (" + std::to_string(info.material.weight * 100 / 255) + "%)").c_str() : "");
    ImGui::Text("Stamp     %s%lld voxels", info.stampEstimated ? "up to " : "", static_cast<long long>(info.stampVoxels));
    ImGui::Text("Memory    %s", panelMegabytes(info.memoryBytes).c_str());
}

void VoxelEditWindow::drawSlotRow(SceneProject& sceneProject, int index) {
    const VoxelVolumeComponent& component = sceneProject.scene->getComponent<VoxelVolumeComponent>(target);
    const VoxelMaterialSlot& slot = component.materialSlots[static_cast<size_t>(index)];
    ImGui::PushID(index);

    const auto edit = [&](const std::function<void(VoxelMaterialSlot&)>& change, bool merge) {
        VoxelMaterialSlot next = slot;
        change(next);
        std::string error;
        if (!setVoxelMaterialSlot(project, sceneProject.id, target, index, next, merge, &error)) setStatus(error, true);
    };

    // the thumbnail: the layer's colour map, else the colour the Slice view tells this slot by
    const float thumb = ImGui::GetFrameHeight() * 2.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const VoxelSlotColor color = voxelSlotColor(index);
    draw->AddRectFilled(p, ImVec2(p.x + thumb, p.y + thumb), IM_COL32(color.r, color.g, color.b, 255), ImGui::GetStyle().FrameRounding);
    const std::string mapPath = slot.layer.colorTexture.getPath(0);
    if (!mapPath.empty()) {
        int width = 0, height = 0;
        const ImTextureID texture = Backend::getApp().getProjectWindow()->getAssetThumbnail(project->getAssetsPath() / mapPath, width, height);
        if (texture && width > 0 && height > 0) Widgets::addImageRounded(draw, texture, p, ImVec2(p.x + thumb, p.y + thumb), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersAll);
    }
    ImGui::InvisibleButton("thumb", ImVec2(thumb, thumb));
    ImGui::SameLine();

    ImGui::BeginGroup();
    char name[128];
    std::snprintf(name, sizeof(name), "%s", slot.name.c_str());
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 1.5f);
    const std::string hint = "Slot " + std::to_string(index);
    if (ImGui::InputTextWithHint("##name", hint.c_str(), name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) || (ImGui::IsItemDeactivatedAfterEdit() && slot.name != name)) {
        const std::string value = name;
        edit([&](VoxelMaterialSlot& next) { next.name = value; }, false);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH "##remove")) {
        std::string error;
        if (removeVoxelMaterialSlot(project, sceneProject.id, target, index, &error)) {
            setStatus("Slot " + std::to_string(index) + " removed.", false);
        } else {
            setStatus(error, true);
            removingSlot = index;
            replaceWith = index == 0 ? 1 : 0;
        }
        ImGui::EndGroup();
        ImGui::PopID();
        return;
    }
    ImGui::SetItemTooltip("Remove this slot (refused while voxels use it)");

    // R43: the Terrain Layer it follows live
    const TerrainComponent* terrain = component.terrain != NULL_ENTITY ? sceneProject.scene->findComponent<TerrainComponent>(component.terrain) : nullptr;
    std::string linked = slot.linkedTerrainLayer < 0 ? std::string("Own material") : "Terrain Layer " + std::to_string(slot.linkedTerrainLayer);
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(!terrain);
    if (ImGui::BeginCombo("##link", linked.c_str())) {
        if (ImGui::Selectable("Own material", slot.linkedTerrainLayer < 0)) {
            std::string error;
            if (!linkVoxelMaterialSlot(project, sceneProject.id, target, index, -1, &error)) setStatus(error, true);
        }
        for (size_t layer = 0; terrain && layer < terrain->surfaceLayers.size(); layer++) {
            const std::string label = "Terrain Layer " + std::to_string(layer);
            if (ImGui::Selectable(label.c_str(), slot.linkedTerrainLayer == static_cast<int>(layer))) {
                std::string error;
                if (!linkVoxelMaterialSlot(project, sceneProject.id, target, index, static_cast<int>(layer), &error)) setStatus(error, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();

    if (removingSlot == index) {
        ImGui::TextWrapped("Voxels use this slot. Replace it with:");
        const std::string current = std::to_string(replaceWith);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::BeginCombo("##replace_with", current.c_str())) {
            for (int other = 0; other < static_cast<int>(component.materialSlots.size()); other++) {
                if (other == index || isVoxelSlotEmpty(component.materialSlots[static_cast<size_t>(other)])) continue;
                const std::string label = std::to_string(other) + ": " + component.materialSlots[static_cast<size_t>(other)].name;
                if (ImGui::Selectable(label.c_str(), other == replaceWith)) replaceWith = other;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const bool canReplace = replaceWith != index && replaceWith < static_cast<int>(component.materialSlots.size());
        ImGui::BeginDisabled(!canReplace);
        const bool replaceClicked = ImGui::Button("Replace With");
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancelClicked = ImGui::Button("Cancel");
        if (replaceClicked) {
            std::string error;
            if (replaceVoxelMaterialSlot(project, sceneProject.id, target, index, replaceWith, &error)) setStatus("Slot " + std::to_string(index) + " replaced and removed.", false);
            else setStatus(error, true);
            removingSlot = -1;
            ImGui::PopID();
            return;
        }
        if (cancelClicked) removingSlot = -1;
    }

    // the same fields, in the same order, as a Terrain Layer's (R62)
    if (slot.linkedTerrainLayer < 0 && ImGui::TreeNode("Layer")) {
        const TerrainSurfaceLayer& layer = slot.layer;
        if (ImGui::BeginTable("voxel_slot_layer", 2, ImGuiTableFlags_SizingStretchProp)) {
            bool pbr = layer.pbr;
            voxelPanelRow("Enable PBR", "Off blends only this slot's colour; on reads every map below");
            if (ImGui::Checkbox("##pbr", &pbr)) edit([&](VoxelMaterialSlot& next) { next.layer.pbr = pbr; }, false);

            const auto mapRow = [&](const char* label, Texture TerrainSurfaceLayer::*map) {
                voxelPanelRow(label);
                ImGui::PushID(label);
                const std::string path = (layer.*map).getPath(0);
                char file[256];
                std::snprintf(file, sizeof(file), "%s", std::filesystem::path(path).filename().string().c_str());
                ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) * 2.0f);
                ImGui::InputTextWithHint("##map", "None", file, sizeof(file), ImGuiInputTextFlags_ReadOnly);
                std::string chosen;
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                        const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
                        if (!dropped.empty() && Util::isImageFile(dropped[0])) chosen = dropped[0];
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FOLDER_OPEN "##browse")) chosen = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
                ImGui::SameLine();
                ImGui::BeginDisabled(path.empty());
                if (ImGui::Button(ICON_FA_XMARK "##clear")) edit([&](VoxelMaterialSlot& next) { next.layer.*map = Texture(); }, false);
                ImGui::EndDisabled();
                if (!chosen.empty()) {
                    if (!project->isInsideAssetsPath(chosen)) {
                        setStatus("A map must be inside the project's assets.", true);
                    } else {
                        const std::string relative = project->normalizeToAssetsRelative(chosen).generic_string();
                        edit([&](VoxelMaterialSlot& next) { next.layer.*map = Texture(relative); }, false);
                    }
                }
                ImGui::PopID();
            };
            const auto factorRow = [&](const char* label, float TerrainSurfaceLayer::*factor, float maxValue) {
                voxelPanelRow(label);
                ImGui::PushID(label);
                float value = layer.*factor;
                if (ImGui::SliderFloat("##factor", &value, 0.0f, maxValue, "%.2f", ImGuiSliderFlags_AlwaysClamp)) edit([&](VoxelMaterialSlot& next) { next.layer.*factor = value; }, true);
                ImGui::PopID();
            };

            mapRow("Color", &TerrainSurfaceLayer::colorTexture);
            if (layer.pbr) {
                voxelPanelRow("Tint", "Multiplies the slot's colour");
                Vector4 tint = layer.colorFactor;
                ui::ColorFieldOptions tintOptions;
                tintOptions.alpha = false;
                tintOptions.label = "Tint";
                const ui::EditPhase tintPhase = ui::colorField("##tint", tint, tintOptions);
                if (tintPhase == ui::EditPhase::Begin) ui::ColorPickerWindow::beginEdit(CommandHandle::get(sceneProject.id), "Tint");
                else if (tintPhase == ui::EditPhase::Changing) edit([&](VoxelMaterialSlot& next) { next.layer.colorFactor = tint; }, true);
                mapRow("Normal", &TerrainSurfaceLayer::normalTexture);
                factorRow("Normal Strength", &TerrainSurfaceLayer::normalStrength, 2.0f);
                mapRow("Roughness", &TerrainSurfaceLayer::roughnessTexture);
                factorRow("Roughness Factor", &TerrainSurfaceLayer::roughnessFactor, 1.0f);
                mapRow("Metallic", &TerrainSurfaceLayer::metallicTexture);
                factorRow("Metallic Factor", &TerrainSurfaceLayer::metallicFactor, 1.0f);
                mapRow("Occlusion", &TerrainSurfaceLayer::occlusionTexture);
                factorRow("Occlusion Strength", &TerrainSurfaceLayer::occlusionStrength, 1.0f);
                mapRow("Height", &TerrainSurfaceLayer::heightTexture);
            }
            voxelPanelRow("UV Scale", "Multiplies the shared detail tiling for this slot alone");
            Vector2 uvScale = layer.uvScale;
            if (ImGui::DragFloat2("##uv_scale", &uvScale.x, 0.01f, 0.0f, 0.0f, "%.2f")) edit([&](VoxelMaterialSlot& next) { next.layer.uvScale = uvScale; }, true);
            voxelPanelRow("UV Offset", "Shifts this slot inside its tile");
            Vector2 uvOffset = layer.uvOffset;
            if (ImGui::DragFloat2("##uv_offset", &uvOffset.x, 0.01f, 0.0f, 0.0f, "%.2f")) edit([&](VoxelMaterialSlot& next) { next.layer.uvOffset = uvOffset; }, true);
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    ImGui::Separator();
    ImGui::PopID();
}

void VoxelEditWindow::drawMaterialsTab(SceneProject& sceneProject) {
    const VoxelVolumeComponent& component = sceneProject.scene->getComponent<VoxelVolumeComponent>(target);
    if (component.materialSlots.size() < static_cast<size_t>(voxel::MAX_SLOTS)) {
        if (ImGui::Button(ICON_FA_PLUS " Add")) {
            std::string error;
            const int index = addVoxelMaterialSlot(project, sceneProject.id, target, "Slot " + std::to_string(component.materialSlots.size()), &error);
            if (index < 0) setStatus(error, true);
        }
    }
    ImGui::Separator();
    for (int i = 0; i < static_cast<int>(component.materialSlots.size()); i++) {
        if (isVoxelSlotEmpty(component.materialSlots[static_cast<size_t>(i)])) {
            ImGui::TextDisabled("%d: (empty)", i);
            continue;
        }
        drawSlotRow(sceneProject, i);
        // a removal or a replace changed the list under the loop: the next frame shows it
        if (i >= static_cast<int>(component.materialSlots.size())) break;
    }
}

void VoxelEditWindow::drawOverlaysTab(SceneProject& sceneProject) {
    ImGui::SeparatorText("Overlay");
    VoxelOverlay current = getVoxelOverlay(target);
    int value = static_cast<int>(current);
    for (int i = 0; i < VOXEL_OVERLAY_COUNT; i++) {
        if (i % 3 != 0) ImGui::SameLine();
        if (ImGui::RadioButton(voxelOverlayLabel(static_cast<VoxelOverlay>(i)), value == i)) setVoxelOverlay(target, static_cast<VoxelOverlay>(i));
    }
    if (current != VoxelOverlay::None) {
        const size_t lines = VoxelSceneOverlay::overlayLineCount(sceneProject.scene);
        const size_t triangles = VoxelSceneOverlay::overlayTriangleCount(sceneProject.scene);
        ImGui::TextDisabled("%zu lines, %zu tinted triangles", lines, triangles);
    }

    ImGui::SeparatorText("Stats");
    std::shared_ptr<VoxelSystem> system = sceneProject.scene->findSystem<VoxelSystem>();
    if (!system) return;
    const std::vector<VoxelStatsRow> rows = voxelStatsRows(system->getStats(target), project->getVoxelSettings());
    if (ImGui::BeginTable("voxel_stats", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        for (const VoxelStatsRow& statsRow : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(statsRow.label.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(statsRow.value.c_str());
        }
        ImGui::EndTable();
    }
}

void VoxelEditWindow::drawVolumeTab(SceneProject& sceneProject) {
    std::shared_ptr<VoxelSystem> system = sceneProject.scene->findSystem<VoxelSystem>();
    const std::shared_ptr<VoxelAsset> asset = system ? system->getAsset(target) : nullptr;
    if (asset && asset->store) {
        const voxel::VolumeDesc& desc = asset->store->desc();
        ImGui::Text("File   %s%s", asset->path.c_str(), asset->dirty ? " *" : "");
        ImGui::Text("Size   %d x %d x %d voxels at %.3f m", desc.dimensions.x, desc.dimensions.y, desc.dimensions.z, desc.voxelSize);
        ImGui::Text("Base   %s", asset->generator.kind.c_str());
    }
    ImGui::BeginDisabled(!asset || !asset->store || !asset->dirty);
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save")) {
        std::string error;
        if (saveVoxelVolume(project, sceneProject.id, target, &error)) setStatus("Saved.", false);
        else setStatus(error, true);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Write the volume's file now. Saving the scene writes it too.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!asset || !asset->store || generation.isRunning());
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Regenerate")) startRegenerate(sceneProject.id, target);
    ImGui::SetItemTooltip("Build the base again from its source. Modified blocks stay.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool terrainBase = asset && (asset->generator.kind == "terrain" || asset->generator.otherKind == "terrain");
    ImGui::BeginDisabled(!terrainBase);
    if (ImGui::Button(ICON_FA_MOUNTAIN_SUN " Re-seed From Terrain")) {
        std::string error;
        voxel::IntBox region;
        region.min = voxel::Int3{reseedMin[0], reseedMin[1], reseedMin[2]};
        region.max = voxel::Int3{reseedMax[0], reseedMax[1], reseedMax[2]};
        const int blocks = reseedVoxelVolumeFromTerrain(project, sceneProject.id, target, reseedWhole ? nullptr : &region, &error);
        if (blocks < 0) setStatus(error, true);
        else setStatus("Re-seeded " + std::to_string(blocks) + " blocks.", false);
    }
    ImGui::SetItemTooltip("Discard modified blocks and take the terrain again. One undo step.");
    ImGui::Checkbox("Whole Volume", &reseedWhole);
    if (!reseedWhole) {
        ImGui::DragInt3("Region Min", reseedMin, 1.0f, 0, 4096);
        ImGui::DragInt3("Region Max", reseedMax, 1.0f, 0, 4096);
        ImGui::SetItemTooltip("Voxels. It widens to whole blocks of the coarsest level.");
    }
    ImGui::EndDisabled();
}
