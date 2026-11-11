// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The inspectors of the spline components (docs/specs/S18-splines.md R24): Spline (type, closed,
// up axis, the knot table synced with the Scene view's selection, Edit Spline and Draw, length,
// the Terrain foldout), Spline Extrude, Spline Instantiate and Spline Animate, with the
// generators' warnings naming the knot they concern.

#include "InspectorWindow.h"
#include "component/MeshComponent.h"
#include "object/spline/Spline.h"
#include "component/Transform.h"
#include "component/MeshComponent.h"

#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "external/IconsFontAwesome6.h"
#include "imgui_internal.h"
#include "reflect/Enums.h"
#include "spline/SplineEditor.h"
#include "spline/SplineEvaluator.h"
#include "subsystem/SplineSystem.h"
#include "util/SplineBakeObjects.h"
#include "util/TerrainSplineBake.h"
#include "window/TerrainEditWindow.h"

#include <algorithm>
#include <string>

using namespace jasperengine;

namespace {

    void warningText(const std::string& text){
        ImGui::TextColored(editor::Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text.c_str());
    }

    void drawSplineWarnings(const std::vector<SplineWarning>& warnings){
        for (const SplineWarning& warning : warnings){
            warningText(warning.message);
        }
    }

    void statsLine(const SplineBuildStats& stats, bool instances){
        if (stats.pending){
            ImGui::TextDisabled(ICON_FA_SPINNER " Building...");
            return;
        }
        if (instances){
            ImGui::TextDisabled("%u pieces in %u chunks, %.2f ms", stats.instances, stats.chunks, stats.buildMs);
        }else{
            ImGui::TextDisabled("%u vertices, %u triangles in %u chunks, %.2f ms", stats.vertices, stats.triangles, stats.chunks, stats.buildMs);
        }
    }

}

