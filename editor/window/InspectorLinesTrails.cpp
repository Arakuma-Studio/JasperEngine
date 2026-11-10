// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector > Line Renderer and Trail Renderer (lines and trails spec 6.8), in Unity's order
// and labels. The Line Renderer's Mode picks the data that draws: Polyline (Unity's positions:
// a count, a row per point, Loop, the Scene view's point editing) or Segments (the engine's
// independent A-B debug segments, the list drawLineSegmentsList keeps). Width is the multiplier
// and the curve over the line (0 is the 1-pixel hairline), Color the gradient over it, then
// corners, caps, alignment and texture. The Trail Renderer has Unity's rows plus the same
// ribbon settings and, in Play, its point count. Every value is a Catalog property edited
// through PropertyCmd (undo, multi-selection, prefab override markers); the curve and gradient
// rows are propertyRow's Curve / Gradient types (S27's Curve and Gradient fields).

#include "InspectorWindow.h"
#include "component/Transform.h"

#include "Catalog.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/LineRendererComponent.h"
#include "component/TrailRendererComponent.h"
#include "external/IconsFontAwesome6.h"
#include "render/SceneRender.h"
#include "shader/ShaderManifest.h"
#include "util/FileDialogs.h"
#include "util/Util.h"
#include "App.h"
#include "Backend.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    std::vector<editor::EnumEntry> entriesLineMode = {
        {(int)LineRendererMode::Polyline, "Polyline"},
        {(int)LineRendererMode::Segments, "Segments"},
    };

    std::vector<editor::EnumEntry> entriesLineWidthUnit = {
        {(int)LineWidthUnit::World, "World"},
        {(int)LineWidthUnit::Pixels, "Pixels"},
    };

    std::vector<editor::EnumEntry> entriesLineAlignment = {
        {(int)LineAlignment::View, "View"},
        {(int)LineAlignment::TransformZ, "Transform Z"},
    };

    std::vector<editor::EnumEntry> entriesLineTextureMode = {
        {(int)LineTextureMode::Stretch, "Stretch"},
        {(int)LineTextureMode::Tile, "Tile"},
        {(int)LineTextureMode::DistributePerSegment, "Distribute Per Segment"},
        {(int)LineTextureMode::RepeatPerSegment, "Repeat Per Segment"},
    };

    using ShaderTextures = std::vector<std::pair<std::string, Texture>>;

}

