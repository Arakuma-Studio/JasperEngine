// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector drawers of the point, line, polygon, instancing and particle renderers (element tables,
// sub-selection, frame lists, particle modules).

#include "InspectorWindow.h"
#include "component/InstancedMeshComponent.h"
#include "reflect/Reflect.h"
#include "reflect/Enums.h"
#include "command/PlayChangeReview.h"
#include "command/type/RemovePreservedComponentCmd.h"
#include "StreamPreserved.h"
#include "DocumentFile.h"
#include "SceneWindow.h"

#include "imgui_internal.h"

#include "util/Util.h"
#include "util/UIUtils.h"
#include "util/FileDialogs.h"
#include "window/CodeEditor.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/HtmlUiImportPanel.h"
#include "util/EntityPayload.h"
#include "util/CameraTextureLink.h"
#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "command/type/MultiPropertyCmd.h"
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
#include "component/LineRendererComponent.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/PanelComponent.h"
#include "util/SHA1.h"
#include "util/ProjectUtils.h"
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

void editor::InspectorWindow::drawPointsComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings settingsInt;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();

    beginTable(cpType, getLabelSize("Transparent"));
    propertyRow(RowPropertyType::Texture, cpType, "texture", "Texture", sceneProject, entities);

    RowSettings transparencySettings;
    transparencySettings.help = "Just for render ordering";
    propertyRowWithAutoButton(RowPropertyType::Bool, cpType, "transparent", "Transparent", "autoTransparency", "Auto Transparency", sceneProject, entities, transparencySettings);

    propertyRow(RowPropertyType::UInt, cpType, "maxPoints", "Max Points", sceneProject, entities, settingsInt);
    endTable();

    drawCustomShaderRow(cpType, ShaderType::POINTS, sceneProject, entities);

    if (entities.size() != 1) {
        separatorText("Points");
        ImGui::TextDisabled("Select a single GameObject to edit points");
        return;
    }

    Entity entity = entities[0];
    PointsComponent& pts = sceneProject->scene->getComponent<PointsComponent>(entity);

    separatorText("Points");

    beginTable(cpType, getLabelSize("Points"), "points_header");
    propertyHeader("Points", -1, false, false);
    ImGui::Text("%zu", pts.points.size());
    float pointsArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Point##points_add", pointsArrowWidth)) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (const Entity& selectedEntity : entities) {
            if (PointsComponent* ptsComp = sceneProject->scene->findComponent<PointsComponent>(selectedEntity)) {
                std::vector<PointData> newPoints = ptsComp->points;
                PointData newPt;
                if (!newPoints.empty()) {
                    newPt = newPoints.back();
                }
                newPoints.push_back(newPt);
                multiCmd->addPropertyCmd<std::vector<PointData>>(project, sceneProject->id, selectedEntity, cpType, "points", newPoints);
            }
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_points", pointsExpanded ? ImGuiDir_Up : ImGuiDir_Down)) {
        pointsExpanded = !pointsExpanded;
    }
    endTable();

    if (!pointsExpanded || pts.points.empty()) {
        return;
    }

    bool removedPoint = false;
    const size_t maxDrawnPoints = 256;
    const size_t drawnPoints = std::min(pts.points.size(), maxDrawnPoints);

    if (drawnPoints < pts.points.size()) {
        ImGui::TextDisabled("Showing first %zu of %zu points", drawnPoints, pts.points.size());
    }

    for (size_t i = 0; i < drawnPoints; i++) {
        ImGui::PushID((int)i);

        std::string pointGroupStr = "point_" + std::to_string(i);
        std::string pointLabel = "[" + std::to_string(i) + "] Point " + std::to_string(i);

        separatorText(pointLabel.c_str());

        beginTable(cpType, getLabelSize("Position"), pointGroupStr);
        propertyHeader("Point", -1, false, false);

        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
        ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);
        float arrowButtonWidth = ImGui::GetFrameHeight();
        float trailingWidth = deleteButtonSize.x + ImGui::GetStyle().ItemSpacing.x + arrowButtonWidth;
        float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingWidth);
        ImGui::SetCursorPosX(targetX);

        if (ImGui::ArrowButton("##toggle_point", pointsButtonGroups[pointGroupStr] ? ImGuiDir_Up : ImGuiDir_Down)) {
            pointsButtonGroups[pointGroupStr] = !pointsButtonGroups[pointGroupStr];
        }
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button(ICON_FA_TRASH_CAN "##delete_point", deleteButtonSize)) {
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (const Entity& selectedEntity : entities) {
                if (PointsComponent* ptsComp = sceneProject->scene->findComponent<PointsComponent>(selectedEntity)) {
                    if (i < ptsComp->points.size()) {
                        std::vector<PointData> newPoints = ptsComp->points;
                        newPoints.erase(newPoints.begin() + (long int)i);
                        multiCmd->addPropertyCmd<std::vector<PointData>>(project, sceneProject->id, selectedEntity, cpType, "points", newPoints);
                    }
                }
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            removedPoint = true;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            endTable();
            ImGui::PopID();
            break;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (pointsButtonGroups[pointGroupStr]) {
            std::string propPrefix = "points[" + std::to_string(i) + "]";
            propertyRow(RowPropertyType::Vector3, cpType, propPrefix + ".position", "Position", sceneProject, entities);
            propertyRow(RowPropertyType::Color4L, cpType, propPrefix + ".color", "Color", sceneProject, entities);
            propertyRow(RowPropertyType::Float, cpType, propPrefix + ".size", "Size", sceneProject, entities);
            propertyRow(RowPropertyType::Float, cpType, propPrefix + ".rotation", "Rotation", sceneProject, entities);
            propertyRow(RowPropertyType::Vector4, cpType, propPrefix + ".textureRect", "Texture Rect", sceneProject, entities);
            propertyRow(RowPropertyType::Bool, cpType, propPrefix + ".visible", "Visible", sceneProject, entities);
        }

        endTable();
        ImGui::PopID();
    }

    if (removedPoint) {
        return;
    }
}

