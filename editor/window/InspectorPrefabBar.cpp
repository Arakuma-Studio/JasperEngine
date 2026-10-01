// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab instances in the Inspector, as Unity shows them (editor/project/PREFABS.md,
// "Editing instances"): the Prefab bar (Open, Select, Overrides) with its Overrides dropdown
// (a tree of every override and a Prefab Source | Override comparison of the selected one),
// overridden values in bold with a blue bar in the margin, added components with a "+"
// badge, removed ones as "(Removed)" entries, and the Apply / Revert context menus.

#include "InspectorWindow.h"

#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/PrefabOverrideCmd.h"
#include "command/type/PrefabOverridesCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/AddEntityToPrefabCmd.h"
#include "window/HierarchyWindow.h"
#include "window/ProjectWindow.h"
#include "App.h"
#include "Backend.h"
#include "Theme.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>

using namespace jasperengine;

namespace {

    // Prefab (prefab) text color, as in the Hierarchy and the rest of the Inspector
    const ImVec4 prefabTextColor = ImVec4(0.4f, 0.7f, 1.0f, 1.0f);
    // Unity's override bar
    const ImU32 overrideBarColor = IM_COL32(1, 153, 235, 255);

    using Kind = editor::PrefabOverrideEntry::Kind;
    using Operation = editor::PrefabOverrideCmd::Operation;

    std::string stateNames(uint8_t bits) {
        std::string names;
        auto add = [&](uint8_t bit, const char* name) {
            if (bits & bit) {
                names += names.empty() ? name : std::string(", ") + name;
            }
        };
        add(editor::PrefabObjectOverrides::StateName, "Name");
        add(editor::PrefabObjectOverrides::StateActive, "Active");
        add(editor::PrefabObjectOverrides::StateTag, "Tag");
        add(editor::PrefabObjectOverrides::StateLayer, "Layer");
        return names;
    }

    std::string prefabName(const std::filesystem::path& path) {
        return path.stem().string();
    }

    bool isNestedName(const std::string& name, const std::string& parent) {
        return name.size() > parent.size() && name.compare(0, parent.size(), parent) == 0 &&
            (name[parent.size()] == '.' || name[parent.size()] == '[');
    }

}

// ---- Rows, headers and menus ----

void editor::InspectorWindow::beginPrefabComponent(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type, bool readOnly){
    prefabRow = PrefabRowContext();
    prefabRow.sceneProject = sceneProject;
    prefabRow.entities = entities;
    prefabRow.type = type;
    prefabRow.readOnly = readOnly;
    for (Entity entity : entities){
        Prefab::ObjectOverrides overrides;
        if (project->getPrefabEditOverrides(sceneProject->id, entity, overrides)){
            prefabRow.active = true;
            prefabRow.overrides[entity] = overrides;
        }
    }
}

void editor::InspectorWindow::endPrefabComponent(){
    prefabRow = PrefabRowContext();
    prefabRowProperty.clear();
}

std::vector<std::string> editor::InspectorWindow::prefabOverridesOf(SceneProject* sceneProject, Entity entity, ComponentType type, const std::string& property) const{
    std::vector<std::string> matching;
    std::set<std::string> names;
    auto cached = prefabRow.sceneProject == sceneProject ? prefabRow.overrides.find(entity) : prefabRow.overrides.end();
    if (cached != prefabRow.overrides.end()){
        auto found = cached->second.properties.find(type);
        if (found != cached->second.properties.end()){
            names = found->second;
        }
    }else{
        names = project->getOverriddenProperties(sceneProject->id, entity, type);
    }
    if (names.empty() || property.empty()){
        return matching;
    }
    if (names.count(PrefabObjectOverrides::WHOLE_COMPONENT)){
        // the row's value is overridden with the rest of the component
        matching.push_back(property);
        return matching;
    }
    for (const std::string& name : names){
        // the row's property, one inside it ("submeshes[0].material" for its row's parts), or
        // the one it is part of
        if (name == property || isNestedName(name, property) || isNestedName(property, name)){
            matching.push_back(name);
        }
    }
    return matching;
}

bool editor::InspectorWindow::isPrefabPropertyOverridden(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type, const std::string& property) const{
    for (Entity entity : entities){
        if (!prefabOverridesOf(sceneProject, entity, type, property).empty()){
            return true;
        }
    }
    return false;
}

void editor::InspectorWindow::drawPrefabOverrideBar(float y0, float y1, bool added){
    (void)added;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    // in the window's margin, past the table cell clipping the row
    drawList->PushClipRect(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y), false);
    const float x0 = windowPos.x + Theme::dpi(1.0f);
    drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + Theme::dpi(2.0f), y1), overrideBarColor);
    drawList->PopClipRect();
}

