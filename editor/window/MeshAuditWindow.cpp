// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshAuditWindow.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/ScenePropertyCmd.h"
#include "component/ModelComponent.h"
#include "external/IconsFontAwesome6.h"
#include "model/ModelSettingsEdit.h"
#include "command/type/SetAssetMetaCmd.h"
#include "util/FileUtils.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

}

MeshAuditWindow::MeshAuditWindow(Project* project): project(project) {}

void MeshAuditWindow::setOpen(bool open){
    windowOpen = open;
    if (open){
        focusRequested = true;
        scanned = false;
    }
}

void MeshAuditWindow::rescan(){
    rows.clear();
    scannedAt = std::chrono::steady_clock::now();
    scanned = true;
    if (project->getProjectPath().empty())
        return;
    ModelUsageIndex::get().refresh(*project);
    const ModelFamily family = (ModelFamily)platform;
    for (const std::string& path : projectModelSources(*project))
        rows.push_back(ModelAudit::row(*project, path, family));
}

void MeshAuditWindow::applyFix(const ModelAuditRow& row, const ModelIssue& issue){
    switch (issue.fix){
        case ModelIssue::Fix::Settings: {
            ModelSettingsEdit edit;
            for (const auto& [key, value] : issue.settings.items())
                edit.set.push_back({ModelAudit::metaKeyOf(key), value.is_boolean() ? YAML::Node(value.get<bool>()) : YAML::Node(value.get<std::string>())});
            const YAML::Node stored = SetAssetMetaCmd::read(project->getAssetsPath() / FileUtils::pathFromUtf8(row.assetPath), ModelImportSettings::metaKey);
            YAML::Node section;
            std::string error;
            std::vector<std::string> changed;
            if (editModelSection(stored, edit, section, error))
                writeModelSections(*project, {row.assetPath}, {section}, "Set Model Import Settings (" + fs::path(row.assetPath).filename().string() + ")", changed, error);
            break;
        }
        case ModelIssue::Fix::Reimport:
            ModelCookQueue::get().invalidate(row.assetPath);
            ModelCookQueue::get().request(row.assetPath, row.family, ModelCookQueue::Selected);
            break;
        case ModelIssue::Fix::SceneLod:
            for (SceneProject& scene : project->getScenes()){
                if (!scene.scene || project->normalizeToProjectRelative(scene.filepath).generic_string() != issue.scene)
                    continue;
                if (CommandHistory* history = CommandHandle::get(scene.id))
                    history->addCommand(new ScenePropertyCmd<bool>(project, scene.id, "mesh_lod_enabled", true));
            }
            break;
        case ModelIssue::Fix::OpenImporter:
            project->setInspectedAssets({fs::path(row.projectPath)});
            break;
        case ModelIssue::Fix::None:
            break;
    }
    scanned = false;
}

