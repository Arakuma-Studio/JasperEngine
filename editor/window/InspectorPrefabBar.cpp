// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's Prefab bar (Overrides, Select, Unpack) and its Overrides dropdown.

#include "InspectorWindow.h"

#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/ComponentToPrefabSharedCmd.h"
#include "command/type/UnpackPrefabCmd.h"
#include "command/type/PrefabOverridesCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/AddEntityToPrefabCmd.h"
#include "window/ProjectWindow.h"
#include "App.h"
#include "Backend.h"
#include "Theme.h"

#include <algorithm>
#include <functional>
#include <memory>

using namespace jasperengine;

// Prefab (prefab) text color, as in the Hierarchy and the rest of the Inspector
static const ImVec4 prefabTextColor = ImVec4(0.4f, 0.7f, 1.0f, 1.0f);

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
    if (std::none_of(prefabRoots.begin(), prefabRoots.end(), [](Entity root) { return root == NULL_ENTITY; })) {
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
        const bool oneInstance = prefabRoots.size() == 1 || std::all_of(prefabRoots.begin(), prefabRoots.end(),
            [&prefabRoots](Entity root) { return root == prefabRoots[0]; });

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Prefab");
        ImGui::SameLine(0.0f, gap);
        const std::string assetName = mixedAsset ? std::string("-") : std::filesystem::path(assetPath).stem().string();
        ImGui::TextColored(prefabTextColor, ICON_FA_CUBE " %s", assetName.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            if (oneInstance) {
                ImGui::SetTooltip("Prefab instance \"%s\" of %s", scene->getEntityName(prefabRoots[0]).c_str(), assetPath.c_str());
            } else {
                ImGui::SetTooltip("%s", mixedAsset ? "Instances of different Prefabs" : assetPath.c_str());
            }
        }

        const char* overridesLabel = "Overrides " ICON_FA_CARET_DOWN;
        const char* selectLabel = "Select";
        const char* unpackLabel = "Unpack " ICON_FA_CARET_DOWN;
        const bool showOverrides = oneInstance && !mixedAsset;
        const float buttonsWidth = ImGui::CalcTextSize(selectLabel).x + ImGui::CalcTextSize(unpackLabel).x +
            style.FramePadding.x * 4.0f + style.ItemInnerSpacing.x +
            (showOverrides ? ImGui::CalcTextSize(overridesLabel).x + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x : 0.0f);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttonsWidth));

        if (showOverrides) {
            if (ImGui::Button(overridesLabel)) {
                ImGui::OpenPopup("prefab_overrides_menu");
            }
            const ImVec2 overridesCorner(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);
            ImGui::SetItemTooltip("Review, revert or apply the overrides of this Prefab instance");
            ImGui::SetNextWindowPos(overridesCorner, ImGuiCond_Appearing);
            if (ImGui::BeginPopup("prefab_overrides_menu")) {
                drawPrefabOverridesPopup(sceneProject, prefabRoots[0], readOnly);
                ImGui::EndPopup();
            }
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        }

        ImGui::BeginDisabled(mixedAsset || assetPath.empty());
        if (ImGui::Button(selectLabel)) {
            if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
                projectWindow->revealAsset(project->getProjectPath() / assetPath);
            }
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Select the Prefab asset in the Project window");

        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        const bool playing = project->isAnyScenePlaying();
        ImGui::BeginDisabled(readOnly || playing);
        if (ImGui::Button(unpackLabel)) {
            ImGui::OpenPopup("prefab_unpack_menu");
        }
        const ImVec2 unpackCorner(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", playing ? "Stop the scene to unpack" : "Turn the Prefab instance into regular GameObjects");

        // Drops down from the button, right aligned
        ImGui::SetNextWindowPos(unpackCorner, ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
        if (ImGui::BeginPopup("prefab_unpack_menu")) {
            if (ImGui::MenuItem(ICON_FA_BOX_OPEN "  Unpack")) {
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new UnpackPrefabCmd(project, sceneProject->id, entities, false));
            }
            ImGui::SetItemTooltip("Nested Prefab instances stay Prefab instances");
            if (ImGui::MenuItem(ICON_FA_BOXES_PACKING "  Unpack Completely")) {
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new UnpackPrefabCmd(project, sceneProject->id, entities, true));
            }
            ImGui::SetItemTooltip("Nested Prefab instances are unpacked too");
            ImGui::EndPopup();
        }
    }
}

