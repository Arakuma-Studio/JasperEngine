// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector drawers of the point, line, polygon, instancing and particle renderers (element tables,
// sub-selection, frame lists, particle modules).

#include "InspectorWindow.h"
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
#include "component/ParticleSystemComponent.h"
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

void editor::InspectorWindow::drawLineRendererComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings settingsInt;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();

    beginTable(cpType, getLabelSize("Max Lines"));
    propertyRow(RowPropertyType::UInt, cpType, "maxLines", "Max Lines", sceneProject, entities, settingsInt);
    endTable();

    drawCustomShaderRow(cpType, ShaderType::LINES, sceneProject, entities);

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

void editor::InspectorWindow::drawParticleSystemComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings settingsFloat;
    settingsFloat.secondColSize = 6 * ImGui::GetFontSize();

    RowSettings settingsInt;
    settingsInt.secondColSize = 6 * ImGui::GetFontSize();

    beginTable(cpType, getLabelSize("Max Per Update"));
    propertyRow(RowPropertyType::UInt, cpType, "maxParticles", "Max Particles", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::Bool, cpType, "emitter", "Emitter", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "loop", "Loop", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "localSpace", "Local Space", sceneProject, entities);
    propertyRow(RowPropertyType::Int, cpType, "rate", "Rate", sceneProject, entities, settingsInt);
    propertyRow(RowPropertyType::Int, cpType, "maxPerUpdate", "Max Per Update", sceneProject, entities, settingsInt);
    endTable();

    // Bursts (single-entity editor; vector edits are not multi-entity safe)
    if (entities.size() == 1) {
        Scene* burstScene = sceneProject->scene;
        ParticleSystemComponent* bp = burstScene ? burstScene->findComponent<ParticleSystemComponent>(entities[0]) : nullptr;
        if (bp) {
            Entity entity = entities[0];
            PropertyData burstProp = Catalog::getProperty(sceneProject->scene, entity, cpType, "bursts");
            std::vector<ParticleBurst>* bursts = static_cast<std::vector<ParticleBurst>*>(burstProp.ref);
            std::vector<ParticleBurst>* defBursts = static_cast<std::vector<ParticleBurst>*>(burstProp.def);

            separatorText("Bursts");
            if (bursts) {
                bool defChanged = defBursts && *bursts != *defBursts;
                beginTable(cpType, getLabelSize("Bursts"), "particle_burst_header");
                if (propertyHeader("Bursts", -1, defChanged, false) && defBursts) {
                    setParticleBursts(cpType, sceneProject, entity, *defBursts);
                    finishProperty = true;
                }
                ImGui::Text("%zu", bursts->size());
                std::string addBurstLabel = ICON_FA_PLUS " Add Burst##particle_burst_add";
                ImGui::SameLine();
                float addBurstButtonWidth = std::max(0.0f, ImGui::GetContentRegionAvail().x);
                if (ImGui::Button(addBurstLabel.c_str(), ImVec2(addBurstButtonWidth, 0))) {
                    std::vector<ParticleBurst> newBursts = *bursts;
                    newBursts.push_back(ParticleBurst());
                    setParticleBursts(cpType, sceneProject, entity, newBursts);
                }
                endTable();
            }
            if (bursts && !bursts->empty()) {
                beginTable(cpType, getLabelSize("Max Count"), "particle_burst_table");
                for (size_t i = 0; i < bursts->size(); i++) {
                    ImGui::PushID((int)(1000 + i));
                    ParticleBurst burst = (*bursts)[i];

                    propertyHeader("Time " + std::to_string(i), -1, false, false);
                    ImGui::SetNextItemWidth(settingsFloat.secondColSize);
                    if (ImGui::DragFloat("##particle_burst_time", &burst.time, settingsFloat.stepSize, 0.0f, 0.0f, settingsFloat.format)) {
                        std::vector<ParticleBurst> newBursts = *bursts;
                        newBursts[i] = burst;
                        setParticleBursts(cpType, sceneProject, entity, newBursts);
                    }
                    float deleteButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    ImGui::SameLine();
                    float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - deleteButtonWidth);
                    ImGui::SetCursorPosX(targetX);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    bool removed = ImGui::Button(ICON_FA_TRASH_CAN "##particle_burst_delete");
                    ImGui::PopStyleColor(2);

                    propertyHeader("Min Count", -1, false, false);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputInt("##particle_burst_min", &burst.minCount)) {
                        std::vector<ParticleBurst> newBursts = *bursts;
                        newBursts[i] = burst;
                        setParticleBursts(cpType, sceneProject, entity, newBursts);
                    }

                    propertyHeader("Max Count", -1, false, false);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputInt("##particle_burst_max", &burst.maxCount)) {
                        std::vector<ParticleBurst> newBursts = *bursts;
                        newBursts[i] = burst;
                        setParticleBursts(cpType, sceneProject, entity, newBursts);
                    }

                    ImGui::PopID();

                    if (removed) {
                        std::vector<ParticleBurst> newBursts = *bursts;
                        newBursts.erase(newBursts.begin() + (std::ptrdiff_t)i);
                        setParticleBursts(cpType, sceneProject, entity, newBursts);
                        break;
                    }
                }
                endTable();
            }
        }
    } else {
        separatorText("Bursts");
        ImGui::TextDisabled("Select a single GameObject to edit bursts");
    }

    const float subsectionIndent = 10.0f;

    ImGui::Indent(subsectionIndent);
    ImGui::PushStyleColor(ImGuiCol_Header, Theme::Colors::NestedHeader);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Colors::NestedHeaderHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Theme::Colors::NestedHeaderActive);
    bool initializersOpen = ImGui::CollapsingHeader("Initializers");
    ImGui::PopStyleColor(3);
    if (initializersOpen) {
        ImGui::Unindent(subsectionIndent);

        separatorText("Life");
        beginTable(cpType, getLabelSize("Max Life"), "life_table");
        propertyRow(RowPropertyType::Float, cpType, "lifeInitializer.minLife", "Min Life", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "lifeInitializer.maxLife", "Max Life", sceneProject, entities, settingsFloat);
        endTable();

        separatorText("Position");
        RowSettings settingsShape;
        settingsShape.enumEntries = enumEntries(reflect::enums::kParticleEmitterShape);
        beginTable(cpType, getLabelSize("Cone Height"), "position_initializer_table");
        propertyRow(RowPropertyType::Enum, cpType, "positionInitializer.shape", "Emitter Shape", sceneProject, entities, settingsShape);

        ParticleEmitterShape shownShape = ParticleEmitterShape::Box;
        if (!entities.empty()) {
            ParticleSystemComponent* sp = sceneProject->scene->findComponent<ParticleSystemComponent>(entities[0]);
            if (sp) shownShape = sp->positionInitializer.shape;
        }

        if (shownShape == ParticleEmitterShape::Box) {
            propertyRow(RowPropertyType::Vector3, cpType, "positionInitializer.minPosition", "Min Position", sceneProject, entities);
            propertyRow(RowPropertyType::Vector3, cpType, "positionInitializer.maxPosition", "Max Position", sceneProject, entities);
        } else if (shownShape == ParticleEmitterShape::Sphere ||
                   shownShape == ParticleEmitterShape::Hemisphere ||
                   shownShape == ParticleEmitterShape::Circle) {
            propertyRow(RowPropertyType::Float, cpType, "positionInitializer.radius", "Radius", sceneProject, entities, settingsFloat);
            propertyRow(RowPropertyType::Float, cpType, "positionInitializer.innerRadius", "Inner Radius", sceneProject, entities, settingsFloat);
        } else if (shownShape == ParticleEmitterShape::Cone) {
            propertyRow(RowPropertyType::Float, cpType, "positionInitializer.coneAngle", "Cone Angle", sceneProject, entities, settingsFloat);
            propertyRow(RowPropertyType::Float, cpType, "positionInitializer.coneHeight", "Cone Height", sceneProject, entities, settingsFloat);
        }
        endTable();

        separatorText("Velocity");
        beginTable(cpType, getLabelSize("Max Velocity"), "velocity_initializer_table");
        propertyRow(RowPropertyType::Vector3, cpType, "velocityInitializer.minVelocity", "Min Velocity", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "velocityInitializer.maxVelocity", "Max Velocity", sceneProject, entities);
        endTable();

        separatorText("Acceleration");
        beginTable(cpType, getLabelSize("Max Acceleration"), "acceleration_initializer_table");
        propertyRow(RowPropertyType::Vector3, cpType, "accelerationInitializer.minAcceleration", "Min Acceleration", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "accelerationInitializer.maxAcceleration", "Max Acceleration", sceneProject, entities);
        endTable();

        separatorText("Color");
        beginTable(cpType, getLabelSize("Use sRGB"), "color_initializer_table");
        propertyRow(RowPropertyType::Color3L, cpType, "colorInitializer.minColor", "Min Color", sceneProject, entities);
        propertyRow(RowPropertyType::Color3L, cpType, "colorInitializer.maxColor", "Max Color", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "colorInitializer.useSRGB", "Use sRGB", sceneProject, entities);
        endTable();

        separatorText("Alpha");
        beginTable(cpType, getLabelSize("Max Alpha"), "alpha_initializer_table");
        propertyRow(RowPropertyType::Float, cpType, "alphaInitializer.minAlpha", "Min Alpha", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "alphaInitializer.maxAlpha", "Max Alpha", sceneProject, entities, settingsFloat);
        endTable();

        separatorText("Size");
        beginTable(cpType, getLabelSize("Max Size"), "size_initializer_table");
        propertyRow(RowPropertyType::Float, cpType, "sizeInitializer.minSize", "Min Size", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "sizeInitializer.maxSize", "Max Size", sceneProject, entities, settingsFloat);
        endTable();

        separatorText("Sprite");
        drawParticleFrameList(cpType, "spriteInitializer.frames", "sprite_initializer_frames", sceneProject, entities);

        separatorText("Rotation");
        beginTable(cpType, getLabelSize("Shortest Path"), "rotation_initializer_table");
        propertyRow(RowPropertyType::Quat, cpType, "rotationInitializer.minRotation", "Min Rotation", sceneProject, entities);
        propertyRow(RowPropertyType::Quat, cpType, "rotationInitializer.maxRotation", "Max Rotation", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "rotationInitializer.shortestPath", "Shortest Path", sceneProject, entities);
        endTable();

        separatorText("Scale");
        beginTable(cpType, getLabelSize("Max Scale"), "scale_initializer_table");
        propertyRow(RowPropertyType::Vector3, cpType, "scaleInitializer.minScale", "Min Scale", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "scaleInitializer.maxScale", "Max Scale", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "scaleInitializer.linearSort", "Linear Sort", sceneProject, entities);
        endTable();

        ImGui::Indent(subsectionIndent);
    }
    ImGui::Unindent(subsectionIndent);

    ImGui::Indent(subsectionIndent);
    ImGui::PushStyleColor(ImGuiCol_Header, Theme::Colors::NestedHeader);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Colors::NestedHeaderHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Theme::Colors::NestedHeaderActive);
    bool modifiersOpen = ImGui::CollapsingHeader("Modifiers");
    ImGui::PopStyleColor(3);
    if (modifiersOpen) {
        ImGui::Unindent(subsectionIndent);

        separatorText("Position");
        beginTable(cpType, getLabelSize("From Position"), "position_modifier_table");
        propertyRow(RowPropertyType::Float, cpType, "positionModifier.fromTime", "From Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "positionModifier.toTime", "To Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Ease, cpType, "positionModifier.function", "Ease", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "positionModifier.fromPosition", "From Position", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "positionModifier.toPosition", "To Position", sceneProject, entities);
        endTable();

        separatorText("Velocity");
        beginTable(cpType, getLabelSize("From Velocity"), "velocity_modifier_table");
        propertyRow(RowPropertyType::Float, cpType, "velocityModifier.fromTime", "From Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "velocityModifier.toTime", "To Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Ease, cpType, "velocityModifier.function", "Ease", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "velocityModifier.fromVelocity", "From Velocity", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "velocityModifier.toVelocity", "To Velocity", sceneProject, entities);
        endTable();

        separatorText("Acceleration");
        beginTable(cpType, getLabelSize("From Acceleration"), "acceleration_modifier_table");
        propertyRow(RowPropertyType::Float, cpType, "accelerationModifier.fromTime", "From Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "accelerationModifier.toTime", "To Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Ease, cpType, "accelerationModifier.function", "Ease", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "accelerationModifier.fromAcceleration", "From Acceleration", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "accelerationModifier.toAcceleration", "To Acceleration", sceneProject, entities);
        endTable();

        separatorText("Color");
        beginTable(cpType, getLabelSize("Mod sRGB"), "color_modifier_table");
        propertyRow(RowPropertyType::Float, cpType, "colorModifier.fromTime", "From Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "colorModifier.toTime", "To Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Ease, cpType, "colorModifier.function", "Ease", sceneProject, entities);
        propertyRow(RowPropertyType::Color3L, cpType, "colorModifier.fromColor", "From Color", sceneProject, entities);
        propertyRow(RowPropertyType::Color3L, cpType, "colorModifier.toColor", "To Color", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "colorModifier.useSRGB", "Mod sRGB", sceneProject, entities);
        endTable();

        if (entities.size() == 1) {
            Scene* gradScene = sceneProject->scene;
            ParticleSystemComponent* gp = gradScene ? gradScene->findComponent<ParticleSystemComponent>(entities[0]) : nullptr;
            if (gp) {
                Entity entity = entities[0];
                PropertyData gradProp = Catalog::getProperty(sceneProject->scene, entity, cpType, "colorGradient");
                ParticleColorGradient* gradient = static_cast<ParticleColorGradient*>(gradProp.ref);
                ParticleColorGradient* defGradient = static_cast<ParticleColorGradient*>(gradProp.def);

                separatorText("Color Gradient");
                if (gradient) {
                    bool defChanged = defGradient && !(*gradient == *defGradient);
                    beginTable(cpType, getLabelSize("Stops"), "particle_gradient_header");
                    if (propertyHeader("Stops", -1, defChanged, false) && defGradient) {
                        setParticleColorGradient(cpType, sceneProject, entity, *defGradient);
                        finishProperty = true;
                    }
                    ImGui::Text("%zu", gradient->stops.size());
                    std::string addStopLabel = ICON_FA_PLUS " Add Stop##particle_grad_add";
                    ImGui::SameLine();
                    float addStopButtonWidth = std::max(0.0f, ImGui::GetContentRegionAvail().x);
                    if (ImGui::Button(addStopLabel.c_str(), ImVec2(addStopButtonWidth, 0))) {
                        ParticleColorGradient newGrad = *gradient;
                        ParticleColorGradientStop stop;
                        if (!newGrad.stops.empty()) {
                            float last = newGrad.stops.back().time;
                            stop.time = std::min(1.0f, last + 0.25f);
                            stop.color = newGrad.stops.back().color;
                        }
                        newGrad.stops.push_back(stop);
                        setParticleColorGradient(cpType, sceneProject, entity, newGrad);
                    }
                    endTable();
                }
                if (gradient && !gradient->stops.empty()) {
                    beginTable(cpType, getLabelSize("Color N"), "particle_gradient_table");
                    for (size_t i = 0; i < gradient->stops.size(); i++) {
                        ImGui::PushID((int)(2000 + i));
                        ParticleColorGradientStop stop = gradient->stops[i];

                        propertyHeader("Time " + std::to_string(i), -1, false, false);
                        float deleteButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                        ImGui::SetNextItemWidth(-deleteButtonWidth - ImGui::GetStyle().ItemSpacing.x);
                        if (ImGui::SliderFloat("##particle_grad_time", &stop.time, 0.0f, 1.0f)) {
                            ParticleColorGradient newGrad = *gradient;
                            newGrad.stops[i] = stop;
                            setParticleColorGradient(cpType, sceneProject, entity, newGrad);
                        }
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                        bool removed = ImGui::Button(ICON_FA_TRASH_CAN "##particle_grad_delete");
                        ImGui::PopStyleColor(2);

                        propertyHeader("Color " + std::to_string(i), -1, false, false);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        float col[3] = { stop.color.x, stop.color.y, stop.color.z };
                        if (ImGui::ColorEdit3("##particle_grad_color", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
                            stop.color = Vector3(col[0], col[1], col[2]);
                            ParticleColorGradient newGrad = *gradient;
                            newGrad.stops[i] = stop;
                            setParticleColorGradient(cpType, sceneProject, entity, newGrad);
                        }

                        ImGui::PopID();

                        if (removed) {
                            ParticleColorGradient newGrad = *gradient;
                            newGrad.stops.erase(newGrad.stops.begin() + (std::ptrdiff_t)i);
                            setParticleColorGradient(cpType, sceneProject, entity, newGrad);
                            break;
                        }
                    }
                    endTable();
                }

                beginTable(cpType, getLabelSize("Grad sRGB"), "particle_gradient_options_table");
                propertyHeader("Grad sRGB", -1, false, false);
                bool useSRGB = gradient ? gradient->useSRGB : gp->colorGradient.useSRGB;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Checkbox("##particle_grad_srgb", &useSRGB)) {
                    ParticleColorGradient newGrad = gradient ? *gradient : gp->colorGradient;
                    newGrad.useSRGB = useSRGB;
                    setParticleColorGradient(cpType, sceneProject, entity, newGrad);
                }
                endTable();
            }
        } else {
            separatorText("Color Gradient");
            ImGui::TextDisabled("Select a single GameObject to edit color gradient");
        }

        separatorText("Alpha");
        beginTable(cpType, getLabelSize("From Alpha"), "alpha_modifier_table");
        propertyRow(RowPropertyType::Float, cpType, "alphaModifier.fromTime", "From Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "alphaModifier.toTime", "To Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Ease, cpType, "alphaModifier.function", "Ease", sceneProject, entities);
        propertyRow(RowPropertyType::Float, cpType, "alphaModifier.fromAlpha", "From Alpha", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "alphaModifier.toAlpha", "To Alpha", sceneProject, entities, settingsFloat);
        endTable();

        separatorText("Size");
        beginTable(cpType, getLabelSize("From Size"), "size_modifier_table");
        propertyRow(RowPropertyType::Float, cpType, "sizeModifier.fromTime", "From Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "sizeModifier.toTime", "To Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Ease, cpType, "sizeModifier.function", "Ease", sceneProject, entities);
        propertyRow(RowPropertyType::Float, cpType, "sizeModifier.fromSize", "From Size", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "sizeModifier.toSize", "To Size", sceneProject, entities, settingsFloat);
        endTable();

        separatorText("Sprite");
        beginTable(cpType, getLabelSize("From Time"), "sprite_modifier_table");
        propertyRow(RowPropertyType::Float, cpType, "spriteModifier.fromTime", "From Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "spriteModifier.toTime", "To Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Ease, cpType, "spriteModifier.function", "Ease", sceneProject, entities);
        endTable();
        drawParticleFrameList(cpType, "spriteModifier.frames", "sprite_modifier_frames", sceneProject, entities);

        separatorText("Rotation");
        beginTable(cpType, getLabelSize("Mod Shortest"), "rotation_modifier_table");
        propertyRow(RowPropertyType::Float, cpType, "rotationModifier.fromTime", "From Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "rotationModifier.toTime", "To Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Ease, cpType, "rotationModifier.function", "Ease", sceneProject, entities);
        propertyRow(RowPropertyType::Quat, cpType, "rotationModifier.fromRotation", "From Rotation", sceneProject, entities);
        propertyRow(RowPropertyType::Quat, cpType, "rotationModifier.toRotation", "To Rotation", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "rotationModifier.shortestPath", "Mod Shortest", sceneProject, entities);
        endTable();

        separatorText("Scale");
        beginTable(cpType, getLabelSize("From Scale"), "scale_modifier_table");
        propertyRow(RowPropertyType::Float, cpType, "scaleModifier.fromTime", "From Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "scaleModifier.toTime", "To Time", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Ease, cpType, "scaleModifier.function", "Ease", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "scaleModifier.fromScale", "From Scale", sceneProject, entities);
        propertyRow(RowPropertyType::Vector3, cpType, "scaleModifier.toScale", "To Scale", sceneProject, entities);
        endTable();

        ImGui::Indent(subsectionIndent);
    }
    ImGui::Unindent(subsectionIndent);
}

