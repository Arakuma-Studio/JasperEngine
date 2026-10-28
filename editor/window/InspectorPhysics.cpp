// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector drawers of the physics components: bodies with their colliders and joints (their tables cannot
// draw them: per-collider-type rows, shape previews, body-picking rows).

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
#include "util/WaterEditorUtils.h"
#include "component/BuoyancyComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/WaterVolumeComponent.h"
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

void editor::InspectorWindow::drawRigidbody2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Rigidbody2DComponent& body = sceneProject->scene->getComponent<Rigidbody2DComponent>(entities[0]);

    auto getSizeForShape = [sceneProject](Entity entity, float defaultSize = 100.0f) -> Vector2 {
        float width = defaultSize;
        float height = defaultSize;

        if (SpriteComponent* spriteComp = sceneProject->scene->findComponent<SpriteComponent>(entity)){
            if (spriteComp->width > 0){
                width = (float)spriteComp->width;
            }
            if (spriteComp->height > 0){
                height = (float)spriteComp->height;
            }
        }else if (RectTransformComponent* layoutComp = sceneProject->scene->findComponent<RectTransformComponent>(entity)){
            if (layoutComp->width > 0){
                width = (float)layoutComp->width;
            }
            if (layoutComp->height > 0){
                height = (float)layoutComp->height;
            }
        }

        return Vector2(width, height);
    };

    RowSettings settingsBodyType;
    settingsBodyType.enumEntries = enumEntries(reflect::enums::kBodyType);

    beginTable(cpType, getLabelSize("Update Colliders"));
    propertyRow(RowPropertyType::Enum, cpType, "type", "Body Type", sceneProject, entities, settingsBodyType);
    endTable();

    separatorText("Colliders");
    ImGui::Text("Colliders: %zu", body.numColliders);

    static int createCollider2DType = (int)Collider2DType::POLYGON;
    const char* createCollider2DLabel = "Polygon";
    for (const EnumEntry& entry : (*enumEntries(reflect::enums::kCollider2DType))){
        if (entry.value == createCollider2DType){
            createCollider2DLabel = entry.name;
            break;
        }
    }

    ImGui::SetNextItemWidth(9 * ImGui::GetFontSize());
    if (ImGui::BeginCombo("##shape2d_create_type", createCollider2DLabel)){
        for (const EnumEntry& entry : (*enumEntries(reflect::enums::kCollider2DType))){
            bool selected = (createCollider2DType == entry.value);
            if (ImGui::Selectable(entry.name, selected)){
                createCollider2DType = entry.value;
            }
            if (selected){
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Collider##rigidbody2d")){
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

        for (Entity entity : entities){
            if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){

                Collider2D shape;
                shape.type = (Collider2DType)createCollider2DType;

                switch (shape.type){
                case Collider2DType::POLYGON:
                {
                    Vector2 layoutSize = getSizeForShape(entity, 0.0f);

                    shape.numVertices = 4;

                    if (layoutSize != Vector2::ZERO){
                        shape.pointA = Vector2(0.0f, 0.0f);
                        shape.pointB = Vector2(layoutSize.x, layoutSize.y);
                        shape.vertices[0] = Vector2(0.0f, 0.0f);
                        shape.vertices[1] = Vector2(layoutSize.x, 0.0f);
                        shape.vertices[2] = Vector2(layoutSize.x, layoutSize.y);
                        shape.vertices[3] = Vector2(0.0f, layoutSize.y);
                    }else{
                        layoutSize = Vector2(100.0f, 100.0f);

                        const float halfW = layoutSize.x * 0.5f;
                        const float halfH = layoutSize.y * 0.5f;

                        shape.pointA = Vector2(-halfW, -halfH);
                        shape.pointB = Vector2(halfW, halfH);
                        shape.vertices[0] = Vector2(-halfW, -halfH);
                        shape.vertices[1] = Vector2(halfW, -halfH);
                        shape.vertices[2] = Vector2(halfW, halfH);
                        shape.vertices[3] = Vector2(-halfW, halfH);
                    }

                    break;
                }
                case Collider2DType::CIRCLE:
                    shape.pointA = Vector2::ZERO;
                    shape.radius = 0.5f;
                    break;
                case Collider2DType::CAPSULE:
                    shape.pointA = Vector2(0.0f, -0.5f);
                    shape.pointB = Vector2(0.0f, 0.5f);
                    shape.radius = 0.5f;
                    break;
                case Collider2DType::SEGMENT:
                    shape.pointA = Vector2(-1.0f, 0.0f);
                    shape.pointB = Vector2(1.0f, 0.0f);
                    break;
                case Collider2DType::CHAIN:
                    shape.numVertices = 4;
                    shape.vertices[0] = Vector2(-0.5f, -0.5f);
                    shape.vertices[1] = Vector2(0.5f, -0.5f);
                    shape.vertices[2] = Vector2(0.5f, 0.5f);
                    shape.vertices[3] = Vector2(-0.5f, 0.5f);
                    break;
                }

                size_t shapeIdx = bodyComp->numColliders;
                multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, "colliders[" + std::to_string(shapeIdx) + "]", shape);
                multiCmd->addPropertyCmd<size_t>(project, sceneProject->id, entity, cpType, "numColliders", shapeIdx + 1);
            }
        }

        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }

    size_t numColliders = body.numColliders;
    for (Entity entity : entities){
        numColliders = std::min(numColliders, sceneProject->scene->getComponent<Rigidbody2DComponent>(entity).numColliders);
    }

    RowSettings settingsShapeType;
    settingsShapeType.enumEntries = enumEntries(reflect::enums::kCollider2DType);

    RowSettings settingsShapeValue;

    for (size_t s = 0; s < numColliders; s++){
        Rigidbody2DComponent& bodyRef = sceneProject->scene->getComponent<Rigidbody2DComponent>(entities[0]);
        Collider2D& shape = bodyRef.colliders[s];

        separatorText(("Collider " + std::to_string(s + 1)).c_str());
        ImGui::PushID((int)s);
        if (ImGui::Button("Remove Collider")){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

            for (Entity entity : entities){
                if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                    if (s >= bodyComp->numColliders){
                        continue;
                    }

                    for (size_t i = s + 1; i < bodyComp->numColliders; i++){
                        multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, "colliders[" + std::to_string(i - 1) + "]", bodyComp->colliders[i]);
                    }

                    multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, "colliders[" + std::to_string(bodyComp->numColliders - 1) + "]", Collider2D());
                    multiCmd->addPropertyCmd<size_t>(project, sceneProject->id, entity, cpType, "numColliders", bodyComp->numColliders - 1);
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();

        std::string shapeKey = "colliders[" + std::to_string(s) + "]";

        beginTable(cpType, getLabelSize("Enable Hit Events"), "rigidbody2d_collider");
        propertyRow(RowPropertyType::Enum, cpType, shapeKey + ".type", "Type", sceneProject, entities, settingsShapeType);

        RowSettings settingsFloat;
        settingsFloat.secondColSize = 6 * ImGui::GetFontSize();

        if (shape.type == Collider2DType::POLYGON){
            propertyHeader("Vertices");
            ImGui::Text("%d", shape.numVertices);
            ImGui::SameLine();
            if (ImGui::Button("Add Vertex")){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                        if (s >= bodyComp->numColliders) continue;
                        Collider2D shapeValue = bodyComp->colliders[s];

                        const uint8_t oldCount = shapeValue.numVertices;
                        if (oldCount >= 2){
                            shapeValue.vertices[oldCount] = shapeValue.vertices[oldCount - 1] + (shapeValue.vertices[oldCount - 1] - shapeValue.vertices[oldCount - 2]);
                        }else if (oldCount == 1){
                            shapeValue.vertices[oldCount] = shapeValue.vertices[0] + Vector2(10.0f, 0.0f);
                        }else{
                            shapeValue.vertices[oldCount] = Vector2::ZERO;
                        }
                        shapeValue.numVertices = oldCount + 1;
                        multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                    }
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
            bool removedVertex = false;
            float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
            float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
            ImVec2 inputVerSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
            if (inputVerSize.x < 100.0f){
                inputVerSize.x = 100.0f;
            }

            RowSettings settingsVertex = settingsShapeValue;
            settingsVertex.secondColSize = inputVerSize.x;

            for (int v = 0; v < (int)shape.numVertices; v++){

                propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".vertices[" + std::to_string(v) + "]", "Vertex " + std::to_string(v + 1), sceneProject, entities, settingsVertex);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                std::string removeVertexId = std::string(ICON_FA_TRASH_CAN) + "##remove_polygon_vertex_" + std::to_string(s) + "_" + std::to_string(v);
                if (ImGui::Button(removeVertexId.c_str())){
                    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                            if (s >= bodyComp->numColliders) continue;

                            Collider2D shapeValue = bodyComp->colliders[s];
                            if (v >= shapeValue.numVertices) continue;

                            for (size_t i = (size_t)v + 1; i < shapeValue.numVertices; i++){
                                shapeValue.vertices[i - 1] = shapeValue.vertices[i];
                            }
                            shapeValue.numVertices -= 1;

                            multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                        }
                    }

                    multiCmd->setNoMerge();
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);

                    removedVertex = true;
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    break;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Remove vertex");
                }
            }

            if (removedVertex){
                endTable();
                return;
            }

            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsFloat);
        }else if (shape.type == Collider2DType::CIRCLE){
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointA", "Center", sceneProject, entities);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsFloat);
        }else if (shape.type == Collider2DType::CAPSULE){
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointA", "Center A", sceneProject, entities);
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointB", "Center B", sceneProject, entities);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsFloat);
        }else if (shape.type == Collider2DType::SEGMENT){
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointA", "Point A", sceneProject, entities);
            propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".pointB", "Point B", sceneProject, entities);
        }else if (shape.type == Collider2DType::CHAIN){
            RowSettings loopSettings;
            loopSettings.help = "An open chain doesn't collide on its first and last edges";
            propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".loop", "Loop", sceneProject, entities, loopSettings);

            propertyHeader("Collision Side");
            if (ImGui::Button("Reverse")){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                        if (s >= bodyComp->numColliders) continue;
                        Collider2D shapeValue = bodyComp->colliders[s];

                        std::reverse(shapeValue.vertices.data(), shapeValue.vertices.data() + shapeValue.numVertices);
                        multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                    }
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
            if (ImGui::IsItemHovered()){
                ImGui::SetTooltip("Flip the side that collides, shown by the ticks in the scene");
            }

            propertyHeader("Vertices");
            ImGui::Text("%d", shape.numVertices);
            ImGui::SameLine();
            if (ImGui::Button("Add Vertex")){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                        if (s >= bodyComp->numColliders) continue;
                        Collider2D shapeValue = bodyComp->colliders[s];

                        const uint8_t oldCount = shapeValue.numVertices;
                        if (oldCount >= 2){
                            shapeValue.vertices[oldCount] = shapeValue.vertices[oldCount - 1] + (shapeValue.vertices[oldCount - 1] - shapeValue.vertices[oldCount - 2]);
                        }else if (oldCount == 1){
                            shapeValue.vertices[oldCount] = shapeValue.vertices[0] + Vector2(10.0f, 0.0f);
                        }else{
                            shapeValue.vertices[oldCount] = Vector2::ZERO;
                        }
                        shapeValue.numVertices = oldCount + 1;
                        multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                    }
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }

            bool removedVertex = false;
            float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
            float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
            ImVec2 inputVerSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
            if (inputVerSize.x < 100.0f){
                inputVerSize.x = 100.0f;
            }

            RowSettings settingsVertex = settingsShapeValue;
            settingsVertex.secondColSize = inputVerSize.x;

            for (int v = 0; v < (int)shape.numVertices; v++){
                propertyRow(RowPropertyType::Vector2, cpType, shapeKey + ".vertices[" + std::to_string(v) + "]", "Vertex " + std::to_string(v + 1), sceneProject, entities, settingsVertex);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                std::string removeVertexId = std::string(ICON_FA_TRASH_CAN) + "##remove_chain_vertex_" + std::to_string(s) + "_" + std::to_string(v);
                if (ImGui::Button(removeVertexId.c_str())){
                    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                            if (s >= bodyComp->numColliders) continue;

                            Collider2D shapeValue = bodyComp->colliders[s];
                            if (v >= shapeValue.numVertices) continue;

                            for (size_t i = (size_t)v + 1; i < shapeValue.numVertices; i++){
                                shapeValue.vertices[i - 1] = shapeValue.vertices[i];
                            }
                            shapeValue.numVertices -= 1;

                            multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                        }
                    }

                    multiCmd->setNoMerge();
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);

                    removedVertex = true;
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    break;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Remove vertex");
                }
            }

            if (removedVertex){
                endTable();
                return;
            }
        }

        if (shape.type != Collider2DType::SEGMENT && shape.type != Collider2DType::CHAIN){
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".density", "Density", sceneProject, entities, settingsFloat);
        }
        propertyRow(RowPropertyType::Float, cpType, shapeKey + ".friction", "Friction", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, shapeKey + ".restitution", "Restitution", sceneProject, entities, settingsFloat);
        if (shape.type != Collider2DType::CHAIN){
            RowSettings sensorSettings;
            sensorSettings.help = "Overlap only: never collides and always reports its sensor events";
            propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".sensor", "Sensor", sceneProject, entities, sensorSettings);
            propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".enableHitEvents", "Enable Hit Events", sceneProject, entities);
            propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".contactEvents", "Contact Events", sceneProject, entities);
            propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".preSolveEvents", "PreSolve Events", sceneProject, entities);
            if (!shape.sensor){
                propertyRow(RowPropertyType::Bool, cpType, shapeKey + ".sensorEvents", "Sensor Events", sceneProject, entities);
            }
        }
        if (shape.type == Collider2DType::POLYGON){
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
            propertyHeader("Reset Collider");
            ImGui::PopStyleColor();
            if (ImGui::SmallButton(("Box##shape_preset_box_" + std::to_string(s)).c_str())){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                        if (s >= bodyComp->numColliders) continue;

                        Collider2D shapeValue = bodyComp->colliders[s];
                        Vector2 layoutSize = getSizeForShape(entity);

                        shapeValue.numVertices = 4;
                        shapeValue.radius = 0.0f;
                        shapeValue.pointA = Vector2(0.0f, 0.0f);
                        shapeValue.pointB = Vector2(layoutSize.x, layoutSize.y);
                        shapeValue.vertices[0] = Vector2(0.0f, 0.0f);
                        shapeValue.vertices[1] = Vector2(layoutSize.x, 0.0f);
                        shapeValue.vertices[2] = Vector2(layoutSize.x, layoutSize.y);
                        shapeValue.vertices[3] = Vector2(0.0f, layoutSize.y);

                        multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                    }
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
            if (ImGui::SmallButton(("Centered Box##shape_preset_centered_box_" + std::to_string(s)).c_str())){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                        if (s >= bodyComp->numColliders) continue;

                        Collider2D shapeValue = bodyComp->colliders[s];
                        Vector2 layoutSize = getSizeForShape(entity);
                        const float halfW = layoutSize.x * 0.5f;
                        const float halfH = layoutSize.y * 0.5f;

                        shapeValue.numVertices = 4;
                        shapeValue.radius = 0.0f;
                        shapeValue.pointA = Vector2(-halfW, -halfH);
                        shapeValue.pointB = Vector2(halfW, halfH);
                        shapeValue.vertices[0] = Vector2(-halfW, -halfH);
                        shapeValue.vertices[1] = Vector2(halfW, -halfH);
                        shapeValue.vertices[2] = Vector2(halfW, halfH);
                        shapeValue.vertices[3] = Vector2(-halfW, halfH);

                        multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                    }
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
            if (ImGui::SmallButton(("Rounded Box##shape_preset_rounded_box_" + std::to_string(s)).c_str())){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (Rigidbody2DComponent* bodyComp = sceneProject->scene->findComponent<Rigidbody2DComponent>(entity)){
                        if (s >= bodyComp->numColliders) continue;

                        Collider2D shapeValue = bodyComp->colliders[s];
                        Vector2 layoutSize = getSizeForShape(entity);
                        const float halfW = layoutSize.x * 0.5f;
                        const float halfH = layoutSize.y * 0.5f;

                        shapeValue.numVertices = 4;
                        shapeValue.radius = std::max(1.0f, std::min(layoutSize.x, layoutSize.y) * 0.04f);
                        shapeValue.pointA = Vector2(-halfW, -halfH);
                        shapeValue.pointB = Vector2(halfW, halfH);
                        shapeValue.vertices[0] = Vector2(-halfW, -halfH);
                        shapeValue.vertices[1] = Vector2(halfW, -halfH);
                        shapeValue.vertices[2] = Vector2(halfW, halfH);
                        shapeValue.vertices[3] = Vector2(-halfW, halfH);

                        multiCmd->addPropertyCmd<Collider2D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                    }
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
        }
        endTable();
    }
}

