// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector drawers of upstream's components that came with SYNC#4 (origin/main 047eccf9b):
// the on-screen controls (ui-multitouch) and the Ragdoll (physics-3d), registered in
// componentDrawer (InspectorDefault.cpp). Their rows read the properties Catalog gives (until the
// components have reflection tables of their own).

#include "InspectorWindow.h"
#include "component/ImageComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/Transform.h"

#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "window/dialog/RagdollWizardDialog.h"
#include "external/IconsFontAwesome6.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>

using namespace jasperengine;

namespace {
    // the actions an on-screen control may feed: the Input Manager's (Project Settings > Input:
    // the actions asset's "Input Manager" map, Unity's defaults without an asset; axes first for
    // a stick or touch area), then the asset's other actions as Map/Action. The asset is read
    // again only when it changes
    std::vector<std::string> inputActionNames(const editor::Project* project, bool axesFirst){
        struct Cache {
            std::filesystem::path asset;
            std::filesystem::file_time_type written;
            bool axesFirst = false;
            bool valid = false;
            std::vector<std::string> names;
        };
        static Cache cache;
        std::error_code ec;
        const std::filesystem::path asset = project->getInputActionsAsset();
        const std::filesystem::file_time_type written = asset.empty() ? std::filesystem::file_time_type()
            : std::filesystem::last_write_time(project->getProjectPath() / asset, ec);
        if (cache.valid && cache.asset == asset && cache.written == written && cache.axesFirst == axesFirst)
            return cache.names;

        std::vector<std::string> names;
        const std::vector<InputManagerAction> manager = project->getInputManagerActions();
        for (int pass = 0; pass < 2; pass++){
            for (const InputManagerAction& action : manager){
                const bool axis = action.type != InputManagerActionType::Button;
                if ((pass == 0) == (axis == axesFirst) && std::find(names.begin(), names.end(), action.name) == names.end())
                    names.push_back(action.name);
            }
        }
        std::string text;
        InputActionAssetDesc desc;
        std::string error;
        if (project->readInputActionsAsset(text) && InputActionAssetDesc::parse(text, desc, error)){
            for (const InputActionMapDesc& map : desc.maps){
                if (map.name == InputManagerFormat::MAP_NAME)
                    continue;
                for (const InputActionDesc& action : map.actions)
                    names.push_back(map.name + "/" + action.name);
            }
        }
        cache = Cache{asset, written, axesFirst, true, names};
        return names;
    }

    const char* const noInputActions = "No actions: Project Settings > Input (Input Manager) or the actions asset";
}

