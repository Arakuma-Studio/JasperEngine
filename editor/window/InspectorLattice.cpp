// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector drawers of the lattice deformer: the lattice (points, presets, poses), its modifier with
// its bindings and bake, its follower and its tracks.

#include "InspectorWindow.h"
#include "component/LatticeFollowerComponent.h"
#include "reflect/Reflect.h"
#include "reflect/Enums.h"
#include "util/EditorScripts.h"
#include "util/NumericExpression.h"
#include "profiler/Profile.h"
#include "command/PlayChangeReview.h"
#include "command/type/RemovePreservedComponentCmd.h"
#include "StreamPreserved.h"
#include "DocumentFile.h"
#include "SceneWindow.h"

#include "imgui_internal.h"

#include "util/Util.h"
#include "util/UIUtils.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "window/CodeEditor.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/HtmlUiImportPanel.h"
#include "util/EntityPayload.h"
#include "util/CameraTextureLink.h"
#include "util/FluidTextureLink.h"
#include "util/FluidEditorTools.h"
#include "util/UIProbe.h"
#include "component/FluidSimulationComponent.h"
#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "lattice/LatticeEditorOps.h"
#include "window/InspectorVatBake.h"
#include "lattice/LatticeSolver.h"
#include "command/type/UnlinkMaterialCmd.h"
#include "command/type/LinkMaterialCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/EntityStateCmd.h"
#include "command/type/SceneNameCmd.h"
#include "command/type/MeshChangeCmd.h"
#include "command/type/ModelClearCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "command/type/ComponentToPrefabSharedCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/AtmospherePresetCmd.h"
#include "render/AtmospherePresets.h"
#include "render/RenderUtil.h"
#include "util/EntityClipboard.h"
#include "command/type/ForkShaderCmd.h"
#include "shader/ShaderManifest.h"
#include "command/type/ScenePropertyCmd.h"
#include "util/Color.h"
#include "util/LightPhotometry.h"
#include "render/SceneRender2D.h"
#include "window/ProjectWindow.h"
#include "window/HierarchyWindow.h"
#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include "Widgets.h"
#include "Factory.h"
#include "component/ActionComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/LineRendererComponent.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/PanelComponent.h"
#include "util/SHA1.h"
#include "util/ProjectUtils.h"
#include "util/NormalMapTilt.h"
#include "Stream.h"
#include "Out.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/AudioSystem.h"
#include "action/Action.h"
#include "pool/TexturePool.h"
#include "yaml-cpp/yaml.h"
#include "soloud.h"
#include "soloud_wav.h"

#include <map>
#include <type_traits>
#include <cstring>
#include <fstream>
#include <cctype>
#include <cfloat>
#include <algorithm>
#include <unordered_set>
#include <memory>

using namespace jasperengine;

static std::vector<editor::EnumEntry>& entriesLatticeInterpolation = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kLatticeInterpolation);

static std::vector<editor::EnumEntry>& entriesLatticeOutside = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kLatticeOutside);

static std::vector<editor::EnumEntry>& entriesLatticeJiggleAnchor = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kLatticeJiggleAnchor);

static std::vector<editor::EnumEntry>& entriesLatticeStage = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kLatticeStage);

static std::vector<editor::EnumEntry>& entriesLatticeMask = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kLatticeMaskChannel);

// display names of the presets, in LatticePreset order (the names MCP and Lua read are
// lattice::presetName's)
static const char* const latticePresetLabels[] = {"Squash", "Stretch", "Taper", "Twist", "Bend", "Bulge", "Shear", "Noise"};
static const char* const latticeAxisLabels[] = {"X", "Y", "Z"};

// a LatticeEditorOps edit that failed says why in the Console
static void reportLatticeEdit(bool done, const std::string& error){
    if (!done){
        editor::Out::error("Lattice: %s", error.c_str());
    }
}

static void drawLatticeWarning(const std::string& text){
    ImGui::PushStyleColor(ImGuiCol_Text, editor::Theme::Colors::WarningText);
    ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
    ImGui::PopStyleColor();
}

// the folded-cell scan depends on these only: a 64-bit FNV-1a of the points and settings
static uint64_t latticeFoldKey(const LatticeComponent& lattice){
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](const void* data, size_t size){
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; i++){
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        }
    };
    const int header[5] = {lattice.resolutionX, lattice.resolutionY, lattice.resolutionZ, (int)lattice.interpolation, (int)lattice.outside};
    mix(header, sizeof(header));
    mix(&lattice.fadeDistance, sizeof(lattice.fadeDistance));
    if (!lattice.offsets.empty()){
        mix(lattice.offsets.data(), lattice.offsets.size() * sizeof(Vector3));
    }
    if (!lattice.dynamicOffsets.empty()){
        mix(lattice.dynamicOffsets.data(), lattice.dynamicOffsets.size() * sizeof(Vector3));
    }
    return hash;
}