// the Line Renderer's Segments mode: Max Lines and the A-B segment list (drawLineRendererComponent,
// InspectorLinesTrails.cpp, draws the rest)
void editor::InspectorWindow::drawLineSegmentsList(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings settingsInt;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();

    beginTable(cpType, getLabelSize("Max Lines"));
    propertyRow(RowPropertyType::UInt, cpType, "maxLines", "Max Lines", sceneProject, entities, settingsInt);
    endTable();

    if (entities.size() != 1) {
        separatorText("Lines");
        ImGui::TextDisabled("Select a single GameObject to edit lines");
        return;
    }

    Entity entity = entities[0];
    LineRendererComponent& lines = sceneProject->scene->getComponent<LineRendererComponent>(entity);

    separatorText("Lines");

    beginTable(cpType, getLabelSize("Lines"), "lines_header");
    propertyHeader("Lines", -1, false, false);
    ImGui::Text("%zu", lines.lines.size());
    float linesArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Line##lines_add", linesArrowWidth)) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (const Entity& selectedEntity : entities) {
            if (LineRendererComponent* linesComp = sceneProject->scene->findComponent<LineRendererComponent>(selectedEntity)) {
                std::vector<LineData> newLines = linesComp->lines;
                LineData newLine;
                if (!newLines.empty()) {
                    newLine = newLines.back();
                } else {
                    newLine.pointB = Vector3(1.0f, 0.0f, 0.0f);
                }
                newLines.push_back(newLine);
                multiCmd->addPropertyCmd<std::vector<LineData>>(project, sceneProject->id, selectedEntity, cpType, "lines", newLines);
            }
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_lines", linesExpanded ? ImGuiDir_Up : ImGuiDir_Down)) {
        linesExpanded = !linesExpanded;
    }
    endTable();

    if (!linesExpanded || lines.lines.empty()) {
        return;
    }

    bool removedLine = false;

    // endpoint sub-selected in the scene viewport (index = lineIndex * 2 + endpoint)
    int selectedLinePoint = -1;
    if (sceneProject->sceneRender && sceneProject->sceneRender->getSelectedLinePointEntity() == entity){
        selectedLinePoint = sceneProject->sceneRender->getSelectedLinePointIndex();
    }

    for (size_t i = 0; i < lines.lines.size(); i++) {
        ImGui::PushID((int)i);

        std::string lineGroupStr = "line_" + std::to_string(i);
        std::string lineLabel = "[" + std::to_string(i) + "] Line " + std::to_string(i);

        bool isLineSelected = (selectedLinePoint >= 0 && selectedLinePoint / 2 == (int)i);

        // sub-selected in the scene viewport: same marking as occluder points
        if (isLineSelected) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubSelectionText);
            separatorText((std::string(ICON_FA_CARET_RIGHT) + " " + lineLabel).c_str());
            ImGui::PopStyleColor();
        } else {
            separatorText(lineLabel.c_str());
        }

        beginTable(cpType, getLabelSize("Point A"), lineGroupStr);
        propertyHeader("Line", -1, false, false);

        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
        ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);
        float arrowButtonWidth = ImGui::GetFrameHeight();
        float trailingWidth = deleteButtonSize.x + ImGui::GetStyle().ItemSpacing.x + arrowButtonWidth;
        float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingWidth);
        ImGui::SetCursorPosX(targetX);

        if (ImGui::ArrowButton("##toggle_line", linesButtonGroups[lineGroupStr] ? ImGuiDir_Up : ImGuiDir_Down)) {
            linesButtonGroups[lineGroupStr] = !linesButtonGroups[lineGroupStr];
        }
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button(ICON_FA_TRASH_CAN "##delete_line", deleteButtonSize)) {
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (const Entity& selectedEntity : entities) {
                if (LineRendererComponent* linesComp = sceneProject->scene->findComponent<LineRendererComponent>(selectedEntity)) {
                    if (i < linesComp->lines.size()) {
                        std::vector<LineData> newLines = linesComp->lines;
                        newLines.erase(newLines.begin() + (long int)i);
                        multiCmd->addPropertyCmd<std::vector<LineData>>(project, sceneProject->id, selectedEntity, cpType, "lines", newLines);
                    }
                }
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            removedLine = true;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            endTable();
            ImGui::PopID();
            break;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (linesButtonGroups[lineGroupStr]) {
            std::string propPrefix = "lines[" + std::to_string(i) + "]";
            bool pointASelected = isLineSelected && (selectedLinePoint % 2 == 0);
            bool pointBSelected = isLineSelected && (selectedLinePoint % 2 == 1);
            propertyRow(RowPropertyType::Vector3, cpType, propPrefix + ".pointA", pointASelected ? (std::string(ICON_FA_CARET_RIGHT) + " Point A") : "Point A", sceneProject, entities);
            propertyRow(RowPropertyType::Color4L, cpType, propPrefix + ".colorA", "Color A", sceneProject, entities);
            propertyRow(RowPropertyType::Vector3, cpType, propPrefix + ".pointB", pointBSelected ? (std::string(ICON_FA_CARET_RIGHT) + " Point B") : "Point B", sceneProject, entities);
            propertyRow(RowPropertyType::Color4L, cpType, propPrefix + ".colorB", "Color B", sceneProject, entities);
        }

        endTable();
        ImGui::PopID();
    }

    if (removedLine) {
        return;
    }
}

