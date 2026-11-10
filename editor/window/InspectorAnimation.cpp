// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector drawers of the animation components: actions, tweens with their preview buttons, sprite
// animations, animations and keyframe tracks (key lists kept aligned with their tangents).

#include "InspectorWindow.h"
#include "component/MeshComponent.h"
#include "window/AnimationWindow.h"
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
#include "component/Transform.h"
#include "component/LatticeTracksComponent.h"
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
#include "component/Transform.h"
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

// Keep Hermite tangents (GLTF CUBICSPLINE) mirrored with values across key
// add/remove so cubic playback survives editing. A new key gets zero tangents:
// its two adjacent segments flatten toward it, the rest of the clip keeps its
// imported shape. If one tangent array is missing or short (corrupt state) it
// is zero-padded back to a valid pair, matching writeTrackKey's repair policy.
template<typename Component, typename ValueType>
static void addTangentAlignCmds(editor::Project* project, uint32_t sceneId, editor::ComponentType cpType,
                                Entity entity, Component* trackComp, editor::MultiPropertyCmd* multiCmd,
                                const ValueType& zeroTangent, long removedIndex /* -1 appends */){
    if (trackComp->inTangents.empty() && trackComp->outTangents.empty()) return;

    auto align = [&](const std::vector<ValueType>& tangents, const char* property){
        std::vector<ValueType> newTangents = tangents;
        if (newTangents.size() < trackComp->values.size()){
            newTangents.resize(trackComp->values.size(), zeroTangent);
        }
        if (removedIndex < 0){
            newTangents.push_back(zeroTangent);
        }else if ((size_t)removedIndex < newTangents.size()){
            newTangents.erase(newTangents.begin() + removedIndex);
        }
        if (newTangents != tangents){
            multiCmd->addPropertyCmd<std::vector<ValueType>>(project, sceneId, entity, cpType, property, newTangents);
        }
    };
    align(trackComp->inTangents, "inTangents");
    align(trackComp->outTangents, "outTangents");
}

// drawTrackValues companion callbacks that keep a track's tangents aligned on
// key add/remove
template<typename Component, typename ValueType>
static auto tangentCallbacks(editor::Project* project, editor::SceneProject* sceneProject, editor::ComponentType cpType, const ValueType& zeroTangent){
    auto onRemove = [project, sceneProject, cpType, zeroTangent](editor::MultiPropertyCmd* multiCmd, Entity entity, Component* comp, size_t i){
        addTangentAlignCmds<Component, ValueType>(project, sceneProject->id, cpType, entity, comp, multiCmd, zeroTangent, (long)i);
    };
    auto onAdd = [project, sceneProject, cpType, zeroTangent](editor::MultiPropertyCmd* multiCmd, Entity entity, Component* comp){
        addTangentAlignCmds<Component, ValueType>(project, sceneProject->id, cpType, entity, comp, multiCmd, zeroTangent, -1);
    };
    return std::make_pair(onRemove, onAdd);
}