void editor::InspectorWindow::drawPrefabApplyRevertItems(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type,
        const std::string& property, uint8_t stateBits, bool readOnly){
    const uint32_t sceneId = sceneProject->id;

    // What each GameObject of the selection has to apply or revert here
    struct Item{
        Entity entity;
        Operation apply;
        Operation revert;
        std::vector<std::string> properties;
        uint8_t state = 0;
    };
    std::vector<Item> items;
    std::string heading;
    for (Entity entity : entities){
        const Prefab::ObjectOverrides overrides = project->getPrefabObjectOverrides(sceneId, entity);
        if (stateBits){
            if (overrides.state & stateBits){
                items.push_back({entity, Operation::ApplyState, Operation::RevertState, {}, static_cast<uint8_t>(overrides.state & stateBits)});
            }
        }else if (!property.empty()){
            std::vector<std::string> names = prefabOverridesOf(sceneProject, entity, type, property);
            if (!names.empty()){
                items.push_back({entity, Operation::ApplyProperty, Operation::RevertProperty, names, 0});
            }
        }else if (overrides.addedComponents.count(type)){
            items.push_back({entity, Operation::ApplyAddedComponent, Operation::RevertAddedComponent, {}, 0});
            heading = "Added Component";
        }else if (overrides.removedComponents.count(type)){
            items.push_back({entity, Operation::ApplyRemovedComponent, Operation::RevertRemovedComponent, {}, 0});
            heading = "Removed Component";
        }else if (overrides.hasComponent(type)){
            const std::set<std::string>& names = overrides.properties.at(type);
            items.push_back({entity, Operation::ApplyProperty, Operation::RevertProperty, std::vector<std::string>(names.begin(), names.end()), 0});
            if (heading.empty()) heading = "Modified Component";
        }
    }
    if (items.empty()){
        return;
    }

    ImGui::Separator();
    if (!heading.empty()){
        ImGui::TextDisabled("%s", heading.c_str());
    }

    // Unity's entries: the outermost prefab first ("Apply as Override in Prefab 'Outer'"),
    // the prefab the GameObject comes from last ("Apply to Prefab 'Inner'")
    const std::vector<std::filesystem::path> targets = project->getPrefabApplyTargets(sceneId, items.front().entity);
    std::string playReason;
    const bool canApplyNow = project->canApplyPrefabOverrides(&playReason);
    auto addCommand = [&](std::vector<std::unique_ptr<Command>> steps){
        if (!steps.empty()){
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new PrefabOverridesCmd(project, sceneId, std::move(steps)));
        }
    };
    for (size_t t = targets.size(); t-- > 0;){
        const std::filesystem::path& target = targets[t];
        const std::string label = (t == 0 ? "Apply to Prefab '" : "Apply as Override in Prefab '") + prefabName(target) + "'";
        std::string reason = playReason;
        bool enabled = !readOnly && canApplyNow;
        if (enabled && items.front().apply == Operation::ApplyProperty){
            enabled = project->canApplyPrefabOverride(sceneId, items.front().entity, type, items.front().properties, target, &reason);
        }else if (enabled && items.front().apply == Operation::ApplyAddedComponent){
            enabled = project->canApplyPrefabOverride(sceneId, items.front().entity, type, {}, target, &reason);
        }
        if (ImGui::MenuItem(label.c_str(), nullptr, false, enabled)){
            std::vector<std::unique_ptr<Command>> steps;
            for (const Item& item : items){
                const std::vector<std::filesystem::path> own = project->getPrefabApplyTargets(sceneId, item.entity);
                if (std::find(own.begin(), own.end(), target) == own.end()){
                    continue;
                }
                steps.emplace_back(new PrefabOverrideCmd(project, sceneId, item.entity, item.apply, type, item.properties, item.state, target));
            }
            addCommand(std::move(steps));
        }
        if (!enabled && !reason.empty()){
            ImGui::SetItemTooltip("%s", reason.c_str());
        }else{
            ImGui::SetItemTooltip("%s", t == 0 ? "The value goes into this Prefab asset, so to all its instances that do not override it" :
                "This Prefab keeps the value as its override of the Prefab nested in it");
        }
    }
    if (ImGui::MenuItem("Revert", nullptr, false, !readOnly)){
        std::vector<std::unique_ptr<Command>> steps;
        for (const Item& item : items){
            steps.emplace_back(new PrefabOverrideCmd(project, sceneId, item.entity, item.revert, type, item.properties, item.state));
        }
        addCommand(std::move(steps));
    }
    ImGui::SetItemTooltip("Discard this instance's override: the Prefab's value comes back");
}