void editor::InspectorWindow::drawSplineComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    SplineEditor& editor = SplineEditor::get();
    const Entity entity = entities[0];
    const SplineComponent& spline = scene->getComponent<SplineComponent>(entity);
    const bool single = entities.size() == 1;

    RowSettings settings;
    settings.secondColSize = 7 * ImGui::GetFontSize();
    RowSettings settingsType;
    settingsType.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineType);
    settingsType.help = "Linear: straight segments; Catmull-Rom: smooth through every knot; Bezier: shaped by each knot's handles";
    RowSettings settingsClosed;
    settingsClosed.help = "The last knot joins the first (three knots or more)";

    beginTable(cpType, getLabelSize("Up Axis"));
    propertyRow(RowPropertyType::Enum, cpType, "type", "Type", sceneProject, entities, settingsType);
    propertyRow(RowPropertyType::Bool, cpType, "closed", "Closed", sceneProject, entities, settingsClosed);
    if (propertyHeader("Up Axis")){
        const bool is3D = spline.spline.upAxis().z == 0.0f;
        const char* labels[] = {"Y (3D)", "Z (2D, the XY plane)"};
        int current = is3D ? 0 : 1;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##upaxis", &current, labels, 2) && single){
            SplineEditState state = SplineEditCmd::capture(spline);
            state.upAxis = current == 0 ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
            SplineEditor::setState(project, sceneProject->id, entity, std::move(state), SplineEditKind::Edit);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("+Y for 3D splines; +Z for 2D splines drawn in the XY plane");
    }
    endTable();

    if (!single){
        return;
    }

    // the tools
    const bool editing = editor.isEditing(sceneProject->id, entity);
    if (editing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_FA_BEZIER_CURVE " Edit Spline")){
        if (editing){
            editor.endEdit(project);
        }else{
            editor.beginEdit(project, sceneProject->id, entity);
        }
    }
    if (editing) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select and move knots in the Scene view (Esc leaves)");
    ImGui::SameLine();
    const bool drawing = editor.isDrawing() && editor.getDrawEntity() == entity;
    if (drawing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_FA_PEN " Draw")){
        if (drawing){
            editor.cancelDraw();
        }else{
            editor.beginDraw(sceneProject->id, entity);
        }
    }
    if (drawing) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draw the spline in the Scene view (a stroke, or a knot per click); Enter finishes, Esc cancels");
    ImGui::SameLine();
    {
        SplineEditor::DrawSettings& draw = editor.getDrawSettings();
        int mode = draw.mode == SplineDrawMode::Freehand ? 0 : 1;
        const char* modes[] = {"Freehand", "Click"};
        ImGui::SetNextItemWidth(6 * ImGui::GetFontSize());
        if (ImGui::Combo("##drawmode", &mode, modes, 2)){
            draw.mode = mode == 0 ? SplineDrawMode::Freehand : SplineDrawMode::Click;
        }
        ImGui::SameLine();
        int snap = static_cast<int>(draw.snap);
        const char* snaps[] = {"No Snap", "Snap to Terrain", "Snap to Surface"};
        ImGui::SetNextItemWidth(8 * ImGui::GetFontSize());
        if (ImGui::Combo("##drawsnap", &snap, snaps, 3)){
            draw.snap = static_cast<SplineSnap>(snap);
        }
    }

    std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>();
    const Spline handle(scene, entity);
    ImGui::Text("Length: %.2f", handle.getLength());
    if (!editor.getStatus().empty() && editing){
        warningText(editor.getStatus());
    }

    // the knot table: rows select (Shift/Ctrl add or toggle), values edit in local space
    const std::vector<spline::SplineKnot>& knots = spline.spline.knots();
    const bool bezier = spline.spline.type() == spline::SplineType::CubicBezier;
    if (ImGui::CollapsingHeader("Knots", ImGuiTreeNodeFlags_DefaultOpen)){
        const int columns = bezier ? 6 : 4;
        if (ImGui::BeginTable("##splineknots", columns, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_ScrollY, ImVec2(0.0f, std::min(12.0f, 1.5f + static_cast<float>(knots.size())) * ImGui::GetFrameHeightWithSpacing()))){
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 2.0f * ImGui::GetFontSize());
            ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Radius", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Roll", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            if (bezier){
                ImGui::TableSetupColumn("Tangent Out", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            }
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < knots.size(); i++){
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = editing && editor.isSelected(static_cast<uint32_t>(i));
                if (ImGui::Selectable(std::to_string(i).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)){
                    if (!editing){
                        editor.beginEdit(project, sceneProject->id, entity);
                    }
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyCtrl || io.KeyShift){
                        editor.toggleSelected(static_cast<uint32_t>(i));
                    }else{
                        editor.setSelection({static_cast<uint32_t>(i)});
                    }
                }
                // one edit of the knot: merged while the field is held, one step after
                spline::SplineKnot knot = knots[i];
                bool changed = false;
                bool done = false;
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                changed |= ImGui::DragFloat3("##p", &knot.position.x, 0.05f);
                done |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                changed |= ImGui::DragFloat("##r", &knot.radius, 0.02f, 0.0f, 1.0e6f);
                done |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                changed |= ImGui::DragFloat("##roll", &knot.rollDeg, 0.5f);
                done |= ImGui::IsItemDeactivatedAfterEdit();
                if (bezier){
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat3("##out", &knot.tangentOut.x, 0.02f)){
                        changed = true;
                        spline::applyTangentMode(knot, spline::SplineHandle::Out);
                    }
                    done |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::TableNextColumn();
                    int mode = static_cast<int>(knot.mode);
                    const char* modes[] = {"Mirrored", "Aligned", "Broken"};
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##mode", &mode, modes, 3)){
                        knot.mode = static_cast<spline::TangentMode>(mode);
                        spline::applyTangentMode(knot, spline::SplineHandle::Out);
                        changed = true;
                        done = true;
                    }
                }
                if (changed){
                    knot.radius = std::max(0.0f, knot.radius);
                    SplineEditState state = SplineEditCmd::capture(spline);
                    state.knots[i] = knot;
                    SplineEditor::setState(project, sceneProject->id, entity, std::move(state), SplineEditKind::Edit, true,
                                           {static_cast<uint32_t>(i)});
                }
                if (done){
                    SplineEditor::endContinuousEdit(sceneProject->id);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(ICON_FA_PLUS " Add Knot")){
            SplineEditState state = SplineEditCmd::capture(spline);
            spline::SplineKnot knot = state.knots.empty() ? spline::SplineKnot() : state.knots.back();
            if (state.knots.size() >= 2){
                knot.position = state.knots.back().position * 2.0f - state.knots[state.knots.size() - 2].position;
            }else if (state.knots.size() == 1){
                knot.position = state.knots.back().position + Vector3(4.0f, 0.0f, 0.0f);
            }
            state.knots.push_back(knot);
            SplineEditor::setState(project, sceneProject->id, entity, std::move(state), SplineEditKind::InsertKnot);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!editing || editor.getSelection().empty());
        if (ImGui::Button(ICON_FA_TRASH " Delete Selected")){
            editor.deleteSelected(project);
        }
        ImGui::EndDisabled();
    }

    // Terrain: the destructive bake (S18 R11, R12)
    if (ImGui::CollapsingHeader("Terrain")){
        RowSettings settingsRegion;
        settingsRegion.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineTerrainRegion);
        settingsRegion.help = "Path: a band along the curve; Area: the inside of a closed spline";
        RowSettings settingsMode;
        settingsMode.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineFlattenMode);
        settingsMode.help = "Flatten raises and lowers; Cut Only only lowers; Fill Only only raises";
        RowSettings settingsUnits = settings;
        RowSettings settings01;
        settings01.floatMin = 0.0f;
        settings01.floatMax = 1.0f;
        beginTable(cpType, getLabelSize("Paint Strength"), "terrain");
        propertyRow(RowPropertyType::Enum, cpType, "terrain.region", "Region", sceneProject, entities, settingsRegion);
        propertyRow(RowPropertyType::Float, cpType, "terrain.extraWidth", "Extra Width", sceneProject, entities, settingsUnits);
        propertyRow(RowPropertyType::FloatPositive, cpType, "terrain.falloff", "Falloff", sceneProject, entities, settingsUnits);
        propertyRow(RowPropertyType::Bool, cpType, "terrain.flatten", "Flatten", sceneProject, entities);
        propertyRow(RowPropertyType::Enum, cpType, "terrain.flattenMode", "Mode", sceneProject, entities, settingsMode);
        propertyRow(RowPropertyType::Float, cpType, "terrain.heightOffset", "Height Offset", sceneProject, entities, settingsUnits);
        propertyRow(RowPropertyType::Bool, cpType, "terrain.paint", "Paint", sceneProject, entities);
        propertyRow(RowPropertyType::UInt, cpType, "terrain.paintLayer", "Paint Layer", sceneProject, entities);
        propertyRow(RowPropertyType::Float_0_1, cpType, "terrain.paintStrength", "Paint Strength", sceneProject, entities, settings01);
        propertyRow(RowPropertyType::FloatPositive, cpType, "terrain.paintFalloff", "Paint Falloff", sceneProject, entities, settingsUnits);
        propertyRow(RowPropertyType::Float_0_1, cpType, "terrain.paintNoise", "Edge Noise", sceneProject, entities, settings01);
        propertyRow(RowPropertyType::Bool, cpType, "terrain.clearFoliage", "Clear Foliage", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "terrain.clearTrees", "Clear Trees", sceneProject, entities);
        propertyRow(RowPropertyType::UInt, cpType, "terrain.foliageMask", "Foliage Layer Mask", sceneProject, entities);
        propertyRow(RowPropertyType::LocalEntity, cpType, "terrain.terrain", "Terrain", sceneProject, entities);
        endTable();

        const Entity terrain = TerrainSplineBake::findTerrain(scene, entity);
        ImGui::BeginDisabled(terrain == NULL_ENTITY);
        if (ImGui::Button(ICON_FA_MOUNTAIN " Apply to Terrain")){
            TerrainBakeChanges changes;
            std::string error;
            if (!TerrainSplineBake::apply(project, sceneProject->id, entity, changes, error)){
                splineBakeMessage = error;
            }else{
                splineBakeMessage.clear();
                for (const std::string& warning : changes.warnings){
                    splineBakeMessage += warning + "\n";
                }
                if (changes.empty()) splineBakeMessage += "Nothing changed.";
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
            ImGui::SetTooltip(terrain == NULL_ENTITY ? "No terrain under the spline" : "Flattens, paints and clears the terrain along the spline (one undo step)");
        }
        ImGui::SameLine();
        bool live = editor.isLiveTerrain();
        if (ImGui::Checkbox("Live Terrain", &live)){
            editor.setLiveTerrain(project, live);
        }
        if (ImGui::IsItemHovered()){
            ImGui::SetTooltip("While editing the spline, the terrain is baked again after every change from the terrain as the edit "
                              "found it; leaving Edit Spline commits one undo step. Later bakes start from the terrain as it then is.");
        }
        if (terrain != NULL_ENTITY && TerrainSplineBake::isHeightMap8Bit(scene, terrain)){
            warningText("The heightmap is 8-bit: a flattened bed steps visibly.");
            if (ImGui::Button("Convert heightmap to 16-bit")){
                std::string error;
                if (TerrainEditWindow* window = Backend::getApp().getTerrainEditWindow()){
                    if (!window->convertHeightMapTo16Bit(sceneProject->id, terrain, error)){
                        splineBakeMessage = error;
                    }
                }
            }
        }
        if (!splineBakeMessage.empty()){
            warningText(splineBakeMessage);
        }
    }

    const Transform* transform = scene->findComponent<Transform>(entity);
    if (transform){
        const Vector3& s = transform->worldScale;
        const float m = std::max(std::fabs(s.x), std::max(std::fabs(s.y), std::fabs(s.z)));
        if (std::fabs(std::fabs(s.x) - m) > m * 1.0e-4f || std::fabs(std::fabs(s.y) - m) > m * 1.0e-4f || std::fabs(std::fabs(s.z) - m) > m * 1.0e-4f){
            warningText("The GameObject's scale is not uniform: the radius uses its largest axis.");
        }
    }
    (void)system;
}