void editor::InspectorWindow::drawActionComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings settingsState;
    settingsState.enumEntries = enumEntries(reflect::enums::kActionState);

    ActionComponent* actionComp = nullptr;
    Entity entity = NULL_ENTITY;
    Scene* scene = sceneProject->scene;
    bool sceneIsStopped = (sceneProject->playState == ScenePlayState::STOPPED);

    if (entities.size() == 1) {
        entity = entities[0];
        actionComp = scene->findComponent<ActionComponent>(entity);

        if (actionPreviewing && (actionPreviewEntity != entity || actionPreviewSceneId != sceneProject->id)) {
            SceneProject* prevSceneProject = project->getScene(actionPreviewSceneId);
            if (prevSceneProject && prevSceneProject->scene) {
                stopActionPreview(prevSceneProject->scene, prevSceneProject);
            } else {
                actionPreviewSnapshot.clear();
                actionPreviewing = false;
                actionPreviewPlaying = false;
                actionPreviewEntity = NULL_ENTITY;
                actionPreviewSceneId = 0;
                releaseActionPreviewOwners();
            }
        }

        if (actionPreviewing && !sceneIsStopped) {
            stopActionPreview(scene, sceneProject);
        }

        if (actionPreviewing && actionPreviewPlaying && sceneIsStopped) {
            float dt = ImGui::GetIO().DeltaTime;
            scene->getSystem<ActionSystem>()->updateActionPreview(dt, entity);
            sceneProject->needUpdateRender = true;

            actionComp = scene->findComponent<ActionComponent>(entity);
            if (actionComp && actionComp->state != ActionState::Running) {
                actionPreviewPlaying = false;
            }
        }
    }

    // Start Offset / Random Start (runtime-core spec 6.7): Random Start overrides the offset, and
    // neither applies to a particle system's controller (it has Start Delay and Prewarm)
    bool randomStart = false;
    bool controller = false;
    if (!entities.empty()) {
        if (ActionComponent* first = scene->findComponent<ActionComponent>(entities[0])) {
            randomStart = first->randomStart;
            controller = scene->getSystem<ActionSystem>()->isParticleController(entities[0], *first);
        }
    }
    RowSettings settingsOffset;
    settingsOffset.help = controller ? "A Particle System's controller starts the system: use its Start Delay or Prewarm"
                                     : "Seconds into the action each new run starts at, in Play mode and builds (previews start at 0)";
    RowSettings settingsRandom;
    settingsRandom.help = controller ? settingsOffset.help
                                     : "Each new run starts at a random point of one cycle (different per GameObject, the same "
                                       "every playthrough); overrides Start Offset";

    beginTable(cpType, getLabelSize("Owned target"));
    propertyRow(RowPropertyType::Enum, cpType, "state", "State", sceneProject, entities, settingsState);
    propertyRow(RowPropertyType::Float, cpType, "speed", "Speed", sceneProject, entities);
    ImGui::BeginDisabled(controller || randomStart);
    propertyRow(RowPropertyType::FloatPositive, cpType, "startOffset", "Start Offset", sceneProject, entities, settingsOffset);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(controller);
    propertyRow(RowPropertyType::Bool, cpType, "randomStart", "Random Start", sceneProject, entities, settingsRandom);
    ImGui::EndDisabled();
    propertyRow(RowPropertyType::LocalEntity, cpType, "target", "Target", sceneProject, entities);
    endTable();

    if (entities.size() != 1 || !actionComp) {
        return;
    }

    // Check if this entity also has AnimationComponent → sync with AnimationWindow
    AnimationWindow* animWindow = Backend::getApp().getAnimationWindow();
    bool hasAnimation = scene->findComponent<AnimationComponent>(entity) != nullptr;
    bool syncWithAnimWindow = hasAnimation && animWindow;

    ImGui::Separator();

    ImGui::BeginDisabled(!sceneIsStopped);

    if (syncWithAnimWindow) {
        // Delegate playback to AnimationWindow
        bool animIsPlaying = animWindow->getIsPlaying() && animWindow->isPreviewingEntity(entity, sceneProject->id);
        bool animIsPreviewing = animWindow->isPreviewingEntity(entity, sceneProject->id);

        if (animIsPlaying) {
            if (ImGui::Button(ICON_FA_PAUSE "##action_pause")) {
                animWindow->externalPause();
            }
        } else {
            if (ImGui::Button(ICON_FA_PLAY "##action_play")) {
                animWindow->externalPlay(entity, sceneProject->id);
            }
        }
        ImGui::SameLine();

        ImGui::BeginDisabled(!animIsPlaying && !animIsPreviewing);
        if (ImGui::Button(ICON_FA_STOP "##action_stop")) {
            animWindow->externalStop();
        }
        ImGui::EndDisabled();
    } else {
        // Standalone action preview (no AnimationComponent)
        if (actionPreviewPlaying) {
            if (ImGui::Button(ICON_FA_PAUSE "##action_pause")) {
                if (actionComp && actionComp->state == ActionState::Running) {
                    Action(scene, entity).pause();
                }
            }
        } else {
            if (ImGui::Button(ICON_FA_PLAY "##action_play")) {
                if (!actionPreviewing) {
                    startActionPreview(entity, scene, sceneProject);
                    actionComp = scene->findComponent<ActionComponent>(entity);
                } else if (actionComp) {
                    if (actionComp->state == ActionState::Stopped) {
                        stopActionPreview(scene, sceneProject);
                        startActionPreview(entity, scene, sceneProject);
                        actionComp = scene->findComponent<ActionComponent>(entity);
                    } else {
                        Action(scene, entity).start();
                    }
                }
                actionPreviewPlaying = true;
            }
        }
        ImGui::SameLine();

        ImGui::BeginDisabled(!actionPreviewPlaying && !actionPreviewing);
        if (ImGui::Button(ICON_FA_STOP "##action_stop")) {
            if (actionPreviewing) {
                stopActionPreview(scene, sceneProject);
                actionComp = scene->findComponent<ActionComponent>(entity);
            }
            actionPreviewPlaying = false;
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();

    // Time display (fixed width so it doesn't jump around)
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.2fs", actionComp->timecount);
    ImGui::SameLine();

    // Timeline progress strip
    float duration = scene->getSystem<ActionSystem>()->getDuration(entity);

    float fraction = (duration > 0) ? std::clamp(actionComp->timecount / duration, 0.0f, 1.0f) : 0.0f;

    float timelineWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    float frameHeight = ImGui::GetFrameHeight();
    float timelineHeight = 6.0f;
    float rounding = timelineHeight * 0.5f;
    ImVec2 timelinePos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##action_timeline_strip", ImVec2(timelineWidth, frameHeight));
    bool timelineHovered = ImGui::IsItemHovered();
    bool timelineClicked = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (sceneIsStopped && duration > 0 && timelineClicked) {
        float mouseX = ImGui::GetIO().MousePos.x;
        float clickFraction = std::clamp((mouseX - timelinePos.x) / timelineWidth, 0.0f, 1.0f);
        float seekTime = clickFraction * duration;

        if (syncWithAnimWindow) {
            animWindow->externalPlay(entity, sceneProject->id);
            animWindow->externalPause();
            animWindow->seekPreviewExternal(scene, sceneProject, seekTime);
            actionComp = scene->findComponent<ActionComponent>(entity);
        } else {
            if (!actionPreviewing) {
                startActionPreview(entity, scene, sceneProject);
                actionComp = scene->findComponent<ActionComponent>(entity);
            }
            if (actionPreviewing && actionComp) {
                // Restore state (with what commands wrote meanwhile) and re-evaluate at seek time
                actionPreviewSnapshot.restore(scene);

                actionComp = scene->findComponent<ActionComponent>(entity);
                if (actionComp) {
                    actionComp->state = ActionState::Stopped;
                    actionComp->timecount = 0;
                    actionComp->stopTrigger = false;
                    actionComp->pauseTrigger = false;
                    actionComp->startTrigger = true;
                }

                // Simulate to the seek time so particles and integrations match the state exactly
                float remainingTime = seekTime;
                float stepSize = 1.0f / 60.0f;
                while (remainingTime > 0.0f) {
                    float currentStep = std::min(stepSize, remainingTime);
                    scene->getSystem<ActionSystem>()->updateActionPreview(currentStep, entity);
                    remainingTime -= currentStep;
                }

                actionPreviewPlaying = false;

                actionComp = scene->findComponent<ActionComponent>(entity);
            }
        }
    }

    if (timelineHovered && duration > 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 trackColor = style.Colors[ImGuiCol_TextDisabled];
    trackColor.w = 0.22f;
    ImVec4 fillColor = style.Colors[ImGuiCol_PlotHistogram];

    ImVec2 barMin(timelinePos.x, timelinePos.y + (frameHeight - timelineHeight) * 0.5f);
    ImVec2 barMax(timelinePos.x + timelineWidth, barMin.y + timelineHeight);
    drawList->AddRectFilled(barMin, barMax, ImGui::GetColorU32(trackColor), rounding);

    if (fraction > 0.0f) {
        ImVec2 fillMax(barMin.x + timelineWidth * fraction, barMax.y);
        drawList->AddRectFilled(barMin, fillMax, ImGui::GetColorU32(fillColor), rounding);
    }

    ImGui::EndDisabled();
}

void editor::InspectorWindow::drawPositionTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    beginTable(cpType, getLabelSize("Start position"));

    // We compute a width that offsets the button
    float buttonSize = ImGui::GetFrameHeight();
    float reservedWidth = -(buttonSize + ImGui::GetStyle().ItemSpacing.x);
    RowSettings vectorSettings;
    vectorSettings.secondColSize = reservedWidth;

    // Start Position Row
    propertyRow(RowPropertyType::Vector3, cpType, "startPosition", "Start position", sceneProject, entities, vectorSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;
        Transform* targetTransform = (target != NULL_ENTITY) ? scene->findComponent<Transform>(target) : nullptr;

        ImGui::SameLine();
        ImGui::BeginDisabled(!targetTransform);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_start_pos")) {
            Vector3 pos = targetTransform->position;
            auto* cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, "startPosition", pos);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current position from target");
        }
    }

    // End Position Row
    propertyRow(RowPropertyType::Vector3, cpType, "endPosition", "End position", sceneProject, entities, vectorSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;
        Transform* targetTransform = (target != NULL_ENTITY) ? scene->findComponent<Transform>(target) : nullptr;

        ImGui::SameLine();
        ImGui::BeginDisabled(!targetTransform);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_end_pos")) {
            Vector3 pos = targetTransform->position;
            auto* cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, "endPosition", pos);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current position from target");
        }
    }

    endTable();
}

void editor::InspectorWindow::drawRotationTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    // a spin turns by an angle around an axis instead of reaching an end rotation
    bool spin = true;
    for (Entity entity : entities){
        RotationTweenComponent* rotation = scene->findComponent<RotationTweenComponent>(entity);
        spin = spin && rotation && rotation->spin;
    }

    RowSettings settingsSpin;
    settingsSpin.help = "Turns the start rotation by an angle around an axis, past half a turn too (a tween to an end rotation never turns more than 180°)";

    RowSettings settingsAngle;
    settingsAngle.secondColSize = 6 * ImGui::GetFontSize();
    settingsAngle.format = "%.1f°";
    settingsAngle.help = "More than 360° turns more than once";

    beginTable(cpType, getLabelSize("Start rotation"));

    float buttonSize = ImGui::GetFrameHeight();
    float reservedWidth = -(buttonSize + ImGui::GetStyle().ItemSpacing.x);
    RowSettings quatSettings;
    quatSettings.secondColSize = reservedWidth;

    propertyRow(RowPropertyType::Quat, cpType, "startRotation", "Start rotation", sceneProject, entities, quatSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;
        Transform* targetTransform = (target != NULL_ENTITY) ? scene->findComponent<Transform>(target) : nullptr;

        ImGui::SameLine();
        ImGui::BeginDisabled(!targetTransform);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_start_rot")) {
            Quaternion rot = targetTransform->rotation;
            auto* cmd = new PropertyCmd<Quaternion>(project, sceneProject->id, entity, cpType, "startRotation", rot);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current rotation from target");
        }
    }

    propertyRow(RowPropertyType::Bool, cpType, "spin", "Spin", sceneProject, entities, settingsSpin);

    if (spin) {
        propertyRow(RowPropertyType::Vector3, cpType, "axis", "Axis", sceneProject, entities);
        propertyRow(RowPropertyType::Float, cpType, "angle", "Angle", sceneProject, entities, settingsAngle);
        endTable();
        return;
    }

    propertyRow(RowPropertyType::Quat, cpType, "endRotation", "End rotation", sceneProject, entities, quatSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;
        Transform* targetTransform = (target != NULL_ENTITY) ? scene->findComponent<Transform>(target) : nullptr;

        ImGui::SameLine();
        ImGui::BeginDisabled(!targetTransform);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_end_rot")) {
            Quaternion rot = targetTransform->rotation;
            auto* cmd = new PropertyCmd<Quaternion>(project, sceneProject->id, entity, cpType, "endRotation", rot);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current rotation from target");
        }
    }

    propertyRow(RowPropertyType::Bool, cpType, "shortestPath", "Shortest path", sceneProject, entities);
    endTable();
}