// The ribbon rows a Line and a Trail Renderer share: width, its unit and curve, colour,
// corners and caps, alignment and texture layout (the texture slot itself: drawLineTextureRow)
void editor::InspectorWindow::drawLineRibbonRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                 bool hairline, bool gradient){
    RowSettings widthSettings;
    widthSettings.stepSize = 0.01f;
    widthSettings.format = "%.3f";
    widthSettings.help = cpType == ComponentType::LineRendererComponent
        ? "Width of the line (Unity widthMultiplier), times the Width Curve over its length. 0 draws the 1-pixel hairline (no curve, caps or texture)."
        : "Width of the trail (Unity widthMultiplier), times the Width Curve from the head (0) to the tail (1).";
    propertyRow(RowPropertyType::FloatPositive, cpType, "widthMultiplier", "Width", sceneProject, entities, widthSettings);
    if (hairline){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Hairline (1 px): set a width for a thick line");
    }

    RowSettings curveSettings;
    curveSettings.help = cpType == ComponentType::LineRendererComponent
        ? "The width over the line's length (0 the start, 1 the end), times Width. Click to edit: drag keys, double-click to add one."
        : "The width over the trail's age (0 the head, 1 the tail), times Width. Click to edit: drag keys, double-click to add one.";
    propertyRow(RowPropertyType::Curve, cpType, "widthCurve", "Width Curve", sceneProject, entities, curveSettings);

    RowSettings unitSettings;
    unitSettings.enumEntries = &entriesLineWidthUnit;
    unitSettings.help = "World: the width in world units (Unity). Pixels: in screen pixels of the render target, the same on screen at any distance (debug lines).";
    propertyRow(RowPropertyType::Enum, cpType, "widthUnit", "Width Unit", sceneProject, entities, unitSettings);

    if (gradient){
        RowSettings colorSettings;
        colorSettings.help = cpType == ComponentType::LineRendererComponent
            ? "The colour and alpha over the line's length. Click to edit: drag the markers, click the strips to add one."
            : "The colour and alpha over the trail's age (0 the head, 1 the tail). Click to edit.";
        propertyRow(RowPropertyType::Gradient, cpType, "colorGradient", "Color", sceneProject, entities, colorSettings);
    }

    RowSettings countSettings;
    countSettings.intMin = 0;
    countSettings.intMax = (int)LINE_RIBBON_MAX_FAN_VERTICES;
    countSettings.secondColSize = 6 * ImGui::GetFontSize();
    countSettings.help = "Vertices of each round corner (0: a sharp miter corner, a bevel past 4x the half width).";
    propertyRow(RowPropertyType::UInt, cpType, "numCornerVertices", "Corner Vertices", sceneProject, entities, countSettings);
    countSettings.help = "Vertices of each round end cap (0: flat ends).";
    propertyRow(RowPropertyType::UInt, cpType, "numCapVertices", "End Cap Vertices", sceneProject, entities, countSettings);

    RowSettings alignSettings;
    alignSettings.enumEntries = &entriesLineAlignment;
    alignSettings.help = "View: the ribbon faces the camera. Transform Z: it lies across the GameObject's Z axis (seen edge-on from the side).";
    propertyRow(RowPropertyType::Enum, cpType, "alignment", "Alignment", sceneProject, entities, alignSettings);

    RowSettings textureModeSettings;
    textureModeSettings.enumEntries = &entriesLineTextureMode;
    textureModeSettings.help = "How the texture runs along: Stretch once over the length, Tile every 1 / Texture Scale.x world units, "
                               "Distribute Per Segment once with each segment the same share, Repeat Per Segment once per segment.";
    propertyRow(RowPropertyType::Enum, cpType, "textureMode", "Texture Mode", sceneProject, entities, textureModeSettings);
    RowSettings scaleSettings;
    scaleSettings.help = "Scales u (along) and v (across) of the texture coordinates.";
    propertyRow(RowPropertyType::Vector2, cpType, "textureScale", "Texture Scale", sceneProject, entities, scaleSettings);

    RowSettings sortSettings;
    sortSettings.help = "Order among the transparent renderers of one Sorting Layer and Order in Layer: lower draws first.";
    propertyRow(RowPropertyType::Int, cpType, "sortPriority", "Sort Priority", sceneProject, entities, sortSettings);
    drawSortingRows(cpType, sceneProject, entities);
}

// The base texture of the built-in line shader (shaderTextures' u_baseTexture): a shader with a
// manifest shows its own texture slots in the shader block instead
void editor::InspectorWindow::drawLineTextureRow(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    if (entities.size() != 1)
        return;
    Scene* scene = sceneProject->scene;
    const Entity entity = entities[0];
    const std::string shader = Catalog::getShaderReferenceForProperty(scene, entity, cpType, "customShader");
    const std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
    if (manifest && manifest->present)
        return;
    ShaderTextures* texturesRef = Catalog::getPropertyRef<ShaderTextures>(scene, entity, cpType, "shaderTextures");
    if (!texturesRef)
        return;
    const ShaderTextures textures = *texturesRef;
    const std::string current = ShaderLibraryValues::getTexture(textures, LINE_BASE_TEXTURE_UNIFORM);
    const uint32_t sceneId = sceneProject->id;

    auto setTexture = [&](const std::string& path){
        ShaderTextures changed = textures;
        if (ShaderLibraryValues::setTexture(changed, LINE_BASE_TEXTURE_UNIFORM, path)){
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new PropertyCmd<ShaderTextures>(project, sceneId, entity, cpType, "shaderTextures", changed));
        }
    };
    auto assign = [&](const std::string& absolutePath){
        if (!project->isInsideAssetsPath(absolutePath)){
            Backend::getApp().registerOutsideAssetsAlert(absolutePath);
            return;
        }
        setTexture(project->normalizeToAssetsRelative(absolutePath).generic_string());
    };

    ImGui::PushID("line_base_texture");
    prefabRowProperty = "shaderTextures";
    if (propertyHeader("Texture", -1, !current.empty())){
        setTexture("");
        prefabRowProperty.clear();
        ImGui::PopID();
        return;
    }
    const float buttonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const std::string shown = current.empty() ? std::string("None (white)") : std::filesystem::path(current).filename().string();
    ImGui::PushStyleColor(ImGuiCol_Text, current.empty() ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] : ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ImGui::Button((shown + "##line_texture_slot").c_str(),
                  ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x), 0));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()){
        ImGui::SetTooltip("%s%s\nThe built-in line shader's base map (u_baseTexture), times the colour.\nIt needs a width: a hairline has no texture coordinates.\nDrop an image here",
                          current.empty() ? "No texture" : "Texture: ", current.c_str());
    }
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            std::vector<std::string> received = Util::getStringsFromPayload(payload);
            if (!received.empty() && Util::isImageFile(received[0]))
                assign(received[0]);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "##line_texture_pick")){
        const std::string selected = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
        if (!selected.empty())
            assign(selected);
    }
    prefabRowProperty.clear();
    ImGui::PopID();
}