void MeshAuditWindow::show(){
    if (!windowOpen)
        return;
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    // wide enough for every column and the issues; a narrower window scrolls sideways
    ImGui::SetNextWindowSize(ImVec2(std::min(1320.0f, ImGui::GetMainViewport()->WorkSize.x * 0.95f), 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)){
        ImGui::End();
        return;
    }
    // what the queue lands shows up: a rescan every few seconds while it works
    if (!scanned || (ModelCookQueue::get().isBusy() && std::chrono::steady_clock::now() - scannedAt > std::chrono::seconds(3)))
        rescan();

    static const char* const platforms[] = {"Desktop", "Mobile", "Web"};
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::Combo("Platform", &platform, platforms, 3)){
        for (const ModelAuditRow& row : rows)
            ModelCookQueue::get().request(row.assetPath, (ModelFamily)platform, ModelCookQueue::Background);
        scanned = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Rescan")) scanned = false;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##audit_filter", "Filter by path", filter, sizeof(filter));
    ImGui::SameLine();
    ImGui::RadioButton("All", &scope, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Problems", &scope, 1);
    ImGui::SameLine();
    ImGui::RadioButton("In the open scene", &scope, 2);

    // the scope's models
    std::set<std::string> sceneModels;
    if (scope == 2){
        if (SceneProject* scene = project->getScene(project->getSelectedSceneId())){
            if (scene->scene && scene->scene->isComponentRegistered<ModelComponent>()){
                auto models = scene->scene->getComponentArray<ModelComponent>();
                for (size_t i = 0; i < models->size(); i++){
                    const std::string& filename = models->getComponentFromIndex(i).filename;
                    if (filename.empty()) continue;
                    const fs::path absolute = project->resolveAssetPath(filename).lexically_normal();
                    if (project->isInsideAssetsPath(absolute)) sceneModels.insert(project->normalizeToAssetsRelative(absolute).generic_string());
                }
            }
        }
    }
    const std::string needle = lower(filter);
    std::vector<const ModelAuditRow*> shown;
    long long triangles = 0;
    uint64_t memory = 0, download = 0, sourceDownload = 0;
    for (const ModelAuditRow& row : rows){
        if (!needle.empty() && lower(row.projectPath).find(needle) == std::string::npos) continue;
        if (scope == 1 && std::none_of(row.issues.begin(), row.issues.end(), [](const ModelIssue& i){ return i.severity != "info"; })) continue;
        if (scope == 2 && !sceneModels.count(row.assetPath)) continue;
        shown.push_back(&row);
        triangles += row.triangles();
        memory += row.memoryBytes();
        download += row.downloadBytes();
        sourceDownload += row.header.fileBytes;
    }
    ImGui::Text("%zu models: %lld tris | memory %s | download %s (sources %s)", shown.size(), triangles, ModelAudit::formatBytes(memory).c_str(),
                ModelAudit::formatBytes(download).c_str(), ModelAudit::formatBytes(sourceDownload).c_str());

    const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    // the numbers keep the width of their whole header (its sort arrow too) or of a long value;
    // Path and Issue share the rest, and have a floor below which the table scrolls sideways
    struct FixedColumn { const char* name; const char* longest; ImGuiTableColumnFlags flags; };
    const ImGuiTableColumnFlags descending = ImGuiTableColumnFlags_PreferSortDescending;
    const FixedColumn numbers[] = {{"Triangles", "12,345,678", ImGuiTableColumnFlags_DefaultSort | descending}, {"Vertices", "12,345,678", descending},
                                   {"Duplicate %", "100%", descending}, {"ACMR", "3.00", descending}, {"Overdraw", "12.34", descending},
                                   {"Memory", "1023.9 MB", descending}, {"Download", "1023.9 MB", descending}, {"LODs", "3", descending},
                                   {"Status", "not cooked", 0}};
    const ImGuiStyle& style = ImGui::GetStyle();
    const float arrow = ImGui::GetFontSize() + style.ItemInnerSpacing.x;
    float fixedWidth = 0.0f;
    std::vector<float> widths;
    for (const FixedColumn& column : numbers){
        widths.push_back(std::max(ImGui::CalcTextSize(column.name).x + arrow, ImGui::CalcTextSize(column.longest).x));
        fixedWidth += widths.back() + style.CellPadding.x * 2.0f;
    }
    const float pathFloor = ImGui::GetFontSize() * 16.0f, issueFloor = ImGui::GetFontSize() * 24.0f;
    const float innerWidth = std::max(ImGui::GetContentRegionAvail().x, fixedWidth + pathFloor + issueFloor);
    if (ImGui::BeginTable("mesh_audit", 11, flags, ImVec2(0.0f, 0.0f), innerWidth)){
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 3.0f);
        for (size_t c = 0; c < widths.size(); c++)
            ImGui::TableSetupColumn(numbers[c].name, ImGuiTableColumnFlags_WidthFixed | numbers[c].flags, widths[c]);
        ImGui::TableSetupColumn("Issue", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableHeadersRow();
        auto cooked = [](const ModelAuditRow* r){ return r->hasState && r->state.status == ModelCookStatus::Cooked; };
        if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount > 0){
            const ImGuiTableColumnSortSpecs& spec = sort->Specs[0];
            auto value = [&](const ModelAuditRow* r) -> double {
                switch (spec.ColumnIndex){
                    case 1: return (double)r->triangles();
                    case 2: return (double)r->vertices();
                    case 3: return cooked(r) ? r->state.record.duplicatePercent : -1.0;
                    case 4: return cooked(r) ? r->state.record.after.acmr : -1.0;
                    case 5: return cooked(r) ? r->state.record.after.overdraw : -1.0;
                    case 6: return (double)r->memoryBytes();
                    case 7: return (double)r->downloadBytes();
                    case 8: return (double)r->lodLevels();
                    case 10: return (double)r->issues.size();
                    default: return 0.0;
                }
            };
            std::stable_sort(shown.begin(), shown.end(), [&](const ModelAuditRow* a, const ModelAuditRow* b){
                if (spec.ColumnIndex == 0 || spec.ColumnIndex == 9){
                    const std::string x = spec.ColumnIndex == 0 ? a->projectPath : a->status(), y = spec.ColumnIndex == 0 ? b->projectPath : b->status();
                    return spec.SortDirection == ImGuiSortDirection_Ascending ? x < y : x > y;
                }
                return spec.SortDirection == ImGuiSortDirection_Ascending ? value(a) < value(b) : value(a) > value(b);
            });
        }
        ImGuiListClipper clipper;
        clipper.Begin((int)shown.size());
        const ModelAuditRow* fixRow = nullptr;
        const ModelIssue* fixIssue = nullptr;
        while (clipper.Step()){
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++){
                const ModelAuditRow& row = *shown[(size_t)i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool error = std::any_of(row.issues.begin(), row.issues.end(), [](const ModelIssue& issue){ return issue.severity == "error"; });
                if (ImGui::Selectable((std::string(error ? ICON_FA_TRIANGLE_EXCLAMATION " " : "") + row.projectPath).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    project->setInspectedAssets({fs::path(row.projectPath)});
                ImGui::TableSetColumnIndex(1); ImGui::Text("%lld", row.triangles());
                ImGui::TableSetColumnIndex(2); ImGui::Text("%lld", row.vertices());
                ImGui::TableSetColumnIndex(3); if (cooked(&row)) ImGui::Text("%.0f%%", row.state.record.duplicatePercent);
                ImGui::TableSetColumnIndex(4); if (cooked(&row)) ImGui::Text("%.2f", row.state.record.after.acmr);
                ImGui::TableSetColumnIndex(5); if (cooked(&row)) ImGui::Text("%.2f", row.state.record.after.overdraw);
                ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(ModelAudit::formatBytes(row.memoryBytes()).c_str());
                ImGui::TableSetColumnIndex(7); ImGui::TextUnformatted(ModelAudit::formatBytes(row.downloadBytes()).c_str());
                ImGui::TableSetColumnIndex(8); ImGui::Text("%d", row.lodLevels());
                ImGui::TableSetColumnIndex(9); ImGui::TextUnformatted(row.status().c_str());
                ImGui::TableSetColumnIndex(10);
                int number = 1;
                for (const ModelIssue& issue : row.issues){
                    if (issue.fix == ModelIssue::Fix::None) continue;
                    ImGui::PushID(number);
                    if (ImGui::SmallButton((std::to_string(number) + ". " + issue.fixLabel).c_str())){
                        fixRow = &row;
                        fixIssue = &issue;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", issue.message.c_str());
                    ImGui::PopID();
                    ImGui::SameLine();
                    number++;
                }
                if (!row.issues.empty()){
                    ImGui::TextDisabled("%s", row.issues.front().message.c_str());
                    if (ImGui::IsItemHovered()){
                        // the column cuts it: every issue of the model, whole
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                        for (const ModelIssue& issue : row.issues) ImGui::TextUnformatted(issue.message.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
        if (fixRow && fixIssue)
            applyFix(*fixRow, *fixIssue);
    }
    ImGui::End();
}
