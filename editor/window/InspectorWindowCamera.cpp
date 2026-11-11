// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's camera shake parts (camera shake spec 9):
// - the Camera Shaker section: Global, Unscaled Time, Mode, the scales, Start Preset (built-ins and
//   the project's .shake files, or one dropped from the Project window), the Trauma foldout, and
//   the Test strip: a preset, Shake, Add Trauma 0.5 and Stop. In play mode they act on the running
//   shaker; in edit mode they preview through the camera's view offset in the Game view
//   (CameraControlSystem::previewShake: never the Transform, never saved), with a live readout;
// - the Camera Follow section: the settings, warnings (no target, a missing one, a foreign
//   writer in play mode, a dead zone on a perspective camera) and Snap Now (one scene undo step
//   in edit mode);
// - the .shake asset view of a Project window click: the fields with units, Load Built-in, the
//   offset graph (sampled at 60 Hz with seed 0, Position / Rotation) redrawn on every edit, and
//   Test on the scene's main camera. Edits are project undo steps (ProjectFileWrites).
// Every component edit is a PropertyCmd / MultiPropertyCmd (scene undo).

#include "InspectorWindow.h"
#include "InspectorWindowCamera.h"

#include "camera/CameraShake.h"
#include "camera/ShakeInstance.h"
#include "camera/ShakePreset.h"
#include "camera/ShakePresets.h"
#include "camera/ShakerRuntime.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/CameraComponent.h"
#include "component/CameraFollowComponent.h"
#include "component/CameraShakerComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "object/CameraFollow.h"
#include "object/CameraShaker.h"
#include "subsystem/CameraControlSystem.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "Out.h"
#include "Scene.h"
#include "Theme.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    struct InspectorCameraState{
        // the Test strip's preset (a built-in name or a .shake path)
        std::string testPreset = "explosion";

        // the project's .shake files (asset-relative), listed again every second
        std::vector<std::string> shakeFiles;
        double shakeFilesTime = -100.0;

        // the inspected .shake: its working copy (live while a field is dragged) and the file's text
        fs::path asset;
        std::string fileText;
        bool parsed = false;
        camera::ShakePresetFile working;
        std::string error;
        std::vector<std::string> warnings;
        double checkedTime = -100.0;
        bool graphRotation = false;
        std::vector<camera::ShakeOffset> samples;
        camera::ShakePreset sampledPreset;
        bool hasSamples = false;

        // whose Test started the edit-mode preview: a GameObject (Camera Shaker section) or a .shake
        uint32_t previewScene = NULL_PROJECT_SCENE;
        Entity previewEntity = NULL_ENTITY;
        fs::path previewAsset;
    };

    InspectorCameraState& cameraState(){
        static InspectorCameraState state;
        return state;
    }

    std::vector<EnumEntry> entriesShakeMode = {
        {(int)CameraShakeMode::Auto, "Auto"},
        {(int)CameraShakeMode::Mode3D, "3D"},
        {(int)CameraShakeMode::Mode2D, "2D"},
    };

    std::vector<EnumEntry> entriesOffsetSpace = {
        {(int)FollowOffsetSpace::World, "World"},
        {(int)FollowOffsetSpace::Target, "Target"},
        {(int)FollowOffsetSpace::TargetYaw, "Target Yaw"},
    };

    bool isShakePath(const std::string& name){
        return Util::isShakePresetFile(name);
    }

    void drawWarning(const std::string& text){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
        ImGui::PopStyleColor();
    }

    void drawNote(const std::string& text){
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped(ICON_FA_CIRCLE_INFO " %s", text.c_str());
        ImGui::PopStyleColor();
    }

    const std::vector<std::string>& projectShakeFiles(Project* project){
        InspectorCameraState& state = cameraState();
        const double now = ImGui::GetTime();
        if (now - state.shakeFilesTime >= 1.0){
            state.shakeFilesTime = now;
            state.shakeFiles.clear();
            const fs::path assets = project->getAssetsPath();
            for (const fs::path& file : ProjectChecks::findProjectFiles(assets, {camera::SHAKE_EXTENSION})){
                if (!project->isInsideAssetsPath(file)) continue;
                state.shakeFiles.push_back(project->normalizeToAssetsRelative(file).generic_string());
            }
        }
        return state.shakeFiles;
    }

    // the preset a Test strip / Start Preset value names; false with the reason in `error`
    bool resolvePreset(Project* project, const std::string& name, camera::ShakePreset& out, std::string& error){
        if (isShakePath(name)){
            std::string text;
            if (!ProjectChecks::readFile(project->resolveAssetPath(name), text)){
                error = "cannot read " + name;
                return false;
            }
            camera::ShakeParseResult parsed = camera::parseShakePreset(text);
            if (!parsed.ok()){
                error = name + ": " + parsed.errorText();
                return false;
            }
            out = parsed.file->preset;
            return true;
        }
        if (const camera::ShakePreset* builtin = camera::findBuiltinShakePreset(name)){
            out = *builtin;
            return true;
        }
        error = "unknown preset " + name;
        return false;
    }

    // a combo of the built-in presets and the project's .shake files; true when `value` changed
    bool presetCombo(Project* project, const char* id, std::string& value, bool allowNone){
        bool changed = false;
        const std::string shown = value.empty() ? std::string("(none)") : value;
        if (ImGui::BeginCombo(id, shown.c_str(), ImGuiComboFlags_HeightLarge)){
            if (allowNone && ImGui::Selectable("(none)", value.empty())){
                value.clear();
                changed = true;
            }
            ImGui::SeparatorText("Built-in");
            for (const camera::BuiltinShakePreset& builtin : camera::builtinShakePresets()){
                if (ImGui::Selectable(builtin.name, value == builtin.name)){
                    value = builtin.name;
                    changed = true;
                }
                ImGui::SetItemTooltip("%s", builtin.description);
            }
            const std::vector<std::string>& files = projectShakeFiles(project);
            ImGui::SeparatorText("Project");
            if (files.empty()){
                ImGui::TextDisabled("No .shake files (Assets > Create > Shake Preset)");
            }
            for (const std::string& file : files){
                if (ImGui::Selectable(file.c_str(), value == file)){
                    value = file;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        // a .shake dropped from the Project window
        if (ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                for (const std::string& file : Util::getStringsFromPayload(payload)){
                    if (!isShakePath(file)) continue;
                    const fs::path absolute = fs::absolute(project->resolveAssetPath(file)).lexically_normal();
                    if (!project->isInsideAssetsPath(absolute)) continue;
                    value = project->normalizeToAssetsRelative(absolute).generic_string();
                    changed = true;
                    break;
                }
            }
            ImGui::EndDragDropTarget();
        }
        return changed;
    }

    // the trauma bar, the instance count and the offset of a runtime
    void drawReadout(const camera::ShakerRuntime* runtime, const char* source){
        const float trauma = runtime ? runtime->trauma.trauma : 0.0f;
        const int instances = runtime ? (int)runtime->instances.size() : 0;
        const Vector3 position = runtime ? runtime->offsetPosition : Vector3(0, 0, 0);
        const Vector3 rotation = runtime ? runtime->offsetRotation : Vector3(0, 0, 0);
        char overlay[48];
        std::snprintf(overlay, sizeof(overlay), "Trauma %.2f", trauma);
        ImGui::ProgressBar(trauma, ImVec2(-FLT_MIN, 0.0f), overlay);
        UIProbe::item("camerashaker.trauma");
        ImGui::TextDisabled("%s: %d shake%s", source, instances, instances == 1 ? "" : "s");
        ImGui::TextDisabled("Offset %.3f, %.3f, %.3f   Rotation %.2f\xC2\xB0, %.2f\xC2\xB0, %.2f\xC2\xB0",
                            position.x, position.y, position.z, rotation.x, rotation.y, rotation.z);
    }

    // the graph: three curves (x red, y green, z blue) of the sampled offsets
    void drawOffsetGraph(const std::vector<camera::ShakeOffset>& samples, bool rotation, float duration){
        const float height = ImGui::GetFontSize() * 8.0f;
        const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + size.x, min.y + size.y);
        ImGui::InvisibleButton("##shakegraph", size);
        UIProbe::item("shake.graph");
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, IM_COL32(24, 24, 24, 255), Theme::dpi(2.0f));
        draw->AddRect(min, max, IM_COL32(70, 70, 70, 255), Theme::dpi(2.0f));
        const float midY = (min.y + max.y) * 0.5f;
        draw->AddLine(ImVec2(min.x, midY), ImVec2(max.x, midY), IM_COL32(80, 80, 80, 255));

        float peak = 0.0f;
        for (const camera::ShakeOffset& sample : samples){
            const Vector3& v = rotation ? sample.rotation : sample.position;
            peak = std::max(peak, std::max(std::fabs(v.x), std::max(std::fabs(v.y), std::fabs(v.z))));
        }
        const float scale = peak > 0.0f ? (size.y * 0.45f) / peak : 0.0f;
        const ImU32 colors[3] = {IM_COL32(230, 90, 90, 255), IM_COL32(110, 210, 110, 255), IM_COL32(100, 150, 240, 255)};
        if (samples.size() >= 2 && scale > 0.0f){
            std::vector<ImVec2> points(samples.size());
            for (int axis = 0; axis < 3; axis++){
                for (size_t i = 0; i < samples.size(); i++){
                    const Vector3& v = rotation ? samples[i].rotation : samples[i].position;
                    const float value = axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
                    points[i] = ImVec2(min.x + size.x * (float)i / (float)(samples.size() - 1), midY - value * scale);
                }
                draw->AddPolyline(points.data(), (int)points.size(), colors[axis], ImDrawFlags_None, Theme::dpi(1.25f));
            }
        }
        char label[96];
        std::snprintf(label, sizeof(label), "peak %.3f %s   %.2f s", peak, rotation ? "deg" : "units", duration);
        draw->AddText(ImVec2(min.x + Theme::dpi(6.0f), min.y + Theme::dpi(4.0f)), IM_COL32(170, 170, 170, 255), label);
        const char* axes[3] = {"x", "y", "z"};
        float x = max.x - Theme::dpi(6.0f) - ImGui::CalcTextSize("x  y  z").x;
        for (int axis = 0; axis < 3; axis++){
            draw->AddText(ImVec2(x, min.y + Theme::dpi(4.0f)), colors[axis], axes[axis]);
            x += ImGui::CalcTextSize("x  ").x;
        }
    }

}