void editor::InspectorWindow::drawSplineExtrudeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const Entity entity = entities[0];
    const SplineExtrudeComponent& extrude = scene->getComponent<SplineExtrudeComponent>(entity);

    RowSettings settings;
    settings.secondColSize = 7 * ImGui::GetFontSize();
    RowSettings settingsShape;
    settingsShape.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineExtrudeShape);
    settingsShape.help = "Flat: a ribbon (2D: an edge); Bevel: a path; Crown: a road; Rectangle: a wall; Tube: a pipe; Custom: your profile; "
                         "Fill: a closed spline's flat area; Water Banks: a river or lake filled to its banks over the terrain";
    RowSettings settingsWidth;
    settingsWidth.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineWidthSource);
    settingsWidth.help = "Radius: each knot's radius (half the width); Constant: the Width below";
    RowSettings settingsGrade;
    settingsGrade.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineGrade);
    settingsGrade.help = "Racked: tilts with the ground; Stepped: level runs that step at the knots";
    RowSettings settingsCorners;
    settingsCorners.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineCornerStyle);
    settingsCorners.help = "At the knots of a Linear spline: sharp (past 120 degrees built Round) or round";
    RowSettings settingsConform;
    settingsConform.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineConform);
    settingsConform.help = "Follow the terrain under the spline: its height, or its height and slope";
    RowSettings settingsCollision;
    settingsCollision.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineCollision);
    settingsCollision.help = "Mesh: static colliders of the generated mesh (3D); Chain 2D: a Box2D chain of the centreline";
    RowSettings settings01;
    settings01.floatMin = 0.0f;
    settings01.floatMax = 1.0f;

    const SplineExtrudeShape shape = extrude.shape;
    const bool swept = shape != SplineExtrudeShape::Fill && shape != SplineExtrudeShape::WaterBanks;
    beginTable(cpType, getLabelSize("Tiles Per Metre U"));
    propertyRow(RowPropertyType::Enum, cpType, "shape", "Shape", sceneProject, entities, settingsShape);
    if (swept){
        propertyRow(RowPropertyType::Enum, cpType, "widthSource", "Width From", sceneProject, entities, settingsWidth);
        propertyRow(RowPropertyType::FloatPositive, cpType, "width", "Width", sceneProject, entities, settings);
    }
    if (shape == SplineExtrudeShape::Bevel){
        propertyRow(RowPropertyType::Float, cpType, "edgeDrop", "Edge Drop", sceneProject, entities, settings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "edgeInset", "Edge Inset", sceneProject, entities, settings);
    }else if (shape == SplineExtrudeShape::Crown){
        propertyRow(RowPropertyType::Float, cpType, "crownRise", "Crown Rise", sceneProject, entities, settings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "shoulderWidth", "Shoulder Width", sceneProject, entities, settings);
        propertyRow(RowPropertyType::Float, cpType, "shoulderDrop", "Shoulder Drop", sceneProject, entities, settings);
    }else if (shape == SplineExtrudeShape::Rectangle){
        propertyRow(RowPropertyType::FloatPositive, cpType, "thickness", "Thickness", sceneProject, entities, settings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "height", "Height", sceneProject, entities, settings);
        propertyRow(RowPropertyType::Enum, cpType, "grade", "Grade", sceneProject, entities, settingsGrade);
        if (extrude.grade == SplineGrade::Stepped){
            propertyRow(RowPropertyType::FloatPositive, cpType, "stepHeight", "Step Height", sceneProject, entities, settings);
        }
        propertyRow(RowPropertyType::Bool, cpType, "levelCourseV", "Level Courses", sceneProject, entities);
    }else if (shape == SplineExtrudeShape::Tube){
        propertyRow(RowPropertyType::UInt, cpType, "tubeSides", "Sides", sceneProject, entities);
    }else if (shape == SplineExtrudeShape::Custom){
        propertyRow(RowPropertyType::Bool, cpType, "customClosed", "Closed Profile", sceneProject, entities);
    }else if (shape == SplineExtrudeShape::WaterBanks){
        propertyRow(RowPropertyType::FloatPositive, cpType, "maxHalfWidth", "Max Half Width", sceneProject, entities, settings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "waterEdgeDrop", "Edge Drop", sceneProject, entities, settings);
        propertyRow(RowPropertyType::Bool, cpType, "seaLevelFloor", "Sea Level Floor", sceneProject, entities);
        if (extrude.seaLevelFloor){
            propertyRow(RowPropertyType::Float, cpType, "seaLevel", "Sea Level", sceneProject, entities, settings);
        }
    }
    if (shape == SplineExtrudeShape::Flat || shape == SplineExtrudeShape::WaterBanks){
        propertyRow(RowPropertyType::Bool, cpType, "flow", "Flow (Water)", sceneProject, entities);
        if (extrude.flow){
            propertyRow(RowPropertyType::Float_0_1, cpType, "flowSpeed", "Flow Speed", sceneProject, entities, settings01);
        }
    }
    propertyRow(RowPropertyType::FloatPositive, cpType, "tilesPerMetreU", "Tiles Per Metre U", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "tilesPerMetreV", "Tiles Per Metre V", sceneProject, entities, settings);
    if (swept){
        propertyRow(RowPropertyType::Enum, cpType, "corners", "Corners", sceneProject, entities, settingsCorners);
        propertyRow(RowPropertyType::Bool, cpType, "endCaps", "End Caps", sceneProject, entities);
        propertyRow(RowPropertyType::FloatPositive, cpType, "endTaper", "End Taper", sceneProject, entities, settings);
        propertyRow(RowPropertyType::Float, cpType, "lateralOffset", "Lateral Offset", sceneProject, entities, settings);
    }
    propertyRow(RowPropertyType::Float, cpType, "verticalOffset", "Vertical Offset", sceneProject, entities, settings);
    if (swept || shape == SplineExtrudeShape::WaterBanks){
        propertyRow(RowPropertyType::Enum, cpType, "conform", "Conform", sceneProject, entities, settingsConform);
        if (extrude.conform == SplineConform::TerrainHeightAndSlope){
            propertyRow(RowPropertyType::Float_0_1, cpType, "slopeBlend", "Slope Blend", sceneProject, entities, settings01);
        }
        propertyRow(RowPropertyType::FloatPositive, cpType, "stationSpacing", "Station Spacing", sceneProject, entities, settings);
    }
    propertyRow(RowPropertyType::Enum, cpType, "collision", "Collision", sceneProject, entities, settingsCollision);
    endTable();

    if (entities.size() != 1){
        return;
    }

    // Custom: the profile's points (x across in half-widths, y up)
    if (shape == SplineExtrudeShape::Custom){
        ImGui::SeparatorText("Profile Points (at most 32)");
        std::vector<SplineProfilePoint> points = extrude.customProfile;
        auto setPoints = [&](const std::vector<SplineProfilePoint>& value){
            auto* cmd = new editor::PropertyCmd<std::vector<SplineProfilePoint>>(project, sceneProject->id, entity, cpType, "customProfile", value);
            cmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        };
        for (size_t i = 0; i < points.size(); i++){
            ImGui::PushID(static_cast<int>(i));
            SplineProfilePoint point = points[i];
            ImGui::SetNextItemWidth(-10.0f * ImGui::GetFontSize());
            bool changed = ImGui::DragFloat2("##p", &point.position.x, 0.01f);
            const bool done = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Hard", &point.hard);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Grounded", &point.grounded);
            points[i] = point;
            if ((changed && !ImGui::IsItemActive()) || done){
                setPoints(points);
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_XMARK)){
                points.erase(points.begin() + static_cast<std::ptrdiff_t>(i));
                setPoints(points);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::BeginDisabled(points.size() >= 32);
        if (ImGui::Button(ICON_FA_PLUS " Add Point")){
            points.push_back(SplineProfilePoint());
            setPoints(points);
        }
        ImGui::EndDisabled();
    }

    if (!scene->findComponent<MeshComponent>(entity)){
        warningText("Spline Extrude draws with this GameObject's Mesh (material, shadows): add a Mesh.");
    }
    if (!scene->findComponent<SplineComponent>(entity)){
        warningText("Spline Extrude sweeps the Spline of its GameObject: add a Spline.");
    }
    if (std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>()){
        statsLine(system->getExtrudeStats(entity), false);
        drawSplineWarnings(system->getExtrudeWarnings(entity));
    }
}

