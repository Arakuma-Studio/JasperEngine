// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector of Data Assets (docs/specs/S95-lua-runtime-gaps.md REQ-21): a .dataasset's fields as
// its class declares them, each edit a named undo step of its file (a drag one step), lists with
// add, remove and reorder, and what the check finds (values of no field are kept and listed).

#include "InspectorWindow.h"

#include "App.h"
#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "dataasset/DataAssets.h"
#include "external/IconsFontAwesome6.h"
#include "window/CodeEditor.h"
#include "window/ScriptValueEditor.h"

#include "imgui.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // one undo step per drag or text edit of one field
    struct DataAssetView {
        fs::path asset;
        unsigned int editSerial = 0;
    };

    DataAssetView& dataAssetView() {
        static auto* view = new DataAssetView();
        return *view;
    }

}

void editor::InspectorWindow::drawDataAssetAsset(const fs::path& asset) {
    DataAssetView& view = dataAssetView();
    if (view.asset != asset) {
        view.asset = asset;
        view.editSerial++;
    }
    drawAssetHeader(ICON_FA_DATABASE, asset, "Data Asset");

    DataAssets::Document document;
    std::string error;
    if (!DataAssets::read(asset, document, &error)) {
        ImGui::TextColored(Theme::Colors::WarningText, "Cannot read this Data Asset: %s", error.c_str());
        return;
    }

    DataAssets::Class dataClass;
    const bool known = DataAssets::findClass(project->getLuaPath(), document.className, dataClass);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Class");
    ImGui::SameLine();
    if (!known) {
        ImGui::TextColored(Theme::Colors::WarningText, "%s (no Lua module of this name holds ---@dataasset)", document.className.c_str());
        return;
    }
    ImGui::TextUnformatted(document.className.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_FILE_CODE "##open_class")) {
        Backend::getApp().getCodeEditor()->openFile(dataClass.file.string(), true);
    }
    ImGui::SetItemTooltip("Open %s", dataClass.luaPath.c_str());

    std::vector<ScriptProperty> schema;
    if (!DataAssets::readSchema(dataClass, schema, error)) {
        ImGui::TextColored(Theme::Colors::WarningText, "%s: %s", dataClass.luaPath.c_str(), error.c_str());
        return;
    }
    ImGui::Separator();

    float labelSize = getLabelSize("Name");
    for (const ScriptProperty& field : schema) labelSize = std::max(labelSize, getLabelSize(field.displayName.empty() ? field.name : field.displayName));

    bool editing = false;
    if (ImGui::BeginTable("data_asset_fields", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, labelSize);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        for (const ScriptProperty& field : schema) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted((field.displayName.empty() ? field.name : field.displayName).c_str());
            ImGui::TableNextColumn();
            ScriptPropertyValue value = DataAssets::valueOf(project, field, document);
            const ScriptValueEdit edit = drawScriptValue(project, nullptr, field, value, field.name);
            editing |= edit.editing;
            if (!edit.changed) continue;
            DataAssets::Document changed = document;
            changed.values[field.name] = DataAssets::toJson(project, field, value);
            const std::string mergeKey = edit.editing ? field.name + "#" + std::to_string(view.editSerial) : std::string();
            const std::string name = field.displayName.empty() ? field.name : field.displayName;
            if (!DataAssets::commit(project, asset, changed, "Set " + document.className + " " + name, mergeKey, &error)) {
                Out::error("Data Asset: %s", error.c_str());
            }
        }
        ImGui::EndTable();
    }
    // the next drag is a step of its own
    if (!editing) view.editSerial++;

    // values of no field (a field renamed in the class) stay in the file until removed
    std::vector<std::string> unknown;
    for (auto it = document.values.begin(); it != document.values.end(); ++it) {
        const bool declared = std::any_of(schema.begin(), schema.end(), [&](const ScriptProperty& field) { return field.name == it.key(); });
        if (!declared) unknown.push_back(it.key());
    }
    const std::vector<std::string> problems = DataAssets::problems(project, document);
    if (!problems.empty()) {
        ImGui::Separator();
        for (const std::string& problem : problems) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", problem.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (!unknown.empty() && ImGui::Button("Remove Unknown Values")) {
        DataAssets::Document changed = document;
        for (const std::string& key : unknown) changed.values.erase(key);
        if (!DataAssets::commit(project, asset, changed, "Remove " + document.className + " Unknown Values", "", &error)) {
            Out::error("Data Asset: %s", error.c_str());
        }
    }
}