void editor::InspectorWindow::drawScaleTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    beginTable(cpType, getLabelSize("Start scale"));

    float buttonSize = ImGui::GetFrameHeight();
    float reservedWidth = -(buttonSize + ImGui::GetStyle().ItemSpacing.x);
    RowSettings vectorSettings;
    vectorSettings.secondColSize = reservedWidth;

    propertyRow(RowPropertyType::Vector3, cpType, "startScale", "Start scale", sceneProject, entities, vectorSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;
        Transform* targetTransform = (target != NULL_ENTITY) ? scene->findComponent<Transform>(target) : nullptr;

        ImGui::SameLine();
        ImGui::BeginDisabled(!targetTransform);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_start_scale")) {
            Vector3 scl = targetTransform->scale;
            auto* cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, "startScale", scl);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current scale from target");
        }
    }

    propertyRow(RowPropertyType::Vector3, cpType, "endScale", "End scale", sceneProject, entities, vectorSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;
        Transform* targetTransform = (target != NULL_ENTITY) ? scene->findComponent<Transform>(target) : nullptr;

        ImGui::SameLine();
        ImGui::BeginDisabled(!targetTransform);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_end_scale")) {
            Vector3 scl = targetTransform->scale;
            auto* cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, "endScale", scl);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current scale from target");
        }
    }

    endTable();
}

void editor::InspectorWindow::drawColorTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    beginTable(cpType, getLabelSize("Start color"));

    float buttonSize = ImGui::GetFrameHeight();
    float reservedWidth = -(buttonSize + ImGui::GetStyle().ItemSpacing.x);
    RowSettings colorSettings;
    colorSettings.secondColSize = reservedWidth;

    propertyRow(RowPropertyType::Color3L, cpType, "startColor", "Start color", sceneProject, entities, colorSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;

        Vector3 targetColor;
        bool hasColor = false;
        if (target != NULL_ENTITY) {
            UIComponent* ui = scene->findComponent<UIComponent>(target);
            if (ui) { targetColor = Vector3(ui->color.x, ui->color.y, ui->color.z); hasColor = true; }
            else {
                MeshComponent* mesh = scene->findComponent<MeshComponent>(target);
                if (mesh) { targetColor = Vector3(mesh->submeshes[0].material.baseColorFactor.x, mesh->submeshes[0].material.baseColorFactor.y, mesh->submeshes[0].material.baseColorFactor.z); hasColor = true; }
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasColor);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_start_color")) {
            auto* cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, "startColor", targetColor);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current color from target");
        }
    }

    propertyRow(RowPropertyType::Color3L, cpType, "endColor", "End color", sceneProject, entities, colorSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;

        Vector3 targetColor;
        bool hasColor = false;
        if (target != NULL_ENTITY) {
            UIComponent* ui = scene->findComponent<UIComponent>(target);
            if (ui) { targetColor = Vector3(ui->color.x, ui->color.y, ui->color.z); hasColor = true; }
            else {
                MeshComponent* mesh = scene->findComponent<MeshComponent>(target);
                if (mesh) { targetColor = Vector3(mesh->submeshes[0].material.baseColorFactor.x, mesh->submeshes[0].material.baseColorFactor.y, mesh->submeshes[0].material.baseColorFactor.z); hasColor = true; }
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasColor);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_end_color")) {
            auto* cmd = new PropertyCmd<Vector3>(project, sceneProject->id, entity, cpType, "endColor", targetColor);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current color from target");
        }
    }

    propertyRow(RowPropertyType::Bool, cpType, "useSRGB", "Use sRGB", sceneProject, entities);
    endTable();
}

void editor::InspectorWindow::drawAlphaTweenComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    beginTable(cpType, getLabelSize("Start alpha"));

    float buttonSize = ImGui::GetFrameHeight();
    float reservedWidth = -(buttonSize + ImGui::GetStyle().ItemSpacing.x);
    RowSettings alphaSettings;
    alphaSettings.secondColSize = reservedWidth;

    propertyRow(RowPropertyType::Float, cpType, "startAlpha", "Start alpha", sceneProject, entities, alphaSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;

        float targetAlpha = 1.0f;
        bool hasAlpha = false;
        if (target != NULL_ENTITY) {
            UIComponent* ui = scene->findComponent<UIComponent>(target);
            if (ui) { targetAlpha = ui->color.w; hasAlpha = true; }
            else {
                MeshComponent* mesh = scene->findComponent<MeshComponent>(target);
                if (mesh) { targetAlpha = mesh->submeshes[0].material.baseColorFactor.w; hasAlpha = true; }
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasAlpha);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_start_alpha")) {
            auto* cmd = new PropertyCmd<float>(project, sceneProject->id, entity, cpType, "startAlpha", targetAlpha);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current alpha from target");
        }
    }

    propertyRow(RowPropertyType::Float, cpType, "endAlpha", "End alpha", sceneProject, entities, alphaSettings);

    if (entities.size() == 1) {
        Entity entity = entities[0];
        ActionComponent* actionComp = scene->findComponent<ActionComponent>(entity);
        Entity target = (actionComp && actionComp->target != NULL_ENTITY && scene->isEntityCreated(actionComp->target)) ? actionComp->target : NULL_ENTITY;

        float targetAlpha = 1.0f;
        bool hasAlpha = false;
        if (target != NULL_ENTITY) {
            UIComponent* ui = scene->findComponent<UIComponent>(target);
            if (ui) { targetAlpha = ui->color.w; hasAlpha = true; }
            else {
                MeshComponent* mesh = scene->findComponent<MeshComponent>(target);
                if (mesh) { targetAlpha = mesh->submeshes[0].material.baseColorFactor.w; hasAlpha = true; }
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasAlpha);
        ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_LOCATION_CROSSHAIRS);
        float padX = std::max(0.0f, (buttonSize - iconSize.x) / 2.0f);
        float padY = std::max(0.0f, (buttonSize - iconSize.y) / 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##get_end_alpha")) {
            auto* cmd = new PropertyCmd<float>(project, sceneProject->id, entity, cpType, "endAlpha", targetAlpha);
            CommandHandle::get(sceneProject->id)->addCommand(cmd);
        }
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Get current alpha from target");
        }
    }

    endTable();
}

