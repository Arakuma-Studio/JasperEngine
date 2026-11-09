// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationStatisticsWindow.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "animation/AnimationClipDocument.h"
#include "animation/ModelSubClips.h"
#include "component/AnimationPlayerComponent.h"
#include "pool/AnimationClipPool.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "window/ProjectWindow.h"

#include "external/IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    bool containsInsensitive(const std::string& text, const char* pattern) {
        if (!pattern || !pattern[0]) return true;
        const std::string needle(pattern);
        return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b) {
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        }) != text.end();
    }

    // a value the tests can read where it is drawn (find_ui_item)
    void cell(const std::string& text, const std::string& id) {
        ImGui::TableNextColumn();
        ImGui::Selectable((text + "##" + id).c_str(), false);
    }

    const char* const kFilters[] = {"All", "Loaded", "Unused", "Over budget"};

}

AnimationStatisticsWindow::~AnimationStatisticsWindow() {
    if (scan) scan->cancel = true;
}

void AnimationStatisticsWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

void AnimationStatisticsWindow::startScan() {
    if (scan) scan->cancel = true;
    auto next = std::make_shared<Scan>();
    scan = next;
    const fs::path root = project->getProjectPath();
    // the open scenes' clips, read here: scenes belong to the main thread
    const bool openScenes = scope == 1;
    std::set<std::string> used;
    if (openScenes) {
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            for (Entity entity : sceneProject->entities) {
                const AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
                if (!player) continue;
                for (const AnimationPlayerClip& entry : player->clips) used.insert(entry.clip);
            }
        }
    }
    const fs::path assets = project->getAssetsPath();
    std::thread([next, root, assets, openScenes, used]() {
        std::vector<anim::ProjectClip> clips = anim::listProjectClips(root, next->notes, &next->cancel);
        if (openScenes) {
            const auto isUsed = [&](const anim::ProjectClip& clip) {
                anim::SubClipReference parts;
                const bool subClip = anim::parseSubClipReference(clip.reference, parts);
                const fs::path file = FileUtils::pathFromUtf8(subClip ? parts.model : clip.reference);
                std::error_code ec;
                const std::string relative = FileUtils::pathToGenericUtf8(fs::relative(file, assets, ec));
                return used.count(subClip ? relative + "#" + parts.clip : relative) > 0;
            };
            clips.erase(std::remove_if(clips.begin(), clips.end(), [&](const anim::ProjectClip& clip) { return !isUsed(clip); }), clips.end());
        }
        next->count = clips.size();
        for (const anim::ProjectClip& clip : clips) {
            if (next->cancel.load()) break;
            next->rows.push_back(anim::readClipStatRow(clip));
            next->finished++;
        }
        next->done = true;
    }).detach();
}

// Users and Loaded are the open scenes' and the pool's now: the main thread's part
void AnimationStatisticsWindow::finishScan() {
    rows = std::move(scan->rows);
    notes = std::move(scan->notes);
    scan.reset();
    for (anim::ClipStatRow& row : rows) {
        row.users = findClipUsers(project, row.clip.display).size();
        row.loaded = AnimationClipPool::get(row.clip.reference) != nullptr;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const anim::ClipStatRow& a, const anim::ClipStatRow& b) {
        return a.report.runtimeBytes > b.report.runtimeBytes;
    });
}