void editor::InspectorWindow::drawPrefabPropertyLabel(const std::string& label){
    const std::string property = prefabRowProperty;
    prefabRowProperty.clear();
    const bool tracked = prefabRow.active && prefabRow.sceneProject && !property.empty();
    const bool overridden = tracked && isPrefabPropertyOverridden(prefabRow.sceneProject, prefabRow.entities, prefabRow.type, property);

    const ImVec2 cellMin = ImGui::GetCursorScreenPos();
    const ImVec2 cellMax(cellMin.x + ImGui::GetContentRegionAvail().x, cellMin.y + ImGui::GetFrameHeight());
    if (overridden){
        ImGui::PushFont(App::getBoldFont(), 0.0f);
    }
    ImGui::Text("%s", label.c_str());
    if (overridden){
        ImGui::PopFont();
        drawPrefabOverrideBar(cellMin.y, cellMax.y);
    }
    if (!overridden){
        return;
    }
    // Unity: a right click on an overridden value offers Apply and Revert
    const std::string popupId = "##prefab_property_menu_" + property;
    if (ImGui::IsMouseHoveringRect(cellMin, cellMax) && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)){
        ImGui::OpenPopup(popupId.c_str());
    }
    if (ImGui::IsMouseHoveringRect(cellMin, cellMax) && !ImGui::IsPopupOpen(popupId.c_str())){
        ImGui::SetItemTooltip("Overridden in this Prefab instance. Right click to apply or revert.");
    }
    // the menu stays usable on a running scene's selection (Revert only, Apply waits for Stop)
    ImGui::BeginDisabled(false);
    if (ImGui::BeginPopup(popupId.c_str())){
        ImGui::TextDisabled("%s", label.c_str());
        drawPrefabApplyRevertItems(prefabRow.sceneProject, prefabRow.entities, prefabRow.type, property, 0, prefabRow.readOnly);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
}

void editor::InspectorWindow::prefabStateContext(SceneProject* sceneProject, const std::vector<Entity>& entities, uint8_t stateBit, bool readOnly,
        float barTop, float barBottom){
    bool overridden = false;
    for (Entity entity : entities){
        if (project->getPrefabObjectOverrides(sceneProject->id, entity).state & stateBit){
            overridden = true;
            break;
        }
    }
    if (!overridden){
        return;
    }
    drawPrefabOverrideBar(barTop, barBottom);
    const std::string popupId = "##prefab_state_menu_" + std::to_string(stateBit);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)){
        ImGui::OpenPopup(popupId.c_str());
    }
    // one menu per field and frame (a field may be two items: Tag's label and its combo)
    static std::map<std::string, int> drawnFrame;
    int& frame = drawnFrame[popupId];
    if (frame == ImGui::GetFrameCount()){
        return;
    }
    frame = ImGui::GetFrameCount();
    if (ImGui::BeginPopup(popupId.c_str())){
        ImGui::TextDisabled("%s", stateNames(stateBit).c_str());
        drawPrefabApplyRevertItems(sceneProject, entities, ComponentType::PrefabComponent, std::string(), stateBit, readOnly);
        ImGui::EndPopup();
    }
}