void editor::InspectorWindow::drawLineRendererComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const LineRendererComponent& first = scene->getComponent<LineRendererComponent>(entities[0]);
    bool mixedMode = false;
    bool anyRibbon = false;
    for (Entity entity : entities){
        const LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
        mixedMode = mixedMode || lines.mode != first.mode;
        anyRibbon = anyRibbon || lines.widthMultiplier > 0.0f;
    }
    const bool polyline = !mixedMode && first.mode == LineRendererMode::Polyline;

    beginTable(cpType, getLabelSize("End Cap Vertices"));

    RowSettings modeSettings;
    modeSettings.enumEntries = &entriesLineMode;
    modeSettings.help = "Polyline: Unity's connected positions (setPosition, positionCount), with corners, a colour gradient and Loop. "
                        "Segments: independent A-B segments with their own colours (debug lines, addLine). Both lists are kept; the mode's draws.";
    propertyRow(RowPropertyType::Enum, cpType, "mode", "Mode", sceneProject, entities, modeSettings);

    if (polyline){
        RowSettings loopSettings;
        loopSettings.help = "Joins the last position back to the first.";
        propertyRow(RowPropertyType::Bool, cpType, "loop", "Loop", sceneProject, entities, loopSettings);
    }
    RowSettings worldSettings;
    worldSettings.help = "The positions are world positions (the GameObject's transform is ignored). Off: they move, turn and scale with it (the width does not scale).";
    propertyRow(RowPropertyType::Bool, cpType, "useWorldSpace", "Use World Space", sceneProject, entities, worldSettings);

    drawLineRibbonRows(cpType, sceneProject, entities, !anyRibbon, polyline);
    if (!polyline && !mixedMode){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Segments use their own colours");
    }
    drawLineTextureRow(cpType, sceneProject, entities);
    endTable();

    drawCustomShaderRow(cpType, ShaderType::LINES, sceneProject, entities);

    if (mixedMode){
        ImGui::SeparatorText("Points");
        ImGui::TextDisabled("The selection mixes Polyline and Segments lines");
        return;
    }
    if (polyline){
        drawLinePositionsList(cpType, sceneProject, entities);
    }else{
        drawLineSegmentsList(cpType, sceneProject, entities);
    }
}

