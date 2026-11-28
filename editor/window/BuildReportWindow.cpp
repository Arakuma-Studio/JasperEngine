// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuildReportWindow.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "audio/AudioImportRegistry.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "cook/BuildReportHistory.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "window/ProjectWindow.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    const ImVec4 kSmallerColor(0.35f, 0.85f, 0.4f, 1.0f);
    const ImVec4 kLargerColor(1.0f, 0.6f, 0.2f, 1.0f);

    std::string platformTitle(const std::string& platform) {
        if (platform == "macos") return "macOS";
        if (platform == "ios") return "iOS";
        if (platform.empty()) return platform;
        std::string text = platform;
        text[0] = (char)std::toupper((unsigned char)text[0]);
        return text;
    }

    std::string deltaText(int64_t delta) {
        if (delta == 0) return "0";
        return (delta < 0 ? "-" : "+") + BuildReport::formatBytes((uint64_t)(delta < 0 ? -delta : delta));
    }

    void deltaCell(int64_t delta) {
        if (delta == 0) {
            ImGui::TextDisabled("0");
            return;
        }
        ImGui::TextColored(delta < 0 ? kSmallerColor : kLargerColor, "%s", deltaText(delta).c_str());
    }

    // the tail tells files apart: the head is the same long prefix on every row
    void pathCell(const std::string& path) {
        std::string shown = path;
        const float room = ImGui::GetContentRegionAvail().x;
        if (ImGui::CalcTextSize(shown.c_str()).x > room) {
            size_t cut = 0;
            while (cut < shown.size() && ImGui::CalcTextSize(("..." + shown.substr(cut)).c_str()).x > room) cut++;
            shown = "..." + shown.substr(cut);
        }
        ImGui::TextUnformatted(shown.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
    }

    std::string lowerCopy(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

    // "entity:12/MeshComponent.material" -> "GameObject 12 > MeshComponent.material", "project/scenes[3]" stays
    std::string fieldTitle(const std::string& field, const std::string& object) {
        if (field.rfind("entity:", 0) != 0) return field;
        const size_t slash = field.find('/');
        const std::string rest = slash == std::string::npos ? std::string() : field.substr(slash + 1);
        return (object.empty() ? "GameObject " + field.substr(7, slash == std::string::npos ? std::string::npos : slash - 7) : object) +
               (rest.empty() ? std::string() : " > " + rest);
    }

}

BuildReportWindow::BuildReportWindow(Project* project): project(project) {}

void BuildReportWindow::setOpen(bool openWindow) {
    windowOpen = openWindow;
    if (openWindow) {
        focusRequested = true;
        // R6: the last export of the session, else the history's newest (after a restart)
        if (file.empty()) {
            const fs::path last = BuildReportHistory::last(project->getProjectPath());
            if (!last.empty()) load(last);
        }
    }
}

void BuildReportWindow::open(const fs::path& path) {
    load(path);
    windowOpen = true;
    focusRequested = true;
}

void BuildReportWindow::load(const fs::path& path) {
    file = path;
    std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", FileUtils::pathToUtf8(path).c_str());
    error.clear();
    if (!BuildReport::load(path, report, error)) report = BuildReport();
    rowsStale = true;
    selected.clear();
    if (comparing) compareWith(baseFile);
}

void BuildReportWindow::compareWith(const fs::path& path) {
    compareError.clear();
    deltaByKey.clear();
    if (!BuildReport::load(path, base, compareError, true)) {
        comparing = false;
        return;
    }
    baseFile = path;
    comparing = true;
    diff = compareBuildReports(base, report);
    for (const BuildReportChange& change : diff.changes) deltaByKey[change.key] = change.delta;
}

void BuildReportWindow::clearComparison() {
    comparing = false;
    baseFile.clear();
    base = BuildReport();
    diff = BuildReportDiff();
    deltaByKey.clear();
    compareError.clear();
}

void BuildReportWindow::refreshHistory() {
    historyLoaded = true;
    historyFor = BuildReport::last();
    history.clear();
    for (const BuildReportHistory::Entry& entry : BuildReportHistory::list(project->getProjectPath())) {
        std::string& label = historyLabels[entry.file];
        if (label.empty()) {
            BuildReport item;
            std::string why;
            if (!BuildReport::load(entry.file, item, why)) continue;
            const std::string when = item.created.size() >= 16 ? item.created.substr(0, 10) + " " + item.created.substr(11, 5) : entry.id;
            label = platformTitle(item.platform) + " \xC2\xB7 " + when + " \xC2\xB7 " + item.method + " \xC2\xB7 " + BuildReport::formatBytes(item.totalBytes);
        }
        history.push_back({entry.file, entry.platform, label});
    }
    // the open report's platform first (R27), newest first within each
    std::stable_sort(history.begin(), history.end(), [this](const HistoryItem& a, const HistoryItem& b) {
        return (a.platform == report.platform) > (b.platform == report.platform);
    });
}

void BuildReportWindow::rebuildRows() {
    rowsStale = false;
    rows.clear();
    const std::string needle = lowerCopy(search);
    const std::vector<std::string>& categories = BuildReport::categoryNames();
    for (size_t i = 0; i < report.files.size(); i++) {
        const BuildReportFile& entry = report.files[i];
        if (categoryFilter > 0 && entry.category != categories[(size_t)categoryFilter - 1]) continue;
        if (!needle.empty() && lowerCopy(entry.path).find(needle) == std::string::npos && lowerCopy(entry.source).find(needle) == std::string::npos)
            continue;
        rows.push_back(i);
    }
    auto deltaOf = [this](const BuildReportFile& entry) {
        const auto it = deltaByKey.find(entry.source.empty() ? entry.path : entry.source);
        return it == deltaByKey.end() ? (int64_t)0 : it->second;
    };
    const std::vector<BuildReportFile>& files = report.files;
    std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
        const BuildReportFile& x = files[a];
        const BuildReportFile& y = files[b];
        bool less = false, greater = false;
        switch (sortColumn) {
            case 0: less = x.path < y.path; greater = y.path < x.path; break;
            case 1: less = x.category < y.category; greater = y.category < x.category; break;
            case 2: less = x.bytes < y.bytes; greater = y.bytes < x.bytes; break;
            case 3: less = x.source < y.source; greater = y.source < x.source; break;
            case 4: less = x.sourceBytes < y.sourceBytes; greater = y.sourceBytes < x.sourceBytes; break;
            default: less = deltaOf(x) < deltaOf(y); greater = deltaOf(y) < deltaOf(x); break;
        }
        if (!less && !greater) return x.path < y.path;
        return sortAscending ? less : greater;
    });
}