void editor::InspectorWindow::drawSpriteAnimationComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    beginTable(cpType, getLabelSize("Interval index"));
    propertyRow(RowPropertyType::Bool, cpType, "loop", "Loop", sceneProject, entities);
    propertyRow(RowPropertyType::Label, cpType, "frameIndex", "Frame index", sceneProject, entities);
    propertyRow(RowPropertyType::Label, cpType, "frameTimeIndex", "Interval index", sceneProject, entities);
    propertyRow(RowPropertyType::Label, cpType, "spriteFrameCount", "Frame count", sceneProject, entities);
    endTable();

    if (entities.size() != 1) {
        separatorText("Animation Frames");
        ImGui::TextDisabled("Select a single GameObject to edit sprite animation frames");
        return;
    }

    Entity entity = entities[0];
    SpriteAnimationComponent& spriteAnim = sceneProject->scene->getComponent<SpriteAnimationComponent>(entity);

    Entity previewEntity = NULL_ENTITY;
    if (ActionComponent* actionComp = sceneProject->scene->findComponent<ActionComponent>(entity)) {
        previewEntity = actionComp->target;
    }

    SpriteComponent* spriteComp = nullptr;
    MeshComponent* meshComp = nullptr;
    if (previewEntity != NULL_ENTITY) {
        spriteComp = sceneProject->scene->findComponent<SpriteComponent>(previewEntity);
        meshComp = sceneProject->scene->findComponent<MeshComponent>(previewEntity);
    }

    Texture previewTexture;
    if (meshComp && meshComp->numSubmeshes > 0) {
        previewTexture = meshComp->submeshes[0].material.baseColorTexture;
    }

    unsigned int visibleFrameCount = std::max(spriteAnim.framesSize, spriteAnim.framesTimeSize);
    bool countsMatch = spriteAnim.framesSize == spriteAnim.framesTimeSize;
    bool canAddFrame = spriteAnim.frames.validIndex(visibleFrameCount) && spriteAnim.framesTime.validIndex(visibleFrameCount);

    separatorText("Animation Frames");

    beginTable(cpType, getLabelSize("Animation Frames"), "sprite_animation_frames_header");
    propertyHeader("Animation Frames", -1, false, false);
    ImGui::Text("%u", visibleFrameCount);
    if (!countsMatch) {
        ImGui::SameLine();
        ImGui::TextDisabled("(frames/intervals mismatch)");
    }
    float spriteAnimArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Frame##sprite_animation_add", spriteAnimArrowWidth, canAddFrame,
            "A Sprite Animation holds at most 65,536 frames")) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (const Entity& selectedEntity : entities) {
            if (SpriteAnimationComponent* animComp = sceneProject->scene->findComponent<SpriteAnimationComponent>(selectedEntity)) {
                unsigned int nextIndex = std::max(animComp->framesSize, animComp->framesTimeSize);
                if (!animComp->frames.validIndex(nextIndex) || !animComp->framesTime.validIndex(nextIndex)) {
                    continue;
                }

                int defaultFrame = 0;
                if (animComp->framesSize > 0 && animComp->frames.validIndex(animComp->framesSize - 1)) {
                    defaultFrame = animComp->frames[animComp->framesSize - 1];
                }

                multiCmd->addPropertyCmd<int>(project, sceneProject->id, selectedEntity, cpType,
                    "frames[" + std::to_string(nextIndex) + "]", defaultFrame);
                multiCmd->addPropertyCmd<int>(project, sceneProject->id, selectedEntity, cpType,
                    "framesTime[" + std::to_string(nextIndex) + "]", 100);
                multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, selectedEntity, cpType,
                    "framesSize", nextIndex + 1);
                multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, selectedEntity, cpType,
                    "framesTimeSize", nextIndex + 1);
            }
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_all_sprite_animation_frames", spriteAnimationFramesExpanded ? ImGuiDir_Up : ImGuiDir_Down)) {
        spriteAnimationFramesExpanded = !spriteAnimationFramesExpanded;
    }
    endTable();

    if (!spriteAnimationFramesExpanded || visibleFrameCount == 0) {
        return;
    }

    RowSettings settingsFrame;
    settingsFrame.stepSize = 1.0f;
    settingsFrame.format = "%.0f";
    settingsFrame.showColors = false;

    RowSettings settingsInterval = settingsFrame;

    for (unsigned int i = 0; i < visibleFrameCount; i++) {
        ImGui::PushID((int)i);

        std::string frameGroupStr = "sprite_animation_frame_" + std::to_string(i);
        std::string framePropId = "frames[" + std::to_string(i) + "]";
        std::string intervalPropId = "framesTime[" + std::to_string(i) + "]";

        bool hasFrameValue = i < spriteAnim.framesSize;
        bool hasIntervalValue = i < spriteAnim.framesTimeSize;
        int frameId = hasFrameValue ? spriteAnim.frames[i] : -1;

        bool hasSpriteFrame = spriteComp && frameId >= 0 && (unsigned int)frameId < spriteComp->numFramesRect;
        Rect frameRect;
        std::string spriteFrameName = "Invalid";
        if (hasSpriteFrame) {
            frameRect = spriteComp->framesRect[frameId].rect;
            if (!spriteComp->framesRect[frameId].name.empty()) {
                spriteFrameName = spriteComp->framesRect[frameId].name;
            } else {
                spriteFrameName = "Frame " + std::to_string(frameId);
            }
        }

        std::string frameLabel = "[" + std::to_string(i) + "] ";
        if (hasFrameValue) {
            frameLabel += spriteFrameName;
        } else {
            frameLabel += "Missing frame";
        }

        separatorText(frameLabel.c_str());

        beginTable(cpType, getLabelSize("Interval (ms)"), frameGroupStr);
        propertyHeader("Frame", -1, false, false);

        float previewSize = ImGui::GetFrameHeight() * 2.2f;
        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
        ImVec2 deleteButtonSize = ImVec2(clearButtonWidth + clearButtonFramePadding * 2, 0);
        ImVec2 arrowButtonSize = ImGui::CalcItemSize(ImVec2(0, 0), ImGui::GetFrameHeight(), ImGui::GetFrameHeight());

        bool hasFramePreview = hasSpriteFrame
                && !previewTexture.empty()
                && frameRect.getWidth() > 0.0f
                && frameRect.getHeight() > 0.0f;
        float trailingWidth = deleteButtonSize.x + ImGui::GetStyle().ItemSpacing.x + arrowButtonSize.x;
        if (hasFramePreview) {
            trailingWidth += previewSize + ImGui::GetStyle().ItemSpacing.x;
        }

        float targetX = ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - trailingWidth);
        ImGui::SetCursorPosX(targetX);

        if (hasFramePreview) {
            drawSpriteFramePreview(&previewTexture, frameRect, ImVec2(previewSize, previewSize), "##sprite_animation_frame_preview");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Sprite frame %d", frameId);
                if (spriteComp && !spriteComp->framesRect[frameId].name.empty()) {
                    ImGui::TextDisabled("%s", spriteComp->framesRect[frameId].name.c_str());
                }

                float tooltipMaxSize = Theme::dpi(200.0f);
                float scale = std::min(tooltipMaxSize / std::max(1.0f, frameRect.getWidth()),
                                       tooltipMaxSize / std::max(1.0f, frameRect.getHeight()));
                scale = std::min(scale, 1.0f);
                ImVec2 tooltipSize(std::max(Theme::dpi(32.0f), frameRect.getWidth() * scale),
                                   std::max(Theme::dpi(32.0f), frameRect.getHeight() * scale));

                drawSpriteFramePreview(&previewTexture, frameRect, tooltipSize, "##sprite_animation_frame_preview_tooltip");
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
        }

        if (ImGui::ArrowButton("##toggle_sprite_animation_frame", spriteAnimationFramesButtonGroups[frameGroupStr] ? ImGuiDir_Up : ImGuiDir_Down)) {
            spriteAnimationFramesButtonGroups[frameGroupStr] = !spriteAnimationFramesButtonGroups[frameGroupStr];
        }
        ImGui::SameLine();

        bool deleted = false;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
        if (ImGui::Button(ICON_FA_TRASH_CAN "##delete_sprite_animation_frame", deleteButtonSize)) {
            MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
            for (const Entity& selectedEntity : entities) {
                if (SpriteAnimationComponent* animComp = sceneProject->scene->findComponent<SpriteAnimationComponent>(selectedEntity)) {
                    if (i < animComp->framesSize) {
                        for (unsigned int j = i; j + 1 < animComp->framesSize; j++) {
                            multiCmd->addPropertyCmd<int>(project, sceneProject->id, selectedEntity, cpType,
                                "frames[" + std::to_string(j) + "]", animComp->frames[j + 1]);
                        }
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, selectedEntity, cpType,
                            "frames[" + std::to_string(animComp->framesSize - 1) + "]", 0);
                        multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, selectedEntity, cpType,
                            "framesSize", animComp->framesSize - 1);
                    }

                    if (i < animComp->framesTimeSize) {
                        for (unsigned int j = i; j + 1 < animComp->framesTimeSize; j++) {
                            multiCmd->addPropertyCmd<int>(project, sceneProject->id, selectedEntity, cpType,
                                "framesTime[" + std::to_string(j) + "]", animComp->framesTime[j + 1]);
                        }
                        multiCmd->addPropertyCmd<int>(project, sceneProject->id, selectedEntity, cpType,
                            "framesTime[" + std::to_string(animComp->framesTimeSize - 1) + "]", 0);
                        multiCmd->addPropertyCmd<unsigned int>(project, sceneProject->id, selectedEntity, cpType,
                            "framesTimeSize", animComp->framesTimeSize - 1);
                    }

                    int nextFrameIndex = 0;
                    if (animComp->framesSize > 1) {
                        nextFrameIndex = std::min(animComp->frameIndex, (int)animComp->framesSize - 2);
                    }
                    multiCmd->addPropertyCmd<int>(project, sceneProject->id, selectedEntity, cpType,
                        "frameIndex", std::max(0, nextFrameIndex));

                    int nextFrameTimeIndex = 0;
                    if (animComp->framesTimeSize > 1) {
                        nextFrameTimeIndex = std::min(animComp->frameTimeIndex, (int)animComp->framesTimeSize - 2);
                    }
                    multiCmd->addPropertyCmd<int>(project, sceneProject->id, selectedEntity, cpType,
                        "frameTimeIndex", std::max(0, nextFrameTimeIndex));
                }
            }
            multiCmd->setNoMerge();
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            deleted = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (!deleted && spriteAnimationFramesButtonGroups[frameGroupStr]) {
            propertyRow(RowPropertyType::Int, cpType, framePropId, "Sprite Frame", sceneProject, entities, settingsFrame);
            propertyRow(RowPropertyType::Int, cpType, intervalPropId, "Interval (ms)", sceneProject, entities, settingsInterval);

            if (previewEntity == NULL_ENTITY) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Preview");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Assign Action target to preview animation frames");
            } else if (!spriteComp) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Preview");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Target GameObject needs a Sprite Renderer to preview animation frames");
            } else if (!meshComp || meshComp->numSubmeshes == 0 || previewTexture.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Preview");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Target GameObject needs a Mesh Renderer with a valid texture for previews");
            } else if (!hasFrameValue) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Preview");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Missing frame entry for this slot");
            } else if (frameId < 0 || (unsigned int)frameId >= spriteComp->numFramesRect) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Preview");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Frame %d is outside SpriteComponent frame range", frameId);
            } else if (!hasIntervalValue) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Interval");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("Missing interval entry for this slot");
            }
        }

        endTable();
        ImGui::PopID();
    }
}

