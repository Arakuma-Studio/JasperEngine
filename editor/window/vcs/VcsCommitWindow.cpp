// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "window/vcs/VcsCommitWindow.h"

#include "AppSettings.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "vcs/VcsService.h"
#include "window/vcs/VcsOverlay.h"
#include "window/vcs/VcsUi.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace jasperengine::editor::vcs {

namespace fs = std::filesystem;

namespace {

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string stateText(const VcsChange& change) {
        if (change.conflict) return "Conflicted";
        if (change.newFile()) return change.index == FileState::Added || change.worktree == FileState::Added ? "Added" : "Untracked";
        if (change.deleted()) return "Deleted";
        if (change.index == FileState::Renamed || !change.oldPath.empty()) return "Renamed";
        return "Modified";
    }

    bool isDocument(const RepoPath& key) {
        const std::string_view path = key;
        return path.size() > 6 && (path.substr(path.size() - 6) == ".scene" || (path.size() > 7 && path.substr(path.size() - 7) == ".prefab"));
    }

}

VcsCommitWindow::VcsCommitWindow(VcsUi* ui): ui(ui) {
}

void VcsCommitWindow::open(const std::vector<fs::path>& preselect) {
    windowOpen = true;
    ImGui::SetWindowFocus(VcsUi::COMMIT_WINDOW);
    preselected.clear();
    unchecked.clear();
    const VcsService& service = ui->getProject()->getVcs();
    for (const fs::path& file : preselect) {
        RepoPath key;
        if (service.toKey(file, key)) preselected.insert(key);
    }
    rowsVersion = ~uint64_t(0);
}

void VcsCommitWindow::rebuildRows() {
    const VcsService& service = ui->getProject()->getVcs();
    rows.clear();
    const SnapshotPtr& snapshot = service.snapshot();
    if (!snapshot) return;
    const auto [begin, end] = snapshot->range(service.projectKey());
    for (const VcsChange* change = begin; change != end; ++change) {
        if (!change->changed()) continue;
        // a .meta whose asset changed too is part of the asset's row (R8)
        const std::string_view asset = metaAssetOf(change->path);
        if (!asset.empty()) {
            const VcsChange* owner = snapshot->find(asset);
            if (owner && owner->changed()) continue;
        }
        Row row;
        row.key = change->path;
        row.state = stateText(*change);
        row.document = isDocument(change->path);
        const VcsChange* meta = snapshot->find(metaOf(change->path));
        if (meta && meta->changed()) row.meta = meta->path;
        rows.push_back(std::move(row));
    }
    // open(preselect): what it names is checked, the rest left out
    if (!preselected.empty()) {
        for (const Row& row : rows) {
            const bool chosen = std::any_of(preselected.begin(), preselected.end(),
                                            [&row](const RepoPath& key) { return pathKeyIsWithin(row.key, key); });
            if (!chosen) unchecked.insert(row.key);
        }
        preselected.clear();
    }
    rowsVersion = service.snapshotVersion();
}