void editor::InspectorWindow::drawLatticeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const Entity entity = entities[0];
    LatticeComponent& lattice = scene->getComponent<LatticeComponent>(entity);
    const bool single = entities.size() == 1;
    const int resolution[3] = {lattice.resolutionX, lattice.resolutionY, lattice.resolutionZ};
    const int pointCount = lattice.resolutionX * lattice.resolutionY * lattice.resolutionZ;

    RowSettings settingsInterpolation;
    settingsInterpolation.enumEntries = &entriesLatticeInterpolation;
    settingsInterpolation.help = "Cubic: smoothest, the mesh follows the cage without touching its inner points. Smooth: through the points, no seams. Sharp: through the points, seams between cells.";
    RowSettings settingsOutside;
    settingsOutside.enumEntries = &entriesLatticeOutside;
    settingsOutside.help = "What happens to vertices outside the cage: Fade eases the deformation out over Fade Distance, Extend carries the border's deformation on.";
    RowSettings settingsFade;
    settingsFade.secondColSize = 6 * ImGui::GetFontSize();
    settingsFade.help = "Distance outside the cage, in lattice units (the cage is 1 across), over which the deformation fades to nothing. 0 cuts it at the border.";

    beginTable(cpType, getLabelSize("Interpolation"), "lattice_main");

    // Resolution: resampled by LatticeEditorOps::setResolution when a field is let go, so the
    // deformation keeps its shape; one undo step for the whole selection
    prefabRowProperty = "resolutionX";
    for (const char* axisProperty : {"resolutionX", "resolutionY", "resolutionZ"}){
        if (prefabRow.active && isPrefabPropertyOverridden(sceneProject, entities, cpType, axisProperty)){
            prefabRowProperty = axisProperty;
            break;
        }
    }
    propertyHeader("Resolution", -1, false, false);
    {
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float fieldWidth = std::max(1.0f, (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f);
        static const char* const axisIds[] = {"##lattice_resolution_x", "##lattice_resolution_y", "##lattice_resolution_z"};
        static const char* const axisFormats[] = {"X %d", "Y %d", "Z %d"};
        for (int axis = 0; axis < 3; axis++){
            if (axis > 0) ImGui::SameLine(0.0f, spacing);
            ImGui::SetNextItemWidth(fieldWidth);
            int value = latticeResolutionAxis == axis ? latticeResolutionPending[axis] : resolution[axis];
            if (ImGui::DragInt(axisIds[axis], &value, 0.05f, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION, axisFormats[axis], ImGuiSliderFlags_AlwaysClamp)){
                latticeResolutionPending[axis] = value;
            }
            if (ImGui::IsItemActivated()){
                latticeResolutionAxis = axis;
                latticeResolutionPending[axis] = value;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()){
                const int newValue = latticeResolutionPending[axis];
                CommandHistory* history = CommandHandle::get(sceneId);
                const size_t mark = history->mark();
                for (Entity target : entities){
                    LatticeComponent* other = scene->findComponent<LatticeComponent>(target);
                    if (!other) continue;
                    int res[3] = {other->resolutionX, other->resolutionY, other->resolutionZ};
                    if (res[axis] == newValue) continue;
                    res[axis] = newValue;
                    std::string error;
                    reportLatticeEdit(LatticeEditorOps::setResolution(project, sceneId, target, res[0], res[1], res[2], &error), error);
                }
                history->groupSince(mark);
            }
            if (ImGui::IsItemDeactivated()){
                latticeResolutionAxis = -1;
            }
        }
        ImGui::SetItemTooltip("Points per axis (2 to 16). Changing it resamples the offsets, so the shape stays.");
    }

    propertyRow(RowPropertyType::Enum, cpType, "interpolation", "Interpolation", sceneProject, entities, settingsInterpolation);
    propertyRow(RowPropertyType::Enum, cpType, "outside", "Outside", sceneProject, entities, settingsOutside);
    if (lattice.outside == LatticeOutside::FADE){
        propertyRow(RowPropertyType::Float, cpType, "fadeDistance", "Fade Distance", sceneProject, entities, settingsFade);
    }
    endTable();

    if (!single){
        ImGui::SeparatorText("Points");
        ImGui::TextDisabled("Select a single lattice to edit its points, presets and poses");
        return;
    }

    // ---- Points: the Scene view's point selection of this lattice
    const std::vector<int> selected = LatticeEditorOps::getSelectedPoints(sceneId, entity);
    ImGui::SeparatorText("Points");
    beginTable(cpType, getLabelSize("Selected"), "lattice_points");
    propertyHeader("Selected", -1, false, false);
    ImGui::Text("%zu of %d", selected.size(), pointCount);

    if (selected.size() == 1 && selected[0] >= 0 && selected[0] < pointCount){
        const int index = selected[0];
        const int i = index % lattice.resolutionX;
        const int j = (index / lattice.resolutionX) % lattice.resolutionY;
        const int k = index / (lattice.resolutionX * lattice.resolutionY);
        propertyHeader("Point", -1, false, false);
        ImGui::Text("(%d, %d, %d)  index %d", i, j, k, index);

        prefabRowProperty = "offsets[" + std::to_string(index) + "]";
        const Vector3 current = (size_t)index < lattice.offsets.size() ? lattice.offsets[index] : Vector3::ZERO;
        propertyHeader("Offset", -1, false, false);
        float value[3] = {current.x, current.y, current.z};
        if (latticeOffsetEditing){
            value[0] = latticeOffsetPending.x;
            value[1] = latticeOffsetPending.y;
            value[2] = latticeOffsetPending.z;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat3("##lattice_point_offset", value, 0.005f, 0.0f, 0.0f, "%.3f")){
            latticeOffsetPending = Vector3(value[0], value[1], value[2]);
        }
        if (ImGui::IsItemActivated()){
            latticeOffsetEditing = true;
            latticeOffsetPending = Vector3(value[0], value[1], value[2]);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && latticeOffsetPending.isValid()){
            std::string error;
            // Record keys the point when it claims the lattice (animation clips 9.R5)
            reportLatticeEdit(LatticeEditorOps::setOffsets(project, sceneId, entity, {index}, {latticeOffsetPending}, &error, true), error);
        }
        if (ImGui::IsItemDeactivated()){
            latticeOffsetEditing = false;
        }
        ImGui::SetItemTooltip("Offset from the rest position, in lattice local units (the cage is 1 across)");
    }
    endTable();

    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttonWidth = std::max(1.0f, (ImGui::GetContentRegionAvail().x - spacing) / 2.0f);
        ImGui::BeginDisabled(selected.empty());
        if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset Selected", ImVec2(buttonWidth, 0))){
            std::string error;
            reportLatticeEdit(LatticeEditorOps::resetPoints(project, sceneId, entity, selected, &error), error);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset All", ImVec2(buttonWidth, 0))){
            std::string error;
            reportLatticeEdit(LatticeEditorOps::resetPoints(project, sceneId, entity, {}, &error), error);
        }
        if (ImGui::Button(ICON_FA_OBJECT_GROUP " Select All", ImVec2(buttonWidth, 0))){
            LatticeEditorOps::setSelectedPoints(sceneId, entity, lattice::selectAll(lattice.resolutionX, lattice.resolutionY, lattice.resolutionZ));
            // the Scene view draws only when asked: show the selected points now
            sceneProject->needUpdateRender = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selected.empty());
        if (ImGui::Button(ICON_FA_XMARK " Select None", ImVec2(buttonWidth, 0))){
            LatticeEditorOps::setSelectedPoints(sceneId, entity, {});
            sceneProject->needUpdateRender = true;
        }
        ImGui::EndDisabled();
    }
    // proportional falloff of the Scene-view point drags (spec §9.2: context menu, inspector, O)
    {
        LatticeFalloff& falloff = LatticeEditorOps::falloff();
        beginTable(cpType, getLabelSize("Falloff Radius"), "lattice_falloff");
        propertyHeader("Falloff", -1, false, false);
        if (ImGui::Checkbox("##lattice_falloff_enabled", &falloff.enabled)){
            sceneProject->needUpdateRender = true;
        }
        ImGui::SetItemTooltip("Proportional falloff (O in the Scene view): dragging points also moves the points around them, less with distance");
        if (falloff.enabled){
            static const char* const curves[] = {"Smooth", "Linear", "Sharp", "Constant"};
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            int curve = (int)falloff.curve;
            if (ImGui::Combo("##lattice_falloff_curve", &curve, curves, IM_ARRAYSIZE(curves))){
                falloff.curve = (LatticeFalloffCurve)curve;
            }
            propertyHeader("Falloff Radius", -1, false, false);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##lattice_falloff_radius", &falloff.radius, 0.01f, 0.01f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)){
                sceneProject->needUpdateRender = true;
            }
            ImGui::SetItemTooltip("In lattice local units (the cage is 1 across); the mouse wheel changes it during a drag");
        }
        endTable();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped(ICON_FA_CIRCLE_INFO " Edit Points: drag the cage's points in the Scene view with the Move, Rotate and Scale tools; right-click there for more.");
    ImGui::PopStyleColor();

    // folded cells: the deformation turns the mesh inside out there
    if (latticeFoldCache.sceneId != sceneId || latticeFoldCache.entity != entity || latticeFoldCache.key != latticeFoldKey(lattice)){
        latticeFoldCache.sceneId = sceneId;
        latticeFoldCache.entity = entity;
        latticeFoldCache.key = latticeFoldKey(lattice);
        latticeFoldCache.cells = lattice::findFoldedCells(lattice);
    }
    if (!latticeFoldCache.cells.empty()){
        const std::array<int, 3>& first = latticeFoldCache.cells[0];
        drawLatticeWarning(std::to_string(latticeFoldCache.cells.size()) + (latticeFoldCache.cells.size() == 1 ? " cell is" : " cells are") +
            " folded inside out (first at cell " + std::to_string(first[0]) + ", " + std::to_string(first[1]) + ", " +
            std::to_string(first[2]) + "): move its points back past each other or lower the preset amount.");
    }

    // ---- Presets: on the selected points, all of them when none is selected
    ImGui::SeparatorText("Presets");
    beginTable(cpType, getLabelSize("Toward"), "lattice_presets");
    propertyHeader("Preset", -1, false, false);
    ImGui::SetNextItemWidth(-1);
    int presetIndex = (int)latticePreset.preset;
    if (ImGui::Combo("##lattice_preset", &presetIndex, latticePresetLabels, IM_ARRAYSIZE(latticePresetLabels))){
        latticePreset.preset = (lattice::LatticePreset)presetIndex;
    }
    propertyHeader("Axis", -1, false, false);
    ImGui::SetNextItemWidth(-1);
    int axisIndex = (int)latticePreset.axis;
    if (ImGui::Combo("##lattice_preset_axis", &axisIndex, latticeAxisLabels, IM_ARRAYSIZE(latticeAxisLabels))){
        latticePreset.axis = (lattice::LatticeAxis)axisIndex;
        // toward defaults to the next axis
        if (latticePreset.toward == latticePreset.axis){
            latticePreset.toward = (lattice::LatticeAxis)((axisIndex + 1) % 3);
        }
    }
    const bool hasToward = latticePreset.preset == lattice::LatticePreset::BEND || latticePreset.preset == lattice::LatticePreset::SHEAR;
    if (hasToward){
        propertyHeader("Toward", -1, false, false);
        ImGui::SetNextItemWidth(-1);
        int towardIndex = (int)latticePreset.toward;
        if (ImGui::Combo("##lattice_preset_toward", &towardIndex, latticeAxisLabels, IM_ARRAYSIZE(latticeAxisLabels))){
            latticePreset.toward = (lattice::LatticeAxis)towardIndex;
        }
    }
    const bool degrees = latticePreset.preset == lattice::LatticePreset::TWIST || latticePreset.preset == lattice::LatticePreset::BEND;
    propertyHeader("Amount", -1, false, false);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##lattice_preset_amount", &latticePreset.amount, degrees ? 0.5f : 0.005f, 0.0f, 0.0f, degrees ? "%.1f deg" : "%.3f");
    if (!std::isfinite(latticePreset.amount)) latticePreset.amount = 0.0f;
    if (latticePreset.preset == lattice::LatticePreset::NOISE){
        propertyHeader("Seed", -1, false, false);
        ImGui::SetNextItemWidth(-1);
        int seed = (int)latticePreset.seed;
        if (ImGui::DragInt("##lattice_preset_seed", &seed, 1.0f, 0, INT32_MAX)){
            latticePreset.seed = (unsigned int)std::max(0, seed);
        }
    }
    propertyHeader("Replace", -1, false, false);
    ImGui::Checkbox("##lattice_preset_replace", &latticePreset.replace);
    ImGui::SetItemTooltip("On: the preset replaces the offsets of the points. Off: it adds to them.");
    endTable();
    {
        const std::string applyLabel = selected.empty() ? ICON_FA_WAND_MAGIC_SPARKLES " Apply to All Points"
                                                        : ICON_FA_WAND_MAGIC_SPARKLES " Apply to " + std::to_string(selected.size()) + " Selected";
        if (ImGui::Button(applyLabel.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))){
            if (!hasToward && latticePreset.toward == latticePreset.axis){
                latticePreset.toward = (lattice::LatticeAxis)(((int)latticePreset.axis + 1) % 3);
            }
            std::string error;
            reportLatticeEdit(LatticeEditorOps::applyPreset(project, sceneId, entity, latticePreset, selected, &error), error);
        }
    }

    // ---- Poses
    ImGui::SeparatorText("Poses");
    beginTable(cpType, getLabelSize("Poses"), "lattice_poses_header");
    prefabRowProperty = "poses";
    propertyHeader("Poses", -1, false, false);
    ImGui::Text("%zu", lattice.poses.size());
    const float posesArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton(ICON_FA_PLUS " Save Current As...##lattice_pose_save", posesArrowWidth)){
        std::string name = "Pose " + std::to_string(lattice.poses.size() + 1);
        for (int n = (int)lattice.poses.size() + 1; std::any_of(lattice.poses.begin(), lattice.poses.end(), [&name](const LatticePose& pose){ return pose.name == name; }); n++){
            name = "Pose " + std::to_string(n + 1);
        }
        std::snprintf(latticePoseName, sizeof(latticePoseName), "%s", name.c_str());
        latticePoseRenaming.clear();
        ImGui::OpenPopup("lattice_pose_name");
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_lattice_poses", latticePosesExpanded ? ImGuiDir_Up : ImGuiDir_Down)){
        latticePosesExpanded = !latticePosesExpanded;
    }

    // one popup for Save Current As (latticePoseRenaming empty) and Rename
    if (ImGui::BeginPopup("lattice_pose_name")){
        ImGui::TextUnformatted(latticePoseRenaming.empty() ? "Save the current offsets as" : "Rename pose to");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(14 * ImGui::GetFontSize());
        const bool entered = ImGui::InputText("##lattice_pose_name_input", latticePoseName, sizeof(latticePoseName), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = latticePoseName;
        const bool exists = std::any_of(lattice.poses.begin(), lattice.poses.end(), [&name](const LatticePose& pose){ return pose.name == name; });
        if (latticePoseRenaming.empty() && exists){
            ImGui::TextDisabled("Overwrites the pose of that name");
        }
        ImGui::BeginDisabled(name.empty() || (!latticePoseRenaming.empty() && exists && name != latticePoseRenaming));
        if (ImGui::Button("OK", ImVec2(6 * ImGui::GetFontSize(), 0)) || (entered && !name.empty())){
            std::string error;
            if (latticePoseRenaming.empty()){
                reportLatticeEdit(LatticeEditorOps::savePose(project, sceneId, entity, name, &error), error);
            }else if (name != latticePoseRenaming){
                reportLatticeEdit(LatticeEditorOps::renamePose(project, sceneId, entity, latticePoseRenaming, name, &error), error);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(6 * ImGui::GetFontSize(), 0))){
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    endTable();

    if (latticePosesExpanded){
        if (lattice.poses.empty()){
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("No poses: Save Current As... keeps the current offsets under a name.");
            ImGui::PopStyleColor();
        }
        for (size_t p = 0; p < lattice.poses.size(); p++){
            const std::string name = lattice.poses[p].name;
            ImGui::PushID((int)p);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name.empty() ? "(unnamed)" : name.c_str());
            if ((int)lattice.poses[p].offsets.size() != pointCount){
                ImGui::SameLine();
                ImGui::TextDisabled("(%zu points, resampled)", lattice.poses[p].offsets.size());
            }
            const ImGuiStyle& style = ImGui::GetStyle();
            const float iconWidth = ImGui::GetFrameHeight();
            const float applyWidth = ImGui::CalcTextSize("Apply").x + style.FramePadding.x * 2.0f;
            const float trailing = applyWidth + iconWidth * 3.0f + style.ItemSpacing.x * 3.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - trailing));
            std::string error;
            if (ImGui::Button("Apply")){
                reportLatticeEdit(LatticeEditorOps::applyPose(project, sceneId, entity, name, 1.0f, &error), error);
            }
            ImGui::SetItemTooltip("Sets the offsets to this pose");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FLOPPY_DISK, ImVec2(iconWidth, 0))){
                reportLatticeEdit(LatticeEditorOps::savePose(project, sceneId, entity, name, &error), error);
            }
            ImGui::SetItemTooltip("Overwrite with the current offsets");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_PEN, ImVec2(iconWidth, 0))){
                std::snprintf(latticePoseName, sizeof(latticePoseName), "%s", name.c_str());
                latticePoseRenaming = name;
                ImGui::OpenPopup("lattice_pose_rename");
            }
            ImGui::SetItemTooltip("Rename");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH_CAN, ImVec2(iconWidth, 0))){
                reportLatticeEdit(LatticeEditorOps::deletePose(project, sceneId, entity, name, &error), error);
            }
            ImGui::SetItemTooltip("Delete");
            if (ImGui::BeginPopup("lattice_pose_rename")){
                ImGui::TextUnformatted("Rename pose to");
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(14 * ImGui::GetFontSize());
                const bool entered = ImGui::InputText("##lattice_pose_rename_input", latticePoseName, sizeof(latticePoseName), ImGuiInputTextFlags_EnterReturnsTrue);
                const std::string newName = latticePoseName;
                const bool taken = newName != name && std::any_of(lattice.poses.begin(), lattice.poses.end(), [&newName](const LatticePose& pose){ return pose.name == newName; });
                if (taken) ImGui::TextDisabled("Another pose has that name");
                ImGui::BeginDisabled(newName.empty() || taken);
                if (ImGui::Button("OK", ImVec2(6 * ImGui::GetFontSize(), 0)) || (entered && !newName.empty() && !taken)){
                    if (newName != name){
                        std::string renameError;
                        reportLatticeEdit(LatticeEditorOps::renamePose(project, sceneId, entity, name, newName, &renameError), renameError);
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(6 * ImGui::GetFontSize(), 0))){
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }

    // ---- Jiggle (play mode only)
    const float subsectionIndent = 10.0f;
    ImGui::Spacing();
    ImGui::Indent(subsectionIndent);
    ImGui::PushStyleColor(ImGuiCol_Header, Theme::Colors::NestedHeader);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Colors::NestedHeaderHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Theme::Colors::NestedHeaderActive);
    const bool jiggleOpen = ImGui::CollapsingHeader("Jiggle##lattice_jiggle");
    ImGui::PopStyleColor(3);
    ImGui::Unindent(subsectionIndent);
    if (jiggleOpen){
        RowSettings settingsJiggle;
        settingsJiggle.help = "Springs on every point that wobble when the lattice moves or is poked (play mode only).";
        RowSettings settingsFloat;
        settingsFloat.secondColSize = 6 * ImGui::GetFontSize();
        RowSettings settingsAnchor;
        settingsAnchor.enumEntries = &entriesLatticeJiggleAnchor;
        settingsAnchor.help = "Face that stays still (the base of a jelly); the opposite face moves the most.";
        RowSettings settingsMax = settingsFloat;
        settingsMax.help = "Largest distance a point springs away, in lattice units.";
        beginTable(cpType, getLabelSize("Max Offset"), "lattice_jiggle");
        propertyRow(RowPropertyType::Bool, cpType, "jiggle", "Enabled", sceneProject, entities, settingsJiggle);
        propertyRow(RowPropertyType::FloatPositive, cpType, "jiggleStiffness", "Stiffness", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::FloatPositive, cpType, "jiggleDamping", "Damping", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "jiggleInertia", "Inertia", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Enum, cpType, "jiggleAnchor", "Anchor", sceneProject, entities, settingsAnchor);
        propertyRow(RowPropertyType::FloatPositive, cpType, "jiggleMaxOffset", "Max Offset", sceneProject, entities, settingsMax);
        endTable();
    }

    // ---- what this lattice deforms (click selects)
    const std::vector<Entity> targets = LatticeEditorOps::findTargets(scene, entity);
    ImGui::SeparatorText(("Deforming (" + std::to_string(targets.size()) + ")").c_str());
    if (targets.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("Nothing yet: add a Lattice Modifier to a mesh and pick this lattice, or create a lattice with the meshes selected.");
        ImGui::PopStyleColor();
    }
    for (Entity target : targets){
        if (!scene->isEntityCreated(target)) continue;
        std::string targetName = scene->getEntityName(target);
        if (targetName.empty()) targetName = "GameObject " + std::to_string(target);
        ImGui::PushID((int)target);
        if (ImGui::Selectable((std::string(ICON_FA_CIRCLE_DOT " ") + targetName).c_str())){
            project->clearSelectedEntities(sceneId);
            project->addSelectedEntity(sceneId, target);
        }
        ImGui::PopID();
    }
}

