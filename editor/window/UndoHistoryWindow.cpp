// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UndoHistoryWindow.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "external/IconsFontAwesome6.h"

#include "imgui.h"
#include "window/EditorModal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* rowIcon(const UndoTimeline::EntryView& view) {
        const EntryInfo& info = view.entry->info;
        if (info.origin.kind == UndoOriginKind::Agent) return ICON_FA_ROBOT;
        if (info.origin.kind == UndoOriginKind::PlayReview) return ICON_FA_PLAY;
        if (info.selectionOnly) return ICON_FA_ARROW_POINTER;
        if (view.project) return ICON_FA_FILE;
        return ICON_FA_CUBE;
    }

    // "12 s", "4 min", "2 h"
    std::string relativeTime(std::chrono::system_clock::time_point time) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - time).count();
        char text[32];
        if (seconds < 60) std::snprintf(text, sizeof(text), "%lld s", (long long)std::max<long long>(0, seconds));
        else if (seconds < 3600) std::snprintf(text, sizeof(text), "%lld min", (long long)(seconds / 60));
        else std::snprintf(text, sizeof(text), "%lld h", (long long)(seconds / 3600));
        return text;
    }

    std::string formatMegabytes(size_t bytes) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
        return text;
    }

    bool matches(const std::string& name, const std::string& filter) {
        if (filter.empty()) return true;
        auto it = std::search(name.begin(), name.end(), filter.begin(), filter.end(), [](char a, char b) {
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        });
        return it != name.end();
    }

    std::string historyLabel(Project* project, const UndoTimeline::EntryView& view) {
        if (view.project) return "Project";
        const SceneProject* scene = project->getScene(view.sceneId);
        return scene ? "Scene '" + scene->name + "'" : "Scene " + std::to_string(view.sceneId);
    }

}

UndoHistoryWindow::UndoHistoryWindow(Project* project): project(project) {
}

void UndoHistoryWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

void UndoHistoryWindow::show() {
    if (!windowOpen) return;

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }

    const uint32_t sceneId = project->getSelectedScene() ? project->getSelectedSceneId() : NULL_PROJECT_SCENE;
    refreshRows(sceneId);
    drawToolbar(sceneId);

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("##UndoHistoryRows", ImVec2(0, -footerHeight), ImGuiChildFlags_Borders)) {
        drawRows(sceneId);
        if (showOtherScenes) {
            drawOtherScenes(sceneId);
        }
    }
    ImGui::EndChild();
    drawFooter(sceneId);

    ImGui::End();
}

void UndoHistoryWindow::refreshRows(uint32_t sceneId) {
    const uint64_t version = CommandHistory::changeCount();
    if (version == rowsVersion && sceneId == rowsScene && showSelection == rowsSelection && rowsFilter == filter) {
        return;
    }
    rowsVersion = version;
    rowsScene = sceneId;
    rowsSelection = showSelection;
    rowsFilter = filter;
    rows = UndoTimeline::entries(sceneId, showSelection);
    if (!rowsFilter.empty()) {
        rows.erase(std::remove_if(rows.begin(), rows.end(), [this](const UndoTimeline::EntryView& view) {
            return !matches(view.entry->name(), rowsFilter);
        }), rows.end());
    }
}