void editor::InspectorWindow::drawLinePositionsList(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    ImGui::SeparatorText("Positions");
    if (entities.size() != 1){
        ImGui::TextDisabled("Select a single GameObject to edit positions");
        return;
    }
    Scene* scene = sceneProject->scene;
    const Entity entity = entities[0];
    const LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
    const uint32_t sceneId = sceneProject->id;
    auto setPositions = [&](const std::vector<Vector3>& positions){
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            new PropertyCmd<std::vector<Vector3>>(project, sceneId, entity, cpType, "positions", positions));
    };

    beginTable(cpType, getLabelSize("Element 000"), "line_positions_header");
    prefabRowProperty = "positions";
    propertyHeader("Count");
    int count = (int)lines.positions.size();
    ImGui::SetNextItemWidth(6 * ImGui::GetFontSize());
    if (ImGui::InputInt("##line_position_count", &count, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)){
        count = std::max(0, std::min(count, 100000));
        std::vector<Vector3> positions = lines.positions;
        // growing repeats the last point (Unity's positionCount)
        const Vector3 fill = positions.empty() ? Vector3(0.0f, 0.0f, 0.0f) : positions.back();
        positions.resize((size_t)count, fill);
        setPositions(positions);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS "##line_position_add")){
        std::vector<Vector3> positions = lines.positions;
        Vector3 next(0.0f, 0.0f, 0.0f);
        if (positions.size() >= 2){
            next = positions.back() + (positions.back() - positions[positions.size() - 2]);
        }else if (positions.size() == 1){
            next = positions.back() + Vector3(1.0f, 0.0f, 0.0f);
        }
        positions.push_back(next);
        setPositions(positions);
    }
    ImGui::SetItemTooltip("Add a point after the last one, continuing the line");
    ImGui::SameLine();
    ImGui::BeginDisabled(lines.positions.empty());
    if (ImGui::Button(ICON_FA_MINUS "##line_position_remove")){
        std::vector<Vector3> positions = lines.positions;
        // the point selected in the Scene view, else the last one
        int removed = (int)positions.size() - 1;
        if (sceneProject->sceneRender && sceneProject->sceneRender->getSelectedLinePointEntity() == entity){
            const int selected = sceneProject->sceneRender->getSelectedLinePointIndex();
            if (selected >= 0 && selected < (int)positions.size()) removed = selected;
            sceneProject->sceneRender->clearLinePointSelection();
        }
        positions.erase(positions.begin() + removed);
        setPositions(positions);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Remove the point selected in the Scene view, else the last one");
    prefabRowProperty.clear();

    // the Scene view's point handles: drag one, Ctrl+click on the line inserts a point, Delete
    // removes the selected one
    if (sceneProject->sceneRender){
        propertyHeader("Scene View");
        bool editing = sceneProject->sceneRender->isLinePointEditing();
        if (ImGui::Checkbox("Edit Points##line_point_editing", &editing)){
            sceneProject->sceneRender->setLinePointEditing(editing);
        }
        ImGui::SameLine();
        helpMarker("Handles on the points of the selected line in the Scene view (Move tool or 2D tool): drag one to move it, "
                   "Ctrl+click on the line to insert a point there, Delete removes the selected point.");
    }
    endTable();

    int selectedPoint = -1;
    if (sceneProject->sceneRender && sceneProject->sceneRender->getSelectedLinePointEntity() == entity){
        selectedPoint = sceneProject->sceneRender->getSelectedLinePointIndex();
    }

    beginTable(cpType, getLabelSize("Element 000"), "line_positions");
    for (size_t i = 0; i < lines.positions.size(); i++){
        ImGui::PushID((int)i);
        const bool selected = selectedPoint == (int)i;
        const std::string label = (selected ? std::string(ICON_FA_CARET_RIGHT " ") : std::string()) + "Element " + std::to_string(i);
        RowSettings rowSettings;
        rowSettings.stepSize = 0.05f;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubSelectionText);
        propertyRow(RowPropertyType::Vector3, cpType, "positions[" + std::to_string(i) + "]", label, sceneProject, entities, rowSettings);
        if (selected) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    endTable();
    if (lines.positions.size() < 2){
        ImGui::TextDisabled("A polyline needs 2 points to draw");
    }
}

void editor::InspectorWindow::drawTrailRendererComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    beginTable(cpType, getLabelSize("Min Vertex Distance"));

    RowSettings timeSettings;
    timeSettings.stepSize = 0.01f;
    timeSettings.format = "%.2f";
    timeSettings.help = "Seconds a point of the trail lives (scaled game time: Time.timeScale slows it).";
    propertyRow(RowPropertyType::FloatPositive, cpType, "time", "Time", sceneProject, entities, timeSettings);

    RowSettings distanceSettings;
    distanceSettings.stepSize = 0.01f;
    distanceSettings.format = "%.3f";
    distanceSettings.help = "A point is added when the GameObject moved this far from the last one (0: one per frame while it moves).";
    propertyRow(RowPropertyType::FloatPositive, cpType, "minVertexDistance", "Min Vertex Distance", sceneProject, entities, distanceSettings);

    RowSettings destructSettings;
    destructSettings.help = "Destroys the GameObject once its trail has no points and it has not moved for Time seconds.";
    propertyRow(RowPropertyType::Bool, cpType, "autodestruct", "AutoDestruct", sceneProject, entities, destructSettings);
    RowSettings emitSettings;
    emitSettings.help = "New points are added while on; the stored ones keep fading either way.";
    propertyRow(RowPropertyType::Bool, cpType, "emitting", "Emitting", sceneProject, entities, emitSettings);

    drawLineRibbonRows(cpType, sceneProject, entities, false, true);
    drawLineTextureRow(cpType, sceneProject, entities);

    // in Play: how many points it keeps now
    if (entities.size() == 1 && sceneProject->playState != ScenePlayState::STOPPED){
        if (const TrailRendererComponent* trail = sceneProject->scene->findComponent<TrailRendererComponent>(entities[0])){
            propertyHeader("Points");
            ImGui::Text("%zu", trail->points.size());
        }
    }
    endTable();

    drawCustomShaderRow(cpType, ShaderType::LINES, sceneProject, entities);
}
