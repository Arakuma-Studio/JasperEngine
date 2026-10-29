// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Hierarchy's version control (docs/specs/S29-version-control.md R27, R29): the scene row
// shows its file's state; with "Show VCS Changes" (the toolbar toggle, on by default while a
// provider is active) each GameObject row shows what the scene diff says of it (Added, Modified,
// Reparented) after its name, clear of the prefab badges on the icon; the scene row's context
// menu holds the Version Control items.

#include "HierarchyWindow.h"

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "vcs/SceneDiffTracker.h"
#include "vcs/VcsService.h"
#include "window/vcs/VcsOverlay.h"
#include "window/vcs/VcsUi.h"

using namespace jasperengine;

bool editor::HierarchyWindow::showsVcsChanges() const {
    return project->getVcs().active() && AppSettings::getVersionControlSettings().showChanges;
}

void editor::HierarchyWindow::showVcsToolbarToggle() {
    if (!project->getVcs().active()) return;
    const bool on = AppSettings::getVersionControlSettings().showChanges;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_FA_CODE_BRANCH "##ShowVcsChanges")) {
        VersionControlSettings settings = AppSettings::getVersionControlSettings();
        settings.showChanges = !on;
        AppSettings::setVersionControlSettings(settings);
    }
    if (on) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Show VCS Changes: GameObjects added, modified or moved since the last commit");
    ImGui::SameLine();
}

void editor::HierarchyWindow::drawVcsRowOverlay(const TreeNode& node, const ImVec2& rowMin, const ImVec2& rowMax) {
    if (node.isChildScene) return;
    const vcs::VcsService& service = project->getVcs();
    if (!service.active()) return;
    vcs::Badge badge = vcs::Badge::None;
    vcs::LockState lock = vcs::LockState::Unsupported;
    std::string tooltip;
    if (node.isScene) {
        // the scene's (or the prefab stage's) file
        const SceneProject* sceneProject = nullptr;
        for (SceneProject* candidate : project->getScenesAndPrefabStages()) {
            if (candidate->id == node.id) sceneProject = candidate;
        }
        if (!sceneProject || sceneProject->filepath.empty()) return;
        std::filesystem::path file = sceneProject->filepath;
        if (const PrefabStage* stage = project->getPrefabStage(node.id)) file = stage->assetPath;
        if (file.is_relative()) file = project->getProjectPath() / file;
        const vcs::VcsChange* change = service.find(file);
        if (!change) return;
        badge = vcs::badgeOf(*change);
        lock = change->lock;
        tooltip = vcs::tooltipOf(*change);
    } else {
        if (!showsVcsChanges()) return;
        const vcs::SceneDiffView* view = project->getSceneDiffs().view(getNodeSceneId(node));
        if (!view) return;
        const vcs::scene::ObjectDiff* object = view->object(node.id);
        if (!object) return;
        badge = vcs::badgeOf(*object);
        tooltip = vcs::tooltipOf(*object);
        if (view->fromDisk) tooltip += "\nShowing saved changes (the scene is too big to compare its unsaved edits)";
    }
    if (badge == vcs::Badge::None && lock != vcs::LockState::LockedByMe && lock != vcs::LockState::LockedByOther) return;
    const std::string label = node.icon + "  " + node.name;
    const float size = ImGui::GetFontSize() * 0.8f;
    const float x = rowMin.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().ItemInnerSpacing.x;
    const ImVec2 pos(x, rowMin.y + (rowMax.y - rowMin.y - size) * 0.5f);
    const float width = vcs::drawBadges(ImGui::GetWindowDrawList(), pos, size, badge, lock);
    if (width > 0.0f && ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + width, pos.y + size)) && !ImGui::GetDragDropPayload()) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }
}

void editor::HierarchyWindow::showVcsMenu(const SceneProject* sceneProject) {
    if (!sceneProject || sceneProject->filepath.empty()) return;
    std::filesystem::path file = sceneProject->filepath;
    if (const PrefabStage* stage = project->getPrefabStage(sceneProject->id)) file = stage->assetPath;
    if (file.is_relative()) file = project->getProjectPath() / file;
    ImGui::Separator();
    Backend::getApp().getVcsUi()->drawContextMenu({file}, sceneProject->id);
}
