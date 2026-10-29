// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "window/vcs/VcsHistoryWindow.h"

#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "vcs/VcsService.h"
#include "window/vcs/VcsUi.h"

#include "imgui.h"

namespace jasperengine::editor::vcs {

namespace fs = std::filesystem;

VcsHistoryWindow::VcsHistoryWindow(VcsUi* ui): ui(ui) {
}

void VcsHistoryWindow::open(const fs::path& path) {
    windowOpen = true;
    ImGui::SetWindowFocus(VcsUi::HISTORY_WINDOW);
    if (path != file || !loaded) {
        file = path;
        entries.clear();
        exhausted = false;
        selected = -1;
        error.clear();
        generation++;
        load(false);
    }
}

void VcsHistoryWindow::load(bool more) {
    VcsService& service = ui->getProject()->getVcs();
    if (!service.active() || loading) return;
    std::optional<RepoPath> key;
    if (!file.empty()) {
        RepoPath path;
        if (!service.toKey(file, path)) {
            error = file.string() + " is not in the repository";
            return;
        }
        key = path;
    }
    loading = true;
    loaded = true;
    const uint64_t asked = generation;
    service.log(key, kPage, more ? static_cast<int>(entries.size()) : 0, [this, asked](const VcsResult& result, std::vector<VcsLogEntry> page) {
        if (asked != generation) return;
        loading = false;
        if (!result.ok) {
            error = result.message;
            return;
        }
        exhausted = page.size() < static_cast<size_t>(kPage);
        entries.insert(entries.end(), page.begin(), page.end());
    });
}

void VcsHistoryWindow::show() {
    if (!windowOpen) return;
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(720.0f, 420.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(VcsUi::HISTORY_WINDOW, &windowOpen)) {
        ImGui::End();
        return;
    }
    Project* project = ui->getProject();
    VcsService& service = project->getVcs();
    if (!service.active()) {
        ImGui::TextWrapped("%s", service.stateMessage().empty() ? "Version control: not available" : service.stateMessage().c_str());
        ImGui::End();
        return;
    }
    if (!loaded) load(false);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", file.empty() ? "The project's history" : ("History of " + file.filename().string()).c_str());
    if (!file.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Show the Project's")) open({});
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ROTATE " Refresh")) {
        entries.clear();
        exhausted = false;
        generation++;
        load(false);
    }
    if (!error.empty()) ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.0f), "%s", error.c_str());
    const bool isDocument = file.extension() == ".scene" || file.extension() == ".prefab";
    if (ImGui::BeginTable("##history", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupColumn("Revision", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(90.0f));
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(120.0f));
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(170.0f));
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < entries.size(); i++) {
            const VcsLogEntry& entry = entries[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string revision = entry.revision.size() > 10 ? entry.revision.substr(0, 10) : entry.revision;
            if (ImGui::Selectable(revision.c_str(), selected == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns)) selected = static_cast<int>(i);
            if (ImGui::BeginPopupContextItem("##revision_menu")) {
                if (!file.empty()) {
                    if (ImGui::MenuItem("Diff with Working Copy")) {
                        if (isDocument) {
                            uint32_t document = 0;
                            for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
                                fs::path path = sceneProject->filepath;
                                if (const PrefabStage* stage = project->getPrefabStage(sceneProject->id)) path = stage->assetPath;
                                if (path.empty()) continue;
                                if (path.is_relative()) path = project->getProjectPath() / path;
                                if (path.lexically_normal() == file.lexically_normal() && sceneProject->scene) document = sceneProject->id;
                            }
                            if (document != 0) ui->openSceneDiff(document, entry.revision);
                            else ui->openTextDiff(file, entry.revision);
                        } else {
                            ui->openTextDiff(file, entry.revision);
                        }
                    }
                    if (isDocument) ImGui::SetItemTooltip("The Scene Diff with this revision as the baseline (the scene open)");
                }
                if (ImGui::MenuItem("Copy Revision")) ImGui::SetClipboardText(entry.revision.c_str());
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.author.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.date.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.message.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (loading) {
        ImGui::TextDisabled("Reading the history...");
    } else if (!exhausted && !entries.empty()) {
        if (ImGui::Button("Load More")) load(true);
    } else {
        ImGui::TextDisabled("%zu revision(s)", entries.size());
    }
    ImGui::End();
}

}
