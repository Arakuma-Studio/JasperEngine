// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CrashReportsWindow.h"

#include "AppSettings.h"
#include "Backend.h"
#include "Project.h"
#include "io/AtomicFile.h"
#include "texture/Texture.h"
#include "util/EditorDiagnostics.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "window/dialog/EditorSettingsWindow.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    const char* const kCrashReportKinds[] = {"crash", "hang", "abnormal_exit", "user"};
    const char* const kCrashReportKindLabels[] = {"Crash", "Hang", "Abnormal Exit", "User"};

    const char* crashKindLabel(const std::string& kind) {
        for (size_t i = 0; i < 4; i++) {
            if (kind == kCrashReportKinds[i]) return kCrashReportKindLabels[i];
        }
        return kind.c_str();
    }

    std::string crashCountList(const std::map<std::string, size_t>& counts) {
        std::string out;
        for (const auto& [name, count] : counts) out += (out.empty() ? "" : ", ") + name + " (" + std::to_string(count) + ")";
        return out;
    }

    bool crashRowMatches(const std::string& text, const std::string& needle) {
        if (needle.empty()) return true;
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        return lower(text).find(lower(needle)) != std::string::npos;
    }

    // what the Report tab shows: crash.txt, the newest hang report, user.txt, or the session marker
    fs::path crashPrimaryFile(const diag::crash::ReportInfo& report) {
        if (report.kind == "user") return report.folder / "user.txt";
        return EditorDiagnostics::primaryFile(report);
    }

}

CrashReportsWindow::CrashReportsWindow(Project* project): project(project) {}

void CrashReportsWindow::setOpen(bool openWindow) {
    windowOpen = openWindow;
    if (openWindow) {
        focusRequested = true;
        if (!loaded) {
            loadLocations();
            refresh();
        }
    }
}

void CrashReportsWindow::loadLocations() {
    const CrashReportsLocations locations = AppSettings::getCrashReportsLocations(project->getProjectPath() / "project.yaml");
    folder = locations.folder;
    symbols = locations.symbols;
}

void CrashReportsWindow::saveLocations() {
    AppSettings::setCrashReportsLocations(project->getProjectPath() / "project.yaml", CrashReportsLocations{folder, symbols});
}

void CrashReportsWindow::refresh() {
    loaded = true;
    error.clear();
    fileTexts.clear();
    shownFolder = folder.empty() ? diag::crash::reportsDirectory() : folder;
    reports = diag::crash::listReportsIn(shownFolder);
    // a game's crash root as pulled from a device holds reports/
    if (reports.empty() && !folder.empty()) {
        std::vector<diag::crash::ReportInfo> inner = diag::crash::listReportsIn(folder / "reports");
        if (!inner.empty()) {
            shownFolder = folder / "reports";
            reports = std::move(inner);
        }
    }
    if (reports.empty() && !folder.empty()) error = "No report folder at " + FileUtils::pathToUtf8(folder) + ": open a reports root (it holds <yyyymmddThhmmssZ>-<pid> folders).";
    signatures.clear();
    for (const diag::crash::ReportInfo& report : reports) signatures[report.id] = crashgroups::signatureOf(report);
    groups = crashgroups::group(reports);
    if (!selectedReport()) {
        selectedId.clear();
        selectedKey.clear();
    }
}

const diag::crash::ReportInfo* CrashReportsWindow::selectedReport() const {
    for (const diag::crash::ReportInfo& report : reports) {
        if (report.id == selectedId) return &report;
    }
    return nullptr;
}

std::vector<CrashReportsWindow::Row> CrashReportsWindow::rows() const {
    std::vector<Row> out;
    auto kindShownFor = [&](const std::string& kind) {
        for (size_t i = 0; i < 4; i++) {
            if (kind == kCrashReportKinds[i]) return kindShown[i];
        }
        return true;
    };
    std::map<int64_t, std::string> isoOf;
    for (const diag::crash::ReportInfo& report : reports) isoOf[report.timeUnix] = report.time;
    if (groupSimilar) {
        for (const crashgroups::Group& g : groups) {
            if (!kindShownFor(g.kind)) continue;
            Row row;
            row.kind = crashKindLabel(g.kind);
            row.text = g.signature;
            row.versions = crashCountList(g.versions);
            row.platforms = crashCountList(g.platforms);
            row.count = g.count;
            row.firstSeen = isoOf[g.firstSeen];
            row.lastSeen = isoOf[g.lastSeen];
            size_t handled = 0;
            for (const diag::crash::ReportInfo& report : reports) {
                if (report.handled && std::find(g.reportIds.begin(), g.reportIds.end(), report.id) != g.reportIds.end()) handled++;
            }
            row.handled = handled == 0 ? "No" : handled == g.count ? "Yes" : std::to_string(handled) + " of " + std::to_string(g.count);
            row.reportId = g.reportIds.empty() ? std::string() : g.reportIds.front();
            row.key = "group " + g.signature;
            if (crashRowMatches(row.text + " " + row.versions + " " + row.platforms + " " + row.kind, search)) out.push_back(std::move(row));
        }
        return out;
    }
    for (const diag::crash::ReportInfo& report : reports) {
        if (!kindShownFor(report.kind)) continue;
        const auto signature = signatures.find(report.id);
        Row row;
        row.kind = crashKindLabel(report.kind);
        row.text = report.reason;
        row.versions = report.version;
        row.platforms = signature == signatures.end() ? std::string() : signature->second.platform;
        row.firstSeen = row.lastSeen = report.time;
        row.handled = report.handled ? "Yes" : "No";
        row.reportId = report.id;
        row.key = report.id;
        if (crashRowMatches(report.id + " " + row.text + " " + row.versions + " " + row.platforms + " " + row.kind, search)) out.push_back(std::move(row));
    }
    return out;
}