void editor::InspectorWindow::drawOnScreenStickComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    static std::vector<editor::EnumEntry> entriesMode = {
        { (int)OnScreenStickMode::FIXED, "Fixed" },
        { (int)OnScreenStickMode::FLOATING, "Floating" },
        { (int)OnScreenStickMode::DYNAMIC, "Dynamic" }
    };
    static std::vector<editor::EnumEntry> entriesAxes = {
        { (int)OnScreenStickAxes::BOTH, "Both" },
        { (int)OnScreenStickAxes::HORIZONTAL, "Horizontal" },
        { (int)OnScreenStickAxes::VERTICAL, "Vertical" }
    };
    static std::vector<editor::EnumEntry> entriesVisibility = {
        { (int)OnScreenVisibility::ALWAYS, "Always" },
        { (int)OnScreenVisibility::TOUCH_DEVICES_ONLY, "Touch Devices Only" }
    };
    const std::vector<std::string> actions = inputActionNames(project, true);

    RowSettings settings;
    RowSettings settingsMode;
    settingsMode.enumEntries = &entriesMode;
    settingsMode.help = "Fixed: the base stays. Floating: it appears under the finger. Dynamic: it also follows the finger past Move Threshold (Joystick Pack).";
    RowSettings settingsAxes;
    settingsAxes.enumEntries = &entriesAxes;
    RowSettings settingsVisibility;
    settingsVisibility.enumEntries = &entriesVisibility;
    settingsVisibility.help = "Touch Devices Only hides it where System.isTouchDevice() is false (the Game view's Simulate Touch counts as touch).";
    RowSettings settingsRange;
    settingsRange.stepSize = 0.01f;
    settingsRange.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsDead = settingsRange;
    settingsDead.help = "|input| at or below this reads 0 (in radii of the base).";
    RowSettings settingsThreshold = settingsRange;
    settingsThreshold.help = "Dynamic: the base follows the finger once it is this far out (radii).";
    RowSettings settingsAction;
    settingsAction.stringSuggestions = &actions;
    settingsAction.suggestionsEmpty = noInputActions;
    RowSettings settingsEntity;
    settingsEntity.entityFilter.set(sceneProject->scene->getComponentId<ImageComponent>());

    beginTable(cpType, getLabelSize("Horizontal Action"));
    propertyRow(RowPropertyType::Enum, cpType, "mode", "Mode", sceneProject, entities, settingsMode);
    propertyRow(RowPropertyType::LocalEntity, cpType, "background", "Background", sceneProject, entities, settingsEntity);
    propertyRow(RowPropertyType::LocalEntity, cpType, "handle", "Handle", sceneProject, entities, settingsEntity);
    propertyRow(RowPropertyType::FloatPositive, cpType, "handleRange", "Handle Range", sceneProject, entities, settingsRange);
    propertyRow(RowPropertyType::Float_0_1, cpType, "deadZone", "Dead Zone", sceneProject, entities, settingsDead);
    propertyRow(RowPropertyType::Enum, cpType, "axes", "Axis Options", sceneProject, entities, settingsAxes);
    propertyRow(RowPropertyType::Bool, cpType, "snapX", "Snap X", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Bool, cpType, "snapY", "Snap Y", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "moveThreshold", "Move Threshold", sceneProject, entities, settingsThreshold);
    propertyRow(RowPropertyType::Bool, cpType, "hideWhenReleased", "Hide Released", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Bool, cpType, "clampToArea", "Clamp To Area", sceneProject, entities, settings);
    propertyRow(RowPropertyType::String, cpType, "horizontalAction", "Horizontal Action", sceneProject, entities, settingsAction);
    propertyRow(RowPropertyType::String, cpType, "verticalAction", "Vertical Action", sceneProject, entities, settingsAction);
    propertyRow(RowPropertyType::Enum, cpType, "visibility", "Visibility", sceneProject, entities, settingsVisibility);
    endTable();

    // live state while playing
    if (entities.size() == 1 && project->isAnyScenePlaying()){
        if (const OnScreenStickComponent* stick = sceneProject->scene->findComponent<OnScreenStickComponent>(entities[0])){
            ImGui::TextDisabled("Value (%.2f, %.2f)  %s", stick->value.x, stick->value.y,
                                stick->pressed ? ("held by pointer " + std::to_string(stick->pointerId)).c_str() : "released");
        }
    }
}

void editor::InspectorWindow::drawOnScreenButtonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    static std::vector<editor::EnumEntry> entriesVisibility = {
        { (int)OnScreenVisibility::ALWAYS, "Always" },
        { (int)OnScreenVisibility::TOUCH_DEVICES_ONLY, "Touch Devices Only" }
    };
    const std::vector<std::string> actions = inputActionNames(project, false);

    RowSettings settings;
    RowSettings settingsAction;
    settingsAction.stringSuggestions = &actions;
    settingsAction.suggestionsEmpty = noInputActions;
    settingsAction.help = "The Input action it presses (Input.getButton(\"Fire1\")); on an axis it adds Value.";
    RowSettings settingsValue;
    settingsValue.stepSize = 0.01f;
    settingsValue.floatMin = -1.0f;
    settingsValue.floatMax = 1.0f;
    settingsValue.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsEnters;
    settingsEnters.help = "A finger already down that slides onto it presses it.";
    RowSettings settingsLeaves;
    settingsLeaves.help = "A finger that slides off it releases it.";
    RowSettings settingsVisibility;
    settingsVisibility.enumEntries = &entriesVisibility;

    beginTable(cpType, getLabelSize("Release On Leave"));
    propertyRow(RowPropertyType::String, cpType, "action", "Action", sceneProject, entities, settingsAction);
    propertyRow(RowPropertyType::Float, cpType, "value", "Value", sceneProject, entities, settingsValue);
    propertyRow(RowPropertyType::Bool, cpType, "pressWhenPointerEnters", "Press On Enter", sceneProject, entities, settingsEnters);
    propertyRow(RowPropertyType::Bool, cpType, "releaseWhenPointerLeaves", "Release On Leave", sceneProject, entities, settingsLeaves);
    propertyRow(RowPropertyType::Enum, cpType, "visibility", "Visibility", sceneProject, entities, settingsVisibility);
    endTable();

    if (entities.size() == 1 && project->isAnyScenePlaying()){
        if (const OnScreenButtonComponent* control = sceneProject->scene->findComponent<OnScreenButtonComponent>(entities[0])){
            ImGui::TextDisabled("%s", control->pressed ? ("Pressed by " + std::to_string(control->pointers.size()) + " pointer(s)").c_str() : "Released");
        }
    }
}

