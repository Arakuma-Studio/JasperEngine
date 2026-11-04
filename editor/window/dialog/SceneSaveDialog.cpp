// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneSaveDialog.h"
#include "external/IconsFontAwesome6.h"
#include "Backend.h"
#include "window/EditorModal.h"
#include "util/FileUtils.h"
#include <algorithm>
#include <vector>

namespace jasperengine {
namespace editor {

void SceneSaveDialog::open(const fs::path& projectPath, const fs::path& initialDirectory, const std::string& defaultName,
        std::function<void(const fs::path&)> onSave,
        std::function<void()> onCancel) {
    m_isOpen = true;
    m_projectPath = projectPath;

    fs::path selectedPath = initialDirectory.empty() ? projectPath : initialDirectory;
    std::error_code ec;
    if (selectedPath.is_relative()) {
        selectedPath = projectPath / selectedPath;
    }

    if (!fs::is_directory(selectedPath, ec) || ec) {
        selectedPath = projectPath;
    } else {
        fs::path relativeToProject = selectedPath.lexically_relative(projectPath);
        if ((selectedPath != projectPath && relativeToProject.empty()) ||
                (!relativeToProject.empty() && *relativeToProject.begin() == "..")) {
            selectedPath = projectPath;
        }
    }

    m_selectedPath = selectedPath.lexically_normal().string();
    m_onSave = onSave;
    m_onCancel = onCancel;

    strncpy(m_fileNameBuffer, defaultName.c_str(), sizeof(m_fileNameBuffer) - 1);
    m_fileNameBuffer[sizeof(m_fileNameBuffer) - 1] = '\0';
}

void SceneSaveDialog::show() {
    if (!m_isOpen) {
        return;
    }

    ImGui::OpenPopup("Save Scene##SaveSceneModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal;

    bool popupOpen = EditorModal::begin("Save Scene##SaveSceneModal", nullptr, flags);

    if (popupOpen) {
        if (ImGui::BeginChild("DirectoryBrowser", ImVec2(300, 200), true)) {
            static ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable;

            if (ImGui::BeginTable("DirectoryTree", 1, tableFlags)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                ImGui::SetNextItemOpen(true, ImGuiCond_Always);

                bool isRootSelected = (m_selectedPath == m_projectPath.string());

                if (ImGui::TreeNodeEx("##root", 
                                    ImGuiTreeNodeFlags_OpenOnArrow | 
                                    ImGuiTreeNodeFlags_SpanFullWidth | 
                                    (isRootSelected ? ImGuiTreeNodeFlags_Selected : 0))) {

                    ImGui::SameLine(0, 0);
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", ICON_FA_FOLDER_OPEN);

                    ImGui::SameLine();
                    ImGui::TextUnformatted("Project");

                    if (ImGui::IsItemClicked() || 
                        (ImGui::IsMouseClicked(0) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))) {
                        m_selectedPath = m_projectPath.string();
                    }

                    displayDirectoryTree(m_projectPath);

                    ImGui::TreePop();
                }

                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        EditorModal::message("Where to save the scene: name and folder (a folder of the project, relative to it), then ok; or "
                             "cancel it and save with an explicit path.");
        // an agent gives the folder as a path, a human clicks it in the tree above
        std::string folder = FileUtils::pathToGenericUtf8(fs::relative(m_selectedPath, m_projectPath));
        if (EditorModal::pathField("folder", folder)) {
            const fs::path chosen = fs::u8path(folder);
            m_selectedPath = (chosen.is_absolute() ? chosen : m_projectPath / chosen).string();
        }
        ImGui::TextUnformatted("Scene Name");
        ImGui::SetNextItemWidth(-1);
        EditorModal::inputText("##fileName", "name", m_fileNameBuffer, sizeof(m_fileNameBuffer));

        const fs::path saveDir = fs::relative(m_selectedPath, m_projectPath);
        const std::string saveIn = (saveDir.empty() || saveDir == ".") ? "Project" : "Project/" + saveDir.generic_string();
        ImGui::TextWrapped("Save in: %s", saveIn.c_str());

        std::string fileName = m_fileNameBuffer;

        // Check if the filename has the correct extension
        if (!fileName.empty() && fileName.find(".scene") == std::string::npos) {
            fileName += ".scene";
            strncpy(m_fileNameBuffer, fileName.c_str(), sizeof(m_fileNameBuffer) - 1);
            m_fileNameBuffer[sizeof(m_fileNameBuffer) - 1] = '\0';
        }

        bool canSave = !fileName.empty();
        fs::path fullPath = fs::path(m_selectedPath) / fileName;
        bool fileExists = fs::exists(fullPath);

        if (fileExists) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "A scene with this name already exists and will be replaced.");
        }
        std::error_code folderError;
        EditorModal::error(!canSave ? std::string("The name is empty.") :
                           fs::is_directory(m_selectedPath, folderError) ? std::string() : "The folder " + m_selectedPath + " does not exist.");

        ImGui::Separator();

        // Unity-style button row: right-aligned, primary first
        float buttonsWidth = 240.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttonsWidth));