void CrashReportsWindow::select(const Row& row) {
    selectedKey = row.key;
    if (selectedId != row.reportId) fileTexts.clear();
    selectedId = row.reportId;
}

const std::string& CrashReportsWindow::fileText(const std::string& name) {
    auto it = fileTexts.find(name);
    if (it != fileTexts.end()) return it->second;
    std::string text;
    if (const diag::crash::ReportInfo* report = selectedReport()) {
        const fs::path file = name == "Report" ? crashPrimaryFile(*report) : report->folder / name;
        std::error_code ec;
        if (fs::is_regular_file(file, ec)) {
            std::ifstream in(file, std::ios::binary);
            text.resize(256 << 10);
            in.read(text.data(), static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<size_t>(in.gcount()));
        }
    }
    return fileTexts.emplace(name, std::move(text)).first->second;
}

std::vector<std::string> CrashReportsWindow::unhandledSelection() const {
    std::vector<std::string> ids;
    if (selectedId.empty()) return ids;
    std::vector<std::string> wanted = {selectedId};
    // a group's row stands for every report in it
    if (groupSimilar) {
        for (const crashgroups::Group& g : groups) {
            if ("group " + g.signature == selectedKey) wanted = g.reportIds;
        }
    }
    for (const diag::crash::ReportInfo& report : reports) {
        if (!report.handled && std::find(wanted.begin(), wanted.end(), report.id) != wanted.end()) ids.push_back(report.id);
    }
    return ids;
}

void CrashReportsWindow::markHandled() {
    const std::vector<std::string> ids = unhandledSelection();
    for (diag::crash::ReportInfo& report : reports) {
        if (std::find(ids.begin(), ids.end(), report.id) == ids.end()) continue;
        bool written = false;
        if (folder.empty()) {
            // the editor's own: the notice's Dismiss (it closes the notice when it is about this report)
            written = EditorDiagnostics::dismiss(report.id);
        } else {
            io::AtomicWriteOptions options;
            options.durability = io::Durability::Cache;
            written = io::writeFileAtomic(report.folder / "handled", std::string_view(), options).ok();
        }
        if (!written) {
            error = "Could not write " + FileUtils::pathToUtf8(report.folder / "handled") + ".";
            return;
        }
        report.handled = true;
    }
}

std::string CrashReportsWindow::symbolicateCommand() const {
    const diag::crash::ReportInfo* report = selectedReport();
    // a group: every report folder beside it; one report: its own folder
    const fs::path target = groupSimilar || !report ? shownFolder : report->folder;
    std::string command = "python3 -I tools/crash/symbolicate.py --group \"" + FileUtils::pathToUtf8(target) + "\"";
    command += symbols.empty() ? std::string(" --symbols <symbols folder>") : " --symbols \"" + FileUtils::pathToUtf8(symbols) + "\"";
    return command;
}

void CrashReportsWindow::drawToolbar() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Folder");
    ImGui::SameLine();
    const std::string preview = folder.empty() ? std::string("Editor Reports") : FileUtils::pathToUtf8(folder);
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.35f, ImGui::GetFontSize() * 24.0f));
    if (ImGui::BeginCombo("##crash_reports_folder", preview.c_str())) {
        if (ImGui::Selectable("Editor Reports", folder.empty()) && !folder.empty()) {
            folder.clear();
            saveLocations();
            refresh();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", FileUtils::pathToUtf8(shownFolder).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Open Folder...")) {
        const std::string picked = FileDialogs::openFileDialog(FileUtils::pathToUtf8(folder.empty() ? shownFolder : folder), FILE_DIALOG_ALL, true);
        if (!picked.empty()) {
            folder = FileUtils::pathFromUtf8(picked);
            saveLocations();
            selectedId.clear();
            selectedKey.clear();
            refresh();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A game's reports root pulled from a device (it holds <yyyymmddThhmmssZ>-<pid> folders)");
    ImGui::SameLine();
    ImGui::Checkbox("Group Similar", &groupSimilar);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One row per signature: the kind, the signal and the first 3 frames of the game");
    ImGui::SameLine();
    if (ImGui::Button("Kinds")) ImGui::OpenPopup("##crash_reports_kinds");
    if (ImGui::BeginPopup("##crash_reports_kinds")) {
        for (size_t i = 0; i < 4; i++) ImGui::Checkbox(kCrashReportKindLabels[i], &kindShown[i]);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Refresh").x -
                                                                       ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##crash_reports_search", "Search", search, sizeof(search));
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refresh();
}

void CrashReportsWindow::drawTable(const std::vector<Row>& shown) {
    const float height = std::max(ImGui::GetFontSize() * 6.0f, ImGui::GetContentRegionAvail().y * 0.45f);
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##crash_reports_table", 8, flags, ImVec2(0.0f, height))) return;
    const float timeWidth = ImGui::CalcTextSize("2026-11-05T13:05:00Z").x;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Abnormal Exit").x);
    ImGui::TableSetupColumn(groupSimilar ? "Signature" : "Reason", ImGuiTableColumnFlags_WidthStretch, 4.0f);
    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Platform", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Count").x);
    ImGui::TableSetupColumn("First Seen", ImGuiTableColumnFlags_WidthFixed, timeWidth);
    ImGui::TableSetupColumn("Last Seen", ImGuiTableColumnFlags_WidthFixed, timeWidth);
    ImGui::TableSetupColumn("Handled", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Handled").x);
    ImGui::TableHeadersRow();
    for (const Row& row : shown) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const std::string label = row.kind + "##row_" + row.key;
        if (ImGui::Selectable(label.c_str(), row.key == selectedKey, ImGuiSelectableFlags_SpanAllColumns)) select(row);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.text.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.text.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.versions.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.platforms.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%zu", row.count);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.firstSeen.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.lastSeen.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.handled.c_str());
    }
    ImGui::EndTable();
}