// up the Transform parents of `entity`: whether `ancestor` is one of them
static bool isLatticeDescendant(Scene* scene, Entity ancestor, Entity entity){
    const Transform* transform = scene->findComponent<Transform>(entity);
    while (transform && transform->parent != NULL_ENTITY){
        if (transform->parent == ancestor) return true;
        transform = scene->findComponent<Transform>(transform->parent);
    }
    return false;
}

// meshes a modifier on `entity` deforms: its own and, with includeChildren, its descendants'
static std::vector<Entity> latticeModifierMeshes(Scene* scene, Entity entity, bool includeChildren){
    std::vector<Entity> meshes;
    auto array = scene->getComponentArray<MeshComponent>();
    for (size_t m = 0; m < array->size(); m++){
        Entity meshEntity = array->getEntity(m);
        if (meshEntity == entity || (includeChildren && isLatticeDescendant(scene, entity, meshEntity))){
            meshes.push_back(meshEntity);
        }
    }
    return meshes;
}

// bindings that reach a mesh (spec §2.1 stacking): its own, then those of the ancestors with
// includeChildren, counting only the ones naming a lattice; a lattice a nearer modifier already
// binds is not applied again (LatticeRender)
static size_t latticeBindingCount(Scene* scene, Entity mesh){
    std::vector<Entity> nearer;
    auto valid = [scene, &nearer](const LatticeModifierComponent& modifier){
        size_t count = 0;
        std::vector<Entity> level;
        for (const LatticeBinding& binding : modifier.bindings){
            if (binding.lattice != NULL_ENTITY && scene->isEntityCreated(binding.lattice) && scene->findComponent<LatticeComponent>(binding.lattice) &&
                    std::find(nearer.begin(), nearer.end(), binding.lattice) == nearer.end()){
                count++;
                level.push_back(binding.lattice);
            }
        }
        if (!modifier.enabled) return size_t(0);
        nearer.insert(nearer.end(), level.begin(), level.end());
        return count;
    };
    size_t count = 0;
    if (const LatticeModifierComponent* own = scene->findComponent<LatticeModifierComponent>(mesh)){
        count += valid(*own);
    }
    const Transform* transform = scene->findComponent<Transform>(mesh);
    Entity ancestor = transform ? transform->parent : NULL_ENTITY;
    while (ancestor != NULL_ENTITY){
        if (const LatticeModifierComponent* inherited = scene->findComponent<LatticeModifierComponent>(ancestor)){
            if (inherited->includeChildren) count += valid(*inherited);
        }
        const Transform* ancestorTransform = scene->findComponent<Transform>(ancestor);
        ancestor = ancestorTransform ? ancestorTransform->parent : NULL_ENTITY;
    }
    return count;
}