void editor::InspectorWindow::drawAnimationComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    beginTable(cpType, getLabelSize("Owned actions"));
    propertyRow(RowPropertyType::Bool, cpType, "loop", "Loop", sceneProject, entities);
    propertyRow(RowPropertyType::Float, cpType, "duration", "Duration", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "ownedActions", "Owned actions", sceneProject, entities);
    propertyRow(RowPropertyType::FloatPositive, cpType, "defaultFadeTime", "Fade time", sceneProject, entities);
    endTable();

    // Open in Animation window, Convert to Clip... (animation clips spec 4.2, 4.5)
    if (entities.size() == 1) {
        drawAnimationClipButtons(sceneProject, entities[0]);
    }

    AnimationComponent& anim = sceneProject->scene->getComponent<AnimationComponent>(entities[0]);

    separatorText("Actions");

    beginTable(cpType, getLabelSize("Actions"), "animation_actions_header");
    prefabRowProperty = "actions";
    propertyHeader("Actions", -1, false, false);
    ImGui::Text("%zu", anim.actions.size());
    if (drawSummaryAddButton("Add Action##animation_actions_add")) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (Entity entity : entities) {
            if (AnimationComponent* animComp = sceneProject->scene->findComponent<AnimationComponent>(entity)) {
                std::vector<ActionFrame> newActions = animComp->actions;
                // Duration 0 = auto: once an action is assigned, the frame follows
                // the action's own duration.
                ActionFrame newFrame = {0.0f, 0.0f, NULL_ENTITY, 0};

                ActionSystem* actionSystem = sceneProject->scene->getSystem<ActionSystem>().get();
                bool overlap;
                do {
                    overlap = false;
                    for (const auto& a : newActions) {
                        if (a.track == newFrame.track) {
                            float startA = a.startTime;
                            float endA = startA + std::max(actionSystem->getFrameDuration(a), 0.01f);
                            float startB = newFrame.startTime;
                            float endB = startB + std::max(actionSystem->getFrameDuration(newFrame), 0.01f);
                            if (std::max(startA, startB) < std::min(endA, endB)) {
                                overlap = true;
                                newFrame.track++;
                                break;
                            }
                        }
                    }
                } while (overlap);

                newActions.push_back(newFrame);
                multiCmd->addPropertyCmd<std::vector<ActionFrame>>(project, sceneProject->id, entity, cpType, "actions", newActions, nullptr);
            }
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }
    endTable();

    if (!anim.actions.empty()) {
        // Entities that cannot be assigned as actions: the edited animations themselves
        // and any animation whose action tree already reaches one (would create a cycle)
        std::vector<Entity> cycleExclusions;
        {
            ActionSystem* actionSystem = sceneProject->scene->getSystem<ActionSystem>().get();
            auto animArray = sceneProject->scene->getComponentArray<AnimationComponent>();
            for (int a = 0; a < (int)animArray->size(); a++){
                Entity candidate = animArray->getEntity(a);
                for (Entity edited : entities){
                    if (actionSystem->isAnimationReachable(candidate, edited)){
                        cycleExclusions.push_back(candidate);
                        break;
                    }
                }
            }
        }

        beginTable(cpType, getLabelSize("Start Time"), "animation_actions_table");
        for (size_t i = 0; i < anim.actions.size(); i++) {
            ImGui::PushID((int)i);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Action %zu", i);
            ImGui::TableSetColumnIndex(1);

            if (ImGui::Button(ICON_FA_TRASH_CAN "##remove_action")) {
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities) {
                    if (AnimationComponent* animComp = sceneProject->scene->findComponent<AnimationComponent>(entity)) {
                        if (i < animComp->actions.size()) {
                            std::vector<ActionFrame> newActions = animComp->actions;
                            newActions.erase(newActions.begin() + i);
                            multiCmd->addPropertyCmd<std::vector<ActionFrame>>(project, sceneProject->id, entity, cpType, "actions", newActions, nullptr);
                        }
                    }
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                ImGui::PopID();
                break; // break early to reset next frame cleanly
            }

            std::string prefix = "actions[" + std::to_string(i) + "]";
            propertyRow(RowPropertyType::UInt, cpType, prefix + ".track", "Track", sceneProject, entities);
            RowSettings settingsStartTime;
            settingsStartTime.stepSize = 0.01f;
            propertyRow(RowPropertyType::Float, cpType, prefix + ".startTime", "Start Time", sceneProject, entities, settingsStartTime);

            // Duration row: "Duration | [auto checkbox] [drag or resolved label]".
            // Checked = auto (duration 0): the frame follows the action's own duration.
            ActionSystem* actionSystem = sceneProject->scene->getSystem<ActionSystem>().get();
            std::string durationId = prefix + ".duration";
            bool isAuto = (anim.actions[i].duration <= 0);

            float* durValue = nullptr;
            std::map<Entity, float> eDurValue;
            bool durDif = false;
            float* durDef = nullptr;
            for (Entity& entity : entities){
                PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, cpType, durationId);
                // frames are counted from the first entity, the others may have fewer
                if (!prop.ref){
                    continue;
                }
                durDef = static_cast<float*>(prop.def);
                eDurValue[entity] = *static_cast<float*>(prop.ref);
                if (durValue && *durValue != eDurValue[entity])
                    durDif = true;
                durValue = &eDurValue[entity];
            }
            float newDurValue = *durValue;
            bool durDefChanged = durDef ? (newDurValue != *durDef) : false;

            prefabRowProperty = durationId;
            if (propertyHeader("Duration", -1, durDefChanged, false)){
                for (Entity& entity : entities){
                    cmd = new PropertyCmd<float>(project, sceneProject->id, entity, cpType, durationId, *durDef,
                        [sceneProject]() { sceneProject->isModified = true; });
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                    finishProperty = true;
                }
            }

            bool autoChecked = isAuto;
            if (ImGui::Checkbox("##auto_duration", &autoChecked)) {
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities) {
                    if (AnimationComponent* animComp = sceneProject->scene->findComponent<AnimationComponent>(entity)) {
                        if (i < animComp->actions.size()) {
                            float newDuration = 0.0f;
                            if (!autoChecked) {
                                // Freeze the currently resolved duration as the explicit value
                                newDuration = actionSystem->getFrameDuration(animComp->actions[i]);
                                if (newDuration <= 0) newDuration = 1.0f;
                            }
                            multiCmd->addPropertyCmd<float>(project, sceneProject->id, entity, cpType, durationId, newDuration,
                                [sceneProject]() { sceneProject->isModified = true; });
                        }
                    }
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Auto: follow the action's own duration");
            }
            ImGui::SameLine();

            if (isAuto) {
                float resolved = actionSystem->getFrameDuration(anim.actions[i]);
                ImGui::TextDisabled("%.2fs (auto)", resolved);
            } else {
                ImGui::SetNextItemWidth(-1);
                if (durDif)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                if (ImGui::DragFloat("##input_duration", &newDurValue, 0.01f, 0.0f, 0.0f, "%.2f")){
                    for (Entity& entity : entities){
                        cmd = new PropertyCmd<float>(project, sceneProject->id, entity, cpType, durationId, newDurValue,
                            [sceneProject]() { sceneProject->isModified = true; });
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                    }
                }
                if (durDif)
                    ImGui::PopStyleColor();
            }

            // Close command merging like propertyRow does
            if (ImGui::IsItemDeactivatedAfterEdit() || finishProperty) {
                if (cmd){
                    cmd->setNoMerge();
                    cmd = nullptr;
                }
                finishProperty = false;
            }
            RowSettings settingsAction;
            settingsAction.entityFilter.set(sceneProject->scene->getComponentId<ActionComponent>());
            settingsAction.excludeEntities = cycleExclusions;
            propertyRow(RowPropertyType::LocalEntity, cpType, prefix + ".action", "Action", sceneProject, entities, settingsAction);

            if (i < anim.actions.size() - 1) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Separator();
                ImGui::TableSetColumnIndex(1);
                ImGui::Separator();
            }

            ImGui::PopID();
        }
        endTable();
    }
}