void AnimationStatisticsWindow::drawTable(uint64_t budgetBytes) {
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("animation_statistics", 9, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Clip");
    ImGui::TableSetupColumn("Source");
    ImGui::TableSetupColumn(anim::findClipReportField("length_s")->label);
    ImGui::TableSetupColumn(anim::findClipReportField("tracks")->label);
    ImGui::TableSetupColumn(anim::findClipReportField("keys")->label);
    ImGui::TableSetupColumn(anim::findClipReportField("runtime_bytes")->label);
    ImGui::TableSetupColumn(anim::findClipReportField("max_error_mm")->label);
    ImGui::TableSetupColumn("Users");
    ImGui::TableSetupColumn("Loaded");
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < rows.size(); i++) {
        const anim::ClipStatRow& row = rows[i];
        if (!anim::clipStatRowShown(row, (anim::ClipStatFilter)filter, budgetBytes)) continue;
        if (!containsInsensitive(row.clip.display, search)) continue;
        const std::string id = std::to_string(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool clicked = ImGui::Selectable((row.name + "##stat_clip_" + row.clip.display).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        if (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            anim::SubClipReference parts;
            const std::string file = anim::parseSubClipReference(row.clip.display, parts) ? parts.model : row.clip.display;
            Backend::getApp().getProjectWindow()->revealAsset(project->getProjectPath() / FileUtils::pathFromUtf8(file));
        }
        ImGui::SetItemTooltip("%s\nDouble-click selects its file in the Project window", row.clip.display.c_str());
        cell(row.source, "stat_source_" + id);
        if (!row.report.ok) {
            ImGui::TableNextColumn();
            ImGui::TextColored(Theme::Colors::ErrorText, "%s", row.report.error.c_str());
            continue;
        }
        cell(anim::formatClipField("length_s", row.report.lengthSeconds), "stat_length_" + row.clip.display);
        cell(anim::formatClipField("tracks", (double)row.report.tracks), "stat_tracks_" + row.clip.display);
        cell(anim::formatClipField("keys", (double)row.report.keys), "stat_keys_" + row.clip.display);
        cell(anim::formatClipField("runtime_bytes", (double)row.report.runtimeBytes), "stat_memory_" + row.clip.display);
        cell(row.maxErrorMm ? anim::formatClipField("max_error_mm", *row.maxErrorMm) : std::string("-"), "stat_error_" + row.clip.display);
        cell(std::to_string(row.users), "stat_users_" + row.clip.display);
        cell(row.loaded ? "yes" : "no", "stat_loaded_" + row.clip.display);
    }
    ImGui::EndTable();
}

void AnimationStatisticsWindow::show() {
    windowFocused = false;
    if (!windowOpen) return;
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    // nine columns: room for them all the first time it opens (a narrow window clips the right ones)
    ImGui::SetNextWindowSize(ImVec2(std::min(Theme::dpi(980.0f), ImGui::GetMainViewport()->WorkSize.x * 0.95f), Theme::dpi(440.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (scan && scan->done.load()) finishScan();

    ImGui::BeginDisabled(scan != nullptr);
    if (ImGui::Button(scope == 0 ? ICON_FA_MAGNIFYING_GLASS " Scan project##stat_scan" : ICON_FA_MAGNIFYING_GLASS " Scan open scenes##stat_scan")) startScan();
    ImGui::EndDisabled();
    UIProbe::item("animationStatistics.scan");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(130.0f));
    const char* const scopes[] = {"Project", "Open scenes"};
    ImGui::Combo("Scope##stat_scope", &scope, scopes, 2);
    if (scan) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu of %zu clips...", scan->finished.load(), scan->count.load());
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##stat_cancel")) {
            scan->cancel = true;
        }
    }

    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    ImGui::InputTextWithHint("##stat_search", "Search", search, sizeof(search));
    ImGui::SameLine();
    ImGui::TextUnformatted("Show");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(120.0f));
    ImGui::Combo("##stat_filter", &filter, kFilters, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    ImGui::DragFloat("Budget per clip##stat_budget", &budgetKB, 1.0f, 1.0f, 1024.0f * 1024.0f, "%.0f KB", ImGuiSliderFlags_AlwaysClamp);
    const uint64_t budgetBytes = (uint64_t)(budgetKB * 1024.0f);

    const anim::ClipStatTotals totals = anim::clipStatTotals(rows);
    const std::string line = "Totals: " + anim::formatClipField("tracks", (double)totals.clips) + (totals.clips == 1 ? " clip" : " clips") +
                             "   Memory " + anim::formatClipField("runtime_bytes", (double)totals.memoryBytes) +
                             "   Loaded now " + anim::formatClipField("runtime_bytes", (double)totals.loadedBytes) +
                             "   Unused " + anim::formatClipField("tracks", (double)totals.unusedClips) + (totals.unusedClips == 1 ? " clip (" : " clips (") +
                             anim::formatClipField("runtime_bytes", (double)totals.unusedBytes) + ")";
    ImGui::Selectable((line + "##stat_totals").c_str(), false);
    if (filter == (int)anim::ClipStatFilter::Unused) {
        ImGui::TextColored(Theme::Colors::WarningText, "Unused means no Animation Player in an open scene lists it. Scripts can name clips: check before you delete one.");
    }
    for (const std::string& note : notes) ImGui::TextColored(Theme::Colors::SubtleText, "%s", note.c_str());
    if (rows.empty() && !scan) {
        ImGui::TextDisabled("Scan to list what every clip costs.");
    } else {
        drawTable(budgetBytes);
    }
    ImGui::End();
}