void editor::InspectorWindow::drawPrefabOverridesPopup(SceneProject* sceneProject, Entity root, bool readOnly){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const bool locked = readOnly || project->isAnyScenePlaying();

    const std::vector<std::pair<Entity, ComponentType>> components = project->getPrefabComponentOverrides(sceneId, root);
    const std::vector<PrefabRemovalInfo> removals = project->getPrefabRemovals(sceneId, root);
    const std::vector<std::pair<Entity, Entity>> added = project->getPrefabAddedGameObjects(sceneId, root);

    // The prefab a GameObject of the instance (or the parent of an added one) belongs to
    auto prefabOf = [&](Entity entity) {
        if (const PrefabComponent* component = scene->findComponent<PrefabComponent>(entity)) {
            const Prefab* prefab = project->getPrefab(component->path);
            const Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, entity) : nullptr;
            if (instance && instance->rootEntity == entity) {
                return std::filesystem::path(component->path);
            }
        }
        return project->findPrefabPathFor(sceneId, entity);
    };

    ImGui::Dummy(ImVec2(Theme::dpi(360.0f), 0.0f));
    ImGui::Text("Overrides of \"%s\"", scene->getEntityName(root).c_str());
    ImGui::Separator();
    if (components.empty() && removals.empty() && added.empty()) {
        ImGui::TextDisabled("No overrides");
        return;
    }

    auto addCommand = [&](Command* command) {
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(command);
    };
    // One override: its label, then Revert and Apply
    auto overrideRow = [&](const std::string& id, const std::string& label, const std::string& detail,
                           const std::string& revertTip, bool canApply, const std::string& applyTip,
                           const std::function<Command*()>& revert, const std::function<Command*()>& apply) {
        ImGui::PushID(id.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        if (!detail.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", detail.c_str());
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginDisabled(locked);
        if (ImGui::SmallButton("Revert")) {
            addCommand(revert());
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", revertTip.c_str());
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::BeginDisabled(locked || !canApply);
        if (ImGui::SmallButton("Apply")) {
            addCommand(apply());
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", applyTip.c_str());
        ImGui::PopID();
    };
    auto section = [](const char* title) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", title);
    };

    if (ImGui::BeginTable("prefab_overrides", 2, ImGuiTableFlags_SizingFixedFit)) {
        if (!components.empty()) {
            section("Overridden Components");
            for (const std::pair<Entity, ComponentType>& component : components) {
                const Entity entity = component.first;
                const ComponentType type = component.second;
                const std::string prefabName = prefabOf(entity).stem().string();
                const bool canApply = project->canApplyComponentOverride(sceneId, entity, type);
                overrideRow("component_" + std::to_string(entity) + "_" + std::to_string(static_cast<int>(type)),
                    scene->getEntityName(entity) + " " ICON_FA_ANGLE_RIGHT " " + Catalog::getComponentDisplayName(type), "",
                    "Discard this instance's values and follow the Prefab again", canApply,
                    canApply ? "Apply these values to Prefab '" + prefabName + "' and its instances that follow it" :
                        std::string("It references GameObjects outside the Prefab instance, which the Prefab cannot hold"),
                    [this, sceneId, entity, type]() -> Command* { return new ComponentToPrefabSharedCmd(project, sceneId, entity, type); },
                    [this, sceneId, entity, type]() -> Command* { return new ApplyPrefabComponentCmd(project, sceneId, entity, type); });
            }
        }
        if (!removals.empty()) {
            section("Removed GameObjects");
            for (const PrefabRemovalInfo& removal : removals) {
                const std::string prefabName = removal.path.stem().string();
                std::string detail = removal.instanceRoot != root ? "(" + prefabName + ")" : std::string();
                if (removal.belowRemoved) {
                    detail += detail.empty() ? "(below a removed GameObject)" : " (below a removed GameObject)";
                }
                overrideRow("removed_" + std::to_string(removal.instanceRoot) + "_" + std::to_string(removal.registryEntity),
                    ICON_FA_MINUS " " + removal.name, detail,
                    removal.belowRemoved ? "Stop removing it: it comes back with the removed GameObject above it" :
                        "Bring it back from the Prefab, in its Prefab place",
                    true, "Delete it from Prefab '" + prefabName + "' and all its instances",
                    [this, sceneId, removal]() -> Command* { return new RevertPrefabRemovalCmd(project, sceneId, removal.path, removal.instanceRoot, removal.registryEntity); },
                    [this, sceneId, removal]() -> Command* { return new ApplyPrefabRemovalCmd(project, sceneId, removal.path, removal.instanceRoot, removal.registryEntity); });
            }
        }
        if (!added.empty()) {
            section("Added GameObjects");
            for (const std::pair<Entity, Entity>& addition : added) {
                const Entity entity = addition.first;
                const Entity parent = addition.second;
                const std::string prefabName = prefabOf(parent).stem().string();
                overrideRow("added_" + std::to_string(entity), ICON_FA_PLUS " " + scene->getEntityName(entity), "",
                    "Delete this GameObject, which the instance added", true,
                    "Add it to Prefab '" + prefabName + "' and all its instances",
                    [this, sceneId, entity]() -> Command* { return new DeleteEntityCmd(project, sceneId, entity); },
                    [this, sceneId, entity, parent]() -> Command* { return new AddEntityToPrefabCmd(project, sceneId, entity, parent); });
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(locked);
    if (ImGui::Button("Revert All")) {
        // Added GameObjects first: their delete reads the scene as it is when made
        std::vector<std::unique_ptr<Command>> steps;
        if (!added.empty()) {
            std::vector<Entity> addedEntities;
            for (const std::pair<Entity, Entity>& addition : added) {
                addedEntities.push_back(addition.first);
            }
            steps.emplace_back(new DeleteEntityCmd(project, sceneId, addedEntities));
        }
        // Removed GameObjects before components, so a reverted component that points at one
        // (a Model at its nodes) finds it back
        for (const PrefabRemovalInfo& removal : removals) {
            steps.emplace_back(new RevertPrefabRemovalCmd(project, sceneId, removal.path, removal.instanceRoot, removal.registryEntity));
        }
        for (const std::pair<Entity, ComponentType>& component : components) {
            steps.emplace_back(new ComponentToPrefabSharedCmd(project, sceneId, component.first, component.second));
        }
        addCommand(new PrefabOverridesCmd(project, sceneId, std::move(steps)));
    }
    ImGui::SetItemTooltip("Discard every override: the instance becomes its Prefab again");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::Button("Apply All")) {
        // Added GameObjects first: each reads where it is when made
        std::vector<std::unique_ptr<Command>> steps;
        for (const std::pair<Entity, Entity>& addition : added) {
            steps.emplace_back(new AddEntityToPrefabCmd(project, sceneId, addition.first, addition.second));
        }
        for (const std::pair<Entity, ComponentType>& component : components) {
            if (project->canApplyComponentOverride(sceneId, component.first, component.second)) {
                steps.emplace_back(new ApplyPrefabComponentCmd(project, sceneId, component.first, component.second));
            }
        }
        for (const PrefabRemovalInfo& removal : removals) {
            steps.emplace_back(new ApplyPrefabRemovalCmd(project, sceneId, removal.path, removal.instanceRoot, removal.registryEntity));
        }
        addCommand(new PrefabOverridesCmd(project, sceneId, std::move(steps)));
    }
    ImGui::SetItemTooltip("Make every override the Prefab's, for all its instances (components referencing GameObjects outside the instance stay overrides)");
    ImGui::EndDisabled();
}