void editor::InspectorWindow::drawKeyframeTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    KeyframeTracksComponent& comp = sceneProject->scene->getComponent<KeyframeTracksComponent>(entities[0]);

    // relative only applies to the transform tracks
    bool transformTracks = true;
    for (Entity entity : entities){
        Signature signature = sceneProject->scene->getSignature(entity);
        transformTracks = transformTracks && (signature.test(sceneProject->scene->getComponentId<TranslateTracksComponent>()) ||
            signature.test(sceneProject->scene->getComponentId<RotateTracksComponent>()) ||
            signature.test(sceneProject->scene->getComponentId<ScaleTracksComponent>()));
    }

    RowSettings settingsLoop;
    settingsLoop.help = "Starts over after the last key";
    RowSettings settingsRelative;
    settingsRelative.help = "Keys are offsets from the pose the target has when the track starts";

    beginTable(cpType, getLabelSize("Interpolation"));
    propertyRow(RowPropertyType::Bool, cpType, "loop", "Loop", sceneProject, entities, settingsLoop);
    if (transformTracks){
        propertyRow(RowPropertyType::Bool, cpType, "relative", "Relative", sceneProject, entities, settingsRelative);
    }
    propertyRow(RowPropertyType::Int, cpType, "index", "Index", sceneProject, entities);
    propertyRow(RowPropertyType::Float, cpType, "interpolation", "Interpolation", sceneProject, entities);
    endTable();

    // Keep easings aligned when a key is removed: removing key k removes one
    // segment (a middle removal merges its neighbors, keeping the leading ease)
    auto reconcileEasings = [this, sceneProject, cpType](MultiPropertyCmd* multiCmd, Entity entity, KeyframeTracksComponent* trackComp, size_t removedIndex){
        size_t oldSegments = trackComp->times.size() > 1 ? trackComp->times.size() - 1 : 0;
        if (oldSegments == 0 || trackComp->easings.empty()){
            return;
        }

        std::vector<EaseType> newEasings = trackComp->easings;
        size_t segRemove = std::min(removedIndex, oldSegments - 1);
        if (segRemove < newEasings.size()){
            newEasings.erase(newEasings.begin() + (long int)segRemove);
        }
        size_t newSegments = oldSegments - 1;
        if (newEasings.size() > newSegments){
            newEasings.resize(newSegments);
        }
        if (newEasings != trackComp->easings){
            multiCmd->addPropertyCmd<std::vector<EaseType>>(project, sceneProject->id, entity, cpType, "easings", newEasings);
        }
    };

    drawTrackValues<KeyframeTracksComponent, float>(cpType, sceneProject, entities, RowPropertyType::Float, 0.0f, "keyframe", &KeyframeTracksComponent::times, "times", reconcileEasings);

    // Per-segment easing: easing i shapes the interpolation from key i to key i+1.
    // Missing entries mean linear, so the list cannot grow past the segment count.
    beginTable(cpType, getLabelSize("Ease 00 - 00"), "keyframe_easings_table");

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Easing");
    ImGui::TableSetColumnIndex(1);

    size_t easingCount = comp.easings.size();
    size_t maxEasingCount = easingCount;
    bool easingCountsDiffer = false;
    bool canAddEasing = false;
    bool anyTrackHasSegments = false;
    for (Entity entity : entities){
        if (KeyframeTracksComponent* trackComp = sceneProject->scene->findComponent<KeyframeTracksComponent>(entity)){
            size_t segments = trackComp->times.size() > 1 ? trackComp->times.size() - 1 : 0;
            easingCountsDiffer = easingCountsDiffer || trackComp->easings.size() != easingCount;
            maxEasingCount = std::max(maxEasingCount, trackComp->easings.size());
            canAddEasing = canAddEasing || trackComp->easings.size() < segments;
            anyTrackHasSegments = anyTrackHasSegments || segments > 0;
        }
    }

    if (easingCountsDiffer){
        ImGui::TextUnformatted("---");
    }else{
        ImGui::Text("%zu", easingCount);
    }
    float easingArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const char* addEasingTooltip = !anyTrackHasSegments
        ? "Add at least two keyframe values before adding easing"
        : "Easing already covers every keyframe segment";
    if (drawSummaryAddButton("Add Ease##keyframe_easings_add", easingArrowWidth, canAddEasing, addEasingTooltip)){
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (Entity entity : entities){
            if (KeyframeTracksComponent* trackComp = sceneProject->scene->findComponent<KeyframeTracksComponent>(entity)){
                size_t segments = trackComp->times.size() > 1 ? trackComp->times.size() - 1 : 0;
                if (trackComp->easings.size() >= segments){
                    continue;
                }

                std::vector<EaseType> newEasings = trackComp->easings;
                EaseType newEasing = newEasings.empty() ? EaseType::LINEAR : newEasings.back();
                newEasings.push_back(newEasing);
                multiCmd->addPropertyCmd<std::vector<EaseType>>(project, sceneProject->id, entity, cpType, "easings", newEasings);
            }
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }

    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_keyframe_easings", keyframeEasingsExpanded ? ImGuiDir_Up : ImGuiDir_Down)){
        keyframeEasingsExpanded = !keyframeEasingsExpanded;
    }

    if (keyframeEasingsExpanded){
        // CUSTOM and CURVE are omitted here: per-segment easings store only the EaseType.
        std::vector<const char*> easeNames;
        std::vector<EaseType> easeValues;
        for (const auto& entry : (*enumEntries(reflect::enums::kEaseType))){
            if (entry.value == (int)EaseType::CUSTOM || entry.value == (int)EaseType::CURVE){
                continue;
            }
            easeNames.push_back(entry.name);
            easeValues.push_back((EaseType)entry.value);
        }

        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;
        float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x;
        ImVec2 inputSize = ImVec2(ImGui::GetContentRegionAvail().x - clearButtonWidth - ImGui::GetStyle().ItemSpacing.x - clearButtonFramePadding * 2, 0);
        if (inputSize.x < 100.0f){
            inputSize.x = 100.0f;
        }

        for (size_t i = 0; i < maxEasingCount; i++){
            ImGui::PushID((int)i);

            EaseType current = EaseType::LINEAR;
            bool hasCurrent = false;
            bool easingValuesDiffer = false;
            bool defChanged = false;
            for (Entity entity : entities){
                if (KeyframeTracksComponent* trackComp = sceneProject->scene->findComponent<KeyframeTracksComponent>(entity)){
                    size_t segments = trackComp->times.size() > 1 ? trackComp->times.size() - 1 : 0;
                    if (i >= segments){
                        continue;
                    }

                    EaseType entityEasing = i < trackComp->easings.size() ? trackComp->easings[i] : EaseType::LINEAR;
                    if (!hasCurrent){
                        current = entityEasing;
                        hasCurrent = true;
                    }else if (current != entityEasing){
                        easingValuesDiffer = true;
                    }
                    defChanged = defChanged || entityEasing != EaseType::LINEAR;
                }
            }

            auto applyEasing = [&](EaseType newType){
                MultiPropertyCmd* multiCmd = nullptr;
                for (Entity entity : entities){
                    if (KeyframeTracksComponent* trackComp = sceneProject->scene->findComponent<KeyframeTracksComponent>(entity)){
                        size_t segments = trackComp->times.size() > 1 ? trackComp->times.size() - 1 : 0;
                        if (i >= segments){
                            continue;
                        }
                        std::vector<EaseType> newEasings = trackComp->easings;
                        newEasings.resize(std::max(newEasings.size(), i + 1), EaseType::LINEAR);
                        newEasings[i] = newType;
                        if (!multiCmd){
                            multiCmd = new MultiPropertyCmd();
                        }
                        multiCmd->addPropertyCmd<std::vector<EaseType>>(project, sceneProject->id, entity, cpType, "easings", newEasings);
                    }
                }
                if (!multiCmd){
                    return;
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
            };

            std::string label = "Ease " + std::to_string(i) + " - " + std::to_string(i + 1);
            if (propertyHeader(label, inputSize.x, defChanged, false)){
                applyEasing(EaseType::LINEAR);
            }

            int itemCurrent = 0;
            for (size_t n = 0; n < easeValues.size(); n++){
                if (easeValues[n] == current){
                    itemCurrent = (int)n;
                    break;
                }
            }

            if (!hasCurrent){
                ImGui::BeginDisabled();
            }
            if (easingValuesDiffer){
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            }
            bool easingChanged = ImGui::Combo("##keyframe_ease", &itemCurrent, easeNames.data(), (int)easeNames.size());
            if (easingValuesDiffer){
                ImGui::PopStyleColor();
            }
            if (!hasCurrent){
                ImGui::EndDisabled();
            }
            if (easingChanged){
                applyEasing(easeValues[itemCurrent]);
            }

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
            if (ImGui::Button(ICON_FA_TRASH_CAN "##remove_keyframe_ease")){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (KeyframeTracksComponent* trackComp = sceneProject->scene->findComponent<KeyframeTracksComponent>(entity)){
                        if (i >= trackComp->easings.size()){
                            continue;
                        }
                        std::vector<EaseType> newEasings = trackComp->easings;
                        newEasings.erase(newEasings.begin() + (long int)i);
                        multiCmd->addPropertyCmd<std::vector<EaseType>>(project, sceneProject->id, entity, cpType, "easings", newEasings);
                    }
                }
                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);

                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                ImGui::PopID();
                break;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()){
                ImGui::SetTooltip("Remove easing");
            }

            ImGui::PopID();
        }
    }

    endTable();
}

