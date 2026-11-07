// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectSaveDialog.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/ShellEnv.h"
#include "Backend.h"
#include "window/Widgets.h"
#include "external/IconsFontAwesome6.h"
#include "window/EditorModal.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <utility>

namespace jasperengine {
namespace editor {

void ProjectSaveDialog::finishSave(const std::string& projectName, const fs::path& projectPath) {
    m_isOpen = false;
    m_cancelRequested = false;
    auto onSave = std::exchange(m_onSave, nullptr);
    m_onCancel = nullptr;
    if (onSave) onSave(projectName, projectPath);
}

void ProjectSaveDialog::finishCancel() {
    m_isOpen = false;
    m_cancelRequested = false;
    auto onCancel = std::exchange(m_onCancel, nullptr);
    m_onSave = nullptr;
    if (onCancel) onCancel();
}

void ProjectSaveDialog::close() {
    if (m_isOpen) {
        finishCancel();
    }
}

void ProjectSaveDialog::abandon() {
    m_isOpen = false;
    m_cancelRequested = false;
    m_onSave = nullptr;
    m_onCancel = nullptr;
}

void ProjectSaveDialog::open(const std::string& defaultName,
        std::function<void(const std::string&, const fs::path&)> onSave,
        std::function<void()> onCancel) {
    m_isOpen = true;
    m_projectPath.clear();
    m_onSave = std::move(onSave);
    m_onCancel = std::move(onCancel);
    m_cancelRequested = false;

    strncpy(m_projectNameBuffer, defaultName.c_str(), sizeof(m_projectNameBuffer) - 1);
    m_projectNameBuffer[sizeof(m_projectNameBuffer) - 1] = '\0';
}

void ProjectSaveDialog::show() {
    if (!m_isOpen) {
        return;
    }

    ImGui::OpenPopup("Save Project##SaveProjectModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal;

    bool popupOpen = EditorModal::begin("Save Project##SaveProjectModal", nullptr, flags);

    float popupWidth = 300;

    if (popupOpen) {
        EditorModal::message("Where to save the project: name and location (an empty folder), then ok; or cancel it and save with an explicit path.");
        ImGui::TextUnformatted("Project Name");
        ImGui::SetNextItemWidth(popupWidth);
        EditorModal::inputText("##projectName", "name", m_projectNameBuffer, sizeof(m_projectNameBuffer));

        ImGui::TextUnformatted("Location");

        Vector2 pathDisplaySize = Vector2(popupWidth - ImGui::CalcTextSize("Browse").x - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().FramePadding.x * 2, ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2);
        std::string agentLocation = FileUtils::pathToUtf8(m_projectPath);
        if (EditorModal::pathField("location", agentLocation)) m_projectPath = FileUtils::pathFromUtf8(agentLocation);
        Widgets::pathDisplay("##ProjectPath", m_projectPath, pathDisplaySize);

        ImGui::SameLine();
        if (ImGui::Button("Browse")) {
            // the user's home folder (none when the system has none to give)
            const std::string homeDirPath = FileUtils::pathToUtf8(ShellEnv::homeDirectory());

            std::string selectedPath = FileDialogs::openFileDialog(homeDirPath, 0, true);
            
            if (!selectedPath.empty()) {
                m_projectPath = selectedPath;
            }
        }

        std::string projectName = m_projectNameBuffer;
        bool canSave = !projectName.empty() && !m_projectPath.empty();
        EditorModal::error(canSave ? std::string() : std::string("The name and the location are needed."));
        
        // Check if the selected directory is empty if it exists
        if (!m_projectPath.empty() && std::filesystem::exists(m_projectPath)) {
            if (!std::filesystem::is_empty(m_projectPath)) {
                canSave = false;
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                                  "Choose an empty folder for the project.");
                EditorModal::error("Choose an empty folder for the project.");
            }
        }

        ImGui::Separator();

        // Unity-style button row: right-aligned, primary first
        float buttonsWidth = 240.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttonsWidth));

        ImGui::BeginDisabled(!canSave);
        bool finished = false;
        if (EditorModal::button("Save", "ok", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            finishSave(projectName, m_projectPath);
            finished = true;
        }
        ImGui::EndDisabled();

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
        if (m_isOpen) {
            // closed without a button (Escape, a click outside): that is Cancel
            finishCancel();
        }
    }
}

} // namespace editor
} // namespace jasperengine