// the Bake Deformation row of the Lattice Modifier (spec §9.4): disabled with the reason when
// nothing can be baked; the bake reports what it wrote and kept in the Console
static void drawLatticeBakeRow(editor::Project* project, editor::SceneProject* sceneProject, Entity entity){
    ImGui::SeparatorText("Bake");
    std::string reason;
    const bool stopped = sceneProject->playState == editor::ScenePlayState::STOPPED;
    const bool canBake = stopped && editor::LatticeEditorOps::canBake(sceneProject->scene, entity, &reason);
    if (!stopped){
        reason = "Stop the scene to bake: it writes a model file and edits the scene.";
    }
    bool clicked = false;
    ImGui::BeginDisabled(!canBake);
    clicked = ImGui::Button(ICON_FA_FILE_EXPORT " Bake Deformation##lattice_bake", ImVec2(-FLT_MIN, 0));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
        ImGui::SetTooltip("%s", canBake ? "Writes the current deformation into a new model file next to the original (<name>_lattice.glb), "
                                          "switches the Model to it and removes the baked bindings (one undo step; the file stays)."
                                        : reason.c_str());
    }
    if (clicked){
        std::string path;
        std::string note;
        std::string error;
        if (editor::LatticeEditorOps::bake(project, sceneProject->id, entity, &path, &note, &error)){
            Log::print("Lattice: baked the deformation into %s", path.c_str());
            size_t start = 0;
            while (start < note.size()){
                size_t end = note.find('\n', start);
                if (end == std::string::npos) end = note.size();
                Log::warn("Lattice bake: %s", note.substr(start, end - start).c_str());
                start = end + 1;
            }
        }else{
            Log::error("Lattice bake: %s", error.c_str());
        }
    }
    // Bake ▾ › Vertex Animation…: the lattice tracks of the clips that move it, baked per vertex
    if (editor::vatbakeentry::available()){
        editor::VatBakeEntry entry;
        entry.kind = editor::VatBakeEntry::Kind::LatticeModifier;
        entry.sceneId = sceneProject->id;
        entry.entity = entity;
        editor::vatbakeentry::bakeButton(project, entry, stopped, "lattice_bake_vat", "lattice.bake");
    }
}

