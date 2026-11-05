// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneSaveDialog.h"
#include "external/IconsFontAwesome6.h"
#include "Backend.h"
#include "Theme.h"
#include "util/UIProbe.h"
#include "window/EditorModal.h"
#include "util/FileUtils.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace jasperengine {
namespace editor {

namespace {

    // Names Windows refuses, whatever the extension
    bool isReservedWindowsName(std::string stem) {
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                         "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
        for (const char* name : reserved) {
            if (stem == name) return true;
        }
        return false;
    }

}

std::string SceneSaveDialog::withSceneExtension(const std::string& name) {
    if (name.empty()) return name;
    return fs::path(name).extension() == ".scene" ? name : name + ".scene";
}

std::string SceneSaveDialog::nameProblem(const fs::path& projectPath, const fs::path& folder, const std::string& name,
        const std::function<std::string(const fs::path&)>& pathProblem) {
    if (name.empty() || name == ".scene") {
        return "Type a name for the scene.";
    }
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        return "The name cannot contain / or \\: pick the folder in the tree above.";
    }
    if (name == "." || name == ".." || name.find(':') != std::string::npos) {
        return "The name cannot be a path: pick the folder in the tree above.";
    }
    for (unsigned char c : name) {
        if (c < 32 || std::strchr("<>\"|?*", c)) {
            return "The name cannot contain < > \" | ? * or control characters (Windows refuses them).";
        }
    }
    const std::string fileName = withSceneExtension(name);
    if (fileName.back() == '.' || fileName.back() == ' ' || name.back() == '.' || name.back() == ' ') {
        return "The name cannot end with a dot or a space (Windows refuses it).";
    }
    if (isReservedWindowsName(fs::path(fileName).stem().string()) || isReservedWindowsName(fs::path(fileName).stem().stem().string())) {
        return "\"" + fs::path(fileName).stem().string() + "\" is a name Windows reserves.";
    }
    const fs::path fullPath = (folder / fileName).lexically_normal();
    const fs::path relative = fullPath.lexically_relative(projectPath.lexically_normal());
    if (relative.empty() || *relative.begin() == "..") {
        return "Scenes are saved inside the project folder.";
    }
    return pathProblem ? pathProblem(fullPath) : std::string();
}

void SceneSaveDialog::finishSave(const fs::path& fullPath) {
    m_isOpen = false;
    m_cancelRequested = false;
    auto onSave = std::exchange(m_onSave, nullptr);
    m_onCancel = nullptr;
    m_pathProblem = nullptr;
    if (onSave) onSave(fullPath);
}

void SceneSaveDialog::finishCancel() {
    m_isOpen = false;
    m_cancelRequested = false;
    auto onCancel = std::exchange(m_onCancel, nullptr);
    m_onSave = nullptr;
    m_pathProblem = nullptr;
    if (onCancel) onCancel();
}

void SceneSaveDialog::close() {
    if (m_isOpen) {
        finishCancel();
    }
}

void SceneSaveDialog::abandon() {
    m_isOpen = false;
    m_cancelRequested = false;
    m_onSave = nullptr;
    m_onCancel = nullptr;
    m_pathProblem = nullptr;
}

void SceneSaveDialog::open(const fs::path& projectPath, const fs::path& initialDirectory, const std::string& defaultName,
        std::function<void(const fs::path&)> onSave,
        std::function<void()> onCancel,
        std::function<std::string(const fs::path&)> pathProblem) {
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
    m_onSave = std::move(onSave);
    m_onCancel = std::move(onCancel);
    m_pathProblem = std::move(pathProblem);
    m_cancelRequested = false;

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
        UIProbe::item("sceneSave.name");

        const fs::path saveDir = fs::relative(m_selectedPath, m_projectPath);
        const std::string saveIn = (saveDir.empty() || saveDir == ".") ? "Project" : "Project/" + saveDir.generic_string();
        ImGui::TextWrapped("Save in: %s", saveIn.c_str());

        const std::string typed = m_fileNameBuffer;
        const std::string fileName = withSceneExtension(typed);
        const fs::path fullPath = (fs::path(m_selectedPath) / fileName).lexically_normal();
        std::error_code folderError;
        std::string problem = nameProblem(m_projectPath, fs::path(m_selectedPath), typed, m_pathProblem);
        if (problem.empty() && !fs::is_directory(m_selectedPath, folderError)) {
            problem = "The folder " + m_selectedPath + " does not exist.";
        }
        const bool canSave = problem.empty();

        if (!problem.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
            ImGui::TextWrapped("%s", problem.c_str());
            ImGui::PopStyleColor();
            UIProbe::item("sceneSave.problem");
        } else {
            std::error_code ec;
            if (fs::exists(fullPath, ec)) {
                ImGui::TextColored(Theme::Colors::WarningText, "%s exists and will be replaced.", fileName.c_str());
            }
        }
        EditorModal::error(problem);

        ImGui::Separator();

        // Unity-style button row: right-aligned, primary first
        float buttonsWidth = 240.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttonsWidth));

        ImGui::BeginDisabled(!canSave);
        const bool savePressed = EditorModal::button("Save", "ok", ImVec2(120, 0));
        UIProbe::item("sceneSave.save");
        ImGui::EndDisabled();
        bool finished = false;
        if (savePressed && canSave) {
            ImGui::CloseCurrentPopup();
            finishSave(fullPath);
            finished = true;
        }

        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel", ImVec2(120, 0)) || m_cancelRequested ||
            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            if (!finished) {
                ImGui::CloseCurrentPopup();
                finishCancel();
            }
        }

        ImGui::EndPopup();
    }
    else {
        // If the popup isn't open anymore but our state says it should be,
        // update our state and make sure to call the cancel callback
        // This happens if user presses ESC or clicks outside the modal
        if (m_isOpen) {
            // closed without a button (Escape, a click outside): that is Cancel
            finishCancel();
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