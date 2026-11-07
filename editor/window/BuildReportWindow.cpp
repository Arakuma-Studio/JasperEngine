// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuildReportWindow.h"

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "util/FileUtils.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

BuildReportWindow::BuildReportWindow(Project* project): project(project) {}

void BuildReportWindow::setOpen(bool openWindow){
    windowOpen = openWindow;
    if (openWindow){
        focusRequested = true;
        if (file.empty() && !BuildReport::last().empty())
            load(BuildReport::last());
    }
}

void BuildReportWindow::open(const fs::path& report){
    load(report);
    windowOpen = true;
    focusRequested = true;
}

void BuildReportWindow::load(const fs::path& path){
    file = path;
    std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", FileUtils::pathToUtf8(path).c_str());
    error.clear();
    if (!BuildReport::load(path, report, error))
        report = BuildReport();
}

void BuildReportWindow::show(){
    if (!windowOpen)
        return;
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)){
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(-90.0f);
    const bool entered = ImGui::InputTextWithHint("##report_path", "an export's build-report.json", pathBuffer, sizeof(pathBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Open") || entered){
        fs::path path = FileUtils::pathFromUtf8(pathBuffer);
        std::error_code ec;
        if (fs::is_directory(path, ec)) path /= BuildReport::kFileName;
        load(path);
    }
    if (!error.empty()){
        ImGui::TextDisabled("%s", error.c_str());
        ImGui::TextDisabled("Every export writes one next to what it builds.");
        ImGui::End();
        return;
    }
    if (file.empty()){
        ImGui::TextDisabled("No export yet in this session: export, or open a build-report.json.");
        ImGui::End();
        return;
    }
    ImGui::Text("Build Report (%s, %s): %s", report.platform.c_str(), report.method.c_str(), BuildReport::formatBytes(report.totalBytes).c_str());
    const float width = ImGui::GetContentRegionAvail().x * 0.5f;
    for (const std::string& category : BuildReport::categoryNames()){
        const auto it = report.categories.find(category);
        const uint64_t bytes = it == report.categories.end() ? 0 : it->second;
        ImGui::Text("%-9s %10s", category.c_str(), BuildReport::formatBytes(bytes).c_str());
        ImGui::SameLine(220.0f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float share = report.totalBytes ? (float)bytes / (float)report.totalBytes : 0.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + std::max(1.0f, width * share), at.y + ImGui::GetTextLineHeight()),
                                                  IM_COL32(86, 180, 233, 220));
        ImGui::NewLine();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Largest files");
    // the numbers keep the width of their whole header or a long value; File and Source share the
    // rest, and have a floor below which the table scrolls sideways
    const float sizeWidth = std::max(ImGui::CalcTextSize("1023.9 MB").x, ImGui::CalcTextSize("Source Size").x);
    const float categoryWidth = ImGui::CalcTextSize("Category  ").x;
    const float cellPadding = ImGui::GetStyle().CellPadding.x * 2.0f;
    const float tableFloor = ImGui::GetFontSize() * 34.0f + categoryWidth + sizeWidth * 2.0f + cellPadding * 5.0f;
    if (ImGui::BeginTable("build_report_files", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                                       ImGuiTableFlags_SizingFixedFit, ImVec2(0.0f, 0.0f), std::max(ImGui::GetContentRegionAvail().x, tableFloor))){
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, categoryWidth);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, sizeWidth);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Source Size", ImGuiTableColumnFlags_WidthFixed, sizeWidth);
        ImGui::TableHeadersRow();
        int row = 0;
        for (const BuildReportFile& entry : report.largest){
            ImGui::PushID(row++);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // the tail tells files apart: the head is the same long prefix on every row
            std::string shown = entry.path;
            const float room = ImGui::GetContentRegionAvail().x;
            if (ImGui::CalcTextSize(shown.c_str()).x > room){
                size_t cut = 0;
                while (cut < shown.size() && ImGui::CalcTextSize(("..." + shown.substr(cut)).c_str()).x > room) cut++;
                shown = "..." + shown.substr(cut);
            }
            ImGui::TextUnformatted(shown.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entry.path.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(entry.category.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(BuildReport::formatBytes(entry.bytes).c_str());
            ImGui::TableSetColumnIndex(3);
            if (!entry.source.empty()){
                // a click shows the source in the Inspector
                if (ImGui::Selectable(entry.source.c_str()))
                    project->setInspectedAssets({project->normalizeToProjectRelative(project->getAssetsPath() / FileUtils::pathFromUtf8(entry.source))});
            }
            ImGui::TableSetColumnIndex(4); if (entry.sourceBytes) ImGui::TextUnformatted(BuildReport::formatBytes(entry.sourceBytes).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