void editor::InspectorWindow::drawTouchAreaComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    static std::vector<editor::EnumEntry> entriesVisibility = {
        { (int)OnScreenVisibility::ALWAYS, "Always" },
        { (int)OnScreenVisibility::TOUCH_DEVICES_ONLY, "Touch Devices Only" }
    };
    const std::vector<std::string> actions = inputActionNames(project, true);

    RowSettings settings;
    RowSettings settingsAction;
    settingsAction.stringSuggestions = &actions;
    settingsAction.suggestionsEmpty = noInputActions;
    RowSettings settingsSensitivity;
    settingsSensitivity.stepSize = 0.01f;
    settingsSensitivity.secondColSize = 6 * ImGui::GetFontSize();
    settingsSensitivity.help = "Canvas pixels to action units; 0.1 is the default Mouse X/Y sensitivity, so a drag feels like the mouse.";
    RowSettings settingsVisibility;
    settingsVisibility.enumEntries = &entriesVisibility;

    beginTable(cpType, getLabelSize("Horizontal Action"));
    propertyRow(RowPropertyType::String, cpType, "horizontalAction", "Horizontal Action", sceneProject, entities, settingsAction);
    propertyRow(RowPropertyType::String, cpType, "verticalAction", "Vertical Action", sceneProject, entities, settingsAction);
    propertyRow(RowPropertyType::Float, cpType, "sensitivity", "Sensitivity", sceneProject, entities, settingsSensitivity);
    propertyRow(RowPropertyType::Bool, cpType, "invertX", "Invert X", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Bool, cpType, "invertY", "Invert Y", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Enum, cpType, "visibility", "Visibility", sceneProject, entities, settingsVisibility);
    endTable();

    if (entities.size() == 1 && project->isAnyScenePlaying()){
        if (const TouchAreaComponent* area = sceneProject->scene->findComponent<TouchAreaComponent>(entities[0])){
            ImGui::TextDisabled("%s", area->pressed ? ("Dragged by pointer " + std::to_string(area->pointerId)).c_str() : "Released");
        }
    }
}

void editor::InspectorWindow::drawRagdollComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RagdollComponent& ragdoll = sceneProject->scene->getComponent<RagdollComponent>(entities[0]);
    const bool playing = sceneProject->playState != ScenePlayState::STOPPED;

    RowSettings settingsSimulated;
    settingsSimulated.help = "Off: the parts are kinematic and follow the animated bones. On: they are dynamic and move the bones (and the skin); the Animation Player pauses";
    RowSettings settingsRoot;
    settingsRoot.help = "The GameObject whose Rigidbodies below it are the parts; none = this one";
    RowSettings settingsAnimator;
    settingsAnimator.help = "The Animation Player paused while simulated; none = the first on the root, its parents, then below it";
    RowSettings settingsValue;
    RowSettings settingsSelf;
    settingsSelf.help = "Off: no part collides with another part";

    beginTable(cpType, getLabelSize("Inherit Velocity"));
    propertyRow(RowPropertyType::Bool, cpType, "simulated", "Simulated", sceneProject, entities, settingsSimulated);
    propertyRow(RowPropertyType::LocalEntity, cpType, "root", "Root", sceneProject, entities, settingsRoot);
    propertyRow(RowPropertyType::LocalEntity, cpType, "animator", "Animator", sceneProject, entities, settingsAnimator);
    propertyRow(RowPropertyType::Bool, cpType, "inheritVelocity", "Inherit Velocity", sceneProject, entities, settingsValue);
    propertyRow(RowPropertyType::Bool, cpType, "selfCollision", "Self Collision", sceneProject, entities, settingsSelf);
    propertyRow(RowPropertyType::FloatPositive, cpType, "totalMass", "Total Mass", sceneProject, entities, settingsValue);
    endTable();

    // the parts below the root: the Rigidbodies the ragdoll switches
    const Entity root = ragdoll.root != NULL_ENTITY ? ragdoll.root : entities[0];
    size_t parts = 0;
    auto transforms = sceneProject->scene->getComponentArray<Transform>();
    if (transforms->hasEntity(root)){
        const size_t first = transforms->getIndex(root);
        const size_t last = sceneProject->scene->findBranchLastIndex(root);
        for (size_t i = first + 1; i <= last && i < transforms->size(); i++){
            if (sceneProject->scene->findComponent<RigidbodyComponent>(transforms->getEntity(i))) parts++;
        }
    }
    ImGui::TextDisabled("%zu parts (Rigidbodies below the root)%s", parts, playing ? "" : "; the wizard makes them");
    if (!playing && entities.size() == 1){
        if (ImGui::Button(ICON_FA_PERSON_FALLING " Create Ragdoll...##ragdoll")){
            RagdollWizardDialog::instance().open(project, sceneProject->id, entities[0]);
        }
    }
}