bool editor::inspectorcamera::writeShakePresetFile(Project* project, const fs::path& path, const camera::ShakePresetFile& file,
                                                   std::string& error){
    const std::string text = camera::writeShakePreset(file);
    std::string current;
    if (ProjectChecks::readFile(path, current) && current == text) return true;
    return ProjectFileWrites::writeUndoable(project, {{path, text}}, error);
}

// ---- the edit-mode preview's owner ----

void editor::InspectorWindow::syncCameraShakePreview(SceneProject* shown, const std::vector<Entity>& selection){
    InspectorCameraState& state = cameraState();
    if (state.previewScene == NULL_PROJECT_SCENE) return;
    SceneProject* sceneProject = project->getScene(state.previewScene);
    std::shared_ptr<CameraControlSystem> system = sceneProject && sceneProject->scene
        ? sceneProject->scene->findSystem<CameraControlSystem>() : nullptr;
    // the Test that started it is still on screen: its GameObject selected, or its .shake inspected
    const bool ownerShown = shown == sceneProject && sceneProject && sceneProject->playState == ScenePlayState::STOPPED &&
        ((state.previewEntity != NULL_ENTITY && selection.size() == 1 && selection[0] == state.previewEntity) ||
         (!state.previewAsset.empty() && project->getInspectedAsset() == state.previewAsset));
    if (!system || !ownerShown){
        if (system) system->stopPreview();
        state.previewScene = NULL_PROJECT_SCENE;
        state.previewEntity = NULL_ENTITY;
        state.previewAsset.clear();
    }
}