void editor::InspectorWindow::drawTranslateTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    auto [onRemove, onAdd] = tangentCallbacks<TranslateTracksComponent>(project, sceneProject, cpType, Vector3::ZERO);
    drawTrackValues<TranslateTracksComponent, Vector3>(cpType, sceneProject, entities, RowPropertyType::Vector3, Vector3::ZERO, "translate", &TranslateTracksComponent::values, "values", onRemove, onAdd);
}

void editor::InspectorWindow::drawRotateTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    auto [onRemove, onAdd] = tangentCallbacks<RotateTracksComponent>(project, sceneProject, cpType, Quaternion(0.0f, 0.0f, 0.0f, 0.0f));
    drawTrackValues<RotateTracksComponent, Quaternion>(cpType, sceneProject, entities, RowPropertyType::Quat, Quaternion::IDENTITY, "rotate", &RotateTracksComponent::values, "values", onRemove, onAdd);
}

void editor::InspectorWindow::drawScaleTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    auto [onRemove, onAdd] = tangentCallbacks<ScaleTracksComponent>(project, sceneProject, cpType, Vector3::ZERO);
    drawTrackValues<ScaleTracksComponent, Vector3>(cpType, sceneProject, entities, RowPropertyType::Vector3, Vector3(1.0f, 1.0f, 1.0f), "scale", &ScaleTracksComponent::values, "values", onRemove, onAdd);
}