void editor::InspectorWindow::drawLatticeModifierComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const Entity entity = entities[0];
    LatticeModifierComponent& modifier = scene->getComponent<LatticeModifierComponent>(entity);

    const std::vector<Entity> meshes = latticeModifierMeshes(scene, entity, modifier.includeChildren);
    bool hasSprite = false;
    bool hasSkinning = false;
    for (Entity mesh : meshes){
        if (scene->findComponent<SpriteComponent>(mesh)) hasSprite = true;
        const MeshComponent& meshComponent = scene->getComponent<MeshComponent>(mesh);
        for (unsigned int s = 0; s < meshComponent.numSubmeshes; s++){
            if (meshComponent.submeshes[s].hasSkinning) hasSkinning = true;
        }
    }

    RowSettings settingsChildren;
    settingsChildren.help = "Also deforms every mesh below this GameObject (a car, a character with its props).";
    RowSettings settingsSubdivisions;
    settingsSubdivisions.intMin = 1;
    settingsSubdivisions.intMax = 64;
    settingsSubdivisions.secondColSize = 6 * ImGui::GetFontSize();
    settingsSubdivisions.help = "A sprite's quad becomes an n x n grid while a lattice deforms it, so it can bend smoothly.";

    beginTable(cpType, getLabelSize("Include Children"), "lattice_modifier_main");
    propertyRow(RowPropertyType::Bool, cpType, "includeChildren", "Include Children", sceneProject, entities, settingsChildren);
    if (hasSprite){
        propertyRow(RowPropertyType::Int, cpType, "subdivisions", "Subdivisions", sceneProject, entities, settingsSubdivisions);
    }
    endTable();

    if (entities.size() != 1){
        ImGui::SeparatorText("Bindings");
        ImGui::TextDisabled("Select a single GameObject to edit its bindings");
        return;
    }

    if (meshes.empty()){
        drawLatticeWarning(modifier.includeChildren ? "Nothing to deform: no mesh, sprite or model on this GameObject or below it."
                                                    : "Nothing to deform: no mesh on this GameObject (turn Include Children on for the meshes below it).");
    }

    // ---- bindings, in the order they deform
    ImGui::SeparatorText("Bindings");
    beginTable(cpType, getLabelSize("Bindings"), "lattice_bindings_header");
    prefabRowProperty = "bindings";
    propertyHeader("Bindings", -1, false, false);
    ImGui::Text("%zu", modifier.bindings.size());
    const float bindingsArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Binding##lattice_binding_add", bindingsArrowWidth)){
        std::vector<LatticeBinding> bindings = modifier.bindings;
        bindings.push_back(LatticeBinding());
        auto* bindingCmd = new PropertyCmd<std::vector<LatticeBinding>>(project, sceneId, entity, cpType, "bindings", bindings);
        bindingCmd->setNoMerge();
        CommandHandle::get(sceneId)->addCommand(bindingCmd);
        latticeBindingsExpanded = true;
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_lattice_bindings", latticeBindingsExpanded ? ImGuiDir_Up : ImGuiDir_Down)){
        latticeBindingsExpanded = !latticeBindingsExpanded;
    }
    endTable();

    std::vector<std::string> warnings;
    for (size_t b = 0; b < modifier.bindings.size(); b++){
        const Entity latticeEntity = modifier.bindings[b].lattice;
        const std::string label = "Binding " + std::to_string(b + 1);
        if (latticeEntity == NULL_ENTITY){
            warnings.push_back(label + ": no lattice picked.");
        }else if (!scene->isEntityCreated(latticeEntity)){
            warnings.push_back(label + ": its lattice was deleted.");
        }else if (!scene->findComponent<LatticeComponent>(latticeEntity)){
            warnings.push_back(label + ": \"" + scene->getEntityName(latticeEntity) + "\" has no Lattice component.");
        }
    }
    // the same lattice bound here and inherited: this binding overrides the inherited one here
    {
        auto bindsLattice = [](const LatticeModifierComponent& other, Entity latticeEntity){
            if (!other.enabled) return false;
            for (const LatticeBinding& binding : other.bindings){
                if (binding.lattice == latticeEntity) return true;
            }
            return false;
        };
        for (const LatticeBinding& binding : modifier.bindings){
            if (binding.lattice == NULL_ENTITY) continue;
            const Transform* transform = scene->findComponent<Transform>(entity);
            Entity ancestor = transform ? transform->parent : NULL_ENTITY;
            for (int hops = 0; ancestor != NULL_ENTITY && hops < 4096; hops++){
                const LatticeModifierComponent* inherited = scene->findComponent<LatticeModifierComponent>(ancestor);
                if (inherited && inherited->includeChildren && bindsLattice(*inherited, binding.lattice)){
                    warnings.push_back("\"" + scene->getEntityName(binding.lattice) + "\" also reaches this GameObject through \"" +
                                       scene->getEntityName(ancestor) + "\" (Include Children): this binding replaces that one here "
                                       "(weight 0 excludes this GameObject). Remove it to inherit again.");
                    break;
                }
                const Transform* ancestorTransform = scene->findComponent<Transform>(ancestor);
                ancestor = ancestorTransform ? ancestorTransform->parent : NULL_ENTITY;
            }
        }
    }
    for (Entity mesh : meshes){
        const size_t count = latticeBindingCount(scene, mesh);
        if (count > LATTICE_MAX_PER_MESH){
            warnings.push_back("\"" + scene->getEntityName(mesh) + "\" is reached by " + std::to_string(count) + " lattices: only the first " +
                               std::to_string(LATTICE_MAX_PER_MESH) + " deform it (its own bindings first, then inherited ones).");
        }
    }
    for (const std::string& warning : warnings){
        drawLatticeWarning(warning);
    }

    if (!latticeBindingsExpanded){
        drawLatticeBakeRow(project, sceneProject, entity);
        return;
    }

    Signature latticeFilter;
    latticeFilter.set(scene->getComponentId<LatticeComponent>());

    for (size_t b = 0; b < modifier.bindings.size(); b++){
        ImGui::PushID((int)b);
        const std::string prefix = "bindings[" + std::to_string(b) + "]";

        // header: name, then move up / down and remove at the right
        ImGui::SeparatorText(("Binding " + std::to_string(b + 1)).c_str());
        const ImGuiStyle& style = ImGui::GetStyle();
        const float iconWidth = ImGui::GetFrameHeight();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - iconWidth * 3.0f - style.ItemSpacing.x * 2.0f));
        auto reorder = [&](std::vector<LatticeBinding> bindings){
            auto* bindingCmd = new PropertyCmd<std::vector<LatticeBinding>>(project, sceneId, entity, cpType, "bindings", bindings);
            bindingCmd->setNoMerge();
            CommandHandle::get(sceneId)->addCommand(bindingCmd);
        };
        bool changedList = false;
        ImGui::BeginDisabled(b == 0);
        if (ImGui::ArrowButton("##binding_up", ImGuiDir_Up)){
            std::vector<LatticeBinding> bindings = modifier.bindings;
            std::swap(bindings[b], bindings[b - 1]);
            reorder(bindings);
            changedList = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Deform earlier");
        ImGui::SameLine();
        ImGui::BeginDisabled(b + 1 >= modifier.bindings.size());
        if (!changedList && ImGui::ArrowButton("##binding_down", ImGuiDir_Down)){
            std::vector<LatticeBinding> bindings = modifier.bindings;
            std::swap(bindings[b], bindings[b + 1]);
            reorder(bindings);
            changedList = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Deform later");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
        if (!changedList && ImGui::Button(ICON_FA_TRASH_CAN "##binding_remove", ImVec2(iconWidth, 0))){
            std::vector<LatticeBinding> bindings = modifier.bindings;
            bindings.erase(bindings.begin() + (long)b);
            reorder(bindings);
            changedList = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Remove binding");
        if (changedList){
            ImGui::PopID();
            break;
        }

        RowSettings settingsLattice;
        settingsLattice.entityFilter = latticeFilter;
        settingsLattice.excludeEntities = {entity};
        settingsLattice.help = "The lattice GameObject; drag one from the Hierarchy or pick it.";
        RowSettings settingsWeight;
        settingsWeight.secondColSize = 6 * ImGui::GetFontSize();
        settingsWeight.help = "Scales this lattice's deformation, 0 to 2.";
        RowSettings settingsStage;
        settingsStage.enumEntries = &entriesLatticeStage;
        settingsStage.help = "Before Skinning deforms the bind pose (a big head in every animation); After Skinning deforms the posed mesh.";
        RowSettings settingsSubmesh;
        settingsSubmesh.intMin = -1;
        settingsSubmesh.intMax = 255;
        settingsSubmesh.secondColSize = 6 * ImGui::GetFontSize();
        settingsSubmesh.help = "Only this submesh (material slot); -1 deforms all of them.";
        RowSettings settingsMask;
        settingsMask.enumEntries = &entriesLatticeMask;
        settingsMask.help = "A vertex colour channel scaling the deformation per vertex (inverted: 1 - channel).";

        beginTable(cpType, getLabelSize("Submesh"), "lattice_binding");
        propertyRow(RowPropertyType::LocalEntity, cpType, prefix + ".lattice", "Lattice", sceneProject, entities, settingsLattice);
        propertyRow(RowPropertyType::Float, cpType, prefix + ".weight", "Weight", sceneProject, entities, settingsWeight);
        if (hasSkinning){
            propertyRow(RowPropertyType::Enum, cpType, prefix + ".stage", "Stage", sceneProject, entities, settingsStage);
        }
        propertyRow(RowPropertyType::Int, cpType, prefix + ".submesh", "Submesh", sceneProject, entities, settingsSubmesh);
        propertyRow(RowPropertyType::Enum, cpType, prefix + ".mask", "Mask", sceneProject, entities, settingsMask);
        endTable();
        ImGui::PopID();
    }

    drawLatticeBakeRow(project, sceneProject, entity);
}

void editor::InspectorWindow::drawLatticeFollowerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const Entity entity = entities[0];
    LatticeFollowerComponent& follower = scene->getComponent<LatticeFollowerComponent>(entity);

    // the follower moves its own subtree: neither field may name it or anything below it
    std::vector<Entity> subtree;
    for (Entity selected : entities){
        subtree.push_back(selected);
    }
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t t = 0; t < transforms->size(); t++){
        Entity other = transforms->getEntity(t);
        for (Entity selected : entities){
            if (isLatticeDescendant(scene, selected, other)){
                subtree.push_back(other);
                break;
            }
        }
    }

    RowSettings settingsLattice;
    settingsLattice.entityFilter.set(scene->getComponentId<LatticeComponent>());
    settingsLattice.excludeEntities = subtree;
    settingsLattice.help = "The lattice whose deformation this GameObject follows.";
    RowSettings settingsRest;
    settingsRest.excludeEntities = subtree;
    settingsRest.help = "Where this GameObject stands undeformed: it is placed where the lattice moves that spot. Not this GameObject or one below it.";
    RowSettings settingsWeight;
    settingsWeight.secondColSize = 6 * ImGui::GetFontSize();

    beginTable(cpType, getLabelSize("Apply Rotation"), "lattice_follower");
    propertyRow(RowPropertyType::LocalEntity, cpType, "lattice", "Lattice", sceneProject, entities, settingsLattice);
    propertyRow(RowPropertyType::LocalEntity, cpType, "rest", "Rest", sceneProject, entities, settingsRest);
    propertyRow(RowPropertyType::Bool, cpType, "applyPosition", "Apply Position", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "applyRotation", "Apply Rotation", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "applyScale", "Apply Scale", sceneProject, entities);
    propertyRow(RowPropertyType::Float, cpType, "weight", "Weight", sceneProject, entities, settingsWeight);
    endTable();

    if (entities.size() != 1){
        return;
    }

    if (follower.lattice != NULL_ENTITY && (!scene->isEntityCreated(follower.lattice) || !scene->findComponent<LatticeComponent>(follower.lattice))){
        drawLatticeWarning("The lattice is missing or has no Lattice component: the follower stays where it is.");
    }else if (follower.lattice == NULL_ENTITY){
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("Pick a lattice and a rest GameObject to follow the deformation.");
        ImGui::PopStyleColor();
    }
    if (follower.rest != NULL_ENTITY && std::find(subtree.begin(), subtree.end(), follower.rest) != subtree.end()){
        drawLatticeWarning("The rest is this GameObject or one below it: the follower is left alone. Use Create Rest Here.");
    }else if (follower.rest != NULL_ENTITY && !scene->isEntityCreated(follower.rest)){
        drawLatticeWarning("The rest GameObject was deleted: the follower stays where it is.");
    }

    // an empty sibling where the follower stands, assigned as its rest: one undo step
    const Transform* transform = scene->findComponent<Transform>(entity);
    ImGui::BeginDisabled(!transform);
    if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS " Create Rest Here", ImVec2(ImGui::GetContentRegionAvail().x, 0)) && transform){
        CommandHistory* history = CommandHandle::get(sceneId);
        const size_t mark = history->mark();
        const Vector3 position = transform->position;
        const Quaternion rotation = transform->rotation;
        const Vector3 scale = transform->scale;
        auto* create = new CreateEntityCmd(project, sceneId, scene->getEntityName(entity) + " Rest", EntityCreationType::GAMEOBJECT, transform->parent);
        create->setQuiet(true);
        create->setNoMerge();
        if (history->addCommand(create)){
            const Entity rest = create->getEntity();
            auto* multi = new MultiPropertyCmd();
            multi->addPropertyCmd<Vector3>(project, sceneId, rest, ComponentType::Transform, "position", position);
            multi->addPropertyCmd<Quaternion>(project, sceneId, rest, ComponentType::Transform, "rotation", rotation);
            multi->addPropertyCmd<Vector3>(project, sceneId, rest, ComponentType::Transform, "scale", scale);
            multi->addPropertyCmd<Entity>(project, sceneId, entity, cpType, "rest", rest);
            multi->setNoMerge();
            history->addCommand(multi);
            history->groupSince(mark);
        }else{
            Out::error("Lattice Follower: could not create the rest GameObject");
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Adds an empty GameObject next to this one, where it stands now, and makes it the rest");
}