        ImGui::BeginDisabled(!canSave);
        if (EditorModal::button("Save", "ok", ImVec2(120, 0))) {
            if (m_onSave) {
                m_onSave(fullPath);
            }
            m_isOpen = false;
            m_cancelRequested = false;

            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel", ImVec2(120, 0)) || m_cancelRequested ||
            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            m_isOpen = false;
            m_cancelRequested = false;

            if (m_onCancel) {
                m_onCancel();
            }

            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    else {
        // If the popup isn't open anymore but our state says it should be,
        // update our state and make sure to call the cancel callback
        // This happens if user presses ESC or clicks outside the modal
        if (m_isOpen) {
            m_isOpen = false;
            m_cancelRequested = false;

            // If dialog was closed without explicit button press,
            // we still need to call the cancel callback
            if (m_onCancel) {
                m_onCancel();
            }
        }
    }
}

void SceneSaveDialog::displayDirectoryTree(const fs::path& currentPath) {
    try {
        std::vector<fs::path> subDirs;

        for (const auto& entry : fs::directory_iterator(currentPath)) {
            if (entry.is_directory()) {
                subDirs.push_back(entry.path());
            }
        }

        std::sort(subDirs.begin(), subDirs.end());

        for (const auto& dirPath : subDirs) {
            // Skip hidden directories (starting with ".")
            if (dirPath.filename().string()[0] == '.') {
                continue;
            }

            ImGui::PushID(dirPath.string().c_str());

            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;

            bool isSelected = (m_selectedPath == dirPath.string());
            if (isSelected) {
                nodeFlags |= ImGuiTreeNodeFlags_Selected;
            }

            // Count subdirectories to determine if we need to show it as a leaf node
            bool hasSubDirs = false;
            try {
                for (const auto& subEntry : fs::directory_iterator(dirPath)) {
                    if (subEntry.is_directory()) {
                        hasSubDirs = true;
                        break;
                    }
                }
            } catch (...) {
                // Ignore directory access errors
            }

            if (!hasSubDirs) {
                nodeFlags |= ImGuiTreeNodeFlags_Leaf;
            }

            std::string displayName = dirPath.filename().string();

            bool nodeOpen = ImGui::TreeNodeEx("##dir", nodeFlags);

            ImGui::SameLine(0, 0); // Position right after the tree connector
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                               nodeOpen ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER);

            ImGui::SameLine();
            ImGui::Text("%s", displayName.c_str());

            // Handle directory selection on click (include the whole row)
            if (ImGui::IsItemClicked() || 
                (ImGui::IsMouseClicked(0) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))) {
                m_selectedPath = dirPath.string();
            }

            if (nodeOpen) {
                if (hasSubDirs) {
                    displayDirectoryTree(dirPath);
                }
                ImGui::TreePop();
            }

            ImGui::PopID();
        }
    } catch (const std::exception& e) {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: %s", e.what());
    }
}

} // namespace editor
} // namespace jasperengine