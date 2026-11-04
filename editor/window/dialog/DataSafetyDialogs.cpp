// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DataSafetyDialogs.h"

#include "Backend.h"
#include "App.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "window/ConsoleWindow.h"

#include "imgui.h"
#include "window/EditorModal.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr const char* kRecoveryTitle = "Recover Unsaved Changes##DataSafety";
    constexpr const char* kDegradedTitle = "Saving Would Lose Data##DataSafety";
    constexpr const char* kLockedTitle = "Project Already Open##DataSafety";
    constexpr size_t kListedIssues = 20;

    bool beginModal(const char* title) {
        if (!ImGui::IsPopupOpen(title)) {
            ImGui::OpenPopup(title);
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        return EditorModal::begin(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    }

    float dataSafetyButtonWidth(const char* label) {
        return std::max(120.0f, ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    }

}

void DataSafetyDialogs::requestRecovery(const SceneBackupInfo& backup) {
    queue.emplace_back(backup);
}

void DataSafetyDialogs::requestDegradedSave(const DegradedSaveRequest& request) {
    queue.emplace_back(request);
}

void DataSafetyDialogs::requestLockedProject(const ProjectLockInfo& info) {
    queue.emplace_back(info);
}

bool DataSafetyDialogs::isShowing() const {
    return !queue.empty();
}

void DataSafetyDialogs::pop() {
    ImGui::CloseCurrentPopup();
    if (!queue.empty()) {
        queue.pop_front();
    }
    confirmingDiscard = false;
}

void DataSafetyDialogs::showFileInConsole(const std::string& file) {
    if (ConsoleWindow* console = Backend::getApp().getConsoleWindow()) {
        console->setSearchText(file);
        console->requestFocus();
    }
}

void DataSafetyDialogs::show(Project& current) {
    if (queue.empty()) {
        return;
    }
    project = &current;
    // copied: a button may queue the next request
    auto front = queue.front();
    if (auto* backup = std::get_if<SceneBackupInfo>(&front)) {
        showRecovery(*backup);
    } else if (auto* request = std::get_if<DegradedSaveRequest>(&front)) {
        showDegradedSave(*request);
    } else if (auto* info = std::get_if<ProjectLockInfo>(&front)) {
        showLockedProject(*info);
    }
}

void DataSafetyDialogs::showRecovery(const SceneBackupInfo& backup) {
    if (!beginModal(kRecoveryTitle)) {
        return;
    }
    const std::string name = backup.source.empty() ? backup.backup.filename().string() : backup.source.filename().string();
    if (!confirmingDiscard) {
        ImGui::Text("\"%s\" has changes from a session that did not close normally (auto-saved %s).",
                    name.c_str(), backup.savedAtClock().c_str());
        EditorModal::message("\"" + name + "\" has changes from a session that did not close normally (auto-saved " + backup.savedAtClock() +
                             "). Decide Later offers them again the next time the scene opens.");
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        if (EditorModal::button("Restore", "yes", ImVec2(dataSafetyButtonWidth("Restore"), 0))) {
            const SceneBackupInfo copy = backup;
            pop();
            // between frames: the scene reopens, its windows must not be drawing it
            Project* target = project;
            Backend::getApp().enqueueMainThreadTask([target, copy, name]() {
                if (copy.kind == BackupKind::Untitled) {
                    target->restoreUntitledBackup(copy);
                    return;
                }
                const uint32_t sceneId = target->findSceneByPath(target->getProjectPath() / copy.source);
                if (sceneId == NULL_PROJECT_SCENE || !target->restoreSceneFromBackup(sceneId)) {
                    Out::error("Could not restore \"%s\" from its auto-saved copy.", name.c_str());
                }
            });
            ImGui::EndPopup();
            return;
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (EditorModal::button("Discard", "no", ImVec2(dataSafetyButtonWidth("Discard"), 0))) {
            confirmingDiscard = true;
        }
        ImGui::SameLine();
        if (EditorModal::button("Decide Later", "cancel", ImVec2(dataSafetyButtonWidth("Decide Later"), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            // the backup stays and is offered again the next time the scene opens
            pop();
        }
    } else {
        ImGui::Text("The auto-saved changes will be deleted.");
        EditorModal::message("The auto-saved changes will be deleted.");
        ImGui::Spacing();
        ImGui::Separator();
        if (EditorModal::button("Cancel", "cancel", ImVec2(dataSafetyButtonWidth("Cancel"), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            confirmingDiscard = false;
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ErrorText);
        if (EditorModal::button("Delete", "yes", ImVec2(dataSafetyButtonWidth("Delete"), 0))) {
            SceneBackups::discard(backup.backup);
            Out::info("Discarded the auto-saved changes of \"%s\".", name.c_str());
            pop();
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndPopup();
}

void DataSafetyDialogs::showDegradedSave(const DegradedSaveRequest& request) {
    if (!beginModal(kDegradedTitle)) {
        return;
    }
    ImGui::Text("\"%s\" did not load completely. Saving it now permanently removes %zu value%s this\neditor could not read:",
                request.documentName.c_str(), request.dropped.size(), request.dropped.size() == 1 ? "" : "s");
    {
        std::string agentMessage = "\"" + request.documentName + "\" did not load completely. Saving it now permanently removes " +
                                   std::to_string(request.dropped.size()) + " value(s) this editor could not read:";
        for (size_t i = 0; i < request.dropped.size() && i < kListedIssues; i++) agentMessage += " " + request.dropped[i].describe() + ";";
        EditorModal::message(agentMessage);
    }
    ImGui::Spacing();
    for (size_t i = 0; i < request.dropped.size() && i < kListedIssues; i++) {
        ImGui::BulletText("%s", request.dropped[i].describe().c_str());
    }
    if (request.dropped.size() > kListedIssues) {
        ImGui::TextDisabled("and %zu more", request.dropped.size() - kListedIssues);
    }
    if (ImGui::SmallButton("Show All in Console")) {
        for (const LoadIssue& issue : request.dropped) {
            Out::error(issue.consoleLine());
        }
        showFileInConsole(request.documentName);
    }
    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    if (EditorModal::button("Cancel", "cancel", ImVec2(dataSafetyButtonWidth("Cancel"), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        pop();
        ImGui::EndPopup();
        return;
    }
    ImGui::SetItemDefaultFocus();
    if (request.saveAs) {
        ImGui::SameLine();
        if (EditorModal::button("Save As...", "no", ImVec2(dataSafetyButtonWidth("Save As..."), 0))) {
            const auto saveAs = request.saveAs;
            pop();
            saveAs();
            ImGui::EndPopup();
            return;
        }
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ErrorText);
    if (EditorModal::button("Save Anyway", "yes", ImVec2(dataSafetyButtonWidth("Save Anyway"), 0))) {
        const auto saveAnyway = request.saveAnyway;
        pop();
        if (saveAnyway) saveAnyway();
    }
    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

void DataSafetyDialogs::showLockedProject(const ProjectLockInfo& info) {
    if (!beginModal(kLockedTitle)) {
        return;
    }
    ImGui::Text("\"%s\" is already open in another JasperEngine editor (PID %lld on host \"%s\"%s%s).",
                info.projectName.c_str(), (long long)info.pid, info.host.c_str(),
                info.since.empty() ? "" : ", since ", info.since.c_str());
    ImGui::Text("Two editors on one project overwrite each other's changes.");
    EditorModal::message("\"" + info.projectName + "\" is already open in another JasperEngine editor (PID " + std::to_string((long long)info.pid) +
                         " on host \"" + info.host + "\"). Two editors on one project overwrite each other's changes.");
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    if (EditorModal::button("Cancel", "cancel", ImVec2(dataSafetyButtonWidth("Cancel"), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        pop();
        ImGui::EndPopup();
        return;
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (EditorModal::button("Open Anyway", "yes", ImVec2(dataSafetyButtonWidth("Open Anyway"), 0))) {
        const auto openAnyway = info.openAnyway;
        pop();
        if (openAnyway) openAnyway();
    }
    ImGui::EndPopup();
}