void editor::InspectorWindow::drawParticleFrameList(ComponentType cpType, const std::string& propertyId, const std::string& tableId, SceneProject* sceneProject, const std::vector<Entity>& entities) {
    if (entities.size() != 1) {
        ImGui::TextDisabled("Select a single GameObject to edit sprite frames");
        return;
    }

    Entity entity = entities[0];
    PropertyData prop = Catalog::getProperty(sceneProject->scene, entity, cpType, propertyId);
    std::vector<int>* frames = static_cast<std::vector<int>*>(prop.ref);
    std::vector<int>* defFrames = static_cast<std::vector<int>*>(prop.def);
    if (!frames) {
        return;
    }

    bool defChanged = defFrames && *frames != *defFrames;
    beginTable(cpType, getLabelSize("Frames"), tableId + "_header");
    prefabRowProperty = propertyId;
    if (propertyHeader("Frames", -1, defChanged, false) && defFrames) {
        setParticleFrames(cpType, propertyId, sceneProject, entity, *defFrames);
        finishProperty = true;
    }
    ImGui::Text("%zu", frames->size());
    std::string addFrameLabel = ICON_FA_PLUS " Add Frame##" + tableId;
    ImGui::SameLine();
    float addFrameButtonWidth = std::max(0.0f, ImGui::GetContentRegionAvail().x);
    if (ImGui::Button(addFrameLabel.c_str(), ImVec2(addFrameButtonWidth, 0))) {
        std::vector<int> newFrames = *frames;
        newFrames.push_back(newFrames.empty() ? 0 : newFrames.back() + 1);
        setParticleFrames(cpType, propertyId, sceneProject, entity, newFrames);
    }
    endTable();

    if (frames->empty()) {
        return;
    }

    const float frameInputWidth = 6.0f * ImGui::GetFontSize();

    beginTable(cpType, getLabelSize("Frame 000"), tableId + "_values");
    for (size_t i = 0; i < frames->size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        propertyHeader("Frame " + std::to_string(i), -1, false, false);

        float deleteButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(frameInputWidth);
        int newValue = (*frames)[i];
        if (ImGui::InputInt(("##particle_frame_value_" + tableId).c_str(), &newValue)) {
            std::vector<int> newFrames = *frames;
            newFrames[i] = newValue;
            setParticleFrames(cpType, propertyId, sceneProject, entity, newFrames);
        }

        ImGui::SameLine();
        float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - deleteButtonWidth);
        ImGui::SetCursorPosX(targetX);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Button((ICON_FA_TRASH_CAN "##particle_frame_delete_" + tableId).c_str())) {
            std::vector<int> newFrames = *frames;
            newFrames.erase(newFrames.begin() + static_cast<std::ptrdiff_t>(i));
            setParticleFrames(cpType, propertyId, sceneProject, entity, newFrames);
            ImGui::PopStyleColor(2);
            ImGui::PopID();
            break;
        }
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }
    endTable();
}
