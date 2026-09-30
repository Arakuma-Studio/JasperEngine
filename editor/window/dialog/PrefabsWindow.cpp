// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabsWindow.h"
#include "Theme.h"

#include <algorithm>

namespace jasperengine::editor {

static constexpr float dialogWidth = 600.0f;
static constexpr float dialogHeight = 420.0f;
static constexpr float dialogButtonWidth = 120.0f;
static constexpr ImGuiWindowFlags noScrollFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

void PrefabsWindow::open(Project* project) {
    m_isOpen = true;
    m_project = project;
    m_entries.clear();

    for (const fs::path& prefabPath : project->findPrefabFiles()) {
        PrefabEntry entry;
        entry.path = prefabPath;

        for (const SceneProject& sceneProject : project->getScenes()) {
            const bool usesPrefab = std::any_of(sceneProject.prefabs.begin(), sceneProject.prefabs.end(),
                [&](const PrefabSceneInfo& info) { return info.prefabPath == prefabPath; });
            if (usesPrefab) {
                entry.usedBy += entry.usedBy.empty() ? sceneProject.name : ", " + sceneProject.name;
            }
        }

        entry.included = !entry.usedBy.empty() || project->isStandalonePrefab(prefabPath);
        m_entries.push_back(std::move(entry));
    }
}

void PrefabsWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Prefabs##PrefabsModal");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImVec2 size(
        std::min(Theme::dpi(dialogWidth), viewport->WorkSize.x * 0.9f),
        std::min(Theme::dpi(dialogHeight), viewport->WorkSize.y * 0.9f)
    );
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal |
                             ImGuiWindowFlags_NoResize |
                             noScrollFlags;

    if (ImGui::BeginPopupModal("Prefabs##PrefabsModal", &m_isOpen, flags)) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            drawPrefabs();
        }
        ImGui::EndPopup();
    }
}

void PrefabsWindow::drawPrefabs() {
    const ImGuiStyle& style = ImGui::GetStyle();

    ImGui::TextWrapped("Prefabs used by a scene are always in the build. Check the others to include them too, "
                       "so scripts can instantiate them by name.");
    ImGui::Spacing();

    const float footerY = ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetFrameHeight();
    const float listHeight = std::max(1.0f, footerY - style.ItemSpacing.y * 2.0f - ImGui::GetCursorPosY());

    if (ImGui::BeginChild("##PrefabsList", ImVec2(0, listHeight), ImGuiChildFlags_Borders)) {
        if (m_entries.empty()) {
            ImGui::TextDisabled("No Prefab found in this project.");
        } else if (ImGui::BeginTable("##PrefabsTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Prefab", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableHeadersRow();

            for (PrefabEntry& entry : m_entries) {
                const bool locked = !entry.usedBy.empty();

                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                ImGui::BeginDisabled(locked);
                ImGui::Checkbox(entry.path.generic_string().c_str(), &entry.included);
                ImGui::EndDisabled();

                ImGui::TableNextColumn();
                if (locked) {
                    ImGui::TextUnformatted(entry.usedBy.c_str());
                } else {
                    ImGui::TextDisabled("-");
                }
            }

            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, footerY - style.ItemSpacing.y));
    ImGui::Separator();

    const float footerWidth = std::max(1.0f, ImGui::GetWindowWidth() - style.WindowPadding.x * 2.0f);
    const float buttonWidth = std::clamp((footerWidth - style.ItemSpacing.x) * 0.5f, 1.0f, Theme::dpi(dialogButtonWidth));
    const float buttonsWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
    // Unity-style footer: buttons on the right, primary first.
    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x + std::max(0.0f, footerWidth - buttonsWidth), footerY));

    if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) {
        applyPrefabs();
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
}

void PrefabsWindow::applyPrefabs() {
    if (!m_project) {
        return;
    }

    // Scene prefabs are built through their scene, listing them would keep them after the last
    // instance is deleted
    std::vector<fs::path> prefabPaths;
    for (const PrefabEntry& entry : m_entries) {
        if (entry.included && entry.usedBy.empty()) {
            prefabPaths.push_back(entry.path);
        }
    }

    m_project->setStandalonePrefabs(std::move(prefabPaths));
    m_project->saveProject();
}

}