void VcsCommitWindow::show() {
    if (!windowOpen) return;
    Project* project = ui->getProject();
    VcsService& service = project->getVcs();
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(620.0f, 520.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(VcsUi::COMMIT_WINDOW, &windowOpen)) {
        ImGui::End();
        return;
    }
    if (!service.active()) {
        ImGui::TextWrapped("%s", service.stateMessage().empty() ? "Version control: not available" : service.stateMessage().c_str());
        ImGui::End();
        return;
    }
    if (rowsVersion != service.snapshotVersion()) rebuildRows();

    // ---- the change list
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##commit_filter", ICON_FA_MAGNIFYING_GLASS "  Filter", filter, sizeof(filter));
    const std::string needle = lower(filter);
    const float listHeight = std::max(ImGui::GetContentRegionAvail().y - Theme::dpi(190.0f), Theme::dpi(120.0f));
    size_t checkedCount = 0;
    std::vector<uint32_t> dirtyDocuments;
    if (ImGui::BeginTable("##commit_changes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                          ImVec2(0, listHeight))) {
        ImGui::TableSetupColumn("##check", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(90.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const Row& row : rows) {
            const bool checked = !unchecked.count(row.key);
            if (checked) checkedCount++;
            if (!needle.empty() && lower(row.key).find(needle) == std::string::npos) continue;
            ImGui::PushID(row.key.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool value = checked;
            if (ImGui::Checkbox("##checked", &value)) {
                if (value) unchecked.erase(row.key);
                else unchecked.insert(row.key);
            }
            ImGui::TableNextColumn();
            const VcsChange* change = service.findKey(row.key);
            if (change) {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                drawBadges(ImGui::GetWindowDrawList(), pos, ImGui::GetFontSize() * 0.85f, badgeOf(*change), change->lock);
                ImGui::Dummy(ImVec2(badgeWidth(ImGui::GetFontSize() * 0.85f) * 2.0f, ImGui::GetFontSize()));
                ImGui::SameLine();
            }
            const std::string label = row.key + (row.meta.empty() ? std::string() : "  (+ .meta)");
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ui->run(VcsAction::DiffAgainstBaseline, {service.toAbsolute(row.key)});
            }
            if (change) ImGui::SetItemTooltip("%s\nDouble-click: %s", tooltipOf(*change).c_str(), row.document ? "Scene Diff" : "text diff");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.state.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::SmallButton("All")) unchecked.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) {
        for (const Row& row : rows) unchecked.insert(row.key);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu selected", checkedCount, rows.size());

    // ---- the message
    ImGui::Spacing();
    ImGui::TextUnformatted("Message");
    ImGui::InputTextMultiline("##commit_message", message, sizeof(message), ImVec2(-FLT_MIN, Theme::dpi(70.0f)));
    const std::string text = message;
    const size_t firstLine = std::min(text.find('\n'), text.size());
    if (firstLine > 72) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Modified)), "The first line has %zu characters: keep it within 72", firstLine);
    } else {
        ImGui::TextDisabled("A first line of at most 72 characters, then a blank line and the details");
    }
    if (service.caps().push) {
        VersionControlSettings settings = AppSettings::getVersionControlSettings();
        bool push = settings.commitAndPush;
        if (ImGui::Checkbox("Commit and Push", &push)) {
            settings.commitAndPush = push;
            AppSettings::setVersionControlSettings(settings);
        }
    }

    // ---- commit
    for (const Row& row : rows) {
        if (unchecked.count(row.key) || !row.document) continue;
        const fs::path file = service.toAbsolute(row.key);
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            fs::path document = sceneProject->filepath;
            if (const PrefabStage* stage = project->getPrefabStage(sceneProject->id)) document = stage->assetPath;
            if (document.empty()) continue;
            if (document.is_relative()) document = project->getProjectPath() / document;
            if (sceneProject->isModified && document.lexically_normal() == file.lexically_normal()) dirtyDocuments.push_back(sceneProject->id);
        }
    }
    const bool blank = text.find_first_not_of(" \t\r\n") == std::string::npos;
    std::string why;
    if (blank) why = "Write a message first";
    else if (checkedCount == 0) why = "Select the files to commit";
    else if (service.busy()) why = "Busy: " + service.busyLabel();
    ImGui::BeginDisabled(!why.empty());
    if (ImGui::Button(ICON_FA_CODE_COMMIT "  Commit")) {
        if (dirtyDocuments.empty()) {
            commit(false);
        } else {
            ui->ask("Save and Commit", "Some selected scenes have unsaved changes: they are saved before the commit.",
                    {{"Save and Commit", [this]() { commit(true); }}, {"Cancel", nullptr}});
        }
    }
    ImGui::EndDisabled();
    if (!why.empty()) ImGui::SetItemTooltip("%s", why.c_str());
    if (service.busy()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", service.busyLabel().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) service.cancelRunning();
    }
    if (!output.empty()) {
        ImGui::Separator();
        if (outputError) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Deleted)));
        ImGui::TextWrapped("%s", output.c_str());
        if (outputError) ImGui::PopStyleColor();
    }
    ImGui::End();
}

void VcsCommitWindow::commit(bool saveFirst) {
    Project* project = ui->getProject();
    VcsService& service = project->getVcs();
    std::vector<RepoPath> keys;
    for (const Row& row : rows) {
        if (unchecked.count(row.key)) continue;
        keys.push_back(row.key);
        if (!row.meta.empty()) keys.push_back(row.meta);
    }
    if (saveFirst) {
        for (const Row& row : rows) {
            if (unchecked.count(row.key) || !row.document) continue;
            const fs::path file = service.toAbsolute(row.key);
            for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
                fs::path document = sceneProject->filepath;
                const bool stage = project->getPrefabStage(sceneProject->id) != nullptr;
                if (stage) document = project->getPrefabStage(sceneProject->id)->assetPath;
                if (document.empty() || !sceneProject->isModified) continue;
                if (document.is_relative()) document = project->getProjectPath() / document;
                if (document.lexically_normal() != file.lexically_normal()) continue;
                std::string error;
                const bool saved = stage ? project->savePrefabStage(sceneProject->id) : project->saveSceneNow(sceneProject->id, {}, &error);
                if (!saved) {
                    output = "Not committed: " + sceneProject->name + " could not be saved" + (error.empty() ? std::string() : ": " + error);
                    outputError = true;
                    return;
                }
            }
        }
    }
    const bool push = service.caps().push && AppSettings::getVersionControlSettings().commitAndPush;
    output = "Committing...";
    outputError = false;
    service.commit(keys, message, push, [this](const VcsResult& result, const std::string& revision) {
        outputError = !result.ok;
        if (result.ok) {
            output = "Committed " + revision + (result.message.empty() ? std::string() : "\n" + result.message);
            message[0] = '\0';
            Out::info("Committed %s", revision.c_str());
        } else {
            output = "Commit failed: " + result.message;
            Out::error("Commit failed: %s", result.message.c_str());
        }
    });
}

}