// ---- Camera Shaker ----

void editor::InspectorWindow::drawCameraShakerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    InspectorCameraState& state = cameraState();
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const Entity entity = entities[0];
    CameraShakerComponent& shaker = scene->getComponent<CameraShakerComponent>(entity);
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    const bool hasCamera = scene->findComponent<CameraComponent>(entity) != nullptr;

    RowSettings globalSettings;
    globalSettings.help = "Receives the CameraShake.* calls (shake, shakeAt, addTrauma, stopAll), like MilkShake's global shakers.";
    RowSettings unscaledSettings;
    unscaledSettings.help = "The shakes advance with the unscaled frame time: they keep going at Time.timeScale 0 (a shake's own unscaled option overrides it).";
    RowSettings modeSettings;
    modeSettings.enumEntries = &entriesShakeMode;
    modeSettings.help = "Auto: 2D for orthographic and UI cameras (and UI GameObjects). 2D shakes x/y in tenths of the view height and roll only.";
    RowSettings scaleSettings;
    scaleSettings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings positionScaleSettings = scaleSettings;
    positionScaleSettings.help = "Multiplies every position offset (0: no movement).";
    RowSettings rotationScaleSettings = scaleSettings;
    rotationScaleSettings.help = "Multiplies every rotation offset (0: no rotation).";

    beginTable(cpType, getLabelSize("Rotation Scale"), "camerashaker");
    propertyRow(RowPropertyType::Bool, cpType, "global", "Global", sceneProject, entities, globalSettings);
    propertyRow(RowPropertyType::Bool, cpType, "unscaledTime", "Unscaled Time", sceneProject, entities, unscaledSettings);
    propertyRow(RowPropertyType::Enum, cpType, "mode", "Mode", sceneProject, entities, modeSettings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "positionScale", "Position Scale", sceneProject, entities, positionScaleSettings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "rotationScale", "Rotation Scale", sceneProject, entities, rotationScaleSettings);

    // Start Preset: a built-in name or a .shake (played on start and each enable)
    prefabRowProperty = "startPreset";
    if (propertyHeader("Start Preset", -1, !shaker.startPreset.empty())){
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new PropertyCmd<std::string>(project, sceneId, entity, cpType, "startPreset", std::string()));
    }
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        std::string value = shaker.startPreset;
        if (presetCombo(project, "##camerashaker_startpreset", value, true) && value != shaker.startPreset){
            for (Entity selected : entities){
                CommandHandle::get(sceneId)->addCommandNoMerge(
                    new PropertyCmd<std::string>(project, sceneId, selected, cpType, "startPreset", value));
            }
        }
        UIProbe::item("camerashaker.startPreset");
        ImGui::SetItemTooltip("Played when the game starts and on each enable, like MilkShake's start shakes (the Menu's Drunk). "
                              "A built-in preset or a .shake file (drop one here).");
    }
    prefabRowProperty.clear();
    // S48: whole art pixels and ticks (2D)
    drawShakerPixelRows(cpType, sceneProject, entities);
    endTable();

    if (!shaker.startPreset.empty()){
        camera::ShakePreset preset;
        std::string error;
        if (!resolvePreset(project, shaker.startPreset, preset, error)){
            drawWarning("Start Preset: " + error + ". Nothing plays.");
        }
    }

    // Trauma (Eiserloh)
    if (ImGui::TreeNodeEx("Trauma##camerashaker", ImGuiTreeNodeFlags_SpanAvailWidth)){
        RowSettings decaySettings = scaleSettings;
        decaySettings.help = "Trauma lost per second (linear).";
        RowSettings powerSettings = scaleSettings;
        powerSettings.floatMin = 1.0f;
        powerSettings.floatMax = 4.0f;
        powerSettings.help = "Shake = trauma ^ power: 2 makes small hits subtle and big ones strong.";
        RowSettings offsetSettings;
        offsetSettings.help = "Position offset at trauma 1, in units (2D: tenths of the view height).";
        RowSettings angleSettings;
        angleSettings.help = "Rotation offset at trauma 1, in degrees (2D: z only).";
        RowSettings frequencySettings = scaleSettings;
        frequencySettings.help = "Noise speed of the trauma shake (MilkShake roughness units).";
        beginTable(cpType, getLabelSize("Max Offset"), "camerashaker_trauma");
        propertyRow(RowPropertyType::FloatPositive, cpType, "traumaDecay", "Decay", sceneProject, entities, decaySettings);
        propertyRow(RowPropertyType::Float, cpType, "traumaPower", "Power", sceneProject, entities, powerSettings);
        propertyRow(RowPropertyType::Vector3, cpType, "traumaMaxOffset", "Max Offset", sceneProject, entities, offsetSettings);
        propertyRow(RowPropertyType::Vector3, cpType, "traumaMaxAngle", "Max Angle", sceneProject, entities, angleSettings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "traumaFrequency", "Frequency", sceneProject, entities, frequencySettings);
        endTable();
        ImGui::TreePop();
    }

    if (entities.size() != 1) return;

    // ---- the Test strip
    ImGui::SeparatorText("Test");
    if (!hasCamera){
        drawNote("Shakes this GameObject's Transform (additive); preview runs in Play mode");
    }
    std::shared_ptr<CameraControlSystem> system = scene->findSystem<CameraControlSystem>();
    const bool canTest = system && (!stopped || hasCamera);
    {
        const float buttons = ImGui::CalcTextSize(ICON_FA_BURST " Shake").x + ImGui::CalcTextSize("+ Trauma 0.5").x +
                              ImGui::CalcTextSize(ICON_FA_STOP).x + ImGui::GetStyle().FramePadding.x * 6.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - buttons, ImGui::GetFontSize() * 6.0f));
        presetCombo(project, "##camerashaker_testpreset", state.testPreset, false);
        UIProbe::item("camerashaker.testPreset");
        ImGui::SetItemTooltip("The preset Shake plays");
        ImGui::SameLine();
        ImGui::BeginDisabled(!canTest);
        if (ImGui::Button(ICON_FA_BURST " Shake##camerashaker_test")){
            camera::ShakePreset preset;
            std::string error;
            if (!resolvePreset(project, state.testPreset, preset, error)){
                Out::warning("Camera Shaker Test: %s", error.c_str());
            }else if (stopped){
                if (system->previewShake(entity, preset, state.testPreset)){
                    state.previewScene = sceneId;
                    state.previewEntity = entity;
                    state.previewAsset.clear();
                    sceneProject->needUpdateRender = true;
                }
            }else{
                ShakeSource source(preset);
                source.name = state.testPreset;
                CameraShaker(scene, entity).shake(source);
            }
        }
        UIProbe::item("camerashaker.shake");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
            ImGui::SetTooltip("%s", stopped ? (hasCamera ? "Preview the preset in the Game view (edit mode: the camera's view only, nothing is saved)"
                                                         : "Transform mode shakes in Play mode only")
                                            : "Shake the running shaker");
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Trauma 0.5##camerashaker_trauma")){
            if (stopped){
                if (system->previewAddTrauma(entity, 0.5f)){
                    state.previewScene = sceneId;
                    state.previewEntity = entity;
                    state.previewAsset.clear();
                    sceneProject->needUpdateRender = true;
                }
            }else{
                CameraShaker(scene, entity).addTrauma(0.5f);
            }
        }
        UIProbe::item("camerashaker.addTrauma");
        ImGui::SetItemTooltip("Add 0.5 trauma (shake = trauma ^ power, decaying)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STOP "##camerashaker_stop")){
            if (stopped){
                system->previewStop();
            }else{
                CameraShaker(scene, entity).stopAll();
                CameraShaker(scene, entity).setTrauma(0.0f);
            }
        }
        UIProbe::item("camerashaker.stop");
        ImGui::SetItemTooltip("Fade every shake out (over its fade out) and clear the trauma");
        ImGui::EndDisabled();
    }

    // the live readout while anything shakes
    const camera::ShakerRuntime* runtime = nullptr;
    const char* source = "Playing";
    if (stopped){
        if (system && system->getPreviewEntity() == entity){
            runtime = system->getPreviewRuntime();
            source = "Preview";
        }
    }else{
        runtime = shaker.runtime.get();
    }
    if (runtime && (!runtime->instances.empty() || runtime->trauma.trauma > 0.0f || runtime->applied)){
        drawReadout(runtime, source);
        if (stopped) sceneProject->needUpdateRender = true;
    }
}