void editor::InspectorWindow::drawRigidbodyComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RigidbodyComponent& body = sceneProject->scene->getComponent<RigidbodyComponent>(entities[0]);

    RowSettings settingsBodyType;
    settingsBodyType.enumEntries = enumEntries(reflect::enums::kBodyType);

    RowSettings settingsMotionQuality;
    settingsMotionQuality.enumEntries = enumEntries(reflect::enums::kRigidbodyMotionQuality);

    RowSettings settingsBodyValue;

    beginTable(cpType, getLabelSize("Water Angular Drag"));
    propertyRow(RowPropertyType::Enum, cpType, "type", "Body Type", sceneProject, entities, settingsBodyType);
    propertyRow(RowPropertyType::Enum, cpType, "motionQuality", "Motion Quality", sceneProject, entities, settingsMotionQuality);
    propertyRow(RowPropertyType::Bool, cpType, "sensor", "Sensor", sceneProject, entities, settingsBodyValue);
    propertyRow(RowPropertyType::Float, cpType, "gravityFactor", "Gravity Factor", sceneProject, entities, settingsBodyValue);
    propertyRow(RowPropertyType::Float, cpType, "friction", "Friction", sceneProject, entities, settingsBodyValue);
    propertyRow(RowPropertyType::Float, cpType, "restitution", "Restitution", sceneProject, entities, settingsBodyValue);
    // floating in a library:water (Water.h): 0 ignores the water
    propertyRow(RowPropertyType::Float, cpType, "buoyancy", "Buoyancy", sceneProject, entities, settingsBodyValue);
    if (body.buoyancy > 0.0f) {
        propertyRow(RowPropertyType::Float, cpType, "waterDrag", "Water Drag", sceneProject, entities, settingsBodyValue);
        propertyRow(RowPropertyType::Float, cpType, "waterAngularDrag", "Water Angular Drag", sceneProject, entities, settingsBodyValue);
    }
    propertyRow(RowPropertyType::Bool, cpType, "overrideMassProperties", "Override Mass", sceneProject, entities, settingsBodyValue);

    if (body.overrideMassProperties) {
        propertyRow(RowPropertyType::Vector3, cpType, "solidBoxSize", "Solid Box Size", sceneProject, entities, settingsBodyValue);
        propertyRow(RowPropertyType::Float, cpType, "solidBoxDensity", "Solid Box Density", sceneProject, entities, settingsBodyValue);
    }
    endTable();

    separatorText("Colliders");
    ImGui::Text("Colliders: %zu", body.numColliders);

    static int createCollider3DType = (int)Collider3DType::BOX;
    const char* createCollider3DLabel = "Unknown";
    for (const EnumEntry& entry : (*enumEntries(reflect::enums::kCollider3DType))){
        if (entry.value == createCollider3DType){
            createCollider3DLabel = entry.name;
            break;
        }
    }

    ImGui::SetNextItemWidth(9 * ImGui::GetFontSize());
    if (ImGui::BeginCombo("##shape3d_create_type", createCollider3DLabel)){
        for (const EnumEntry& entry : (*enumEntries(reflect::enums::kCollider3DType))){
            bool selected = (createCollider3DType == entry.value);
            if (ImGui::Selectable(entry.name, selected)){
                createCollider3DType = entry.value;
            }
            if (selected){
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Collider##rigidbody")){
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

        for (Entity entity : entities){
            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){

                Collider3D shape;
                shape.type = (Collider3DType)createCollider3DType;
                if (shape.type == Collider3DType::MESH){
                    shape.source = Collider3DSource::ENTITY_MESH;
                    if (sceneProject->scene->findComponent<MeshComponent>(entity)){
                        shape.sourceEntity = entity;
                    }
                }else if (shape.type == Collider3DType::CONVEX_HULL){
                    shape.source = Collider3DSource::ENTITY_MESH;
                    if (sceneProject->scene->findComponent<MeshComponent>(entity)){
                        shape.sourceEntity = entity;
                    }
                }else if (shape.type == Collider3DType::HEIGHTFIELD){
                    shape.source = Collider3DSource::ENTITY_HEIGHTFIELD;
                    shape.samplesSize = 256;
                    if (sceneProject->scene->findComponent<TerrainComponent>(entity)){
                        shape.sourceEntity = entity;
                    }
                }

                size_t shapeIdx = bodyComp->numColliders;
                multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, "colliders[" + std::to_string(shapeIdx) + "]", shape);
                multiCmd->addPropertyCmd<size_t>(project, sceneProject->id, entity, cpType, "numColliders", shapeIdx + 1);
            }
        }

        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }

    size_t numColliders = body.numColliders;
    for (Entity entity : entities){
        numColliders = std::min(numColliders, sceneProject->scene->getComponent<RigidbodyComponent>(entity).numColliders);
    }

    RowSettings settingsShapeSourceConvexHull;
    settingsShapeSourceConvexHull.enumEntries = enumEntries(reflect::enums::kCollider3DSource, reflect::enums::bit((int)Collider3DSource::RAW_VERTICES) | reflect::enums::bit((int)Collider3DSource::ENTITY_MESH));

    RowSettings settingsShapeSourceMesh;
    settingsShapeSourceMesh.enumEntries = enumEntries(reflect::enums::kCollider3DSource, reflect::enums::bit((int)Collider3DSource::RAW_MESH) | reflect::enums::bit((int)Collider3DSource::ENTITY_MESH));

    RowSettings settingsShapeSourceHeightfield;
    settingsShapeSourceHeightfield.enumEntries = enumEntries(reflect::enums::kCollider3DSource, reflect::enums::bit((int)Collider3DSource::ENTITY_HEIGHTFIELD));

    RowSettings settingsShapeValue;

    auto normalizeCollider3DSource = [](Collider3DType type, Collider3DSource source) {
        if (type == Collider3DType::CONVEX_HULL){
            if (source != Collider3DSource::RAW_VERTICES && source != Collider3DSource::ENTITY_MESH){
                return Collider3DSource::RAW_VERTICES;
            }
        }else if (type == Collider3DType::MESH){
            if (source != Collider3DSource::RAW_MESH && source != Collider3DSource::ENTITY_MESH){
                return Collider3DSource::RAW_MESH;
            }
        }else if (type == Collider3DType::HEIGHTFIELD){
            if (source != Collider3DSource::ENTITY_HEIGHTFIELD){
                return Collider3DSource::ENTITY_HEIGHTFIELD;
            }
        }

        return source;
    };

    for (size_t s = 0; s < numColliders; s++){
        RigidbodyComponent& bodyRef = sceneProject->scene->getComponent<RigidbodyComponent>(entities[0]);
        Collider3D& shape = bodyRef.colliders[s];
        Transform* bodyTransform = sceneProject->scene->findComponent<Transform>(entities[0]);
        const Vector3 bodyScale = bodyTransform ? bodyTransform->scale : Vector3::UNIT_SCALE;
        const float physicsOffset = (shape.position * bodyScale).length();
        const bool suspiciousSingleShapeOffset = bodyRef.numColliders == 1 && physicsOffset > Stream::MAX_SINGLE_SHAPE_PHYSICS_OFFSET;

        separatorText(("Collider " + std::to_string(s + 1)).c_str());
        ImGui::PushID((int)s);
        if (ImGui::Button("Remove Collider")){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

            for (Entity entity : entities){
                if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                    if (s >= bodyComp->numColliders){
                        continue;
                    }

                    for (size_t i = s + 1; i < bodyComp->numColliders; i++){
                        multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, "colliders[" + std::to_string(i - 1) + "]", bodyComp->colliders[i]);
                    }

                    multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, "colliders[" + std::to_string(bodyComp->numColliders - 1) + "]", Collider3D());
                    multiCmd->addPropertyCmd<size_t>(project, sceneProject->id, entity, cpType, "numColliders", bodyComp->numColliders - 1);
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();

        std::string shapeKey = "colliders[" + std::to_string(s) + "]";

        beginTable(cpType, getLabelSize("Bottom Radius"), "rigidbody_collider");

        PropertyData shapeTypeProp = Catalog::getProperty(sceneProject->scene, entities[0], cpType, shapeKey + ".type");
        int* shapeTypeDef = static_cast<int*>(shapeTypeProp.def);
        int shapeTypeValue = *static_cast<int*>(shapeTypeProp.ref);
        bool shapeTypeMixed = false;
        for (size_t entityIndex = 1; entityIndex < entities.size(); entityIndex++){
            PropertyData otherShapeTypeProp = Catalog::findProperty(sceneProject->scene, entities[entityIndex], cpType, shapeKey + ".type");
            // shapes are counted from the first body, the others may have fewer
            if (!otherShapeTypeProp.ref){
                continue;
            }
            if (*static_cast<int*>(otherShapeTypeProp.ref) != shapeTypeValue){
                shapeTypeMixed = true;
                break;
            }
        }

        int shapeTypeItemCurrent = 0;
        for (size_t i = 0; i < (*enumEntries(reflect::enums::kCollider3DType)).size(); ++i) {
            if ((*enumEntries(reflect::enums::kCollider3DType))[i].value == shapeTypeValue) {
                shapeTypeItemCurrent = static_cast<int>(i);
                break;
            }
        }

        int shapeTypeItemDefault = shapeTypeItemCurrent;
        bool shapeTypeDefChanged = false;
        if (shapeTypeDef){
            for (size_t i = 0; i < (*enumEntries(reflect::enums::kCollider3DType)).size(); ++i) {
                if ((*enumEntries(reflect::enums::kCollider3DType))[i].value == *shapeTypeDef) {
                    shapeTypeItemDefault = static_cast<int>(i);
                    break;
                }
            }
            shapeTypeDefChanged = (shapeTypeItemCurrent != shapeTypeItemDefault);
        }

        if (propertyHeader("Type", -1, shapeTypeDefChanged, false)){
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            Collider3DType resetType = static_cast<Collider3DType>((*enumEntries(reflect::enums::kCollider3DType))[shapeTypeItemDefault].value);

            for (Entity entity : entities){
                if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                    if (s >= bodyComp->numColliders) continue;

                    multiCmd->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, shapeKey + ".type", (int)resetType);

                    Collider3DSource normalizedSource = normalizeCollider3DSource(resetType, bodyComp->colliders[s].source);
                    if (normalizedSource != bodyComp->colliders[s].source){
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, shapeKey + ".source", (int)normalizedSource);
                    }
                    if (bodyComp->colliders[s].sourceEntity == NULL_ENTITY && normalizedSource != Collider3DSource::NONE){
                        multiCmd->addPropertyCmd<Entity>(project, sceneProject->id, entity, cpType, shapeKey + ".sourceEntity", entity);
                    }
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }

        std::vector<const char*> shapeTypeNames;
        for (const auto& entry : (*enumEntries(reflect::enums::kCollider3DType))) {
            shapeTypeNames.push_back(entry.name);
        }

        if (shapeTypeMixed)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Combo(("##combo_" + shapeKey + ".type").c_str(), &shapeTypeItemCurrent, shapeTypeNames.data(), static_cast<int>(shapeTypeNames.size()))) {
            Collider3DType newShapeType = static_cast<Collider3DType>((*enumEntries(reflect::enums::kCollider3DType))[shapeTypeItemCurrent].value);
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

            for (Entity entity : entities){
                if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                    if (s >= bodyComp->numColliders) continue;

                    multiCmd->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, shapeKey + ".type", (int)newShapeType);

                    Collider3DSource normalizedSource = normalizeCollider3DSource(newShapeType, bodyComp->colliders[s].source);
                    if (normalizedSource != bodyComp->colliders[s].source){
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, entity, cpType, shapeKey + ".source", (int)normalizedSource);
                    }
                    if (bodyComp->colliders[s].sourceEntity == NULL_ENTITY && normalizedSource != Collider3DSource::NONE){
                        multiCmd->addPropertyCmd<Entity>(project, sceneProject->id, entity, cpType, shapeKey + ".sourceEntity", entity);
                    }
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }
        if (shapeTypeMixed)
            ImGui::PopStyleColor();

        Collider3DType shapeTypeUI = sceneProject->scene->getComponent<RigidbodyComponent>(entities[0]).colliders[s].type;
        Collider3DSource shapeSourceUI = sceneProject->scene->getComponent<RigidbodyComponent>(entities[0]).colliders[s].source;
        Collider3DSource normalizedSourceUI = normalizeCollider3DSource(shapeTypeUI, shapeSourceUI);

        propertyRow(RowPropertyType::Vector3, cpType, shapeKey + ".position", "Position", sceneProject, entities, settingsShapeValue);

        if (suspiciousSingleShapeOffset){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Warning");
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 196, 64, 255));
            ImGui::TextUnformatted("Large scaled collider offset can destabilize joints.");
            ImGui::PopStyleColor();
            ImGui::SameLine();

            std::string resetButtonId = "Reset##shape3d_pos_reset_" + std::to_string(s);
            if (ImGui::SmallButton(resetButtonId.c_str())){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

                for (Entity entity : entities){
                    if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                        if (s >= bodyComp->numColliders){
                            continue;
                        }

                        multiCmd->addPropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, shapeKey + ".position", Vector3::ZERO);
                    }
                }

                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
        }

        propertyRow(RowPropertyType::Quat, cpType, shapeKey + ".rotation", "Rotation", sceneProject, entities, settingsShapeValue);

        if (shape.type == Collider3DType::BOX){
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".width", "Width", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".height", "Height", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".depth", "Depth", sceneProject, entities, settingsShapeValue);
        }else if (shape.type == Collider3DType::SPHERE){
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsShapeValue);
        }else if (shape.type == Collider3DType::CAPSULE || shape.type == Collider3DType::CYLINDER){
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".halfHeight", "Half Height", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".radius", "Radius", sceneProject, entities, settingsShapeValue);
        }else if (shape.type == Collider3DType::TAPERED_CAPSULE){
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".halfHeight", "Half Height", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".topRadius", "Top Radius", sceneProject, entities, settingsShapeValue);
            propertyRow(RowPropertyType::Float, cpType, shapeKey + ".bottomRadius", "Bottom Radius", sceneProject, entities, settingsShapeValue);
        }

        if (shapeTypeUI == Collider3DType::CONVEX_HULL){
            propertyRow(RowPropertyType::Enum, cpType, shapeKey + ".source", "Source", sceneProject, entities, settingsShapeSourceConvexHull);
            if (normalizedSourceUI == Collider3DSource::ENTITY_MESH){
                propertyRow(RowPropertyType::LocalEntity, cpType, shapeKey + ".sourceEntity", "Source GameObject", sceneProject, entities, settingsShapeValue);
            }else if (normalizedSourceUI == Collider3DSource::RAW_VERTICES){
                propertyHeader("Vertices");
                ImGui::Text("%d", shape.numVertices);
                ImGui::SameLine();
                if (ImGui::Button(("Add Vertex##convex_" + std::to_string(s)).c_str())){
                    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                            if (s >= bodyComp->numColliders) continue;
                            Collider3D shapeValue = bodyComp->colliders[s];
                            const uint16_t oldCount = shapeValue.numVertices;
                            if (oldCount >= 2){
                                shapeValue.vertices[oldCount] = shapeValue.vertices[oldCount - 1] + (shapeValue.vertices[oldCount - 1] - shapeValue.vertices[oldCount - 2]);
                            }else if (oldCount == 1){
                                shapeValue.vertices[oldCount] = shapeValue.vertices[0] + Vector3(1.0f, 0.0f, 0.0f);
                            }else{
                                shapeValue.vertices[oldCount] = Vector3::ZERO;
                            }
                            shapeValue.numVertices = oldCount + 1;
                            multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                        }
                    }
                    multiCmd->setNoMerge();
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                }

                float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
                float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
                ImVec2 inputVerSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
                if (inputVerSize.x < 100.0f){
                    inputVerSize.x = 100.0f;
                }
                RowSettings settingsVertex = settingsShapeValue;
                settingsVertex.secondColSize = inputVerSize.x;

                for (int v = 0; v < (int)shape.numVertices; v++){
                    propertyRow(RowPropertyType::Vector3, cpType, shapeKey + ".vertices[" + std::to_string(v) + "]", "Vertex " + std::to_string(v + 1), sceneProject, entities, settingsVertex);
                    ImGui::SameLine();

                    std::string removeVertexId = std::string(ICON_FA_TRASH_CAN) + "##remove_convex_vertex_" + std::to_string(s) + "_" + std::to_string(v);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(removeVertexId.c_str())){
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        for (Entity entity : entities){
                            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                                if (s >= bodyComp->numColliders) continue;
                                Collider3D shapeValue = bodyComp->colliders[s];
                                if (v >= shapeValue.numVertices) continue;
                                for (int vi = v; vi < shapeValue.numVertices - 1; vi++){
                                    shapeValue.vertices[vi] = shapeValue.vertices[vi + 1];
                                }
                                shapeValue.numVertices--;
                                multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                            }
                        }
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove vertex");
                    }
                }
            }
        }else if (shapeTypeUI == Collider3DType::MESH){
            propertyRow(RowPropertyType::Enum, cpType, shapeKey + ".source", "Source", sceneProject, entities, settingsShapeSourceMesh);
            if (normalizedSourceUI == Collider3DSource::ENTITY_MESH){
                propertyRow(RowPropertyType::LocalEntity, cpType, shapeKey + ".sourceEntity", "Source GameObject", sceneProject, entities, settingsShapeValue);
            }else if (normalizedSourceUI == Collider3DSource::RAW_MESH){
                // Vertices
                propertyHeader("Vertices");
                ImGui::Text("%d", shape.numVertices);
                ImGui::SameLine();
                if (ImGui::Button(("Add Vertex##mesh_" + std::to_string(s)).c_str())){
                    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                            if (s >= bodyComp->numColliders) continue;
                            Collider3D shapeValue = bodyComp->colliders[s];
                            const uint16_t oldCount = shapeValue.numVertices;
                            if (oldCount >= 2){
                                shapeValue.vertices[oldCount] = shapeValue.vertices[oldCount - 1] + (shapeValue.vertices[oldCount - 1] - shapeValue.vertices[oldCount - 2]);
                            }else if (oldCount == 1){
                                shapeValue.vertices[oldCount] = shapeValue.vertices[0] + Vector3(1.0f, 0.0f, 0.0f);
                            }else{
                                shapeValue.vertices[oldCount] = Vector3::ZERO;
                            }
                            shapeValue.numVertices = oldCount + 1;
                            multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                        }
                    }
                    multiCmd->setNoMerge();
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                }

                float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
                float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
                ImVec2 inputVerSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
                if (inputVerSize.x < 100.0f){
                    inputVerSize.x = 100.0f;
                }
                RowSettings settingsVertex = settingsShapeValue;
                settingsVertex.secondColSize = inputVerSize.x;

                for (int v = 0; v < (int)shape.numVertices; v++){
                    propertyRow(RowPropertyType::Vector3, cpType, shapeKey + ".vertices[" + std::to_string(v) + "]", "Vertex " + std::to_string(v + 1), sceneProject, entities, settingsVertex);
                    ImGui::SameLine();

                    std::string removeVertexId = std::string(ICON_FA_TRASH_CAN) + "##remove_mesh_vertex_" + std::to_string(s) + "_" + std::to_string(v);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(removeVertexId.c_str())){
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        for (Entity entity : entities){
                            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                                if (s >= bodyComp->numColliders) continue;
                                Collider3D shapeValue = bodyComp->colliders[s];
                                if (v >= shapeValue.numVertices) continue;
                                for (int vi = v; vi < shapeValue.numVertices - 1; vi++){
                                    shapeValue.vertices[vi] = shapeValue.vertices[vi + 1];
                                }
                                shapeValue.numVertices--;
                                multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                            }
                        }
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove vertex");
                    }
                }

                // Triangles (indices)
                const int triangleCount = shape.numIndices / 3;
                propertyHeader("Triangles");
                ImGui::Text("%d", triangleCount);
                ImGui::SameLine();
                if (ImGui::Button(("Add Triangle##mesh_tri_" + std::to_string(s)).c_str())){
                    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                            if (s >= bodyComp->numColliders) continue;
                            Collider3D shapeValue = bodyComp->colliders[s];
                            const uint16_t idx = shapeValue.numIndices;
                            shapeValue.indices[idx] = 0;
                            shapeValue.indices[idx + 1] = (shapeValue.numVertices > 1) ? (uint16_t)1 : (uint16_t)0;
                            shapeValue.indices[idx + 2] = (shapeValue.numVertices > 2) ? (uint16_t)2 : (uint16_t)0;
                            shapeValue.numIndices = idx + 3;
                            multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                        }
                    }
                    multiCmd->setNoMerge();
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                }

                for (int t = 0; t < triangleCount; t++){
                    int baseIdx = t * 3;
                    std::string triLabel = "Triangle " + std::to_string(t + 1);
                    propertyHeader(triLabel);

                    int triIndices[3] = { (int)shape.indices[baseIdx], (int)shape.indices[baseIdx + 1], (int)shape.indices[baseIdx + 2] };
                    std::string triId = "##mesh_tri_" + std::to_string(s) + "_" + std::to_string(t);

                    ImGui::SetNextItemWidth(inputVerSize.x);
                    if (ImGui::InputInt3(triId.c_str(), triIndices)){
                        for (int k = 0; k < 3; k++){
                            triIndices[k] = std::max(0, std::min(triIndices[k], (int)shape.numVertices - 1));
                        }
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        for (Entity entity : entities){
                            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                                if (s >= bodyComp->numColliders) continue;
                                Collider3D shapeValue = bodyComp->colliders[s];
                                if (baseIdx + 2 >= shapeValue.numIndices) continue;
                                shapeValue.indices[baseIdx] = (uint16_t)triIndices[0];
                                shapeValue.indices[baseIdx + 1] = (uint16_t)triIndices[1];
                                shapeValue.indices[baseIdx + 2] = (uint16_t)triIndices[2];
                                multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                            }
                        }
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    ImGui::SameLine();
                    std::string removeTriId = std::string(ICON_FA_TRASH_CAN) + "##remove_mesh_tri_" + std::to_string(s) + "_" + std::to_string(t);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
                    if (ImGui::Button(removeTriId.c_str())){
                        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                        for (Entity entity : entities){
                            if (RigidbodyComponent* bodyComp = sceneProject->scene->findComponent<RigidbodyComponent>(entity)){
                                if (s >= bodyComp->numColliders) continue;
                                Collider3D shapeValue = bodyComp->colliders[s];
                                if (baseIdx + 2 >= shapeValue.numIndices) continue;
                                for (int ii = baseIdx; ii < shapeValue.numIndices - 3; ii++){
                                    shapeValue.indices[ii] = shapeValue.indices[ii + 3];
                                }
                                shapeValue.numIndices -= 3;
                                multiCmd->addPropertyCmd<Collider3D>(project, sceneProject->id, entity, cpType, shapeKey, shapeValue);
                            }
                        }
                        multiCmd->setNoMerge();
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove triangle");
                    }
                }
            }
        }else if (shapeTypeUI == Collider3DType::HEIGHTFIELD){
            propertyRow(RowPropertyType::Enum, cpType, shapeKey + ".source", "Source", sceneProject, entities, settingsShapeSourceHeightfield);
            if (normalizedSourceUI == Collider3DSource::ENTITY_HEIGHTFIELD){
                propertyRow(RowPropertyType::LocalEntity, cpType, shapeKey + ".sourceEntity", "Source GameObject", sceneProject, entities, settingsShapeValue);
            }
            propertyRow(RowPropertyType::UInt, cpType, shapeKey + ".samplesSize", "Samples Size", sceneProject, entities, settingsShapeValue);
        }

        propertyRow(RowPropertyType::Float, cpType, shapeKey + ".density", "Density", sceneProject, entities, settingsShapeValue);
        endTable();
    }
}