void editor::InspectorWindow::drawPolygonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    if (entities.size() != 1) {
        separatorText("Vertices");
        ImGui::TextDisabled("Select a single GameObject to edit polygon vertices");
        return;
    }

    Entity entity = entities[0];
    PolygonComponent& polygon = sceneProject->scene->getComponent<PolygonComponent>(entity);

    separatorText("Vertices");

    beginTable(cpType, getLabelSize("Vertices"), "polygon_points_header");
    propertyHeader("Vertices", -1, false, false);
    ImGui::Text("%zu", polygon.points.size());
    float polygonArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Vertex##polygon_add", polygonArrowWidth)) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (const Entity& selectedEntity : entities) {
            if (PolygonComponent* polygonComp = sceneProject->scene->findComponent<PolygonComponent>(selectedEntity)) {
                std::vector<PolygonPoint> newPoints = polygonComp->points;
                PolygonPoint newPoint;
                if (!newPoints.empty()) {
                    newPoint = newPoints.back();
                }
                newPoints.push_back(newPoint);
                multiCmd->addPropertyCmd<std::vector<PolygonPoint>>(project, sceneProject->id, selectedEntity, cpType, "points", newPoints);
            }
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_polygon", polygonExpanded ? ImGuiDir_Up : ImGuiDir_Down)) {
        polygonExpanded = !polygonExpanded;
    }
    endTable();

    if (!polygonExpanded || polygon.points.empty()) {
        return;
    }

    // vertex sub-selected in the scene viewport (Polygon = non-mesh)
    int selectedVertex = -1;
    if (sceneProject->sceneRender && sceneProject->sceneRender->getSelectedPolygonPointEntity() == entity
        && !sceneProject->sceneRender->isSelectedPolygonPointMesh()){
        selectedVertex = sceneProject->sceneRender->getSelectedPolygonPointIndex();
    }

    bool removedPoint = false;

    for (size_t i = 0; i < polygon.points.size(); i++) {
        ImGui::PushID((int)i);

        std::string pointGroupStr = "polygon_point_" + std::to_string(i);
        std::string pointLabel = "[" + std::to_string(i) + "] Vertex " + std::to_string(i);

        bool vertexSelected = ((int)i == selectedVertex);

        // sub-selected in the scene viewport: same marking as occluder/line points
        if (vertexSelected) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubSelectionText);
            separatorText((std::string(ICON_FA_CARET_RIGHT) + " " + pointLabel).c_str());
            ImGui::PopStyleColor();
        } else {
            separatorText(pointLabel.c_str());
        }

        beginTable(cpType, getLabelSize("Position"), pointGroupStr);
        propertyHeader("Vertex", -1, false, false);

        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
        ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);
        float arrowButtonWidth = ImGui::GetFrameHeight();
        float trailingWidth = deleteButtonSize.x + ImGui::GetStyle().ItemSpacing.x + arrowButtonWidth;
        float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingWidth);
        ImGui::SetCursorPosX(targetX);

        if (ImGui::ArrowButton("##toggle_polygon_point", polygonButtonGroups[pointGroupStr] ? ImGuiDir_Up : ImGuiDir_Down)) {
            polygonButtonGroups[pointGroupStr] = !polygonButtonGroups[pointGroupStr];
        }
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button(ICON_FA_TRASH_CAN "##delete_polygon_point", deleteButtonSize)) {
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (const Entity& selectedEntity : entities) {
                if (PolygonComponent* polygonComp = sceneProject->scene->findComponent<PolygonComponent>(selectedEntity)) {
                    if (i < polygonComp->points.size()) {
                        std::vector<PolygonPoint> newPoints = polygonComp->points;
                        newPoints.erase(newPoints.begin() + (long int)i);
                        multiCmd->addPropertyCmd<std::vector<PolygonPoint>>(project, sceneProject->id, selectedEntity, cpType, "points", newPoints);
                    }
                }
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            removedPoint = true;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            endTable();
            ImGui::PopID();
            break;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (polygonButtonGroups[pointGroupStr]) {
            std::string propPrefix = "points[" + std::to_string(i) + "]";
            propertyRow(RowPropertyType::Vector3, cpType, propPrefix + ".position", vertexSelected ? (std::string(ICON_FA_CARET_RIGHT) + " Position") : "Position", sceneProject, entities);
            propertyRow(RowPropertyType::Color4L, cpType, propPrefix + ".color", "Color", sceneProject, entities);
        }

        endTable();
        ImGui::PopID();
    }

    if (removedPoint) {
        return;
    }
}