void editor::InspectorWindow::drawMorphTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    MorphTracksComponent& comp = sceneProject->scene->getComponent<MorphTracksComponent>(entities[0]);

    float firstColSize = getLabelSize("Value 000");
    beginTable(cpType, firstColSize, "morph_values_header");

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Values");
    ImGui::TableSetColumnIndex(1);

    ImGui::Text("%zu", comp.values.size());
    float morphValuesArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton("Add Value##morph_values_add", morphValuesArrowWidth)) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (Entity entity : entities){
            if (MorphTracksComponent* trackComp = sceneProject->scene->findComponent<MorphTracksComponent>(entity)){
                auto newValues = trackComp->values;
                std::vector<float> newValue;
                if (!newValues.empty()){
                    newValue = newValues.back();
                }
                newValues.push_back(newValue);
                multiCmd->addPropertyCmd<std::vector<std::vector<float>>>(project, sceneProject->id, entity, cpType, "values", newValues);
                std::vector<float> zeroTangent(newValue.size(), 0.0f);
                addTangentAlignCmds<MorphTracksComponent, std::vector<float>>(project, sceneProject->id, cpType, entity, trackComp, multiCmd, zeroTangent, -1);
            }
        }

        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }

    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_morph_values", trackValuesExpanded["morph"] ? ImGuiDir_Up : ImGuiDir_Down)){
        trackValuesExpanded["morph"] = !trackValuesExpanded["morph"];
    }

    bool removedValue = false;

    if (trackValuesExpanded["morph"]){
        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;

        for (size_t i = 0; i < comp.values.size(); i++){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Value %zu", i);
            ImGui::TableSetColumnIndex(1);

            ImGui::Text("%zu weights", comp.values[i].size());

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
            std::string removeId = std::string(ICON_FA_TRASH_CAN) + "##remove_morph_value_" + std::to_string(i);
            if (ImGui::Button(removeId.c_str())){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (MorphTracksComponent* trackComp = sceneProject->scene->findComponent<MorphTracksComponent>(entity)){
                        if (i < trackComp->values.size()){
                            auto newValues = trackComp->values;
                            newValues.erase(newValues.begin() + (long int)i);
                            multiCmd->addPropertyCmd<std::vector<std::vector<float>>>(project, sceneProject->id, entity, cpType, "values", newValues);
                            std::vector<float> zeroTangent(trackComp->values.back().size(), 0.0f);
                            addTangentAlignCmds<MorphTracksComponent, std::vector<float>>(project, sceneProject->id, cpType, entity, trackComp, multiCmd, zeroTangent, (long)i);
                        }
                    }
                }

                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);

                removedValue = true;
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                break;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove value");
            }

            for (size_t j = 0; j < comp.values[i].size(); j++){
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("  Weight %zu", j);
                ImGui::TableSetColumnIndex(1);

                float val = comp.values[i][j];
                std::string weightId = "##morph_w_" + std::to_string(i) + "_" + std::to_string(j);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat(weightId.c_str(), &val, 0.01f, 0.0f, 0.0f, "%.4f")){
                    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        if (MorphTracksComponent* trackComp = sceneProject->scene->findComponent<MorphTracksComponent>(entity)){
                            if (i < trackComp->values.size() && j < trackComp->values[i].size()){
                                auto newValues = trackComp->values;
                                newValues[i][j] = val;
                                multiCmd->addPropertyCmd<std::vector<std::vector<float>>>(project, sceneProject->id, entity, cpType, "values", newValues);
                            }
                        }
                    }
                    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                }
            }
        }
    }

    if (removedValue){
        endTable();
        return;
    }

    endTable();
}

// a warning line of the lattice tracks (as the lattice drawers draw theirs)
static void drawTrackWarning(const std::string& text){
    ImGui::PushStyleColor(ImGuiCol_Text, editor::Theme::Colors::WarningText);
    ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
    ImGui::PopStyleColor();
}

void editor::InspectorWindow::drawLatticeTracksComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    LatticeTracksComponent& comp = sceneProject->scene->getComponent<LatticeTracksComponent>(entities[0]);

    float firstColSize = getLabelSize("Key 000");
    beginTable(cpType, firstColSize, "lattice_tracks_header");

    prefabRowProperty = "points";
    propertyHeader("Points", -1, false, false);
    if (comp.points.empty()){
        ImGui::TextDisabled("none");
    }else{
        std::string list;
        for (size_t p = 0; p < comp.points.size() && list.size() < 64; p++){
            list += (p ? ", " : "") + std::to_string(comp.points[p]);
        }
        if (list.size() >= 64) list += ", ...";
        ImGui::Text("%zu", comp.points.size());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", list.c_str());
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Keys");
    ImGui::TableSetColumnIndex(1);

    ImGui::Text("%zu", comp.values.size());
    float keysArrowWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (drawSummaryAddButton("Add Key##lattice_values_add", keysArrowWidth)) {
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (Entity entity : entities){
            if (LatticeTracksComponent* trackComp = sceneProject->scene->findComponent<LatticeTracksComponent>(entity)){
                auto newValues = trackComp->values;
                std::vector<Vector3> newValue(trackComp->points.size(), Vector3::ZERO);
                if (!newValues.empty()){
                    newValue = newValues.back();
                }
                newValues.push_back(newValue);
                multiCmd->addPropertyCmd<std::vector<std::vector<Vector3>>>(project, sceneProject->id, entity, cpType, "values", newValues);
                std::vector<Vector3> zeroTangent(newValue.size(), Vector3::ZERO);
                addTangentAlignCmds<LatticeTracksComponent, std::vector<Vector3>>(project, sceneProject->id, cpType, entity, trackComp, multiCmd, zeroTangent, -1);
            }
        }

        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    }

    ImGui::SameLine();
    if (ImGui::ArrowButton("##toggle_lattice_values", trackValuesExpanded["lattice"] ? ImGuiDir_Up : ImGuiDir_Down)){
        trackValuesExpanded["lattice"] = !trackValuesExpanded["lattice"];
    }

    if (trackValuesExpanded["lattice"]){
        float clearButtonFramePadding = ImGui::GetStyle().FramePadding.x / 4.0f;

        for (size_t i = 0; i < comp.values.size(); i++){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Key %zu", i);
            ImGui::TableSetColumnIndex(1);

            size_t moved = 0;
            for (const Vector3& offset : comp.values[i]){
                if (offset != Vector3::ZERO) moved++;
            }
            ImGui::Text("%zu offsets, %zu moved", comp.values[i].size(), moved);

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(clearButtonFramePadding, ImGui::GetStyle().FramePadding.y));
            std::string removeId = std::string(ICON_FA_TRASH_CAN) + "##remove_lattice_value_" + std::to_string(i);
            bool removed = false;
            if (ImGui::Button(removeId.c_str())){
                MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
                for (Entity entity : entities){
                    if (LatticeTracksComponent* trackComp = sceneProject->scene->findComponent<LatticeTracksComponent>(entity)){
                        if (i < trackComp->values.size()){
                            auto newValues = trackComp->values;
                            newValues.erase(newValues.begin() + (long int)i);
                            multiCmd->addPropertyCmd<std::vector<std::vector<Vector3>>>(project, sceneProject->id, entity, cpType, "values", newValues);
                            std::vector<Vector3> zeroTangent(trackComp->points.size(), Vector3::ZERO);
                            addTangentAlignCmds<LatticeTracksComponent, std::vector<Vector3>>(project, sceneProject->id, cpType, entity, trackComp, multiCmd, zeroTangent, (long)i);
                        }
                    }
                }

                multiCmd->setNoMerge();
                CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
                removed = true;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove key");
            }
            if (removed){
                break;
            }
        }
    }

    endTable();

    if (entities.size() == 1 && !comp.values.empty()){
        for (const std::vector<Vector3>& key : comp.values){
            if (key.size() != comp.points.size()){
                drawTrackWarning("A key has " + std::to_string(key.size()) + " offsets for " + std::to_string(comp.points.size()) +
                                   " points: the extra ones are ignored, the missing ones stay at rest.");
                break;
            }
        }
    }
}