void editor::InspectorWindow::drawRemovedPrefabComponents(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly){
    // Unity lists a component the instance removed where it was, greyed and struck through,
    // with Apply / Revert on its context menu
    std::set<ComponentType> removed;
    for (Entity entity : entities){
        const Prefab::ObjectOverrides overrides = project->getPrefabObjectOverrides(sceneProject->id, entity);
        removed.insert(overrides.removedComponents.begin(), overrides.removedComponents.end());
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    for (ComponentType type : removed){
        ImGui::PushID(("removed_component_" + std::to_string(static_cast<int>(type))).c_str());
        const std::string name = Catalog::getComponentDisplayName(type);
        const float height = ImGui::GetFrameHeight() + Theme::dpi(2.0f);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + height);
        ImGui::InvisibleButton("##removed_header", ImVec2(max.x - min.x, height));
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsItemClicked(ImGuiMouseButton_Left);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg, 0.5f));
        drawList->AddLine(min, ImVec2(max.x, min.y), ImGui::GetColorU32(ImGuiCol_Border), Theme::dpi(1.0f));
        drawPrefabOverrideBar(min.y, max.y);
        const float fontSize = ImGui::GetFontSize();
        const float textY = min.y + (height - fontSize) * 0.5f;
        float x = min.x + style.FramePadding.x + fontSize + style.ItemInnerSpacing.x;
        const ImU32 grey = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        drawList->AddText(ImVec2(x, textY), grey, Catalog::getComponentIcon(type));
        x += fontSize * 1.25f + style.ItemInnerSpacing.x;
        const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
        drawList->AddText(ImVec2(x, textY), grey, name.c_str());
        // struck through
        drawList->AddLine(ImVec2(x, textY + fontSize * 0.55f), ImVec2(x + nameSize.x, textY + fontSize * 0.55f), grey, Theme::dpi(1.0f));
        drawList->AddText(ImVec2(x + nameSize.x + style.ItemInnerSpacing.x, textY), grey, "(Removed)");
        if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("removed_component_menu")){
            ImGui::SetTooltip("Removed Component: this Prefab instance removed it. Click to apply or revert.");
        }
        if (clicked){
            ImGui::OpenPopup("removed_component_menu");
        }
        if (ImGui::BeginPopup("removed_component_menu")){
            ImGui::TextDisabled("%s", name.c_str());
            drawPrefabApplyRevertItems(sceneProject, entities, type, std::string(), 0, readOnly);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

// ---- The Prefab bar ----

void editor::InspectorWindow::drawPrefabBar(SceneProject* sceneProject, const std::vector<Entity>& entities, bool readOnly, float gap){
    Scene* scene = sceneProject->scene;
    const ImGuiStyle& style = ImGui::GetStyle();

    // Unity's Missing Prefab bar: the asset of this instance is gone; the scene keeps what
    // it stored for the instance and rebuilds it when the asset comes back
    if (!entities.empty() && std::all_of(entities.begin(), entities.end(), [&](Entity entity) {
            return project->isMissingPrefab(sceneProject->id, entity);
        })) {
        const MissingPrefabInstance* missing = project->getMissingPrefab(sceneProject->id, entities[0]);
        const ImVec4 missingColor(0.96f, 0.36f, 0.36f, 1.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Prefab");
        ImGui::SameLine(0.0f, gap);
        ImGui::TextColored(missingColor, ICON_FA_TRIANGLE_EXCLAMATION " Missing Prefab");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("The Prefab asset of this instance cannot be found.\n"
                "Last known path: %s\nGUID: %s\n"
                "Its overrides are kept, and it is relinked when the asset comes back.",
                missing ? missing->path.c_str() : "", missing && !missing->guid.empty() ? missing->guid.c_str() : "none");
        }
        if (missing && !missing->path.empty()) {
            ImGui::SameLine(0.0f, gap);
            ImGui::TextDisabled("%s", missing->path.c_str());
        }
        return;
    }

    const std::vector<Entity> prefabRoots = project->getOutermostPrefabRoots(sceneProject->id, entities);
    if (std::any_of(prefabRoots.begin(), prefabRoots.end(), [](Entity root) { return root == NULL_ENTITY; })) {
        return;
    }
    std::string assetPath;
    bool mixedAsset = false;
    for (size_t i = 0; i < prefabRoots.size(); i++) {
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(prefabRoots[i]);
        const std::string path = component ? component->path : std::string();
        if (i == 0) {
            assetPath = path;
        } else if (path != assetPath) {
            mixedAsset = true;
        }
    }
    const bool oneInstance = std::all_of(prefabRoots.begin(), prefabRoots.end(),
        [&prefabRoots](Entity root) { return root == prefabRoots[0]; });

    // Unity's row: "Prefab" then Open, Select and the Overrides dropdown
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(prefabTextColor, ICON_FA_CUBE " Prefab");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        if (mixedAsset) {
            ImGui::SetTooltip("Instances of different Prefabs");
        } else if (oneInstance) {
            ImGui::SetTooltip("Prefab instance \"%s\" of %s", scene->getEntityName(prefabRoots[0]).c_str(), assetPath.c_str());
        } else {
            ImGui::SetTooltip("Instances of %s", assetPath.c_str());
        }
    }

    const char* openLabel = "Open";
    const char* selectLabel = "Select";
    const char* overridesLabel = "Overrides " ICON_FA_CARET_DOWN;
    const bool showOverrides = oneInstance && !mixedAsset;
    const float buttonsWidth = ImGui::CalcTextSize(openLabel).x + ImGui::CalcTextSize(selectLabel).x + ImGui::CalcTextSize(overridesLabel).x +
        style.FramePadding.x * 6.0f + style.ItemInnerSpacing.x * 2.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + gap, ImGui::GetWindowContentRegionMax().x - buttonsWidth));

    // Open: Prefab Mode on the asset (agent D's App::openPrefabAsset). PREFAB-MODE-WIRE: when
    // it exists, set prefabModeAvailable and uncomment the call below.
    constexpr bool prefabModeAvailable = false;
    ImGui::BeginDisabled(!prefabModeAvailable || mixedAsset || assetPath.empty());
    if (ImGui::Button(openLabel)) {
        // PREFAB-MODE-WIRE: Backend::getApp().openPrefabAsset(assetPath);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", prefabModeAvailable ? "Open the Prefab asset in Prefab Mode" : "Prefab Mode is not available yet");

    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::BeginDisabled(mixedAsset || assetPath.empty());
    if (ImGui::Button(selectLabel)) {
        if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
            projectWindow->revealAsset(project->getProjectPath() / assetPath);
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Select the Prefab asset in the Project window");

    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::BeginDisabled(!showOverrides);
    if (ImGui::Button(overridesLabel)) {
        prefabOverridesSelection.clear();
        prefabOverridesRoot = prefabRoots[0];
        ImGui::OpenPopup("prefab_overrides_menu");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", showOverrides ? "Review, revert or apply the overrides of this Prefab instance" :
        "Select one Prefab instance to review its overrides");
    const ImVec2 overridesCorner(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);

    // Drops down from the button, right aligned
    ImGui::SetNextWindowPos(overridesCorner, ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
    if (showOverrides && ImGui::BeginPopup("prefab_overrides_menu")) {
        drawPrefabOverridesPopup(sceneProject, prefabRoots[0], readOnly);
        ImGui::EndPopup();
    }
}

// ---- The Overrides dropdown ----

std::string editor::InspectorWindow::formatPrefabValue(EntityRegistry* registry, const PropertyData& prop){
    if (!prop.ref){
        return "-";
    }
    char buffer[160];
    switch (prop.type){
        case PropertyType::Vector2: {
            const Vector2& v = *static_cast<Vector2*>(prop.ref);
            snprintf(buffer, sizeof(buffer), "(%.3g, %.3g)", v.x, v.y);
            return buffer;
        }
        case PropertyType::Vector3: {
            const Vector3& v = *static_cast<Vector3*>(prop.ref);
            snprintf(buffer, sizeof(buffer), "(%.3g, %.3g, %.3g)", v.x, v.y, v.z);
            return buffer;
        }
        case PropertyType::Vector4: {
            const Vector4& v = *static_cast<Vector4*>(prop.ref);
            snprintf(buffer, sizeof(buffer), "(%.3g, %.3g, %.3g, %.3g)", v.x, v.y, v.z, v.w);
            return buffer;
        }
        case PropertyType::Quat: {
            const Vector3 euler = static_cast<Quaternion*>(prop.ref)->getEulerAngles(RotationOrder::ZYX);
            snprintf(buffer, sizeof(buffer), "(%.3g, %.3g, %.3g)", euler.x, euler.y, euler.z);
            return buffer;
        }
        case PropertyType::Enum:
            return std::to_string(*static_cast<int*>(prop.ref));
        case PropertyType::Material: {
            const Material& material = *static_cast<Material*>(prop.ref);
            const Vector4& c = material.baseColorFactor;
            snprintf(buffer, sizeof(buffer), "%s (%.2g, %.2g, %.2g, %.2g)", material.name.empty() ? "Material" : material.name.c_str(), c.x, c.y, c.z, c.w);
            return buffer;
        }
        case PropertyType::Texture: {
            const std::string path = static_cast<Texture*>(prop.ref)->getPath(0);
            return path.empty() ? "None" : path;
        }
        case PropertyType::Entity:
        case PropertyType::EntityReference: {
            const Entity entity = *static_cast<Entity*>(prop.ref);
            if (entity == NULL_ENTITY){
                return "None";
            }
            return registry && registry->isEntityCreated(entity) ? registry->getEntityName(entity) : "GameObject " + std::to_string(entity);
        }
        case PropertyType::Custom:
            return "(value)";
        default:
            return formatPropertyLabelValue(prop);
    }
}

void editor::InspectorWindow::drawPrefabComparison(SceneProject* sceneProject, const PrefabOverrideEntry& entry, Entity root, bool locked){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;

    auto addCommand = [&](Command* command) {
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(command);
    };

    // Title
    std::string title;
    switch (entry.kind) {
        case Kind::State: title = scene->getEntityName(entry.entity) + " (GameObject)"; break;
        case Kind::Property: title = scene->getEntityName(entry.entity) + " " ICON_FA_ANGLE_RIGHT " " + Catalog::getComponentDisplayName(entry.type); break;
        case Kind::AddedComponent: title = scene->getEntityName(entry.entity) + " " ICON_FA_ANGLE_RIGHT " " + Catalog::getComponentDisplayName(entry.type) + " (Added)"; break;
        case Kind::RemovedComponent: title = scene->getEntityName(entry.entity) + " " ICON_FA_ANGLE_RIGHT " " + Catalog::getComponentDisplayName(entry.type) + " (Removed)"; break;
        case Kind::AddedGameObject: title = scene->getEntityName(entry.entity) + " (Added GameObject)"; break;
        case Kind::RemovedGameObject: title = entry.removal.name + " (Removed GameObject)"; break;
    }
    ImGui::TextUnformatted(title.c_str());

    // Revert and Apply for this item
    std::vector<std::filesystem::path> targets;
    if (entry.kind == Kind::AddedGameObject) {
        targets.push_back(project->findPrefabPathFor(sceneId, entry.parent));
        if (project->isPrefabInstanceRoot(sceneId, entry.parent)) {
            if (const PrefabComponent* component = scene->findComponent<PrefabComponent>(entry.parent)) {
                targets.front() = component->path;
            }
        }
    } else if (entry.kind == Kind::RemovedGameObject) {
        targets.push_back(entry.removal.path);
    } else {
        targets = project->getPrefabApplyTargets(sceneId, entry.entity);
    }
    const std::vector<std::string> names(entry.properties.begin(), entry.properties.end());
    auto applyCommand = [&](const std::filesystem::path& target) -> Command* {
        switch (entry.kind) {
            case Kind::State: return new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::ApplyState, ComponentType::PrefabComponent, {}, entry.state, target);
            case Kind::Property: return new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::ApplyProperty, entry.type, names, 0, target);
            case Kind::AddedComponent: return new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::ApplyAddedComponent, entry.type, {}, 0, target);
            case Kind::RemovedComponent: return new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::ApplyRemovedComponent, entry.type, {}, 0, target);
            case Kind::AddedGameObject: return new AddEntityToPrefabCmd(project, sceneId, entry.entity, entry.parent);
            case Kind::RemovedGameObject: return new ApplyPrefabRemovalCmd(project, sceneId, entry.removal.path, entry.removal.instanceRoot, entry.removal.registryEntity);
        }
        return nullptr;
    };
    auto revertCommand = [&]() -> Command* {
        switch (entry.kind) {
            case Kind::State: return new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::RevertState, ComponentType::PrefabComponent, {}, entry.state);
            case Kind::Property: return new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::RevertProperty, entry.type, names);
            case Kind::AddedComponent: return new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::RevertAddedComponent, entry.type);
            case Kind::RemovedComponent: return new PrefabOverrideCmd(project, sceneId, entry.entity, Operation::RevertRemovedComponent, entry.type);
            case Kind::AddedGameObject: return new DeleteEntityCmd(project, sceneId, entry.entity);
            case Kind::RemovedGameObject: return new RevertPrefabRemovalCmd(project, sceneId, entry.removal.path, entry.removal.instanceRoot, entry.removal.registryEntity);
        }
        return nullptr;
    };
    std::string reason;
    bool canApply = !locked && project->canApplyPrefabOverrides(&reason);
    if (canApply && (entry.kind == Kind::Property || entry.kind == Kind::AddedComponent)) {
        canApply = project->canApplyPrefabOverride(sceneId, entry.entity, entry.type, names, {}, &reason);
    }

    ImGui::SameLine();
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool menuApply = targets.size() > 1;
    const char* applyLabel = menuApply ? "Apply " ICON_FA_CARET_DOWN : "Apply";
    const float buttonsWidth = ImGui::CalcTextSize("Revert").x + ImGui::CalcTextSize(applyLabel).x + style.FramePadding.x * 4.0f + style.ItemInnerSpacing.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttonsWidth));
    ImGui::BeginDisabled(locked);
    if (ImGui::Button("Revert")) {
        addCommand(revertCommand());
        prefabOverridesSelection.clear();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Discard this override: the Prefab's value comes back");
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button(applyLabel)) {
        if (menuApply) {
            ImGui::OpenPopup("prefab_apply_targets");
        } else if (!targets.empty()) {
            addCommand(applyCommand(targets.front()));
            prefabOverridesSelection.clear();
        }
    }
    ImGui::EndDisabled();
    if (!canApply && !reason.empty()) {
        ImGui::SetItemTooltip("%s", reason.c_str());
    } else if (!targets.empty()) {
        ImGui::SetItemTooltip("Apply to Prefab '%s'%s", prefabName(targets.front()).c_str(), menuApply ? " or a Prefab holding it" : "");
    }
    if (ImGui::BeginPopup("prefab_apply_targets")) {
        for (size_t t = targets.size(); t-- > 0;) {
            const std::string label = (t == 0 ? "Apply to Prefab '" : "Apply as Override in Prefab '") + prefabName(targets[t]) + "'";
            std::string targetReason;
            bool enabled = true;
            if (entry.kind == Kind::Property || entry.kind == Kind::AddedComponent) {
                enabled = project->canApplyPrefabOverride(sceneId, entry.entity, entry.type, names, targets[t], &targetReason);
            }
            if (ImGui::MenuItem(label.c_str(), nullptr, false, enabled)) {
                addCommand(applyCommand(targets[t]));
                prefabOverridesSelection.clear();
            }
            if (!enabled && !targetReason.empty()) {
                ImGui::SetItemTooltip("%s", targetReason.c_str());
            }
        }
        ImGui::EndPopup();
    }
    if (prefabOverridesSelection.empty()) {
        return; // done: the next frame lists what is left
    }

    ImGui::Separator();

    // Prefab Source | Override, property by property
    auto row = [&](const std::string& name, const std::string& source, const std::string& own, bool differs) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextWrapped("%s", source.c_str());
        ImGui::TableSetColumnIndex(2);
        if (differs) ImGui::PushFont(App::getBoldFont(), 0.0f);
        ImGui::TextWrapped("%s", own.c_str());
        if (differs) ImGui::PopFont();
    };
    if (!ImGui::BeginTable("prefab_comparison", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
        return;
    }
    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Prefab Source", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Override", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    switch (entry.kind) {
        case Kind::State: {
            std::string name, tag;
            bool active = true;
            int layer = 0;
            const bool known = project->getPrefabSourceState(sceneId, entry.entity, name, active, tag, layer);
            auto bit = [&](uint8_t b) { return (entry.state & b) != 0; };
            if (bit(PrefabObjectOverrides::StateName)) row("Name", known ? name : "-", scene->getEntityName(entry.entity), true);
            if (bit(PrefabObjectOverrides::StateActive)) row("Active", known ? (active ? "true" : "false") : "-", scene->isEntityActive(entry.entity) ? "true" : "false", true);
            if (bit(PrefabObjectOverrides::StateTag)) row("Tag", known ? tag : "-", scene->getEntityTag(entry.entity), true);
            if (bit(PrefabObjectOverrides::StateLayer)) row("Layer", known ? std::to_string(layer) : "-", std::to_string(scene->getEntityLayer(entry.entity)), true);
            break;
        }
        case Kind::Property:
        case Kind::RemovedComponent: {
            PrefabSourceValue source;
            const bool hasSource = project->getPrefabSourceValue(sceneId, entry.entity, entry.type, std::string(), source) && source.registry;
            std::vector<std::string> shown;
            if (entry.kind == Kind::RemovedComponent || entry.properties.count(PrefabObjectOverrides::WHOLE_COMPONENT)) {
                EntityRegistry* registry = hasSource ? source.registry.get() : scene;
                const Entity entity = hasSource ? source.entity : entry.entity;
                for (auto& [name, data] : Catalog::findEntityProperties(registry, entity, entry.type)) {
                    shown.push_back(name);
                }
            } else {
                shown.assign(entry.properties.begin(), entry.properties.end());
            }
            for (const std::string& name : shown) {
                const std::string sourceText = hasSource ? formatPrefabValue(scene, Catalog::findProperty(source.registry.get(), source.entity, entry.type, name)) : "-";
                const std::string ownText = entry.kind == Kind::RemovedComponent ? std::string("(Removed)") :
                    formatPrefabValue(scene, Catalog::findProperty(scene, entry.entity, entry.type, name));
                row(name, sourceText, ownText, entry.kind == Kind::RemovedComponent || sourceText != ownText || entry.properties.count(name) != 0);
            }
            break;
        }
        case Kind::AddedComponent:
            for (auto& [name, data] : Catalog::findEntityProperties(scene, entry.entity, entry.type)) {
                row(name, "(not in the Prefab)", formatPrefabValue(scene, data), true);
            }
            break;
        case Kind::AddedGameObject: {
            row("GameObject", "(not in the Prefab)", scene->getEntityName(entry.entity), true);
            for (ComponentType type : Catalog::findComponents(scene, entry.entity)) {
                row("Component", "", Catalog::getComponentDisplayName(type), true);
            }
            break;
        }
        case Kind::RemovedGameObject:
            row("GameObject", entry.removal.name, entry.removal.belowRemoved ? "(Removed with its parent)" : "(Removed)", true);
            break;
    }
    ImGui::EndTable();
    (void)root;
}