void editor::InspectorWindow::drawMeshPolygonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    beginTable(cpType, getLabelSize("Flip Y"));
    propertyRowWithAutoButton(RowPropertyType::Bool, cpType, "flipY", "Flip Y", "automaticFlipY", "Automatic Flip Y", sceneProject, entities);
    endTable();

    if (entities.size() == 1) {
        MeshPolygonComponent& polygon = sceneProject->scene->getComponent<MeshPolygonComponent>(entities[0]);
        separatorText("Size");
        beginTable(cpType, getLabelSize("Height"));
        propertyHeader("Width", -1, false, false);
        ImGui::Text("%u", polygon.width);
        propertyHeader("Height", -1, false, false);
        ImGui::Text("%u", polygon.height);
        endTable();
    }

    if (entities.size() != 1) {
        separatorText("Vertices");
        ImGui::TextDisabled("Select a single GameObject to edit mesh polygon vertices");
        return;
    }

    Entity entity = entities[0];
    MeshPolygonComponent& polygon = sceneProject->scene->getComponent<MeshPolygonComponent>(entity);

    separatorText("Vertices");

    beginTable(cpType, getLabelSize("Vertices"), "mesh_polygon_points_header");
    propertyHeader("Vertices", -1, false, false);
    ImGui::Text("%zu", polygon.points.size());
    float polygonArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Vertex##mesh_polygon_add", polygonArrowWidth)) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (const Entity& selectedEntity : entities) {
            if (MeshPolygonComponent* polygonComp = sceneProject->scene->findComponent<MeshPolygonComponent>(selectedEntity)) {
                std::vector<PolygonPoint> newPoints = polygonComp->points;
                PolygonPoint newPoint;
                if (!newPoints.empty()) {
                    newPoint = newPoints.back();
                }
                newPoints.push_back(newPoint);
                multiCmd->addPropertyCmd<std::vector<PolygonPoint>>(project, sceneProject->id, selectedEntity, cpType, "points", newPoints);
            }
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_mesh_polygon", meshPolygonExpanded ? ImGuiDir_Up : ImGuiDir_Down)) {
        meshPolygonExpanded = !meshPolygonExpanded;
    }
    endTable();

    if (!meshPolygonExpanded || polygon.points.empty()) {
        return;
    }

    // vertex sub-selected in the scene viewport (MeshPolygon = mesh)
    int selectedVertex = -1;
    if (sceneProject->sceneRender && sceneProject->sceneRender->getSelectedPolygonPointEntity() == entity
        && sceneProject->sceneRender->isSelectedPolygonPointMesh()){
        selectedVertex = sceneProject->sceneRender->getSelectedPolygonPointIndex();
    }

    bool removedPoint = false;

    for (size_t i = 0; i < polygon.points.size(); i++) {
        ImGui::PushID((int)i);

        std::string pointGroupStr = "mesh_polygon_point_" + std::to_string(i);
        std::string pointLabel = "[" + std::to_string(i) + "] Vertex " + std::to_string(i);

        bool vertexSelected = ((int)i == selectedVertex);

        // sub-selected in the scene viewport: same marking as occluder/line points
        if (vertexSelected) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubSelectionText);
            separatorText((std::string(ICON_FA_CARET_RIGHT) + " " + pointLabel).c_str());
            ImGui::PopStyleColor();
        } else {
            separatorText(pointLabel.c_str());
        }

        beginTable(cpType, getLabelSize("Position"), pointGroupStr);
        propertyHeader("Vertex", -1, false, false);

        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
        ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);
        float arrowButtonWidth = ImGui::GetFrameHeight();
        float trailingWidth = deleteButtonSize.x + ImGui::GetStyle().ItemSpacing.x + arrowButtonWidth;
        float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingWidth);
        ImGui::SetCursorPosX(targetX);

        if (ImGui::ArrowButton("##toggle_mesh_polygon_point", meshPolygonButtonGroups[pointGroupStr] ? ImGuiDir_Up : ImGuiDir_Down)) {
            meshPolygonButtonGroups[pointGroupStr] = !meshPolygonButtonGroups[pointGroupStr];
        }
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button(ICON_FA_TRASH_CAN "##delete_mesh_polygon_point", deleteButtonSize)) {
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (const Entity& selectedEntity : entities) {
                if (MeshPolygonComponent* polygonComp = sceneProject->scene->findComponent<MeshPolygonComponent>(selectedEntity)) {
                    if (i < polygonComp->points.size()) {
                        std::vector<PolygonPoint> newPoints = polygonComp->points;
                        newPoints.erase(newPoints.begin() + (long int)i);
                        multiCmd->addPropertyCmd<std::vector<PolygonPoint>>(project, sceneProject->id, selectedEntity, cpType, "points", newPoints);
                    }
                }
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            removedPoint = true;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            endTable();
            ImGui::PopID();
            break;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (meshPolygonButtonGroups[pointGroupStr]) {
            std::string propPrefix = "points[" + std::to_string(i) + "]";
            propertyRow(RowPropertyType::Vector3, cpType, propPrefix + ".position", vertexSelected ? (std::string(ICON_FA_CARET_RIGHT) + " Position") : "Position", sceneProject, entities);
            propertyRow(RowPropertyType::Color4L, cpType, propPrefix + ".color", "Color", sceneProject, entities);
        }

        endTable();
        ImGui::PopID();
    }

    if (removedPoint) {
        return;
    }
}

