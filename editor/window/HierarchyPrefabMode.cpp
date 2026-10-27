// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Hierarchy's part of Prefab Mode: Unity's "Open Asset in Context / in Isolation" on
// prefab instances (context menu and the row's open arrow), and a stage's header (its back
// arrow and menu)

#include "HierarchyWindow.h"

#include "EditorHost.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "util/BidiText.h"
#include "shortcuts/CommandCatalog.h"

using namespace jasperengine;

namespace {

    // A flat icon button on a Hierarchy row, right-aligned at `rightX`
    bool rowIconButton(const char* id, const char* icon, float rightX, const ImVec4& color) {
        const float width = ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float x = rightX - width;
        if (x > ImGui::GetCursorPosX() + 10.0f) {
            ImGui::SameLine(x);
        } else {
            ImGui::SameLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
        ImGui::PushID(id);
        const bool pressed = ImGui::Button(icon);
        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        return pressed;
    }

}

bool editor::HierarchyWindow::getOpenPrefabTarget(const TreeNode& node, std::filesystem::path& path, Entity& instanceRoot) const {
    if (node.isScene || node.isChildScene || node.isPlayCreated || node.isMissingPrefab) {
        return false;
    }
    if (node.entitySceneId != 0 && node.entitySceneId != project->getSelectedSceneId()) {
        return false; // a child scene's GameObject: open its scene to work on it
    }
    const uint32_t sceneId = project->getSelectedSceneId();
    if (node.isPrefabRoot && !node.nestedPrefabFilepath.empty()) {
        // an instance root opens its own prefab
        path = node.nestedPrefabFilepath;
        instanceRoot = node.id;
        return true;
    }
    if (node.isPrefab && !node.prefabFilepath.empty()) {
        // a GameObject of an instance opens the prefab it belongs to, around its instance
        const Prefab* prefab = project->getPrefab(node.prefabFilepath);
        const Entity root = prefab ? prefab->getRootEntity(sceneId, node.id) : NULL_ENTITY;
        if (root == NULL_ENTITY) {
            return false;
        }
        path = node.prefabFilepath;
        instanceRoot = root;
        return true;
    }
    return false;
}

void editor::HierarchyWindow::showOpenPrefabItems(const TreeNode& node) {
    std::filesystem::path path;
    Entity instanceRoot = NULL_ENTITY;
    const bool canOpen = getOpenPrefabTarget(node, path, instanceRoot);
    const uint32_t sceneId = project->getSelectedSceneId();
    if (ImGui::MenuItem(ICON_FA_UP_RIGHT_FROM_SQUARE "  Open Asset in Context", nullptr, false, canOpen)) {
        editor::getEditorHost().openPrefabAsset(path, sceneId, instanceRoot, true);
    }
    ImGui::SetItemTooltip("Prefab Mode with this scene shown behind the prefab");
    if (ImGui::MenuItem(ICON_FA_CUBE "  Open Asset in Isolation", nullptr, false, canOpen)) {
        editor::getEditorHost().openPrefabAsset(path, sceneId, instanceRoot, false);
    }
    ImGui::SetItemTooltip("Prefab Mode with the prefab alone");
}

void editor::HierarchyWindow::showPrefabStageMenu(SceneProject* sceneProject) {
    PrefabStage* stage = project->getPrefabStage(sceneProject->id);
    if (!stage) {
        return;
    }
    const uint32_t sceneId = sceneProject->id;
    const size_t depth = project->getPrefabStageDepth(sceneId);

    if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save Prefab", CommandCatalog::editor().menuShortcutText("file.save").c_str(), false, project->hasUnsavedPrefabStage(sceneId))) {
        project->savePrefabStage(sceneId);
    }
    bool autoSave = project->isPrefabAutoSave();
    if (ImGui::MenuItem("Auto Save", nullptr, &autoSave)) {
        project->setPrefabAutoSave(autoSave);
    }

    ImGui::Separator();

    if (ImGui::BeginMenu(ICON_FA_CUBE "  GameObject")) {
        // a prefab has one root: new GameObjects go below it
        showCreateMenuItems(false, stage->root, false);
        ImGui::EndMenu();
    }

    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Select Prefab Asset")) {
        revealInProject(stage->assetPath);
    }

    ImGui::Separator();

    Project* proj = project;
    if (depth > 1) {
        const PrefabStage* below = project->getPrefabStageAt(depth - 2);
        const std::string label = std::string(ICON_FA_ARROW_LEFT "  Back to ") + (below ? below->sceneProject.name : std::string("Previous Prefab"));
        if (ImGui::MenuItem(label.c_str())) {
            editor::getEditorHost().enqueueMainThreadTask([proj, depth]() { proj->closePrefabStages(depth - 1); });
        }
    }
    if (ImGui::MenuItem(ICON_FA_ARROW_LEFT "  Exit Prefab Mode")) {
        editor::getEditorHost().enqueueMainThreadTask([proj]() { proj->closePrefabStages(0); });
    }
}

void editor::HierarchyWindow::drawPrefabRowButtons(const TreeNode& node, bool renaming) {
    if (renaming) {
        return;
    }

    // A stage's header: the back arrow, left of the ⋮ button
    if (node.isScene) {
        if (!project->isPrefabStage(node.id)) {
            return;
        }
        const size_t depth = project->getPrefabStageDepth(node.id);
        const float menuWidth = ImGui::CalcTextSize(ICON_FA_ELLIPSIS_VERTICAL).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (rowIconButton("PrefabStageBack", ICON_FA_ARROW_LEFT, ImGui::GetWindowContentRegionMax().x - menuWidth - ImGui::GetStyle().ItemSpacing.x,
                ImGui::GetStyle().Colors[ImGuiCol_Text])) {
            Project* proj = project;
            editor::getEditorHost().enqueueMainThreadTask([proj, depth]() { proj->closePrefabStages(depth > 0 ? depth - 1 : 0); });
        }
        ImGui::SetItemTooltip(depth > 1 ? "Back to the previous prefab" : "Back to the scene (exits Prefab Mode)");
    }
    // An instance root's open arrow is showPrefabRowArrow's (HierarchyPrefabRows.cpp), which
    // opens through openPrefabFromHierarchy below
}

void editor::HierarchyWindow::openPrefabFromHierarchy(const std::filesystem::path& path, Entity instanceRoot) {
    // Unity: from an instance, Prefab Mode in context of it; from inside a stage it goes on top
    if (instanceRoot != NULL_ENTITY) {
        editor::getEditorHost().openPrefabAsset(path, project->getSelectedSceneId(), instanceRoot, true);
    } else {
        editor::getEditorHost().openPrefabAsset(path);
    }
}
