// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The History panel (docs/specs/S82-title-designer.md R50-R52, D11): the project-history steps
// that touched the shown document, read from the one undo timeline and filtered by the step's
// touched path; jumping undoes or redoes with UndoTimeline::jumpTo, and warns when steps of
// other histories lie between (Restore This Version writes that version as one new step instead).
// Hovering a row previews that version as a transient state.

#include "TitleDesignerWindow.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "command/type/WriteFilesCmd.h"
#include "external/IconsFontAwesome6.h"

#include "imgui.h"

#include <chrono>
#include <ctime>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace fs = std::filesystem;

namespace {

    // the document text a step leaves (its WriteFilesCmd's content for the file), null when it
    // did not write it
    const std::string* stepText(const UndoEntry* entry, const fs::path& file) {
        if (!entry || !entry->cmd) return nullptr;
        if (std::string(entry->cmd->typeName()) != "WriteFilesCmd") return nullptr;
        return static_cast<const WriteFilesCmd*>(entry->cmd.get())->contentOf(file);
    }

}

void TitleDesignerWindow::drawHistory() {
    if (!doc.isOpen() || !project) {
        ImGui::TextDisabled("No Title Art open");
        return;
    }
    const fs::path file = doc.document()->absolutePath();
    const uint32_t scene = project->getSelectedSceneId();
    const std::vector<UndoTimeline::EntryView> all = UndoTimeline::entries(scene, false);
    struct Row {
        const UndoEntry* entry;
        bool applied;
        size_t timelineIndex;
    };
    std::vector<Row> rows;
    for (size_t i = 0; i < all.size(); i++) {
        if (!all[i].project) continue;
        if (stepText(all[i].entry, file)) rows.push_back({all[i].entry, all[i].applied, i});
    }
    ImGui::BeginChild("##historylist", ImVec2(0, 0), false);
    // "(Opened)": before the first step of this document
    {
        const bool atStart = rows.empty() || !rows.front().applied;
        if (ImGui::Selectable("(Opened)", atStart) && !rows.empty() && rows.front().applied) {
            // undo every step of this document: the timeline back to before the first
            historyJumpEntry = 0;
            historyInterleaved.clear();
            for (size_t i = rows.front().timelineIndex; i < all.size(); i++) {
                if (all[i].applied && all[i].entry && !stepText(all[i].entry, file)) historyInterleaved.push_back(all[i].entry->name());
            }
            if (historyInterleaved.empty()) {
                std::string why;
                const uint64_t before = rows.front().timelineIndex > 0 ? all[rows.front().timelineIndex - 1].entry->info.id : 0;
                if (!UndoTimeline::jumpTo(scene, before, &why)) reportError(why);
            } else {
                historyJumpEntry = ~0ull;
                ImGui::OpenPopup("##interleaved");
            }
        }
    }
    uint64_t hoveredNow = 0;
    for (const Row& row : rows) {
        ImGui::PushID((int)row.entry->info.id);
        const UndoOrigin& origin = row.entry->info.origin;
        const char* icon = origin.kind == UndoOriginKind::Agent ? ICON_FA_ROBOT : ICON_FA_USER;
        const std::time_t t = std::chrono::system_clock::to_time_t(row.entry->info.time);
        char time[16];
        std::strftime(time, sizeof(time), "%H:%M:%S", std::localtime(&t));
        std::string label = std::string(icon) + " " + row.entry->name();
        if (origin.kind == UndoOriginKind::Agent && !origin.client.empty()) label += "  (" + origin.client + ")";
        if (!row.applied) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const bool clicked = ImGui::Selectable((label + "##h").c_str(), false);
        if (!row.applied) ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        ImGui::TextDisabled("%s", time);
        if (ImGui::IsItemHovered() || ImGui::IsItemHovered(ImGuiHoveredFlags_RectOnly)) hoveredNow = row.entry->info.id;
        if (clicked) {
            // the steps of other histories between here and the newest (R51)
            historyInterleaved.clear();
            const bool undoing = row.applied;
            const size_t from = undoing ? row.timelineIndex + 1 : 0;
            const size_t to = undoing ? all.size() : row.timelineIndex;
            for (size_t i = from; i < to; i++) {
                const bool counts = undoing ? all[i].applied : !all[i].applied;
                if (counts && all[i].entry && !stepText(all[i].entry, file)) historyInterleaved.push_back(all[i].entry->name());
            }
            historyJumpEntry = row.entry->info.id;
            if (historyInterleaved.empty()) {
                std::string why;
                if (!UndoTimeline::jumpTo(scene, historyJumpEntry, &why)) reportError(why);
                historyJumpEntry = 0;
            } else {
                ImGui::OpenPopup("##interleaved");
            }
        }
        ImGui::PopID();
    }
    if (ImGui::BeginPopup("##interleaved")) {
        std::string list;
        for (size_t i = 0; i < historyInterleaved.size() && i < 5; i++) list += (i ? ", " : "") + historyInterleaved[i];
        ImGui::Text("Also undoes %zu other step%s: %s", historyInterleaved.size(), historyInterleaved.size() == 1 ? "" : "s", list.c_str());
        if (ImGui::Button("Jump Anyway")) {
            std::string why;
            const uint64_t target = historyJumpEntry == ~0ull ? (rows.front().timelineIndex > 0 ? all[rows.front().timelineIndex - 1].entry->info.id : 0)
                                                             : historyJumpEntry;
            if (!UndoTimeline::jumpTo(scene, target, &why)) reportError(why);
            historyJumpEntry = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Restore This Version") && historyJumpEntry && historyJumpEntry != ~0ull) {
            // that version's text as one new step, leaving everything else alone
            const UndoEntry* entry = nullptr;
            for (const Row& row : rows) {
                if (row.entry->info.id == historyJumpEntry) entry = row.entry;
            }
            if (const std::string* text = stepText(entry, file)) {
                std::string error;
                if (!doc.commitText(*text, "Restore Title Version '" + entry->name() + "'", &error)) reportError(error);
            }
            historyJumpEntry = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    // hovering a row previews that version (R52)
    if (hoveredNow != historyHover) {
        historyHover = hoveredNow;
        if (hoveredNow == 0) {
            if (doc.preview() && doc.preview()->kind == PreviewItem::Kind::HistoryVersion) doc.clearPreview();
        } else {
            for (const Row& row : rows) {
                if (row.entry->info.id != hoveredNow) continue;
                if (const std::string* text = stepText(row.entry, file)) {
                    PreviewItem item;
                    item.kind = PreviewItem::Kind::HistoryVersion;
                    item.id = std::to_string(hoveredNow);
                    item.label = row.entry->name();
                    item.text = *text;
                    item.historyEntry = hoveredNow;
                    item.stepName = "Restore Title Version '" + row.entry->name() + "'";
                    std::string error;
                    doc.setPreview(item, &error);
                }
            }
        }
    }
}
