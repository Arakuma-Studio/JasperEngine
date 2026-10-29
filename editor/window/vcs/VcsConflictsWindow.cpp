// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "window/vcs/VcsConflictsWindow.h"

#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "io/AtomicFile.h"
#include "vcs/VcsEditorOps.h"
#include "vcs/VcsService.h"
#include "window/vcs/VcsOverlay.h"
#include "window/vcs/VcsUi.h"

#include "imgui.h"

#include <algorithm>

namespace jasperengine::editor::vcs {

namespace fs = std::filesystem;

namespace {

    bool isDocument(const RepoPath& key) {
        const fs::path extension = pathFromUtf8(key).extension();
        return extension == ".scene" || extension == ".prefab";
    }

}

VcsConflictsWindow::VcsConflictsWindow(VcsUi* ui): ui(ui) {
}

void VcsConflictsWindow::open() {
    windowOpen = true;
    ImGui::SetWindowFocus(VcsUi::CONFLICTS_WINDOW);
}

void VcsConflictsWindow::startSmartMerge(const RepoPath& key) {
    VcsService& service = ui->getProject()->getVcs();
    selected = key;
    merging = true;
    merged = false;
    error.clear();
    status.clear();
    resolutions.clear();
    conflicts.clear();
    smartMerge(service, key, {}, [this, key](const SmartMergeOutcome& outcome) {
        if (selected != key) return;
        merging = false;
        if (!outcome.ok) {
            error = outcome.error;
            return;
        }
        sides = outcome.sides;
        result = outcome.merge;
        conflicts = result.conflicts;
        merged = true;
    });
}

void VcsConflictsWindow::remerge() {
    // the sides are here already: the merge is pure and quick
    result = scene::mergeSceneTexts(sides.hasBase ? sides.base : std::string(), sides.ours, sides.theirs, resolutions);
    if (!result.ok) error = result.error;
}

void VcsConflictsWindow::markResolved() {
    Project* project = ui->getProject();
    VcsService& service = project->getVcs();
    const fs::path file = service.toAbsolute(selected);
    const io::WriteResult written = io::writeFileAtomic(file, result.text);
    if (!written.ok()) {
        error = "Could not write " + file.filename().string() + ": " + io::describeWriteError(written);
        return;
    }
    const RepoPath key = selected;
    service.run("Marking " + key + " resolved...", [key](IVcsProvider& provider, const jobs::CancelToken&) {
        return provider.markResolved(std::vector<RepoPath>{key});
    }, [this, key](const VcsResult& done) {
        if (!done.ok) {
            error = "Mark Resolved failed: " + done.message;
            return;
        }
        Out::info("Resolved %s", key.c_str());
        if (selected == key) {
            selected.clear();
            merged = false;
            status = key + " resolved";
        }
    });
}

void VcsConflictsWindow::show() {
    if (!windowOpen) return;
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(720.0f, 460.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(VcsUi::CONFLICTS_WINDOW, &windowOpen)) {
        ImGui::End();
        return;
    }
    Project* project = ui->getProject();
    VcsService& service = project->getVcs();
    if (!service.active() || !service.snapshot()) {
        ImGui::TextWrapped("%s", service.stateMessage().empty() ? "Version control: not available" : service.stateMessage().c_str());
        ImGui::End();
        return;
    }
    std::vector<RepoPath> conflicted;
    for (const VcsChange& change : service.snapshot()->entries) {
        if (change.conflict) conflicted.push_back(change.path);
    }
    if (conflicted.empty()) {
        ImGui::TextDisabled("No conflicts.");
        if (!status.empty()) ImGui::TextUnformatted(status.c_str());
        ImGui::End();
        return;
    }
    ImGui::BeginChild("##conflicted", ImVec2(Theme::dpi(240.0f), 0), true);
    for (const RepoPath& key : conflicted) {
        if (ImGui::Selectable((ICON_FA_TRIANGLE_EXCLAMATION "  " + key).c_str(), key == selected)) {
            selected = key;
            merged = false;
            error.clear();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##conflict_detail");
    if (selected.empty() || std::find(conflicted.begin(), conflicted.end(), selected) == conflicted.end()) {
        ImGui::TextDisabled("Choose a file");
    } else if (isDocument(selected)) {
        ImGui::TextUnformatted(selected.c_str());
        ImGui::BeginDisabled(merging || service.busy());
        if (ImGui::Button(ICON_FA_CODE_MERGE "  Smart Merge")) startSmartMerge(selected);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Merge the two versions GameObject by GameObject, property by property; what both changed is left for you");
        if (merging) ImGui::TextDisabled("Merging...");
        if (merged) {
            ImGui::Text("%zu conflict(s) in the merge; %zu GameObject(s) of theirs renumbered", conflicts.size(), result.renumbered.size());
            if (ImGui::BeginTable("##merge_conflicts", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.0f))) {
                ImGui::TableSetupColumn("GameObject");
                ImGui::TableSetupColumn("Property");
                ImGui::TableSetupColumn("Ours");
                ImGui::TableSetupColumn("Theirs");
                ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(130.0f));
                ImGui::TableHeadersRow();
                bool changed = false;
                for (const scene::MergeConflict& conflict : conflicts) {
                    ImGui::PushID(conflict.key().c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(conflict.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(conflict.path.empty() ? "(the GameObject)" : conflict.path.c_str());
                    ImGui::SetItemTooltip("%s\nBase: %s", conflict.reason.c_str(), conflict.base.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(conflict.ours.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(conflict.theirs.c_str());
                    ImGui::TableNextColumn();
                    auto it = resolutions.find(conflict.key());
                    int side = it != resolutions.end() && it->second == scene::MergeSide::Theirs ? 1 : 0;
                    bool picked = ImGui::RadioButton("Ours", &side, 0);
                    ImGui::SameLine();
                    picked = ImGui::RadioButton("Theirs", &side, 1) || picked;
                    if (picked) {
                        resolutions[conflict.key()] = side == 1 ? scene::MergeSide::Theirs : scene::MergeSide::Ours;
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
                if (changed) remerge();
            }
            ImGui::BeginDisabled(!result.ok || service.busy());
            if (ImGui::Button(ICON_FA_CHECK "  Mark Resolved")) markResolved();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Write the merged file (Ours where nothing was chosen) and mark it resolved");
        }
    } else {
        ImGui::TextUnformatted(selected.c_str());
        const RepoPath key = selected;
        ImGui::BeginDisabled(service.busy());
        for (const bool ours : {true, false}) {
            if (!ours) ImGui::SameLine();
            if (ImGui::Button(ours ? "Use Ours" : "Use Theirs")) {
                service.run(ours ? "Using ours..." : "Using theirs...", [key, ours](IVcsProvider& provider, const jobs::CancelToken&) {
                    return provider.resolveWith(key, ours);
                }, [this](const VcsResult& done) {
                    if (!done.ok) error = done.message;
                });
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Open in External Tool")) {
            std::string why;
            if (!Platform::openWithSystemDefault(service.toAbsolute(key).string(), &why)) error = why;
        }
    }
    if (!error.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Deleted)), "%s", error.c_str());
    ImGui::EndChild();
    ImGui::End();
}

}