void UndoHistoryWindow::drawToolbar(uint32_t sceneId) {
    ImGui::BeginDisabled(!UndoTimeline::canUndo(sceneId));
    if (ImGui::Button(ICON_FA_ARROW_LEFT "##UndoHistoryBack")) {
        std::string why;
        status = UndoTimeline::undo(sceneId, &why) ? std::string() : why;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", UndoTimeline::undoLabel(sceneId).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!UndoTimeline::canRedo(sceneId));
    if (ImGui::Button(ICON_FA_ARROW_RIGHT "##UndoHistoryForward")) {
        std::string why;
        status = UndoTimeline::redo(sceneId, &why) ? std::string() : why;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", UndoTimeline::redoLabel(sceneId).c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x * 0.4f));
    ImGui::InputTextWithHint("##UndoHistoryFilter", "Filter", filter, sizeof(filter));
    ImGui::SameLine();
    ImGui::Checkbox("Show Selection Changes", &showSelection);
    ImGui::SameLine();
    ImGui::Checkbox("Show other scenes", &showOtherScenes);
    if (!status.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", status.c_str());
    }
}

// the next step to undo or redo, when it refuses (S47 R33: a sprite saved from outside the editor)
std::string UndoHistoryWindow::refusalOf(const UndoTimeline::EntryView& view) const {
    if (std::string(view.entry->cmd->typeName()) != "SpriteEditCmd") return std::string();
    const uint32_t sceneId = project->getSelectedScene() ? project->getSelectedSceneId() : NULL_PROJECT_SCENE;
    const UndoTimeline::EntryView nextUndo = UndoTimeline::nextUndo(sceneId);
    if (view.applied) return nextUndo && nextUndo.entry->info.id == view.entry->info.id ? view.entry->cmd->undoRefusal() : std::string();
    const UndoTimeline::EntryView nextRedo = UndoTimeline::nextRedo(sceneId);
    return nextRedo && nextRedo.entry->info.id == view.entry->info.id ? view.entry->cmd->redoRefusal() : std::string();
}

void UndoHistoryWindow::drawTooltip(const UndoTimeline::EntryView& view) const {
    const EntryInfo& info = view.entry->info;
    const std::time_t time = std::chrono::system_clock::to_time_t(info.time);
    char clock[32] = {};
    if (const std::tm* local = std::localtime(&time)) {
        std::strftime(clock, sizeof(clock), "%H:%M:%S", local);
    }
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(view.entry->name().c_str());
    ImGui::Separator();
    ImGui::Text("Time: %s", clock);
    ImGui::Text("History: %s", historyLabel(project, view).c_str());
    std::string origin = undoOriginKindName(info.origin.kind);
    if (!info.origin.client.empty()) origin += " (" + info.origin.client + (info.origin.tool.empty() ? "" : ", " + info.origin.tool) + ")";
    ImGui::Text("Origin: %s", origin.c_str());
    ImGui::Text("Size: %s (estimate)", formatMegabytes(info.bytes).c_str());
    ImGui::Text("Type: %s", view.entry->cmd->typeName());
    if (const std::string refusal = refusalOf(view); !refusal.empty()) ImGui::TextWrapped("%s", refusal.c_str());
    if (!view.editable) ImGui::TextDisabled("Edit-time step: not while the scene plays");
    ImGui::EndTooltip();
}

void UndoHistoryWindow::drawRows(uint32_t sceneId) {
    UndoTimeline::EntryView top = UndoTimeline::nextUndo(sceneId);
    const uint64_t currentId = top ? top.entry->info.id : 0;
    CommandHistory* sceneHistory = UndoTimeline::sceneHistory(sceneId);
    const bool playing = sceneHistory && sceneHistory->isInPlaySession();

    // "(Start)": everything undone
    if (ImGui::Selectable("(Start)##UndoHistoryStart", currentId == 0)) {
        jump(sceneId, 0);
    }

    ImGuiListClipper clipper;
    clipper.Begin((int)rows.size());
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            const UndoTimeline::EntryView& view = rows[i];
            ImGui::PushID((int)i);
            // the play floor: rows below it are the edit-time steps
            if (playing && view.editable && i > 0 && !rows[i - 1].editable) {
                ImGui::SeparatorText("Play Mode");
            }
            ImGui::BeginDisabled(!view.editable);
            const bool dim = !view.applied || !refusalOf(view).empty();
            if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            const std::string label = std::string(rowIcon(view)) + "  " + view.entry->name();
            if (ImGui::Selectable(label.c_str(), view.entry->info.id == currentId, ImGuiSelectableFlags_AllowOverlap)) {
                jump(sceneId, view.entry->info.id);
            }
            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            const std::string when = relativeTime(view.entry->info.time);
            ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(when.c_str()).x));
            ImGui::TextDisabled("%s", when.c_str());
            if (dim) ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if (hovered) drawTooltip(view);
            ImGui::PopID();
        }
    }
}

void UndoHistoryWindow::drawOtherScenes(uint32_t sceneId) {
    ImGui::Spacing();
    ImGui::SeparatorText("Other scenes (read-only)");
    for (SceneProject* scene : project->getScenesAndPrefabStages()) {
        if (scene->id == sceneId) continue;
        CommandHistory* history = CommandHandle::find(scene->id);
        if (!history || history->entries().empty()) continue;
        if (ImGui::TreeNode((scene->name + "##UndoOther" + std::to_string(scene->id)).c_str())) {
            const std::vector<const UndoEntry*> entries = history->entries();
            for (size_t i = 0; i < entries.size(); i++) {
                if (!showSelection && entries[i]->info.selectionOnly) continue;
                if (i >= history->appliedCount()) ImGui::TextDisabled("%s", entries[i]->name().c_str());
                else ImGui::TextUnformatted(entries[i]->name().c_str());
            }
            ImGui::TreePop();
        }
    }
}

void UndoHistoryWindow::drawFooter(uint32_t sceneId) {
    const size_t limit = CommandHistory::memoryLimit;
    ImGui::Text("Memory: %s / %s", formatMegabytes(CommandHistory::totalBytes()).c_str(),
                limit == 0 ? "unlimited" : formatMegabytes(limit).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("An estimate of what every undo history holds");
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Clear History...").x - ImGui::GetStyle().FramePadding.x * 2));
    // the play floor is the history's: not while the scene plays
    CommandHistory* sceneHistory = UndoTimeline::sceneHistory(sceneId);
    ImGui::BeginDisabled(sceneHistory && sceneHistory->isInPlaySession());
    if (ImGui::Button("Clear History...")) {
        ImGui::OpenPopup("Clear Undo History");
    }
    ImGui::EndDisabled();
    if (EditorModal::begin("Clear Undo History", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditorModal::message("Clear the undo history of this scene and of the project's assets? This can't be undone.");
        ImGui::TextUnformatted("Clear the undo history of this scene and of the project's assets?\nThis can't be undone.");
        if (EditorModal::button("Clear", "yes")) {
            if (CommandHistory* history = UndoTimeline::sceneHistory(sceneId)) history->clear();
            if (CommandHistory* history = UndoTimeline::getProjectHistory()) history->clear();
            status.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void UndoHistoryWindow::jump(uint32_t sceneId, uint64_t entryId) {
    std::string why;
    status = UndoTimeline::jumpTo(sceneId, entryId, &why) ? std::string() : why;
}