// ---- Camera Follow ----

void editor::InspectorWindow::drawCameraFollowComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const Entity entity = entities[0];
    CameraFollowComponent& follow = scene->getComponent<CameraFollowComponent>(entity);
    const CameraComponent* camera = scene->findComponent<CameraComponent>(entity);
    const bool orthographic = camera && (camera->type == CameraType::CAMERA_ORTHO || camera->type == CameraType::CAMERA_UI);
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;

    RowSettings targetSettings;
    targetSettings.excludeEntities = entities;
    targetSettings.help = "The GameObject followed (this scene). None: the Camera Follow does nothing.";
    RowSettings offsetSettings;
    offsetSettings.help = "From the target, in Offset Space.";
    RowSettings spaceSettings;
    spaceSettings.enumEntries = &entriesOffsetSpace;
    spaceSettings.help = "World: fixed axes. Target: turns with the target. Target Yaw: turns with the target's heading only.";
    RowSettings dampingSettings;
    dampingSettings.help = "SmoothDamp smooth time per world axis, in seconds (Unity's Mathf.SmoothDamp); 0 = rigid.";
    RowSettings speedSettings;
    speedSettings.secondColSize = 6 * ImGui::GetFontSize();
    speedSettings.help = "SmoothDamp max speed, units per second; 0 = unlimited.";
    RowSettings lookSettings;
    lookSettings.help = camera && camera->useTarget ? "Aim at the target: sets the camera's Target (it looks at its target)."
                                                    : "Aim at the target: turns this GameObject (a camera looks down its -Z).";
    RowSettings lookOffsetSettings;
    lookOffsetSettings.indent = true;
    lookOffsetSettings.help = "Added to the target's position (world): look above its feet.";
    RowSettings rotationSettings = speedSettings;
    rotationSettings.indent = true;
    rotationSettings.help = "Seconds to cover 99% of a turn; 0 = rigid.";
    RowSettings zoneSettings;
    zoneSettings.help = "Width and height (world x / y) of the rectangle the target moves in without moving the camera.";
    RowSettings boundsSettings;
    boundsSettings.help = "Clamp the follower's position to a world box.";
    RowSettings minSettings;
    minSettings.indent = true;
    RowSettings maxSettings;
    maxSettings.indent = true;
    RowSettings unscaledSettings;
    unscaledSettings.help = "Follow with the unscaled frame time (keeps following at Time.timeScale 0).";

    beginTable(cpType, getLabelSize("Rotation Damping"), "camerafollow");
    propertyRow(RowPropertyType::LocalEntity, cpType, "target", "Target", sceneProject, entities, targetSettings);
    propertyRow(RowPropertyType::Vector3, cpType, "offset", "Offset", sceneProject, entities, offsetSettings);
    propertyRow(RowPropertyType::Enum, cpType, "offsetSpace", "Offset Space", sceneProject, entities, spaceSettings);
    propertyRow(RowPropertyType::Vector3, cpType, "damping", "Damping", sceneProject, entities, dampingSettings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxSpeed", "Max Speed", sceneProject, entities, speedSettings);
    propertyRow(RowPropertyType::Bool, cpType, "lookAt", "Look At", sceneProject, entities, lookSettings);
    if (follow.lookAt){
        propertyRow(RowPropertyType::Vector3, cpType, "lookAtOffset", "Look Offset", sceneProject, entities, lookOffsetSettings);
        if (!(camera && camera->useTarget)){
            propertyRow(RowPropertyType::FloatPositive, cpType, "rotationDamping", "Rotation Damping", sceneProject, entities, rotationSettings);
        }
    }
    if (orthographic){
        propertyRow(RowPropertyType::Vector2, cpType, "deadZone", "Dead Zone", sceneProject, entities, zoneSettings);
    }
    propertyRow(RowPropertyType::Bool, cpType, "useBounds", "Bounds", sceneProject, entities, boundsSettings);
    if (follow.useBounds){
        propertyRow(RowPropertyType::Vector3, cpType, "boundsMin", "Min", sceneProject, entities, minSettings);
        propertyRow(RowPropertyType::Vector3, cpType, "boundsMax", "Max", sceneProject, entities, maxSettings);
    }
    propertyRow(RowPropertyType::Bool, cpType, "unscaledTime", "Unscaled Time", sceneProject, entities, unscaledSettings);
    endTable();

    if (entities.size() != 1) return;

    std::shared_ptr<CameraControlSystem> followSystem = scene->findSystem<CameraControlSystem>();
    if (follow.targetPersistent){
        if (followSystem && followSystem->targetAlive(follow, entity)){
            drawNote("Follows a persistent (DontDestroyOnLoad) GameObject.");
        }else{
            drawWarning("The persistent (DontDestroyOnLoad) target is gone: the camera holds where it is.");
        }
    }else if (follow.target == NULL_ENTITY){
        drawWarning("No Target: pick the GameObject to follow.");
    }else if (!scene->isEntityCreated(follow.target) || !scene->findComponent<Transform>(follow.target)){
        drawWarning("The Target is missing (destroyed, or in another scene): the camera holds where it is.");
    }
    if (!orthographic && (follow.deadZone.x > 0.0f || follow.deadZone.y > 0.0f)){
        drawNote("The Dead Zone applies to orthographic and UI cameras only: it is ignored here.");
    }
    if (!stopped && follow.foreignWriteDetected){
        drawWarning("Something else (a script, an animation) also moves this Transform: the Camera Follow overwrites it. "
                    "Disable the Camera Follow while something else moves it.");
    }

    // Snap Now: placed at the target without damping (edit mode: one undo step)
    std::shared_ptr<CameraControlSystem> system = scene->findSystem<CameraControlSystem>();
    ImGui::BeginDisabled(!system || follow.target == NULL_ENTITY);
    if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS " Snap Now##camerafollow_snap")){
        if (!stopped){
            CameraControlSystem::snap(follow);
        }else{
            CameraFollowComponent copy = follow;
            FollowPlacement placement = system->computeFollow(entity, copy, 0.0f, true);
            if (placement.valid){
                MultiPropertyCmd* multi = new MultiPropertyCmd();
                multi->addPropertyCmd<Vector3>(project, sceneId, entity, ComponentType::Transform, "position", placement.position);
                if (placement.hasRotation){
                    multi->addPropertyCmd<Quaternion>(project, sceneId, entity, ComponentType::Transform, "rotation", placement.rotation);
                }
                if (placement.hasCameraTarget){
                    multi->addPropertyCmd<Vector3>(project, sceneId, entity, ComponentType::CameraComponent, "target", placement.cameraTarget);
                }
                CommandHandle::get(sceneId)->addCommandNoMerge(multi);
            }
        }
    }
    ImGui::EndDisabled();
    UIProbe::item("camerafollow.snap");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
        ImGui::SetTooltip("%s", stopped ? "Place the camera at the target now (one undo step)" : "Place the camera at the target on the next frame, without damping");
    }
}