void editor::InspectorWindow::drawPrefabOverridesPopup(SceneProject* sceneProject, Entity root, bool readOnly){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const bool locked = readOnly;

    const std::vector<PrefabOverrideEntry> entries = project->getPrefabOverrideEntries(sceneId, root);
    const PrefabComponent* rootComponent = scene->findComponent<PrefabComponent>(root);
    const std::filesystem::path rootPath = rootComponent ? std::filesystem::path(rootComponent->path) : std::filesystem::path();

    ImGui::Text("Review, Revert or Apply Overrides on \"%s\"", scene->getEntityName(root).c_str());
    ImGui::TextDisabled("Prefab: %s", rootPath.generic_string().c_str());
    ImGui::Separator();
    if (entries.empty()) {
        ImGui::Dummy(ImVec2(Theme::dpi(360.0f), 0.0f));
        ImGui::TextDisabled("No overrides");
        return;
    }

    // The items, keyed for the selection
    auto keyOf = [](const PrefabOverrideEntry& entry) {
        switch (entry.kind) {
            case Kind::State: return "s" + std::to_string(entry.entity);
            case Kind::AddedGameObject: return "a" + std::to_string(entry.entity);
            case Kind::RemovedGameObject: return "r" + std::to_string(entry.removal.instanceRoot) + "_" + std::to_string(entry.removal.registryEntity);
            default: return "c" + std::to_string(entry.entity) + "_" + std::to_string(static_cast<int>(entry.type));
        }
    };
    if (prefabOverridesRoot != root) {
        prefabOverridesRoot = root;
        prefabOverridesSelection.clear();
    }
    const PrefabOverrideEntry* selected = nullptr;
    for (const PrefabOverrideEntry& entry : entries) {
        if (keyOf(entry) == prefabOverridesSelection) {
            selected = &entry;
        }
    }

    // Grouped by GameObject (added ones under their parent, removed ones under their instance)
    std::vector<Entity> groups;
    std::map<Entity, std::vector<const PrefabOverrideEntry*>> byGroup;
    for (const PrefabOverrideEntry& entry : entries) {
        Entity group = entry.entity;
        if (entry.kind == Kind::AddedGameObject) group = entry.parent;
        if (entry.kind == Kind::RemovedGameObject) group = entry.removal.instanceRoot;
        if (!byGroup.count(group)) groups.push_back(group);
        byGroup[group].push_back(&entry);
    }

    const ImVec2 size(Theme::dpi(760.0f), Theme::dpi(360.0f));
    ImGui::BeginChild("##prefab_overrides_tree", ImVec2(size.x * 0.42f, size.y), ImGuiChildFlags_Borders);
    for (Entity group : groups) {
        const std::string icon = HierarchyWindow::getObjectIcon(scene->getSignature(group), scene);
        const std::string label = icon + "  " + scene->getEntityName(group) + "##group_" + std::to_string(group);
        const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        if (!open) {
            continue;
        }
        for (const PrefabOverrideEntry* entry : byGroup[group]) {
            std::string itemLabel;
            bool greyed = false;
            switch (entry->kind) {
                case Kind::State: itemLabel = ICON_FA_CUBE "  GameObject (" + stateNames(entry->state) + ")"; break;
                case Kind::Property: itemLabel = std::string(Catalog::getComponentIcon(entry->type)) + "  " + Catalog::getComponentDisplayName(entry->type); break;
                case Kind::AddedComponent: itemLabel = ICON_FA_PLUS "  " + Catalog::getComponentDisplayName(entry->type) + " (Added)"; break;
                case Kind::RemovedComponent: itemLabel = ICON_FA_MINUS "  " + Catalog::getComponentDisplayName(entry->type) + " (Removed)"; greyed = true; break;
                case Kind::AddedGameObject: itemLabel = ICON_FA_PLUS "  " + scene->getEntityName(entry->entity) + " (Added GameObject)"; break;
                case Kind::RemovedGameObject: itemLabel = ICON_FA_MINUS "  " + entry->removal.name + " (Removed GameObject)"; greyed = true; break;
            }
            const std::string key = keyOf(*entry);
            if (greyed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (ImGui::Selectable((itemLabel + "##" + key).c_str(), prefabOverridesSelection == key)) {
                prefabOverridesSelection = key;
            }
            if (greyed) ImGui::PopStyleColor();
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##prefab_overrides_comparison", ImVec2(size.x * 0.58f - ImGui::GetStyle().ItemSpacing.x, size.y), ImGuiChildFlags_Borders);
    if (selected) {
        drawPrefabComparison(sceneProject, *selected, root, locked);
    } else {
        ImGui::TextDisabled("Select an override to compare it with the Prefab");
    }
    ImGui::EndChild();

    // Revert All / Apply All (to the instance's Prefab, the outermost), one undo step each
    ImGui::Separator();
    std::string reason;
    const bool canApply = !locked && project->canApplyPrefabOverrides(&reason);
    ImGui::BeginDisabled(locked);
    if (ImGui::Button("Revert All")) {
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(PrefabOverridesCmd::revertAll(project, sceneId, root));
        prefabOverridesSelection.clear();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Discard every override: the instance becomes its Prefab again");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button("Apply All")) {
        if (PrefabOverridesCmd* command = PrefabOverridesCmd::applyAll(project, sceneId, root)) {
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(command);
        }
        prefabOverridesSelection.clear();
    }
    ImGui::EndDisabled();
    if (!canApply && !reason.empty()) {
        ImGui::SetItemTooltip("%s", reason.c_str());
    } else {
        ImGui::SetItemTooltip("Apply every override to Prefab '%s' (values referencing GameObjects outside it stay overrides)", prefabName(rootPath).c_str());
    }
}