void editor::InspectorWindow::drawInstancedMeshComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings settingsUInt;
    settingsUInt.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings fadeSettings;
    fadeSettings.help = "Instances shrink toward their base between Fade Start and Fade End, using horizontal distance in the mesh's local space, instead of popping out. Fade End must be greater than Fade Start.";

    beginTable(cpType, getLabelSize("Cylindrical Billboard"));
    propertyRow(RowPropertyType::UInt, cpType, "maxInstances", "Max Instances", sceneProject, entities, settingsUInt);
    propertyRow(RowPropertyType::Bool, cpType, "instancedBillboard", "Billboard", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "instancedCylindricalBillboard", "Cylindrical Billboard", sceneProject, entities);
    RowSettings cullSettings;
    cullSettings.help = "Skip instances outside the camera and shadow views. Turn off for shaders that move instances away from their bounds.";
    propertyRow(RowPropertyType::Bool, cpType, "cullInstances", "Cull Instances", sceneProject, entities, cullSettings);
    RowSettings distanceSettings = settingsUInt;
    distanceSettings.help = "Drop instances farther than this from the main camera, in world units. Zero means unlimited. A hard cut, so hide it with fog. Requires Cull Instances.";
    propertyRow(RowPropertyType::FloatPositive, cpType, "cullDistance", "Cull Distance", sceneProject, entities, distanceSettings);
    propertyRow(RowPropertyType::Bool, cpType, "distanceFade", "Distance Fade", sceneProject, entities, fadeSettings);
    if (sceneProject->scene->getComponent<InstancedMeshComponent>(entities[0]).distanceFade) {
        propertyRow(RowPropertyType::FloatPositive, cpType, "fadeStart", "Fade Start", sceneProject, entities, settingsUInt);
        propertyRow(RowPropertyType::FloatPositive, cpType, "fadeEnd", "Fade End", sceneProject, entities, settingsUInt);
    }
    endTable();

    if (entities.size() != 1) {
        separatorText("Instances");
        ImGui::TextDisabled("Select a single GameObject to edit instances");
        return;
    }

    Entity entity = entities[0];
    InstancedMeshComponent& instmesh = sceneProject->scene->getComponent<InstancedMeshComponent>(entity);

    if (ProjectUtils::hasModelMeshChildrenWithoutRootGeometry(sceneProject->scene, entity)){
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped(
            ICON_FA_TRIANGLE_EXCLAMATION
            " Instances will not render: this model stores its geometry in child mesh GameObjects. "
            "Right-click the model in the Hierarchy and choose 'Merge static model'.");
        ImGui::PopStyleColor();
    }

    separatorText("Instances");

    beginTable(cpType, getLabelSize("Instances"), "instanced_mesh_instances_header");
    prefabRowProperty = "instances";
    propertyHeader("Instances", -1, false, false);
    ImGui::Text("%zu", instmesh.instances.size());
    float instancesArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Instance##instanced_mesh_add", instancesArrowWidth)) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (const Entity& selectedEntity : entities) {
            if (InstancedMeshComponent* instComp = sceneProject->scene->findComponent<InstancedMeshComponent>(selectedEntity)) {
                std::vector<InstanceData> newInstances = instComp->instances;
                InstanceData newInst;
                if (!newInstances.empty()) {
                    newInst = newInstances.back();
                }
                newInstances.push_back(newInst);
                multiCmd->addPropertyCmd<std::vector<InstanceData>>(project, sceneProject->id, selectedEntity, cpType, "instances", newInstances);
            }
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_instances", instancesExpanded ? ImGuiDir_Up : ImGuiDir_Down)) {
        instancesExpanded = !instancesExpanded;
    }
    endTable();

    if (!instancesExpanded || instmesh.instances.empty()) {
        return;
    }

    bool removedInstance = false;

    for (size_t i = 0; i < instmesh.instances.size(); i++) {
        ImGui::PushID((int)i);

        std::string instanceGroupStr = "instance_" + std::to_string(i);
        std::string instanceLabel = "[" + std::to_string(i) + "] Instance " + std::to_string(i);

        bool isInstanceSelected = sceneProject->sceneRender &&
            sceneProject->sceneRender->getSelectedInstanceIndex() == (int)i &&
            sceneProject->sceneRender->getSelectedInstanceEntity() == entities[0];

        // sub-selected in the scene viewport: same marking as occluder points
        if (isInstanceSelected) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubSelectionText);
            separatorText((std::string(ICON_FA_CARET_RIGHT) + " " + instanceLabel).c_str());
            ImGui::PopStyleColor();
        } else {
            separatorText(instanceLabel.c_str());
        }
        if (lastHeaderRowClicked()) {
            syncSubSelection(sceneProject, entities[0], -1, (int)i);
        }

        beginTable(cpType, getLabelSize("Position"), instanceGroupStr);
        propertyHeader("Instance", -1, false, false);

        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
        ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);
        float arrowButtonWidth = ImGui::GetFrameHeight();
        float trailingWidth = deleteButtonSize.x + ImGui::GetStyle().ItemSpacing.x + arrowButtonWidth;
        float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingWidth);
        ImGui::SetCursorPosX(targetX);

        if (ImGui::ArrowButton("##toggle_instance", instancesButtonGroups[instanceGroupStr] ? ImGuiDir_Up : ImGuiDir_Down)) {
            instancesButtonGroups[instanceGroupStr] = !instancesButtonGroups[instanceGroupStr];
        }
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button(ICON_FA_TRASH_CAN "##delete_instance", deleteButtonSize)) {
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (const Entity& selectedEntity : entities) {
                if (InstancedMeshComponent* instComp = sceneProject->scene->findComponent<InstancedMeshComponent>(selectedEntity)) {
                    if (i < instComp->instances.size()) {
                        std::vector<InstanceData> newInstances = instComp->instances;
                        newInstances.erase(newInstances.begin() + (long int)i);
                        multiCmd->addPropertyCmd<std::vector<InstanceData>>(project, sceneProject->id, selectedEntity, cpType, "instances", newInstances);
                    }
                }
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            removedInstance = true;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            endTable();
            ImGui::PopID();
            break;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (instancesButtonGroups[instanceGroupStr]) {
            std::string propPrefix = "instances[" + std::to_string(i) + "]";
            propertyRow(RowPropertyType::Vector3, cpType, propPrefix + ".position", "Position", sceneProject, entities);
            propertyRow(RowPropertyType::Quat, cpType, propPrefix + ".rotation", "Rotation", sceneProject, entities);
            propertyRow(RowPropertyType::Vector3, cpType, propPrefix + ".scale", "Scale", sceneProject, entities);
            propertyRow(RowPropertyType::Color4L, cpType, propPrefix + ".color", "Color", sceneProject, entities);
            propertyRow(RowPropertyType::Bool, cpType, propPrefix + ".visible", "Visible", sceneProject, entities);
        }

        endTable();
        ImGui::PopID();
    }

    if (removedInstance) {
        return;
    }
}