void BuildReportWindow::select(const std::string& path) {
    selected = path;
}

void BuildReportWindow::showAsset(const std::string& asset) {
    const fs::path absolute = project->getAssetsPath() / FileUtils::pathFromUtf8(asset);
    std::error_code ec;
    if (!fs::exists(absolute, ec)) return;
    // the importers (textures, models) read the list, every other asset's Inspector the one asset
    project->setInspectedAssets({project->normalizeToProjectRelative(absolute)});
    project->setInspectedAsset(absolute);
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) projectWindow->revealAsset(absolute);
}

void BuildReportWindow::drawToolbar() {
    ImGui::SetNextItemWidth(-90.0f);
    const bool entered = ImGui::InputTextWithHint("##report_path", "an export's build-report.json", pathBuffer, sizeof(pathBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Open") || entered) {
        fs::path path = FileUtils::pathFromUtf8(pathBuffer);
        std::error_code ec;
        if (fs::is_directory(path, ec)) path /= BuildReport::kFileName;
        load(path);
    }
    if (!historyLoaded || historyFor != BuildReport::last()) refreshHistory();

    // Report: the project's history (R27)
    std::string current = "Report";
    for (const HistoryItem& item : history)
        if (item.file == file) current = item.label;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    if (ImGui::BeginCombo("##report_history", current.c_str())) {
        if (history.empty()) ImGui::TextDisabled("No report in this project's history yet");
        for (const HistoryItem& item : history)
            if (ImGui::Selectable(item.label.c_str(), item.file == file)) load(item.file);
        ImGui::EndCombo();
    }
    UIProbe::item("Report");
    ImGui::SameLine();
    if (ImGui::Button("Compare With...")) ImGui::OpenPopup("##compare_with");
    if (ImGui::BeginPopup("##compare_with")) {
        if (history.size() <= 1) ImGui::TextDisabled("No other report in the history");
        for (const HistoryItem& item : history) {
            if (item.file == file) continue;
            const std::string label = item.label + (item.platform != report.platform ? "  (another platform)" : "");
            if (ImGui::Selectable(label.c_str(), item.file == baseFile)) compareWith(item.file);
        }
        ImGui::Separator();
        if (ImGui::Selectable("Browse...")) {
            const std::string picked = FileDialogs::openFileDialog(FileUtils::pathToUtf8(file.parent_path()));
            if (!picked.empty()) {
                fs::path path = FileUtils::pathFromUtf8(picked);
                std::error_code ec;
                if (fs::is_directory(path, ec)) path /= BuildReport::kFileName;
                compareWith(path);
            }
        }
        ImGui::EndPopup();
    }
    if (comparing) {
        ImGui::SameLine();
        if (ImGui::Button("Clear Comparison")) {
            clearComparison();
            rowsStale = true;
        }
    }
    if (!compareError.empty()) ImGui::TextColored(kLargerColor, "Compare With: %s", compareError.c_str());
}

void BuildReportWindow::drawCategories() {
    std::string title = "Build Report (" + report.platform + ", " + report.method + "): " + BuildReport::formatBytes(report.totalBytes);
    if (comparing) {
        title += "   " + deltaText((int64_t)diff.totalAfter - (int64_t)diff.totalBefore) + " vs " +
                 (base.id.empty() ? FileUtils::pathToUtf8(baseFile) : base.id);
        if (diff.platformDiffers) title += " (another platform: texture families differ)";
        if (diff.partial) title += " (partial: the older report lists its 25 largest files)";
    }
    ImGui::TextUnformatted(title.c_str());
    // S99 R12: the size the Download Budget holds this build to, against the project's budget now
    ModelFamily family;
    if (BuildReport::budgetFamilyOf(report.platform, family)){
        const uint64_t actual = report.budgetedBytes();
        const uint64_t budget = BuildReport::budgetBytesOf(project->getDownloadBudgetMB(family));
        const std::string size = (report.platform == "web" ? "Download " + BuildReport::formatBytes(actual) + " (gzip estimate)"
                                                           : "Size " + BuildReport::formatBytes(actual));
        const std::string label = std::string("Download Budget (") + modelFamilyLabel(family) + ")";
        if (!budget)
            ImGui::TextDisabled("%s; %s: off (Project Settings > Player > Publishing Settings)", size.c_str(), label.c_str());
        else if (actual > budget)
            ImGui::TextColored(Theme::Colors::ErrorText, ICON_FA_TRIANGLE_EXCLAMATION " %s, over the %s of %s", size.c_str(), label.c_str(),
                               BuildReport::formatBytes(budget).c_str());
        else
            ImGui::Text("%s, within the %s of %s", size.c_str(), label.c_str(), BuildReport::formatBytes(budget).c_str());
    }
    const int columns = comparing ? 5 : 3;
    if (!ImGui::BeginTable("build_report_categories", columns, ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("Category");
    if (comparing) {
        ImGui::TableSetupColumn("Before");
        ImGui::TableSetupColumn("After");
        ImGui::TableSetupColumn("Delta");
    } else {
        ImGui::TableSetupColumn("Size");
    }
    ImGui::TableSetupColumn("##bar", ImGuiTableColumnFlags_WidthStretch);
    if (comparing) ImGui::TableHeadersRow();
    for (const std::string& category : BuildReport::categoryNames()) {
        const auto it = report.categories.find(category);
        const uint64_t bytes = it == report.categories.end() ? 0 : it->second;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(category.c_str());
        if (comparing) {
            const auto sizes = diff.categories[category];
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(BuildReport::formatBytes(sizes.first).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(BuildReport::formatBytes(sizes.second).c_str());
            ImGui::TableNextColumn();
            deltaCell((int64_t)sizes.second - (int64_t)sizes.first);
        } else {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(BuildReport::formatBytes(bytes).c_str());
        }
        ImGui::TableNextColumn();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float share = report.totalBytes ? (float)bytes / (float)report.totalBytes : 0.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + std::max(1.0f, width * share), at.y + ImGui::GetTextLineHeight()),
                                                  IM_COL32(86, 180, 233, 220));
        ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
    }
    ImGui::EndTable();
}

void BuildReportWindow::drawFilesTab() {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (ImGui::InputTextWithHint("##files_search", "Search files", search, sizeof(search))) rowsStale = true;
    ImGui::SameLine();
    const std::vector<std::string>& categories = BuildReport::categoryNames();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::BeginCombo("##files_category", categoryFilter == 0 ? "All categories" : categories[(size_t)categoryFilter - 1].c_str())) {
        if (ImGui::Selectable("All categories", categoryFilter == 0)) { categoryFilter = 0; rowsStale = true; }
        for (size_t i = 0; i < categories.size(); i++)
            if (ImGui::Selectable(categories[i].c_str(), categoryFilter == (int)i + 1)) { categoryFilter = (int)i + 1; rowsStale = true; }
        ImGui::EndCombo();
    }
    // a report from before every file was listed has its largest only
    const std::vector<BuildReportFile>& source = report.hasFiles ? report.files : report.largest;
    if (!report.hasFiles) ImGui::TextDisabled("This report predates the full file list: its 25 largest files.");
    // the numbers keep the width of their whole header or a long value; File and Source share the
    // rest, and have a floor below which the table scrolls sideways
    const float sizeWidth = std::max(ImGui::CalcTextSize("1023.9 MB").x, ImGui::CalcTextSize("Source Size").x);
    const float categoryWidth = ImGui::CalcTextSize("Category  ").x;
    const float cellPadding = ImGui::GetStyle().CellPadding.x * 2.0f;
    const int columns = comparing ? 6 : 5;
    const float tableFloor = ImGui::GetFontSize() * 34.0f + categoryWidth + sizeWidth * (float)(columns - 3) + cellPadding * (float)columns;
    if (!ImGui::BeginTable("build_report_files", columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                                          ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable,
                           ImVec2(0.0f, 0.0f), std::max(ImGui::GetContentRegionAvail().x, tableFloor)))
        return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f, 0);
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, categoryWidth, 1);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
                            sizeWidth, 2);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 2.0f, 3);
    ImGui::TableSetupColumn("Source Size", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, sizeWidth, 4);
    if (comparing) ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, sizeWidth, 5);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty && specs->SpecsCount > 0) {
            sortColumn = (int)specs->Specs[0].ColumnUserID;
            sortAscending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            rowsStale = true;
            specs->SpecsDirty = false;
        }
    }
    if (report.hasFiles && rowsStale) rebuildRows();
    const size_t count = report.hasFiles ? rows.size() : source.size();
    ImGuiListClipper clipper;
    clipper.Begin((int)count);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            const BuildReportFile& entry = source[report.hasFiles ? rows[(size_t)row] : (size_t)row];
            ImGui::PushID(row);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable("##row", selected == entry.path, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                select(entry.path);
            ImGui::SameLine(0.0f, 0.0f);
            pathCell(entry.path);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(entry.category.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(BuildReport::formatBytes(entry.bytes).c_str());
            ImGui::TableSetColumnIndex(3);
            // a click shows the source in the Inspector
            if (!entry.source.empty() && ImGui::SmallButton(entry.source.c_str())) showAsset(entry.source);
            ImGui::TableSetColumnIndex(4); if (entry.sourceBytes) ImGui::TextUnformatted(BuildReport::formatBytes(entry.sourceBytes).c_str());
            if (comparing) {
                ImGui::TableSetColumnIndex(5);
                const auto it = deltaByKey.find(entry.source.empty() ? entry.path : entry.source);
                deltaCell(it == deltaByKey.end() ? 0 : it->second);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void BuildReportWindow::drawChangesTab() {
    ImGui::Checkbox("Added", &showAdded);
    ImGui::SameLine();
    ImGui::Checkbox("Removed", &showRemoved);
    ImGui::SameLine();
    ImGui::Checkbox("Grown", &showGrown);
    ImGui::SameLine();
    ImGui::Checkbox("Shrunk", &showShrunk);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu added, %zu removed, %zu grown, %zu shrunk, %zu unchanged", diff.added, diff.removed, diff.grown, diff.shrunk, diff.unchanged);
    std::vector<const BuildReportChange*> shown;
    for (const BuildReportChange& change : diff.changes) {
        if ((change.change == "added" && !showAdded) || (change.change == "removed" && !showRemoved) ||
            (change.change == "grown" && !showGrown) || (change.change == "shrunk" && !showShrunk))
            continue;
        shown.push_back(&change);
    }
    if (!ImGui::BeginTable("build_report_changes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                                          ImGuiTableFlags_SizingFixedFit))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Change");
    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Category");
    ImGui::TableSetupColumn("Before");
    ImGui::TableSetupColumn("After");
    ImGui::TableSetupColumn("Delta");
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin((int)shown.size());
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            const BuildReportChange& change = *shown[(size_t)row];
            ImGui::PushID(row);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::string kind = change.change;
            kind[0] = (char)std::toupper((unsigned char)kind[0]);
            ImGui::TextColored(change.delta < 0 ? kSmallerColor : kLargerColor, "%s", kind.c_str());
            ImGui::TableNextColumn();
            // the row is the file's key (a removed file has no path in this build)
            if (ImGui::Selectable(change.key.c_str(), selected == change.path, ImGuiSelectableFlags_SpanAllColumns))
                select(change.change == "removed" ? change.key : change.path);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(change.category.c_str());
            ImGui::TableNextColumn(); if (change.before) ImGui::TextUnformatted(BuildReport::formatBytes(change.before).c_str());
            ImGui::TableNextColumn(); if (change.after) ImGui::TextUnformatted(BuildReport::formatBytes(change.after).c_str());
            ImGui::TableNextColumn(); deltaCell(change.delta);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void BuildReportWindow::drawUnreferencedTab() {
    if (!report.reachability) {
        ImGui::TextDisabled(report.hasFiles ? "This export had no Referenced By data (the asset index was still building): export again."
                                            : "This report predates Referenced By: export again.");
        return;
    }
    const std::vector<const BuildReportFile*> files = report.unreferenced();
    uint64_t total = 0;
    for (const BuildReportFile* entry : files) total += entry->bytes;
    ImGui::Text("%zu file(s), %s", files.size(), BuildReport::formatBytes(total).c_str());
    ImGui::TextDisabled("Nothing in the index reaches them; a script may still load one by a path it builds at run time.");
    if (!ImGui::BeginTable("build_report_unreferenced", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Size");
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin((int)files.size());
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            const BuildReportFile& entry = *files[(size_t)row];
            ImGui::PushID(row);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(entry.path.c_str(), selected == entry.path, ImGuiSelectableFlags_SpanAllColumns)) select(entry.path);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(BuildReport::formatBytes(entry.bytes).c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void BuildReportWindow::drawLeftOutTab() {
    // S98 REQ-10, REQ-45: what Asset Stripping (unreferenced) and Exclude From Build left out; the
    // two lists share the height
    if (!report.stripped.empty()) {
        ImGui::TextDisabled("Left out of the build: %zu asset(s), %s (Project Settings > Player > Build)", report.strippedCount(),
                            BuildReport::formatBytes(report.strippedBytes()).c_str());
        if (ImGui::BeginTable("build_report_stripped", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                                             ImGuiTableFlags_SizingFixedFit, ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.5f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableHeadersRow();
            int row = 0;
            for (const BuildReportStripped& entry : report.stripped) {
                ImGui::PushID(row++);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                // a click shows it in the Inspector
                if (ImGui::Selectable(entry.path.c_str())) project->setInspectedAssets({FileUtils::pathFromUtf8(entry.path)});
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(BuildReport::formatBytes(entry.bytes).c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(entry.reason.c_str());
                if (entry.reason == "unreferenced" && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Nothing the build ships references it. A script that loads it by a path it builds needs it in Always Include.");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
    }
    ImGui::Text("%zu source(s), %s not shipped as themselves", report.leftOut.size(), BuildReport::formatBytes(report.leftOutBytes()).c_str());
    if (!ImGui::BeginTable("build_report_left_out", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                                          ImGuiTableFlags_SizingFixedFit))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Size");
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin((int)report.leftOut.size());
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            const BuildReportLeftOut& item = report.leftOut[(size_t)row];
            ImGui::PushID(row);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(item.source.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) showAsset(item.source);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(BuildReport::leftOutReasonText(item.reason).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(BuildReport::formatBytes(item.bytes).c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void BuildReportWindow::drawBudgetsTab() {
    if (ImGui::Button("Edit Budgets...")) Backend::getApp().getProjectSettingsWindow().open(project, "Player", "Size Budgets");
    if (report.budgets.empty()) {
        ImGui::TextDisabled("No size budget applied to this build (Project Settings > Player > Size Budgets).");
        return;
    }
    if (!ImGui::BeginTable("build_report_budgets", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("Category");
    ImGui::TableSetupColumn("Platform");
    ImGui::TableSetupColumn("Size / Limit");
    ImGui::TableSetupColumn("##bar", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Status");
    ImGui::TableHeadersRow();
    for (const BuildReportBudget& budget : report.budgets) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(budget.category.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(platformTitle(budget.platform).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%s / %s", BuildReport::formatBytes(budget.bytes).c_str(), BuildReport::formatBytes(budget.limitBytes).c_str());
        ImGui::TableNextColumn();
        const float fraction = budget.limitBytes ? std::min(1.0f, (float)((double)budget.bytes / (double)budget.limitBytes)) : 1.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, budget.overBytes() ? kLargerColor : kSmallerColor);
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), "");
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        if (budget.overBytes()) ImGui::TextColored(kLargerColor, "Over by %s", BuildReport::formatBytes(budget.overBytes()).c_str());
        else ImGui::TextColored(kSmallerColor, "Under");
    }
    ImGui::EndTable();
}

void BuildReportWindow::drawAudioDetail(const std::string& asset) {
    const fs::path absolute = project->getAssetsPath() / FileUtils::pathFromUtf8(asset);
    if (audioFor != asset) {
        audioFor = asset;
        audioChannels = AudioImportRegistry::channelsOf(*project, absolute);
        std::string why;
        audioSettings = AudioImportSettings();
        audioSettingsRead = AudioImportSettings::load(absolute, audioSettings, why);
    }
    if (audioChannels.channels < 2 || !audioSettingsRead) return;
    static const char* const modes[] = {"Off", "On", "Auto"};
    char width[48] = "";
    if (audioChannels.sideMidDb) std::snprintf(width, sizeof(width), " \xC2\xB7 width %.1f dB", *audioChannels.sideMidDb);
    ImGui::SeparatorText("Audio");
    ImGui::Text("Channels: %s%s \xC2\xB7 Force To Mono: %s", audioChannels.channels == 2 ? "Stereo" : "Multichannel", width,
                modes[(int)audioSettings.forceToMono]);
    // Auto would fold this clip: a positional voice pans it whole and a WAV ships at half the bytes
    if (audioSettings.forceToMono == AudioImportSettings::ForceToMono::Off && audioChannels.autoPicksMono() &&
        ImGui::Button("Set Force To Mono: Auto")) {
        AudioImportSettings next = audioSettings;
        next.forceToMono = AudioImportSettings::ForceToMono::Auto;
        auto* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(absolute), AudioImportSettings::metaKey, next.toYaml());
        command->setNoMerge();
        if (project->getProjectCommandHistory()->addCommand(command)) audioFor.clear();
    }
}

void BuildReportWindow::drawDetail() {
    if (selected.empty()) {
        ImGui::TextDisabled("Select a file to see why it ships.");
        return;
    }
    ImGui::TextUnformatted(selected.c_str());
    const BuildReportReferrers why = report.referencedBy(selected);
    if (!why.asset.empty() && BuildReport::categoryOf(why.file.empty() ? why.asset : why.file) == "audio") drawAudioDetail(why.asset);
    ImGui::SeparatorText("Referenced By");
    if (!report.reachability) {
        ImGui::TextDisabled("This report has no Referenced By data: export again.");
        return;
    }
    if (why.reason == "engine") {
        ImGui::TextDisabled("Part of the engine (the player and its libraries).");
    } else if (why.reason == "generated") {
        ImGui::TextDisabled("Written by the export (the asset map, cook indexes, import tables...).");
    } else if (why.reason == "unreferenced") {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Unreferenced: %s.", BuildReport::unreferencedNote());
        ImGui::PopTextWrapPos();
    } else if (why.reason == "not_in_build" && why.chain.empty()) {
        ImGui::TextDisabled("Not in this build.");
    }
    for (size_t i = 0; i < why.chain.size(); i++) {
        const BuildReportStep& step = why.chain[i];
        ImGui::PushID((int)i);
        if (i) ImGui::TextDisabled("  %s", ICON_FA_ARROW_UP);
        const bool root = step.kind == "root";
        const std::string label = root ? step.asset + "  (root)" : step.asset;
        if (ImGui::Selectable(label.c_str())) showAsset(step.asset);
        if (!root && !step.field.empty()) {
            const auto reach = report.reachability->find(step.asset);
            const std::string via = reach == report.reachability->end() ? std::string() : reach->second.via;
            ImGui::TextDisabled("    %s: %s (%s)", via.c_str(), fieldTitle(step.field, step.object).c_str(), step.kind.c_str());
        }
        ImGui::PopID();
    }
}

void BuildReportWindow::show() {
    if (!windowOpen)
        return;
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(900, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    drawToolbar();
    if (!error.empty()) {
        ImGui::TextDisabled("%s", error.c_str());
        ImGui::TextDisabled("Every export writes one next to what it builds.");
        ImGui::End();
        return;
    }
    if (file.empty()) {
        ImGui::TextDisabled("No export yet: export, or open a build-report.json.");
        ImGui::End();
        return;
    }
    drawCategories();
    ImGui::Separator();
    // the tables keep the window's width; Referenced By sits below them
    const float detailHeight = ImGui::GetFontSize() * 9.0f;
    ImGui::BeginChild("##build_report_tabs", ImVec2(0.0f, -detailHeight));
    if (ImGui::BeginTabBar("##build_report_tab_bar")) {
        if (ImGui::BeginTabItem("Files")) {
            drawFilesTab();
            ImGui::EndTabItem();
        }
        if (comparing && ImGui::BeginTabItem("Changes")) {
            drawChangesTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Unreferenced")) {
            drawUnreferencedTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Left Out")) {
            drawLeftOutTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Budgets")) {
            drawBudgetsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::BeginChild("##build_report_detail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    drawDetail();
    ImGui::EndChild();
    ImGui::End();
}