void editor::InspectorWindow::drawJoint2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Joint2DComponent& joint = sceneProject->scene->getComponent<Joint2DComponent>(entities[0]);

    auto markJoint2DDirty = [sceneProject, entities](){
        for (Entity entity : entities){
            if (Joint2DComponent* jointComp = sceneProject->scene->findComponent<Joint2DComponent>(entity)){
                jointComp->needUpdateJoint = true;
            }
        }
    };

    RowSettings settingsJointType;
    settingsJointType.enumEntries = enumEntries(reflect::enums::kJoint2DType);
    settingsJointType.onValueChanged = markJoint2DDirty;

    RowSettings settingsJointValue;
    settingsJointValue.onValueChanged = markJoint2DDirty;
    settingsJointValue.entityFilter.set(sceneProject->scene->getComponentId<Rigidbody2DComponent>());

    beginTable(cpType, getLabelSize("Joint Type"));
    propertyRow(RowPropertyType::Enum, cpType, "type", "Joint Type", sceneProject, entities, settingsJointType);
    propertyRow(RowPropertyType::LocalEntity, cpType, "bodyA", "Rigidbody A", sceneProject, entities, settingsJointValue);
    propertyRow(RowPropertyType::LocalEntity, cpType, "bodyB", "Rigidbody B", sceneProject, entities, settingsJointValue);

    endTable();

    bool hasTypeSettings = joint.type != Joint2DType::MOTOR;
    if (!hasTypeSettings){
        return;
    }

    const char* joint2DTableLabel = "Anchor";
    if (joint.type == Joint2DType::DISTANCE){
        joint2DTableLabel = "Anchor A";
    }else if (joint.type == Joint2DType::MOUSE){
        joint2DTableLabel = "Target";
    }

    separatorText("Joint settings");
    std::string joint2DTypeTableId = "joint2d_type_settings_" + std::to_string((int)joint.type);
    beginTable(cpType, getLabelSize(joint2DTableLabel), joint2DTypeTableId);

    if (joint.type == Joint2DType::DISTANCE){
        propertyRow(RowPropertyType::Bool, cpType, "autoAnchors", "Auto Anchors", sceneProject, entities, settingsJointValue);
        if (!joint.autoAnchors){
            propertyRow(RowPropertyType::Vector2, cpType, "anchorA", "Anchor A", sceneProject, entities, settingsJointValue);
            propertyRow(RowPropertyType::Vector2, cpType, "anchorB", "Anchor B", sceneProject, entities, settingsJointValue);
        }
        propertyRow(RowPropertyType::Bool, cpType, "rope", "Rope", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint2DType::REVOLUTE || joint.type == Joint2DType::WELD){
        propertyRow(RowPropertyType::Vector2, cpType, "anchorA", "Anchor", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint2DType::PRISMATIC || joint.type == Joint2DType::WHEEL){
        propertyRow(RowPropertyType::Vector2, cpType, "anchorA", "Anchor", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector2, cpType, "axis", "Axis", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint2DType::MOUSE){
        propertyRow(RowPropertyType::Vector2, cpType, "target", "Target", sceneProject, entities, settingsJointValue);
    }

    endTable();
}

void editor::InspectorWindow::drawJoint3DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Joint3DComponent& joint = sceneProject->scene->getComponent<Joint3DComponent>(entities[0]);

    auto markJoint3DDirty = [sceneProject, entities](){
        for (Entity entity : entities){
            if (Joint3DComponent* jointComp = sceneProject->scene->findComponent<Joint3DComponent>(entity)){
                jointComp->needUpdateJoint = true;
            }
        }
    };

    RowSettings settingsJointType;
    settingsJointType.enumEntries = enumEntries(reflect::enums::kJoint3DType);
    settingsJointType.onValueChanged = markJoint3DDirty;

    RowSettings settingsJointValue;
    settingsJointValue.onValueChanged = markJoint3DDirty;
    settingsJointValue.entityFilter.set(sceneProject->scene->getComponentId<RigidbodyComponent>());

    beginTable(cpType, getLabelSize("Joint Type"));
    propertyRow(RowPropertyType::Enum, cpType, "type", "Joint Type", sceneProject, entities, settingsJointType);
    propertyRow(RowPropertyType::LocalEntity, cpType, "bodyA", "Rigidbody A", sceneProject, entities, settingsJointValue);
    propertyRow(RowPropertyType::LocalEntity, cpType, "bodyB", "Rigidbody B", sceneProject, entities, settingsJointValue);

    endTable();

    bool hasTypeSettings = joint.type != Joint3DType::FIXED;
    if (!hasTypeSettings){
        return;
    }

    const char* joint3DTableLabel = "Anchor";
    switch (joint.type){
        case Joint3DType::DISTANCE:
            joint3DTableLabel = "Auto Anchors";
            break;
        case Joint3DType::POINT:
            joint3DTableLabel = "Anchor";
            break;
        case Joint3DType::HINGE:
            joint3DTableLabel = "Normal";
            break;
        case Joint3DType::CONE:
            joint3DTableLabel = "Twist Axis";
            break;
        case Joint3DType::PRISMATIC:
            joint3DTableLabel = "Limits Min";
            break;
        case Joint3DType::SWINGTWIST:
            joint3DTableLabel = "Normal HalfCone";
            break;
        case Joint3DType::SIXDOF:
            joint3DTableLabel = "Anchor A";
            break;
        case Joint3DType::GEAR:
            joint3DTableLabel = "Teeth Gear A";
            break;
        case Joint3DType::RACKANDPINON:
            joint3DTableLabel = "Rack Length";
            break;
        case Joint3DType::PULLEY:
            joint3DTableLabel = "Fixed Point A";
            break;
        case Joint3DType::PATH:
            joint3DTableLabel = "Position";
            break;
        default:
            break;
    }

    separatorText("Joint settings");
    std::string joint3DTypeTableId = "joint3d_type_settings_" + std::to_string((int)joint.type);
    beginTable(cpType, getLabelSize(joint3DTableLabel), joint3DTypeTableId);

    if (joint.type == Joint3DType::DISTANCE){
        propertyRow(RowPropertyType::Bool, cpType, "autoAnchors", "Auto Anchors", sceneProject, entities, settingsJointValue);
        if (!joint.autoAnchors){
            propertyRow(RowPropertyType::Vector3, cpType, "anchorA", "Anchor A", sceneProject, entities, settingsJointValue);
            propertyRow(RowPropertyType::Vector3, cpType, "anchorB", "Anchor B", sceneProject, entities, settingsJointValue);
        }
    }else if (joint.type == Joint3DType::POINT){
        propertyRow(RowPropertyType::Vector3, cpType, "anchor", "Anchor", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::HINGE){
        propertyRow(RowPropertyType::Vector3, cpType, "anchor", "Anchor", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "axis", "Axis", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "normal", "Normal", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::CONE){
        propertyRow(RowPropertyType::Vector3, cpType, "anchor", "Anchor", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "twistAxis", "Twist Axis", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::PRISMATIC){
        propertyRow(RowPropertyType::Vector3, cpType, "axis", "Axis", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Float, cpType, "limitsMin", "Limits Min", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Float, cpType, "limitsMax", "Limits Max", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::SWINGTWIST){
        propertyRow(RowPropertyType::Vector3, cpType, "anchor", "Anchor", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "twistAxis", "Twist Axis", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "planeAxis", "Plane Axis", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Float, cpType, "normalHalfConeAngle", "Normal HalfCone", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Float, cpType, "planeHalfConeAngle", "Plane HalfCone", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Float, cpType, "twistMinAngle", "Twist Min Angle", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Float, cpType, "twistMaxAngle", "Twist Max Angle", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::SIXDOF){
        propertyRow(RowPropertyType::Vector3, cpType, "anchorA", "Anchor A", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "anchorB", "Anchor B", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "axisX", "Axis X", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "axisY", "Axis Y", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::GEAR){
        propertyRow(RowPropertyType::LocalEntity, cpType, "hingeA", "Hinge A", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::LocalEntity, cpType, "hingeB", "Hinge B", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "numTeethGearA", "Teeth Gear A", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "numTeethGearB", "Teeth Gear B", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::RACKANDPINON){
        propertyRow(RowPropertyType::LocalEntity, cpType, "hinge", "Hinge", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::LocalEntity, cpType, "slider", "Slider", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "numTeethRack", "Teeth Rack", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "numTeethGear", "Teeth Gear", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Int, cpType, "rackLength", "Rack Length", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::PULLEY){
        propertyRow(RowPropertyType::Vector3, cpType, "anchorA", "Anchor A", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "anchorB", "Anchor B", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "fixedPointA", "Fixed Point A", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Vector3, cpType, "fixedPointB", "Fixed Point B", sceneProject, entities, settingsJointValue);
    }else if (joint.type == Joint3DType::PATH){
        propertyRow(RowPropertyType::Vector3, cpType, "pathPosition", "Position", sceneProject, entities, settingsJointValue);
        propertyRow(RowPropertyType::Bool, cpType, "isLooping", "Looping", sceneProject, entities, settingsJointValue);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Points");
        ImGui::TableSetColumnIndex(1);

        ImGui::Text("%zu", joint.pathPoints.size());
        if (drawSummaryAddButton("Add Point##joint3d_path_add")) {
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (Entity entity : entities){
                if (Joint3DComponent* jointComp = sceneProject->scene->findComponent<Joint3DComponent>(entity)){
                    std::vector<Vector3> newPoints = jointComp->pathPoints;
                    Vector3 newPoint = Vector3::ZERO;
                    if (!newPoints.empty()){
                        Vector3 lastPoint = newPoints.back();
                        newPoint = Vector3(lastPoint.x, lastPoint.y, lastPoint.z + 1.0f);
                    }
                    newPoints.push_back(newPoint);
                    multiCmd->addPropertyCmd<std::vector<Vector3>>(project, sceneProject->id, entity, cpType, "pathPoints", newPoints, settingsJointValue.onValueChanged);
                }
            }

            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        }

        bool removedPoint = false;
        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
        ImVec2 inputSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
        if (inputSize.x < 100.0f){
            inputSize.x = 100.0f;
        }

        for (size_t pointIndex = 0; pointIndex < joint.pathPoints.size(); pointIndex++){
            std::string pointId = "pathPoints[" + std::to_string(pointIndex) + "]";
            std::string pointLabel = "Point " + std::to_string(pointIndex);

            RowSettings settingsPathPoint = settingsJointValue;
            settingsPathPoint.secondColSize = inputSize.x;
            propertyRow(RowPropertyType::Vector3, cpType, pointId, pointLabel, sceneProject, entities, settingsPathPoint);

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
            std::string removePointId = std::string(ICON_FA_TRASH_CAN) + "##remove_path_point_" + std::to_string(pointIndex);
            if (ImGui::Button(removePointId.c_str())){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (Joint3DComponent* jointComp = sceneProject->scene->findComponent<Joint3DComponent>(entity)){
                        if (pointIndex < jointComp->pathPoints.size()){
                            std::vector<Vector3> newPoints = jointComp->pathPoints;
                            newPoints.erase(newPoints.begin() + (long int)pointIndex);
                            multiCmd->addPropertyCmd<std::vector<Vector3>>(project, sceneProject->id, entity, cpType, "pathPoints", newPoints, settingsJointValue.onValueChanged);
                        }
                    }
                }

                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);

                removedPoint = true;
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                break;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove point");
            }
        }

        if (removedPoint){
            endTable();
            return;
        }
    }

    endTable();
}

// S22's gameplay components: the character controllers, the water volume and the buoyancy

void editor::InspectorWindow::drawCharacterController2DComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsSlope = settings;
    settingsSlope.help = "Surfaces steeper than this (degrees) are not walked on: they act as walls";
    RowSettings settingsStep = settings;
    settingsStep.help = "Steps up to this high (points) are climbed while walking on the ground; 0 climbs none";
    RowSettings settingsSkin = settings;
    settingsSkin.help = "How far (points) the capsule stays off what it touches";
    RowSettings settingsPush = settings;
    settingsPush.help = "Dynamic bodies it walks into get an impulse of this (N) x dt along the move";
    RowSettings settingsPlatforms;
    settingsPlatforms.help = "Carried by the kinematic or dynamic body it stands on";

    beginTable(cpType, getLabelSize("Min Move Distance"));
    propertyRow(RowPropertyType::Vector2, cpType, "center", "Center", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "height", "Height", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "radius", "Radius", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "slopeLimit", "Slope Limit", sceneProject, entities, settingsSlope);
    propertyRow(RowPropertyType::FloatPositive, cpType, "stepOffset", "Step Offset", sceneProject, entities, settingsStep);
    propertyRow(RowPropertyType::FloatPositive, cpType, "skinWidth", "Skin Width", sceneProject, entities, settingsSkin);
    propertyRow(RowPropertyType::FloatPositive, cpType, "minMoveDistance", "Min Move Distance", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "pushStrength", "Push Strength", sceneProject, entities, settingsPush);
    propertyRow(RowPropertyType::Bool, cpType, "pushBodies", "Push Bodies", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Bool, cpType, "movingPlatforms", "Moving Platforms", sceneProject, entities, settingsPlatforms);
    endTable();

    for (Entity entity : entities){
        if (scene->findComponent<Rigidbody2DComponent>(entity)){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextWrapped("A Character Controller 2D does nothing on a GameObject with a Rigidbody 2D: remove one of them.");
            break;
        }
    }

    // what the last move and physics step left, while the scene plays
    if (sceneProject->playState != ScenePlayState::STOPPED && entities.size() == 1){
        const CharacterController2DComponent& controller = scene->getComponent<CharacterController2DComponent>(entities[0]);
        std::string flags;
        if (controller.collisionFlags & CharacterCollision_Sides) flags += " Sides";
        if (controller.collisionFlags & CharacterCollision_Above) flags += " Above";
        if (controller.collisionFlags & CharacterCollision_Below) flags += " Below";
        ImGui::SeparatorText("Runtime");
        ImGui::TextDisabled("Grounded: %s   On Steep Slope: %s", controller.isGrounded ? "yes" : "no", controller.isOnSteepSlope ? "yes" : "no");
        ImGui::TextDisabled("Collision Flags:%s", flags.empty() ? " None" : flags.c_str());
        ImGui::TextDisabled("Velocity: %.1f, %.1f", controller.velocity.x, controller.velocity.y);
        ImGui::TextDisabled("Ground Velocity: %.1f, %.1f", controller.groundVelocity.x, controller.groundVelocity.y);
    }
}

void editor::InspectorWindow::drawCharacterControllerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsSlope = settings;
    settingsSlope.help = "Surfaces steeper than this (degrees) are not walked on: they act as walls";
    RowSettings settingsStep = settings;
    settingsStep.help = "Steps up to this high are climbed while walking on the ground; 0 climbs none";
    RowSettings settingsSkin = settings;
    settingsSkin.help = "How far the capsule stays off what it touches (Jolt's character padding)";
    RowSettings settingsSize = settings;
    settingsSize.help = "While playing a new size is tried at once and refused when it does not fit there (a crouch under a ceiling)";
    RowSettings settingsPush = settings;
    settingsPush.help = "The most force (N) it pushes dynamic bodies with";
    RowSettings settingsFlag;
    RowSettings settingsDetect;
    settingsDetect.help = "A kinematic inner body: rays, triggers and dynamic bodies see it";
    RowSettings settingsCharacters;
    settingsCharacters.help = "Other controllers block it";
    RowSettings settingsEdges;
    settingsEdges.help = "Smoother over the inner edges of meshes, at a cost";

    beginTable(cpType, getLabelSize("Enhanced Edge Removal"));
    propertyRow(RowPropertyType::Vector3, cpType, "center", "Center", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::FloatPositive, cpType, "height", "Height", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::FloatPositive, cpType, "radius", "Radius", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::FloatPositive, cpType, "slopeLimit", "Slope Limit", sceneProject, entities, settingsSlope);
    propertyRow(RowPropertyType::FloatPositive, cpType, "stepOffset", "Step Offset", sceneProject, entities, settingsStep);
    propertyRow(RowPropertyType::FloatPositive, cpType, "skinWidth", "Skin Width", sceneProject, entities, settingsSkin);
    propertyRow(RowPropertyType::FloatPositive, cpType, "minMoveDistance", "Min Move Distance", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "mass", "Mass", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "pushStrength", "Push Strength", sceneProject, entities, settingsPush);
    propertyRow(RowPropertyType::Bool, cpType, "pushBodies", "Push Bodies", sceneProject, entities, settingsFlag);
    propertyRow(RowPropertyType::Bool, cpType, "movingPlatforms", "Moving Platforms", sceneProject, entities, settingsFlag);
    propertyRow(RowPropertyType::Bool, cpType, "inheritPlatformYaw", "Inherit Platform Yaw", sceneProject, entities, settingsFlag);
    propertyRow(RowPropertyType::Bool, cpType, "detectCollisions", "Detect Collisions", sceneProject, entities, settingsDetect);
    propertyRow(RowPropertyType::Bool, cpType, "characterCollisions", "Character Collisions", sceneProject, entities, settingsCharacters);
    propertyRow(RowPropertyType::Bool, cpType, "enhancedEdgeRemoval", "Enhanced Edge Removal", sceneProject, entities, settingsEdges);
    endTable();

    for (Entity entity : entities){
        if (scene->findComponent<RigidbodyComponent>(entity)){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextWrapped("A Character Controller does nothing on a GameObject with a Rigidbody: remove one of them.");
            break;
        }
    }

    // what the last move and physics step left, while the scene plays
    if (sceneProject->playState != ScenePlayState::STOPPED && entities.size() == 1){
        const CharacterControllerComponent& controller = scene->getComponent<CharacterControllerComponent>(entities[0]);
        std::string flags;
        if (controller.collisionFlags & CharacterCollision_Sides) flags += " Sides";
        if (controller.collisionFlags & CharacterCollision_Above) flags += " Above";
        if (controller.collisionFlags & CharacterCollision_Below) flags += " Below";
        ImGui::SeparatorText("Runtime");
        ImGui::TextDisabled("Grounded: %s   On Steep Slope: %s", controller.isGrounded ? "yes" : "no", controller.isOnSteepSlope ? "yes" : "no");
        ImGui::TextDisabled("Collision Flags:%s", flags.empty() ? " None" : flags.c_str());
        ImGui::TextDisabled("Velocity: %.2f, %.2f, %.2f", controller.velocity.x, controller.velocity.y, controller.velocity.z);
        ImGui::TextDisabled("Ground Velocity: %.2f, %.2f, %.2f", controller.groundVelocity.x, controller.groundVelocity.y, controller.groundVelocity.z);
    }
}

void editor::InspectorWindow::drawWaterVolumeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const WaterVolumeComponent& volume = scene->getComponent<WaterVolumeComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsSize = settings;
    settingsSize.help = "Local box under the surface: X and Z its extent, Y the depth";
    RowSettings settingsOffset = settings;
    settingsOffset.help = "Local height of the surface (the waves move it up and down)";
    RowSettings settingsInfinite;
    settingsInfinite.help = "An ocean: no extent and no bottom";
    RowSettings settingsDensity = settings;
    settingsDensity.help = "kg/m3: what floats in it settles as deep as its density is of this";
    RowSettings settingsFlow = settings;
    settingsFlow.help = "World m/s: what floats drifts with it";
    RowSettings settingsDrag = settings;
    settingsDrag.help = "Per second, against moving through the water (Linear) and turning in it (Angular)";
    RowSettings settingsWaves;
    settingsWaves.enumEntries = enumEntries(reflect::enums::kWaterVolumeWaves);
    settingsWaves.help = "From Material: the waves of this GameObject's library:water material (WAVES), the ones it draws";

    beginTable(cpType, getLabelSize("Surface Offset"));
    propertyRow(RowPropertyType::Vector3, cpType, "size", "Size", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::Float, cpType, "surfaceOffset", "Surface Offset", sceneProject, entities, settingsOffset);
    propertyRow(RowPropertyType::Bool, cpType, "infinite", "Infinite", sceneProject, entities, settingsInfinite);
    propertyRow(RowPropertyType::FloatPositive, cpType, "density", "Density", sceneProject, entities, settingsDensity);
    propertyRow(RowPropertyType::Vector3, cpType, "flow", "Flow", sceneProject, entities, settingsFlow);
    propertyRow(RowPropertyType::FloatPositive, cpType, "linearDrag", "Linear Drag", sceneProject, entities, settingsDrag);
    propertyRow(RowPropertyType::FloatPositive, cpType, "angularDrag", "Angular Drag", sceneProject, entities, settingsDrag);
    propertyRow(RowPropertyType::Enum, cpType, "waves", "Waves", sceneProject, entities, settingsWaves);
    endTable();

    // Fit to Mesh: the volume over the GameObject's mesh, its surface at the mesh's top
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entities[0]);
    const bool fits = entities.size() == 1 && mesh && !mesh->aabb.isNull();
    ImGui::BeginDisabled(!fits);
    if (ImGui::Button(ICON_FA_EXPAND " Fit to Mesh")){
        const Vector3 min = mesh->aabb.getMinimum();
        const Vector3 max = mesh->aabb.getMaximum();
        auto multiCmd = new MultiPropertyCmd();
        multiCmd->addPropertyCmd<Vector3>(project, sceneProject->id, entities[0], cpType, "size",
                                          Vector3(max.x - min.x, volume.size.y, max.z - min.z));
        multiCmd->addPropertyCmd<float>(project, sceneProject->id, entities[0], cpType, "surfaceOffset", max.y);
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        multiCmd->setNoMerge();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(fits ? "Size X and Z and the surface from this GameObject's mesh bounds" : "Needs a mesh on this GameObject");

    // the drawn surface is linear between vertices; the physics follows the exact waves
    if (entities.size() == 1 && volume.waves == WaterVolumeWaves::FROM_MATERIAL){
        const float waveLength = WaterEditorUtils::materialWaveLength(scene, entities[0]);
        const float spacing = WaterEditorUtils::meshVertexSpacing(scene, entities[0]);
        if (waveLength > 0.0f && spacing > waveLength * 0.6f / 8.0f){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextWrapped("The mesh's vertices are %.2f apart, more than an eighth of the shorter wave (%.2f): the drawn surface "
                               "departs from the one things float on by more than about 8%% of the wave height. Subdivide the mesh.",
                               spacing, waveLength * 0.6f / 8.0f);
        }
    }
}

void editor::InspectorWindow::drawBuoyancyComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const BuoyancyComponent& buoyancy = scene->getComponent<BuoyancyComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsColumns;
    settingsColumns.enumEntries = enumEntries(reflect::enums::kBuoyancyColumns);
    settingsColumns.help = "Where it samples the water: a grid on the bottom of its colliders, or the points below";
    RowSettings settingsVolume = settings;
    settingsVolume.help = "Times the colliders' volume: more floats higher";
    RowSettings settingsDrag = settings;
    settingsDrag.help = "Times the water's drags";

    beginTable(cpType, getLabelSize("Volume Scale"));
    propertyRow(RowPropertyType::Enum, cpType, "columns", "Columns", sceneProject, entities, settingsColumns);
    propertyRow(RowPropertyType::FloatPositive, cpType, "volumeScale", "Volume Scale", sceneProject, entities, settingsVolume);
    propertyRow(RowPropertyType::FloatPositive, cpType, "dragScale", "Drag Scale", sceneProject, entities, settingsDrag);
    endTable();

    if (buoyancy.columns == BuoyancyColumns::CUSTOM && entities.size() == 1){
        const Entity entity = entities[0];
        auto setPoints = [&](const std::vector<Vector3>& points){
            auto* cmd = new editor::PropertyCmd<std::vector<Vector3>>(project, sceneProject->id, entity, cpType, "customPoints", points);
            cmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        };
        ImGui::SeparatorText("Column Points (local, at most 9)");
        std::vector<Vector3> points = buoyancy.customPoints;
        for (size_t i = 0; i < points.size(); i++){
            ImGui::PushID((int)i);
            Vector3 point = points[i];
            ImGui::SetNextItemWidth(-3.0f * ImGui::GetFontSize());
            if (ImGui::DragFloat3("##point", &point.x, 0.01f) ){
                points[i] = point;
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                setPoints(points);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_XMARK)){
                points.erase(points.begin() + (long)i);
                setPoints(points);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::BeginDisabled(points.size() >= 9);
        if (ImGui::Button(ICON_FA_PLUS " Add Point")){
            points.push_back(Vector3(0.0f, 0.0f, 0.0f));
            setPoints(points);
        }
        ImGui::EndDisabled();
    }

    auto warning = [](const char* text){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text);
    };
    for (Entity entity : entities){
        const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
        if (!body || body->type != BodyType::DYNAMIC){
            warning("Buoyancy floats a dynamic Rigidbody: add one to this GameObject (or make it Dynamic).");
            break;
        }
    }
    if (scene->getComponentArray<WaterVolumeComponent>()->size() == 0)
        warning("There is no Water Volume in this scene: nothing floats it.");

    // its effective density against the water's
    if (entities.size() == 1){
        float volume = 0.0f, mass = 0.0f;
        const float water = WaterEditorUtils::waterDensity(scene, entities[0]);
        if (WaterEditorUtils::colliderMass(scene, entities[0], volume, mass) && water > 0.0f){
            const float effective = mass / (volume * std::max(buoyancy.volumeScale, 1e-6f));
            ImGui::TextDisabled("Effective density %.0f kg/m3 (water %.0f)", effective, water);
            if (effective >= water)
                warning("At least as dense as the water: it will sink or hover.");
        }
    }
}