void editor::InspectorWindow::drawSplineInstantiateComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const Entity entity = entities[0];
    const SplineInstantiateComponent& instantiate = scene->getComponent<SplineInstantiateComponent>(entity);

    RowSettings settings;
    settings.secondColSize = 7 * ImGui::GetFontSize();
    RowSettings settingsMode;
    settingsMode.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineInstantiateMode);
    settingsMode.help = "Spacing: a piece every Spacing; Tile: pieces end to end by their bounds; Fence: posts and spans";
    RowSettings settingsFit;
    settingsFit.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineSpacingFit);
    settingsFit.help = "Fit To Length: stretched to land on both ends; Fixed Pitch: exactly the spacing";
    RowSettings settingsGrade;
    settingsGrade.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineGrade);
    settingsGrade.help = "Racked: spans tilt with the ground; Stepped: spans stay level and step at the posts";
    RowSettings settingsPlant;
    settingsPlant.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplinePlantMode);
    settingsPlant.help = "Bounds Min: the piece's bottom on the curve; Pivot: its pivot on the curve";
    RowSettings settingsConform;
    settingsConform.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineConform);
    RowSettings settings01;
    settings01.floatMin = 0.0f;
    settings01.floatMax = 1.0f;

    const bool fence = instantiate.mode == SplineInstantiateMode::Fence;
    beginTable(cpType, getLabelSize("Span Max Stretch"));
    propertyRow(RowPropertyType::Enum, cpType, "mode", "Mode", sceneProject, entities, settingsMode);
    if (!fence){
        if (instantiate.mode == SplineInstantiateMode::Spacing){
            propertyRow(RowPropertyType::FloatPositive, cpType, "spacing", "Spacing", sceneProject, entities, settings);
        }
        propertyRow(RowPropertyType::Enum, cpType, "fit", "Fit", sceneProject, entities, settingsFit);
    }else{
        propertyRow(RowPropertyType::FloatPositive, cpType, "postPitch", "Post Pitch", sceneProject, entities, settings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "spanMaxStretch", "Span Max Stretch", sceneProject, entities, settings);
        propertyRow(RowPropertyType::Enum, cpType, "fenceGrade", "Grade", sceneProject, entities, settingsGrade);
        propertyRow(RowPropertyType::FloatPositive, cpType, "crestPitch", "Crest Pitch", sceneProject, entities, settings);
    }
    propertyRow(RowPropertyType::Float, cpType, "lateralOffset", "Lateral Offset", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Float, cpType, "verticalOffset", "Vertical Offset", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "spacingJitter", "Spacing Jitter", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "yawJitter", "Yaw Jitter", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "lateralJitter", "Lateral Jitter", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Vector2, cpType, "scaleRange", "Scale Range", sceneProject, entities);
    propertyRow(RowPropertyType::Float_0_1, cpType, "dropout", "Dropout", sceneProject, entities, settings01);
    propertyRow(RowPropertyType::UInt, cpType, "seed", "Seed", sceneProject, entities);
    propertyRow(RowPropertyType::FloatPositive, cpType, "endTrim", "End Trim", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Enum, cpType, "plantMode", "Plant", sceneProject, entities, settingsPlant);
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxTilt", "Max Tilt", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Enum, cpType, "conform", "Conform", sceneProject, entities, settingsConform);
    if (instantiate.conform == SplineConform::TerrainHeightAndSlope){
        propertyRow(RowPropertyType::Float_0_1, cpType, "slopeBlend", "Slope Blend", sceneProject, entities, settings01);
    }
    propertyRow(RowPropertyType::FloatPositive, cpType, "stationSpacing", "Station Spacing", sceneProject, entities, settings);
    endTable();

    if (entities.size() != 1){
        return;
    }

    // the pools: a model or prefab and its weight, at most six each
    auto drawPool = [&](const char* name, const char* title){
        const std::vector<SplinePoolEntry>* pool = Catalog::getPropertyRef<std::vector<SplinePoolEntry>>(scene, entity, cpType, name);
        if (!pool){
            return;
        }
        ImGui::SeparatorText(title);
        beginTable(cpType, getLabelSize("Weight"), name);
        for (size_t i = 0; i < pool->size(); i++){
            const std::string prefix = std::string(name) + "[" + std::to_string(i) + "]";
            propertyRow(RowPropertyType::Asset, cpType, prefix + ".asset", "Piece " + std::to_string(i), sceneProject, entities);
            propertyRow(RowPropertyType::FloatPositive, cpType, prefix + ".weight", "Weight", sceneProject, entities, settings);
        }
        endTable();
        ImGui::PushID(name);
        ImGui::BeginDisabled(pool->size() >= SplineInstantiateComponent::MAX_POOL_ENTRIES);
        if (ImGui::Button(ICON_FA_PLUS " Add")){
            std::vector<SplinePoolEntry> value = *pool;
            value.push_back(SplinePoolEntry());
            auto* cmd = new editor::PropertyCmd<std::vector<SplinePoolEntry>>(project, sceneProject->id, entity, cpType, name, value);
            cmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(pool->empty());
        if (ImGui::Button(ICON_FA_MINUS " Remove Last")){
            std::vector<SplinePoolEntry> value = *pool;
            value.pop_back();
            auto* cmd = new editor::PropertyCmd<std::vector<SplinePoolEntry>>(project, sceneProject->id, entity, cpType, name, value);
            cmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    };
    if (fence){
        drawPool("posts", "Posts");
        drawPool("spans", "Spans");
        drawPool("gates", "Gates");
        drawPool("crests", "Crests");
        drawPool("caps", "Caps");

        ImGui::SeparatorText("Span Overrides (at most 16)");
        RowSettings settingsKind;
        settingsKind.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineSpanOverrideKind);
        settingsKind.help = "Gate: a piece of the gates pool; Piece: this piece of the spans pool";
        beginTable(cpType, getLabelSize("Piece"), "overrides");
        for (size_t i = 0; i < instantiate.spanOverrides.size(); i++){
            const std::string prefix = "spanOverrides[" + std::to_string(i) + "]";
            propertyRow(RowPropertyType::UInt, cpType, prefix + ".knot", "Knot", sceneProject, entities);
            propertyRow(RowPropertyType::UInt, cpType, prefix + ".span", "Span", sceneProject, entities);
            propertyRow(RowPropertyType::Enum, cpType, prefix + ".kind", "Kind", sceneProject, entities, settingsKind);
            propertyRow(RowPropertyType::UInt, cpType, prefix + ".piece", "Piece", sceneProject, entities);
        }
        endTable();
        ImGui::BeginDisabled(instantiate.spanOverrides.size() >= SplineInstantiateComponent::MAX_SPAN_OVERRIDES);
        if (ImGui::Button(ICON_FA_PLUS " Add Override")){
            std::vector<SplineSpanOverride> value = instantiate.spanOverrides;
            value.push_back(SplineSpanOverride());
            auto* cmd = new editor::PropertyCmd<std::vector<SplineSpanOverride>>(project, sceneProject->id, entity, cpType, "spanOverrides", value);
            cmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(instantiate.spanOverrides.empty());
        if (ImGui::Button(ICON_FA_MINUS " Remove Last##override")){
            std::vector<SplineSpanOverride> value = instantiate.spanOverrides;
            value.pop_back();
            auto* cmd = new editor::PropertyCmd<std::vector<SplineSpanOverride>>(project, sceneProject->id, entity, cpType, "spanOverrides", value);
            cmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
        ImGui::EndDisabled();
    }else{
        drawPool("items", "Pieces");
    }

    if (!scene->findComponent<SplineComponent>(entity)){
        warningText("Spline Instantiate places along the Spline of its GameObject: add a Spline.");
    }
    if (std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>()){
        statsLine(system->getInstantiateStats(entity), true);
        drawSplineWarnings(system->getInstantiateWarnings(entity));
    }
    if (ImGui::Button(ICON_FA_CUBES " Bake to GameObjects")){
        std::string error;
        if (!SplineBakeObjects::bake(project, sceneProject->id, entity, error)){
            splineBakeMessage = error;
        }
    }
    if (ImGui::IsItemHovered()){
        ImGui::SetTooltip("Places the pieces as GameObjects of the scene (one undo step) and disables this component");
    }
}

void editor::InspectorWindow::drawSplineAnimateComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const SplineAnimateComponent& animate = scene->getComponent<SplineAnimateComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 7 * ImGui::GetFontSize();
    RowSettings settingsMethod;
    settingsMethod.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineAnimateMethod);
    settingsMethod.help = "Duration: the whole spline in Duration seconds; Speed: Speed world units per second";
    RowSettings settingsLoop;
    settingsLoop.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineLoop);
    RowSettings settingsEasing;
    settingsEasing.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineEasing);
    settingsEasing.help = "Once only";
    RowSettings settingsAlignment;
    settingsAlignment.enumEntries = InspectorWindow::enumEntries(reflect::enums::kSplineAlignment);
    settingsAlignment.help = "None: keeps its rotation; Spline Tangent: faces along the curve, world up; Spline Tangent And Up: "
                             "also rolls with it; World: the world's axes";
    RowSettings settings01;
    settings01.floatMin = 0.0f;
    settings01.floatMax = 1.0f;

    beginTable(cpType, getLabelSize("Vertical Offset"));
    propertyRow(RowPropertyType::LocalEntity, cpType, "spline", "Spline", sceneProject, entities);
    propertyRow(RowPropertyType::Enum, cpType, "method", "Method", sceneProject, entities, settingsMethod);
    if (animate.method == SplineAnimateMethod::Duration){
        propertyRow(RowPropertyType::FloatPositive, cpType, "duration", "Duration", sceneProject, entities, settings);
    }else{
        propertyRow(RowPropertyType::FloatPositive, cpType, "speed", "Speed", sceneProject, entities, settings);
    }
    propertyRow(RowPropertyType::Enum, cpType, "loop", "Loop", sceneProject, entities, settingsLoop);
    if (animate.loop == SplineLoop::Once){
        propertyRow(RowPropertyType::Enum, cpType, "easing", "Easing", sceneProject, entities, settingsEasing);
    }
    propertyRow(RowPropertyType::Enum, cpType, "alignment", "Alignment", sceneProject, entities, settingsAlignment);
    propertyRow(RowPropertyType::Float_0_1, cpType, "startOffset", "Start Offset", sceneProject, entities, settings01);
    propertyRow(RowPropertyType::Float, cpType, "lateralOffset", "Lateral Offset", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Float, cpType, "verticalOffset", "Vertical Offset", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Bool, cpType, "playOnAwake", "Play On Awake", sceneProject, entities);
    endTable();

    if (entities.size() == 1){
        if (animate.spline == NULL_ENTITY || !scene->findComponent<SplineComponent>(animate.spline)){
            warningText("Pick the GameObject with the Spline to follow.");
        }else{
            const Spline spline(scene, animate.spline);
            ImGui::TextDisabled("Spline length %.2f: %.2f s at this setting", spline.getLength(),
                                animate.method == SplineAnimateMethod::Duration ? animate.duration
                                : spline.getLength() / std::max(animate.speed, 1.0e-4f));
        }
    }
}