// ---- the .shake asset view ----

void editor::InspectorWindow::drawShakePresetAsset(const fs::path& asset){
    InspectorCameraState& state = cameraState();
    drawAssetHeader(ICON_FA_BURST, asset, "Shake Preset");

    // read again when another file is inspected or the file changed (an edit, an undo, outside)
    const double now = ImGui::GetTime();
    if (state.asset != asset || now - state.checkedTime >= 0.5){
        state.checkedTime = now;
        std::string text;
        const bool read = ProjectChecks::readFile(asset, text);
        if (state.asset != asset || !read || text != state.fileText){
            state.asset = asset;
            state.fileText = read ? text : std::string();
            state.warnings.clear();
            state.error.clear();
            state.parsed = false;
            if (!read){
                state.error = "cannot read the file";
            }else{
                camera::ShakeParseResult parsed = camera::parseShakePreset(text);
                if (parsed.ok()){
                    state.working = *parsed.file;
                    state.warnings = parsed.warnings;
                    state.parsed = true;
                }else{
                    state.error = parsed.errorText();
                }
            }
        }
    }
    if (!state.parsed){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", state.error.c_str());
        ImGui::PopStyleColor();
        return;
    }

    // a finished edit: one step of the project history, written canonically
    auto commit = [&](){
        std::string error;
        if (!inspectorcamera::writeShakePresetFile(project, asset, state.working, error)){
            Out::error("Shake Preset: %s", error.c_str());
            return;
        }
        state.fileText = camera::writeShakePreset(state.working);
    };

    camera::ShakePreset& preset = state.working.preset;
    const float labelWidth = ImGui::CalcTextSize("Position Influence (units)").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    bool changed = false;
    if (ImGui::BeginTable("##shakepreset", 2, ImGuiTableFlags_SizingFixedFit)){
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](const char* label, const char* help){
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SetItemTooltip("%s", help);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
        };
        row("Type", "One Shot fades in, then out right away; Sustained holds until stopped; Kick is a pixel-art hit: a decaying "
                    "whole-pixel jolt over Fade Out seconds (no noise, no roll).");
        int type = preset.type == camera::ShakeType::Kick ? 2 : (preset.type == camera::ShakeType::Sustained ? 1 : 0);
        if (ImGui::Combo("##shake_type", &type, "One Shot\0Sustained\0Kick\0")){
            preset.type = type == 2 ? camera::ShakeType::Kick : (type == 1 ? camera::ShakeType::Sustained : camera::ShakeType::OneShot);
            changed = true;
            commit();
        }
        UIProbe::item("shake.type");
        row("Units", "View: tenths of the view height in 2D, world units in 3D. Pixels: art pixels (the Pixel Perfect Camera's grid, "
                     "else screen pixels).");
        int units = preset.units == camera::ShakeUnits::Pixels ? 1 : 0;
        if (ImGui::Combo("##shake_units", &units, "View\0Pixels\0")){
            preset.units = units == 1 ? camera::ShakeUnits::Pixels : camera::ShakeUnits::View;
            changed = true;
            commit();
        }
        UIProbe::item("shake.units");
        auto floatRow = [&](const char* label, const char* id, float& value, float speed, const char* help){
            row(label, help);
            if (ImGui::DragFloat(id, &value, speed, 0.0f, 10000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) changed = true;
            UIProbe::item(std::string("shake.") + (id + 2));
            if (ImGui::IsItemDeactivatedAfterEdit()) commit();
        };
        auto vectorRow = [&](const char* label, const char* id, Vector3& value, const char* help){
            row(label, help);
            float v[3] = {value.x, value.y, value.z};
            if (ImGui::DragFloat3(id, v, 0.05f, -10000.0f, 10000.0f, "%.2f")){
                value = Vector3(v[0], v[1], v[2]);
                changed = true;
            }
            UIProbe::item(std::string("shake.") + (id + 2));
            if (ImGui::IsItemDeactivatedAfterEdit()) commit();
        };
        floatRow("Strength", "##strength", preset.strength, 0.005f, "Amplitude multiplier");
        floatRow("Roughness", "##roughness", preset.roughness, 0.1f, "Noise speed: higher is jerkier");
        floatRow("Fade In (s)", "##fade_in", preset.fadeIn, 0.01f, "Seconds to full strength (0: instant)");
        floatRow("Fade Out (s)", "##fade_out", preset.fadeOut, 0.01f, "Seconds to stop (0: instant); a Kick's length");
        floatRow("Tick (Hz)", "##tick", preset.tick, 1.0f, "The offset changes only on ticks of 1 / Tick seconds (0: every frame; a Kick steps at 60)");
        vectorRow("Position Influence (units)", "##position_influence", preset.positionInfluence,
                  "Per-axis amplitude: world units in 3D, tenths of the view height in 2D");
        vectorRow("Rotation Influence (degrees)", "##rotation_influence", preset.rotationInfluence,
                  "Per-axis amplitude in degrees (2D: z, the roll, only)");
        ImGui::EndTable();
    }

    // Load Built-in: a built-in's values into this file (one undo step)
    if (ImGui::Button("Load Built-in " ICON_FA_CARET_RIGHT "##shake_builtin")){
        ImGui::OpenPopup("##shake_builtin_menu");
    }
    UIProbe::item("shake.loadBuiltin");
    if (ImGui::BeginPopup("##shake_builtin_menu")){
        for (const camera::BuiltinShakePreset& builtin : camera::builtinShakePresets()){
            if (ImGui::MenuItem(builtin.name)){
                preset = builtin.preset;
                changed = true;
                commit();
            }
            ImGui::SetItemTooltip("%s", builtin.description);
        }
        ImGui::EndPopup();
    }

    // Test: the scene's main camera (the edit-mode preview, or live while it plays)
    ImGui::SameLine();
    SceneProject* sceneProject = project->getSelectedScene();
    const Entity mainCamera = sceneProject ? project->getGameCamera(sceneProject->id) : NULL_ENTITY;
    std::shared_ptr<CameraControlSystem> system = sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<CameraControlSystem>() : nullptr;
    ImGui::BeginDisabled(!system || mainCamera == NULL_ENTITY);
    if (ImGui::Button(ICON_FA_BURST " Test##shake_test")){
        const std::string name = project->normalizeToAssetsRelative(asset).generic_string();
        if (sceneProject->playState == ScenePlayState::STOPPED){
            if (system->previewShake(mainCamera, preset, name)){
                state.previewScene = sceneProject->id;
                state.previewEntity = NULL_ENTITY;
                state.previewAsset = asset;
                sceneProject->needUpdateRender = true;
            }
        }else{
            ShakeSource source(preset);
            source.name = name;
            if (sceneProject->scene->findComponent<CameraShakerComponent>(mainCamera)){
                CameraShaker(sceneProject->scene, mainCamera).shake(source);
            }else{
                CameraShake::shake(source);
            }
        }
    }
    ImGui::EndDisabled();
    UIProbe::item("shake.test");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
        ImGui::SetTooltip("%s", mainCamera == NULL_ENTITY ? "The selected scene has no camera" :
                                "Shake the scene's main camera in the Game view (edit mode: a preview of its view only)");
    }

    for (const std::string& warning : state.warnings){
        drawWarning(warning);
    }

    // the graph, sampled again when a value changed
    ImGui::Spacing();
    if (ImGui::RadioButton("Position", !state.graphRotation)) state.graphRotation = false;
    UIProbe::item("shake.graph.position");
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotation", state.graphRotation)) state.graphRotation = true;
    UIProbe::item("shake.graph.rotation");
    const float duration = camera::shakePreviewDuration(preset);
    if (changed || !state.hasSamples || state.sampledPreset != preset){
        state.samples = camera::sampleShakePreset(preset, duration, 60.0f, 0);
        state.sampledPreset = preset;
        state.hasSamples = true;
    }
    drawOffsetGraph(state.samples, state.graphRotation, duration);
}