void CrashReportsWindow::drawDetail() {
    const diag::crash::ReportInfo* report = selectedReport();
    ImGui::BeginDisabled(report == nullptr);
    if (ImGui::Button("Show in Folder") && report) EditorSettingsWindow::openInFileManager(report->folder);
    ImGui::SameLine();
    if (ImGui::Button("Copy Path") && report) ImGui::SetClipboardText(FileUtils::pathToUtf8(report->folder).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(report && unhandledSelection().empty());
    if (ImGui::Button("Mark Handled")) markHandled();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Writes `handled` in the report's folder: it is not offered again");
    ImGui::SameLine();
    if (ImGui::Button("Copy Symbolicate Command")) ImGui::SetClipboardText(symbolicateCommand().c_str());
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("tools/crash/symbolicate.py --group names the frames from your build's symbols");
    }
    ImGui::SameLine();
    if (ImGui::Button("Symbols Folder...")) {
        const std::string picked = FileDialogs::openFileDialog(FileUtils::pathToUtf8(symbols), FILE_DIALOG_ALL, true);
        if (!picked.empty()) {
            symbols = FileUtils::pathFromUtf8(picked);
            saveLocations();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", symbols.empty() ? "The symbols Copy Symbolicate Command names: a release's symbols/ folder"
                                                : FileUtils::pathToUtf8(symbols).c_str());
    }
    if (!report) {
        ImGui::TextDisabled("Select a report.");
        return;
    }
    if (!ImGui::BeginTabBar("##crash_reports_tabs")) return;
    struct Tab { const char* label; const char* file; };
    const Tab tabs[] = {{"Report", "Report"}, {"Journal", "journal.txt"}, {"Log Tail", "log-tail.txt"}, {"Device", "device.json"}};
    for (const Tab& tab : tabs) {
        if (!ImGui::BeginTabItem(tab.label)) continue;
        const std::string& text = fileText(tab.file);
        if (ImGui::BeginChild("##crash_report_text", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
            if (text.empty()) ImGui::TextDisabled("This report has no %s.", tab.file == std::string("Report") ? "report file" : tab.file);
            else ImGui::TextUnformatted(text.data(), text.data() + text.size());
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Screenshot")) {
        const fs::path file = report->folder / "screenshot.jpg";
        SheetPreviewTextures::Image* image = screenshots.get(project, FileUtils::pathToUtf8(file));
        TextureRender* render = image ? image->texture.getRender() : nullptr;
        if (render && render->isCreated()) {
            // fit, keeping the aspect
            const ImVec2 room = ImGui::GetContentRegionAvail();
            const float scale = std::min(room.x / static_cast<float>(image->width), room.y / static_cast<float>(image->height));
            const ImVec2 size(std::max(1.0f, image->width * scale), std::max(1.0f, image->height * scale));
            if (ImGui::ImageButton("##crash_report_screenshot", ImTextureRef(Backend::getImGuiTexture(render)), size)) {
                EditorSettingsWindow::openInFileManager(file);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open %s", FileUtils::pathToUtf8(file).c_str());
        } else {
            ImGui::TextDisabled("This report has no screenshot.jpg.");
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void CrashReportsWindow::show() {
    if (!windowOpen) return;
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(980, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    if (!loaded) {
        loadLocations();
        refresh();
    }
    drawToolbar();
    if (!error.empty()) ImGui::TextDisabled("%s", error.c_str());
    const std::vector<Row> shown = rows();
    if (reports.empty() && error.empty()) {
        ImGui::TextDisabled("No reports in %s.", FileUtils::pathToUtf8(shownFolder).c_str());
    } else {
        drawTable(shown);
    }
    ImGui::Separator();
    drawDetail();
    ImGui::End();
}
