// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsepriteImporterView.h"

#include "pixelart/draw/SpriteDocCache.h"

#include <chrono>

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "command/type/ClipSceneActions.h"
#include "command/type/SetAssetMetaCmd.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/aseprite/AsepriteEditorHooks.h"
#include "pixelart/aseprite/AsepriteImporter.h"
#include "sprite/SpriteSheetPool.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "window/EditorModal.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace ase = jasperengine::editor::pixelart::aseprite;
using Json = nlohmann::json;

namespace {

    // every setting the view edits, as JSON pointers into AsepriteImportSettings::toJson
    const std::vector<std::string>& aseViewLeaves(){
        static const std::vector<std::string> leaves = {
            "/import_mode", "/layers/include_hidden", "/layers/ignore_prefixes", "/layers/include", "/layers/exclude",
            "/pixels_per_unit", "/pivot/source", "/pivot/slice", "/pivot/preset", "/pivot/custom", "/border/slice", "/trim",
            "/clips/generate", "/clips/tags", "/clips/events/layer", "/texture/layout", "/texture/padding", "/texture/extrude",
            "/texture/max_size", "/texture/compression"};
        return leaves;
    }

    Json aseViewAt(const Json& root, const std::string& pointer){
        const Json::json_pointer at(pointer);
        return root.contains(at) ? root.at(at) : Json();
    }

    std::vector<std::string> aseViewStrings(const Json& value){
        std::vector<std::string> out;
        if (value.is_array())
            for (const Json& item : value)
                if (item.is_string()) out.push_back(item.get<std::string>());
        return out;
    }

    // "color_dodge" -> "Color Dodge"
    std::string aseViewTitle(const std::string& name){
        std::string out;
        bool start = true;
        for (char c : name){
            if (c == '_'){
                out += ' ';
                start = true;
                continue;
            }
            out += start ? (char)std::toupper((unsigned char)c) : c;
            start = false;
        }
        return out;
    }

    std::string aseViewTrim(const std::string& text){
        size_t begin = 0, end = text.size();
        while (begin < end && std::isspace((unsigned char)text[begin])) begin++;
        while (end > begin && std::isspace((unsigned char)text[end - 1])) end--;
        return text.substr(begin, end - begin);
    }

    const char* aseViewDirection(ase::TagDirection direction){
        switch (direction){
            case ase::TagDirection::Forward: return "Forward";
            case ase::TagDirection::Reverse: return "Reverse";
            case ase::TagDirection::PingPong: return "Ping-pong";
            case ase::TagDirection::PingPongReverse: return "Ping-pong Reverse";
        }
        return "Forward";
    }

    bool aseViewNinePatch(const ase::Slice& slice){
        if (slice.flags & 1) return true;
        for (const ase::SliceKey& key : slice.keys)
            if (key.center) return true;
        return false;
    }

    bool aseViewHasPivot(const ase::Slice& slice){
        if (slice.flags & 2) return true;
        for (const ase::SliceKey& key : slice.keys)
            if (key.pivot) return true;
        return false;
    }

    bool aseViewSensor(const ase::Slice& slice){
        const ase::PropertyMap* properties = slice.userData.userProperties();
        const ase::PropertyValue* sensor = properties ? ase::findProperty(*properties, "sensor") : nullptr;
        return sensor && sensor->type == ase::PropertyType::Bool && std::get<bool>(sensor->value);
    }

    // a path under (or equal to) a group path
    bool aseViewUnder(const std::string& path, const std::string& group){
        return path == group || (path.size() > group.size() && path.compare(0, group.size(), group) == 0 && path[group.size()] == '/');
    }

    void aseViewLabel(const char* label, const char* tooltip){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        ImGui::TableSetColumnIndex(1);
    }

    void aseViewProblem(const editor::pixelart::PixelProblem& problem){
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", problem.message.c_str());
        ImGui::PopTextWrapPos();
        if (!problem.fix.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", problem.fix.c_str());
    }

}

AsepriteImporterView::AsepriteImporterView(Project* project): project(project) {}

bool AsepriteImporterView::storedChanged() const{
    for (const File& file : files){
        std::error_code ec;
        const fs::path meta = SetAssetMetaCmd::metaPathOf(file.absolute);
        const bool exists = fs::is_regular_file(meta, ec);
        if (exists != file.metaExists) return true;
        if (exists && fs::last_write_time(meta, ec) != file.metaTime) return true;
    }
    return false;
}

void AsepriteImporterView::select(const std::vector<fs::path>& selection){
    std::vector<std::string> assets;
    for (const fs::path& path : selection){
        const fs::path absolute = (project->getProjectPath() / path).lexically_normal();
        if (!project->isInsideAssetsPath(absolute) || !SpriteSheetPool::isAsepritePath(absolute.generic_string()))
            continue;
        assets.push_back(project->normalizeToAssetsRelative(absolute).generic_string());
    }
    std::string key;
    for (const std::string& path : assets)
        key += path + "\n";
    // the stored settings changed under us (undo, an MCP write): read them again, unless edits wait
    if (key == selectionKey && (!touched.empty() || !storedChanged()))
        return;
    if (key != selectionKey){
        extractStatus.clear();
        applyProblems.clear();
    }
    selectionKey = key;
    files.clear();
    touched.clear();
    mixed.clear();
    ase::AsepriteImporter& importer = ase::AsepriteImporter::get();
    for (const std::string& assetPath : assets){
        File file;
        file.assetPath = assetPath;
        file.absolute = (project->getAssetsPath() / assetPath).lexically_normal();
        std::error_code ec;
        const fs::path meta = SetAssetMetaCmd::metaPathOf(file.absolute);
        file.metaExists = fs::is_regular_file(meta, ec);
        if (file.metaExists) file.metaTime = fs::last_write_time(meta, ec);
        if (importer.getProject() == project) {
            file.settings = importer.settingsOf(assetPath, &file.problems);
        } else {
            ase::AsepriteImportSettings::fromYaml(SetAssetMetaCmd::read(file.absolute, ase::AsepriteImporter::kSettingsKey), file.settings, file.problems);
        }
        files.push_back(std::move(file));
    }
    if (files.empty())
        return;
    working = files[0].settings.toJson();
    const Json first = working;
    for (size_t i = 1; i < files.size(); i++){
        const Json other = files[i].settings.toJson();
        for (const std::string& leaf : aseViewLeaves())
            if (aseViewAt(first, leaf) != aseViewAt(other, leaf)) mixed.insert(leaf);
    }
}

void AsepriteImporterView::readStructure(){
    const File& file = files[0];
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(file.absolute, ec);
    if (structure.assetPath == file.assetPath && structure.time == time)
        return;
    structure = Structure();
    structure.assetPath = file.assetPath;
    structure.time = time;
    ase::ReadOptions quick;
    quick.pixels = false;
    quick.provenance = false;
    ase::ReadResult read = ase::readFile(file.absolute, quick);
    if (read.ok()) structure.document = std::move(read.document);
    else structure.error = read.error.message;
}

Json AsepriteImporterView::workingValue(const std::string& pointer) const{
    return aseViewAt(working, pointer);
}

void AsepriteImporterView::setWorking(const std::string& pointer, Json value){
    working[Json::json_pointer(pointer)] = std::move(value);
    touched.insert(pointer);
    mixed.erase(pointer);
}

ase::AsepriteImportSettings AsepriteImporterView::workingSettings() const{
    ase::AsepriteImportSettings settings = files[0].settings;
    Json patch = Json::object();
    for (const std::string& pointer : touched)
        patch[Json::json_pointer(pointer)] = workingValue(pointer);
    std::vector<editor::pixelart::PixelProblem> ignored;
    settings.applyJson(patch, ignored);
    return settings;
}

void AsepriteImporterView::revert(){
    touched.clear();
    applyProblems.clear();
    selectionKey.clear();
}

void AsepriteImporterView::apply(){
    if (touched.empty())
        return;
    Json patch = Json::object();
    for (const std::string& pointer : touched)
        patch[Json::json_pointer(pointer)] = workingValue(pointer);
    applyProblems.clear();
    // every file's own settings with the edited keys only: unknown keys and the rest stay
    std::vector<ase::AsepriteImportSettings> results;
    for (const File& file : files){
        ase::AsepriteImportSettings settings = file.settings;
        if (!settings.applyJson(patch, applyProblems))
            return;
        results.push_back(std::move(settings));
    }
    CommandHistory* history = project->getProjectCommandHistory();
    InteractiveEdit edit = history->beginInteractive("Set Aseprite Import Settings");
    for (size_t i = 0; i < files.size(); i++){
        Command* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(files[i].absolute),
                                               ase::AsepriteImporter::kSettingsKey, results[i].toYaml());
        if (command->isNoOp()){
            delete command;
            continue;
        }
        command->setNoMerge();
        // the importer's meta listener imports the file again
        history->addCommand(command);
    }
    edit.commit();
    touched.clear();
    selectionKey.clear();
}

void AsepriteImporterView::extractClips(){
    ase::AsepriteImporter& importer = ase::AsepriteImporter::get();
    CommandHistory* history = project->getProjectCommandHistory();
    InteractiveEdit edit = history->beginInteractive("Extract Clips");
    size_t written = 0;
    std::string failure;
    for (const File& file : files){
        const std::shared_ptr<const ase::ImportRecord> record = importer.record(file.assetPath);
        if (!record) continue;
        const std::string stem = FileUtils::pathToUtf8(file.absolute.stem());
        for (const ase::ImportRecord::Clip& clip : record->clips){
            const fs::path target = file.absolute.parent_path() / FileUtils::pathFromUtf8(stem + " " + clip.name + ".anim");
            ClipActionResult result;
            std::string error;
            if (ClipSceneActions::extractSubClip(project, 0, NULL_ENTITY, file.assetPath + "#" + clip.name, FileUtils::pathToUtf8(target),
                                                 false, result, error)){
                written++;
            }else if (failure.empty()){
                failure = error;
            }
        }
    }
    edit.commit();
    extractStatus = "Extracted " + std::to_string(written) + " clip(s)" + (failure.empty() ? std::string(".") : "; " + failure);
    if (failure.empty()) Out::info("[Aseprite] %s", extractStatus.c_str());
    else Out::warning("[Aseprite] %s", extractStatus.c_str());
}

bool AsepriteImporterView::comboRow(const char* label, const std::string& pointer, const std::vector<std::string>& names,
                                    const std::vector<std::string>& labels, const char* tooltip){
    aseViewLabel(label, tooltip);
    ImGui::SetNextItemWidth(-1);
    const bool isMixed = mixed.count(pointer) > 0;
    const Json value = workingValue(pointer);
    const std::string shown = value.is_string() ? value.get<std::string>() : value.dump();
    size_t index = names.size();
    for (size_t i = 0; i < names.size(); i++)
        if (names[i] == shown) index = i;
    bool changed = false;
    const std::string id = "##" + pointer;
    const std::string preview = isMixed ? std::string("\xE2\x80\x94") : index < labels.size() ? labels[index] : shown;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    if (ImGui::BeginCombo(id.c_str(), preview.c_str())){
        for (size_t i = 0; i < names.size(); i++){
            if (ImGui::Selectable(labels[i].c_str(), i == index && !isMixed)){
                // the combo lists strings; numbers (Max Size) go back as numbers
                Json picked = names[i];
                if (value.is_number_integer()) picked = std::stoi(names[i]);
                setWorking(pointer, picked);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (isMixed) ImGui::PopItemFlag();
    return changed;
}

bool AsepriteImporterView::boolRow(const char* label, const std::string& pointer, const char* tooltip){
    aseViewLabel(label, tooltip);
    const bool isMixed = mixed.count(pointer) > 0;
    const Json value = workingValue(pointer);
    bool on = value.is_boolean() && value.get<bool>();
    const std::string id = "##" + pointer;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    const bool changed = ImGui::Checkbox(id.c_str(), &on);
    if (isMixed) ImGui::PopItemFlag();
    if (changed) setWorking(pointer, on);
    return changed;
}

bool AsepriteImporterView::intRow(const char* label, const std::string& pointer, int lo, int hi, const char* tooltip){
    aseViewLabel(label, tooltip);
    ImGui::SetNextItemWidth(-1);
    const bool isMixed = mixed.count(pointer) > 0;
    const Json value = workingValue(pointer);
    int number = value.is_number_integer() ? value.get<int>() : lo;
    const std::string id = "##" + pointer;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    const bool changed = ImGui::InputInt(id.c_str(), &number);
    if (isMixed) ImGui::PopItemFlag();
    if (changed) setWorking(pointer, std::clamp(number, lo, hi));
    return changed;
}

bool AsepriteImporterView::textRow(const char* label, const std::string& pointer, const char* tooltip){
    aseViewLabel(label, tooltip);
    ImGui::SetNextItemWidth(-1);
    const bool isMixed = mixed.count(pointer) > 0;
    const Json value = workingValue(pointer);
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s", isMixed ? "" : value.is_string() ? value.get<std::string>().c_str() : "");
    const std::string id = "##" + pointer;
    const bool changed = ImGui::InputTextWithHint(id.c_str(), isMixed ? "\xE2\x80\x94" : "", buffer, sizeof(buffer));
    if (changed) setWorking(pointer, std::string(buffer));
    return changed;
}

bool AsepriteImporterView::listRow(const char* label, const std::string& pointer, const char* hint, const char* tooltip){
    aseViewLabel(label, tooltip);
    ImGui::SetNextItemWidth(-1);
    const bool isMixed = mixed.count(pointer) > 0;
    std::string joined;
    for (const std::string& item : aseViewStrings(workingValue(pointer)))
        joined += (joined.empty() ? "" : ", ") + item;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s", isMixed ? "" : joined.c_str());
    const std::string id = "##" + pointer;
    if (!ImGui::InputTextWithHint(id.c_str(), isMixed ? "\xE2\x80\x94" : hint, buffer, sizeof(buffer)))
        return false;
    Json list = Json::array();
    std::string text = buffer;
    size_t start = 0;
    while (start <= text.size()){
        const size_t comma = text.find(',', start);
        const std::string item = aseViewTrim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!item.empty()) list.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    setWorking(pointer, list);
    return true;
}

void AsepriteImporterView::drawGeneral(){
    comboRow("Import Mode", "/import_mode", {"sprite_sheet", "layers_as_sprites"}, {"Merge Frames", "Individual Layers"},
             "Merge Frames: the visible layers flattened into one sprite sheet. Individual Layers: one sprite per layer.");

    // Pixels Per Unit: the project's (S41) or this file's own
    aseViewLabel("Pixels Per Unit", "How many sprite pixels make one world unit.");
    {
        const bool isMixed = mixed.count("/pixels_per_unit") > 0;
        const Json value = workingValue("/pixels_per_unit");
        bool useProject = value.is_string();
        if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        if (ImGui::Checkbox("Project Default##ppu", &useProject))
            setWorking("/pixels_per_unit", useProject ? Json("project") : Json(16));
        if (isMixed) ImGui::PopItemFlag();
        if (!useProject && !isMixed){
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            int ppu = value.is_number_integer() ? value.get<int>() : 16;
            if (ImGui::InputInt("##ppu_value", &ppu))
                setWorking("/pixels_per_unit", std::clamp(ppu, 1, 4096));
        }
    }

    const ase::Document* document = files.size() == 1 && structure.document ? &*structure.document : nullptr;
    comboRow("Pivot", "/pivot/source", {"slice", "preset", "custom"}, {"From Slice", "Preset", "Custom"},
             "Where each frame's pivot comes from: a slice's pivot (else the preset), a preset point or a normalized point.");
    const std::string pivotSource = workingValue("/pivot/source").is_string() ? workingValue("/pivot/source").get<std::string>() : std::string();
    if (pivotSource == "slice"){
        std::vector<std::string> names = {""}, labels = {"Default ('pivot', else the first with a pivot)"};
        if (document)
            for (const ase::Slice& slice : document->slices)
                if (aseViewHasPivot(slice)){
                    names.push_back(slice.name);
                    labels.push_back(slice.name);
                }
        comboRow("Pivot Slice", "/pivot/slice", names, labels, "The slice whose pivot the frames take.");
    }
    if (pivotSource != "custom"){
        comboRow(pivotSource == "slice" ? "Fallback Preset" : "Preset", "/pivot/preset",
                 {"center", "top_center", "bottom_center", "left_center", "right_center", "top_left", "bottom_left", "top_right", "bottom_right"},
                 {"Center", "Top", "Bottom", "Left", "Right", "Top Left", "Bottom Left", "Top Right", "Bottom Right"}, nullptr);
    }else{
        aseViewLabel("Custom Pivot", "Normalized: (0, 0) is the bottom left, (1, 1) the top right.");
        const Json value = workingValue("/pivot/custom");
        float point[2] = {0.5f, 0.0f};
        if (value.is_array() && value.size() == 2){
            point[0] = value[0].get<float>();
            point[1] = value[1].get<float>();
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat2("##/pivot/custom", point, 0.01f, 0.0f, 1.0f, "%.2f"))
            setWorking("/pivot/custom", Json::array({std::clamp(point[0], 0.0f, 1.0f), std::clamp(point[1], 0.0f, 1.0f)}));
    }

    std::vector<std::string> borderNames = {"", "none"}, borderLabels = {"First 9-Slice", "None"};
    if (document)
        for (const ase::Slice& slice : document->slices)
            if (aseViewNinePatch(slice)){
                borderNames.push_back(slice.name);
                borderLabels.push_back(slice.name);
            }
    comboRow("Border", "/border/slice", borderNames, borderLabels,
             "The 9-slice whose centre gives the frames' border (frames with a border are not trimmed).");
    boolRow("Trim", "/trim", "Cut the fully transparent pixels around each frame.");
}

void AsepriteImporterView::toggleLayer(const ase::Document& document, int layer, bool on){
    const std::string path = document.layerPath(layer);
    std::vector<std::string> include = aseViewStrings(workingValue("/layers/include"));
    std::vector<std::string> exclude = aseViewStrings(workingValue("/layers/exclude"));
    auto drop = [&path](std::vector<std::string>& list){
        list.erase(std::remove_if(list.begin(), list.end(), [&path](const std::string& entry){ return aseViewUnder(entry, path); }), list.end());
    };
    if (on){
        drop(exclude);
        setWorking("/layers/exclude", exclude);
        // still out (hidden, an ignored prefix): named in include
        std::vector<editor::pixelart::PixelProblem> warnings;
        const std::vector<int> imported = ase::selectLayers(document, workingSettings(), warnings);
        bool any = false;
        for (int l : imported)
            if (aseViewUnder(document.layerPath(l), path)) any = true;
        if (!any){
            include.push_back(path);
            setWorking("/layers/include", include);
        }
    }else{
        drop(include);
        exclude.push_back(path);
        setWorking("/layers/include", include);
        setWorking("/layers/exclude", exclude);
    }
}

void AsepriteImporterView::drawLayerRows(const ase::Document& document, int group, const std::set<int>& imported){
    const std::vector<std::string> exclude = aseViewStrings(workingValue("/layers/exclude"));
    for (int layer : document.children(group)){
        const ase::Layer& info = document.layers[(size_t)layer];
        const std::string path = document.layerPath(layer);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(layer);
        bool on = false;
        if (info.isGroup()){
            for (int l : imported)
                if (aseViewUnder(document.layerPath(l), path)) on = true;
        }else{
            on = imported.count(layer) > 0;
        }
        // a layer under an excluded group follows the group
        bool groupExcluded = false;
        for (const std::string& entry : exclude)
            if (entry != path && aseViewUnder(path, entry)) groupExcluded = true;
        ImGui::BeginDisabled(groupExcluded || info.reference() || files.size() != 1);
        if (ImGui::Checkbox("##on", &on))
            toggleLayer(document, layer, on);
        ImGui::EndDisabled();
        if (info.reference() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Reference layers are never imported");
        ImGui::SameLine();
        const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen |
                                         (info.isGroup() ? 0 : ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        if (!info.visible()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const bool open = ImGui::TreeNodeEx("##layer", flags, "%s%s%s", info.isGroup() ? ICON_FA_FOLDER " " : "", info.name.c_str(),
                                            info.visible() ? "" : " (hidden)");
        if (!info.visible()) ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        if (!info.isGroup()) ImGui::TextUnformatted(aseViewTitle(ase::blendModeName(info.blend)).c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d%%", (int)std::lround(info.opacity * 100.0 / 255.0));
        if (info.isGroup() && open){
            drawLayerRows(document, layer, imported);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void AsepriteImporterView::drawLayers(){
    if (ImGui::BeginTable("aseprite_layers_settings", 2, ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        boolRow("Include Hidden Layers", "/layers/include_hidden", "Import the layers hidden in Aseprite too.");
        listRow("Ignore Prefixes", "/layers/ignore_prefixes", "_, ref",
                "Layers (and groups) whose names start with one of these are not imported. Comma separated.");
        ImGui::EndTable();
    }
    if (files.size() != 1){
        ImGui::TextDisabled("Select one file to pick its layers.");
        return;
    }
    if (!structure.document){
        if (!structure.error.empty()) ImGui::TextDisabled("%s", structure.error.c_str());
        return;
    }
    const ase::Document& document = *structure.document;
    std::vector<editor::pixelart::PixelProblem> warnings;
    const std::vector<int> selected = ase::selectLayers(document, workingSettings(), warnings);
    const std::set<int> imported(selected.begin(), selected.end());
    if (ImGui::BeginTable("aseprite_layer_tree", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("Blend", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Opacity", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableHeadersRow();
        drawLayerRows(document, -1, imported);
        ImGui::EndTable();
    }
    for (const editor::pixelart::PixelProblem& warning : warnings)
        aseViewProblem(warning);
}

void AsepriteImporterView::drawAnimation(){
    if (ImGui::BeginTable("aseprite_animation_settings", 2, ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        boolRow("Generate Animation Clips", "/clips/generate", "One animation clip per tag (the file's frames when it has none).");
        textRow("Events Layer", "/clips/events/layer", "The layer whose cels' user data text becomes animation events (onFootstep(\"left\")).");
        ImGui::EndTable();
    }
    if (files.size() != 1 || !structure.document)
        return;
    const ase::Document& document = *structure.document;
    if (document.tags.empty()){
        ImGui::TextDisabled("No tags: one clip of all %d frames.", document.frameCount());
        return;
    }
    const std::shared_ptr<const ase::ImportRecord> record = ase::AsepriteImporter::get().record(files[0].assetPath);
    const bool generate = workingValue("/clips/generate").is_boolean() && workingValue("/clips/generate").get<bool>();
    std::vector<std::string> tags = aseViewStrings(workingValue("/clips/tags"));
    if (ImGui::BeginTable("aseprite_tags", 6, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Tag");
        ImGui::TableSetupColumn("Frames");
        ImGui::TableSetupColumn("Direction");
        ImGui::TableSetupColumn("Repeat");
        ImGui::TableSetupColumn("Length");
        ImGui::TableHeadersRow();
        for (size_t t = 0; t < document.tags.size(); t++){
            const ase::Tag& tag = document.tags[t];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID((int)t);
            bool on = tags.empty() || std::find(tags.begin(), tags.end(), tag.name) != tags.end();
            ImGui::BeginDisabled(!generate);
            if (ImGui::Checkbox("##clip", &on)){
                // an empty list means every tag
                std::vector<std::string> next;
                for (const ase::Tag& other : document.tags){
                    const bool was = tags.empty() || std::find(tags.begin(), tags.end(), other.name) != tags.end();
                    if (other.name == tag.name ? on : was) next.push_back(other.name);
                }
                if (next.size() == document.tags.size()) next.clear();
                setWorking("/clips/tags", next);
                tags = next;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            const editor::pixelart::PixelRgba color = tag.color();
            ImGui::TextColored(ImVec4(color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1.0f), ICON_FA_TAG);
            ImGui::SameLine();
            ImGui::TextUnformatted(tag.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d-%d (%d)", tag.from, tag.to, tag.to - tag.from + 1);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(aseViewDirection(tag.direction));
            ImGui::TableSetColumnIndex(4);
            if (tag.repeat == 0) ImGui::TextUnformatted("Loop");
            else ImGui::Text("%u", (unsigned)tag.repeat);
            ImGui::TableSetColumnIndex(5);
            const ase::ImportRecord::Clip* clip = record ? record->findClip(tag.name) : nullptr;
            if (clip) ImGui::Text("%.2f s", clip->length);
            else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
}

void AsepriteImporterView::drawSlices(){
    if (files.size() != 1 || !structure.document)
        return;
    const ase::Document& document = *structure.document;
    if (document.slices.empty()){
        ImGui::TextDisabled("No slices.");
        return;
    }
    // what the settings make of each slice (the importer's rules, §6.4)
    const ase::AsepriteImportSettings settings = workingSettings();
    std::string pivotSlice, borderSlice;
    if (settings.pivotSource == ase::PivotSource::Slice){
        for (const ase::Slice& slice : document.slices)
            if (settings.pivotSlice.empty() ? slice.name == "pivot" : slice.name == settings.pivotSlice) pivotSlice = slice.name;
        if (pivotSlice.empty() && settings.pivotSlice.empty())
            for (const ase::Slice& slice : document.slices)
                if (pivotSlice.empty() && aseViewHasPivot(slice)) pivotSlice = slice.name;
    }
    if (settings.borderSlice != "none"){
        for (const ase::Slice& slice : document.slices)
            if (borderSlice.empty() && (settings.borderSlice.empty() ? aseViewNinePatch(slice) : slice.name == settings.borderSlice))
                borderSlice = slice.name;
    }
    if (ImGui::BeginTable("aseprite_slices", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("Slice");
        ImGui::TableSetupColumn("Keys");
        ImGui::TableSetupColumn("Feeds");
        ImGui::TableHeadersRow();
        for (const ase::Slice& slice : document.slices){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(slice.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%zu", slice.keys.size());
            ImGui::TableSetColumnIndex(2);
            std::string feeds;
            if (slice.name == pivotSlice) feeds += "Pivot, ";
            if (slice.name == borderSlice) feeds += "Border, ";
            feeds += aseViewSensor(slice) ? "Collider (sensor)" : "Collider";
            ImGui::TextUnformatted(feeds.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A 2D collider with Slice \"%s\" takes its box on every frame", slice.name.c_str());
        }
        ImGui::EndTable();
    }
}

void AsepriteImporterView::drawTexture(){
    if (!ImGui::BeginTable("aseprite_texture_settings", 2, ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    comboRow("Layout", "/texture/layout", {"packed", "grid"}, {"Packed", "Grid"},
             "Packed: trimmed frames packed tightly. Grid: every frame in a cell of the canvas size.");
    intRow("Padding", "/texture/padding", 0, 64, "Transparent pixels between frames.");
    intRow("Extrude", "/texture/extrude", 0, 16, "Edge pixels repeated around each frame, so filtering and mips do not bleed.");
    comboRow("Max Size", "/texture/max_size", {"256", "512", "1024", "2048", "4096", "8192"}, {"256", "512", "1024", "2048", "4096", "8192"},
             "The largest page side; more frames go to more pages.");
    comboRow("Compression", "/texture/compression", {"none", "project"}, {"None (Lossless)", "Project Default"},
             "None keeps every pixel exact. Project Default cooks the pages with the project's texture rules.");
    ImGui::EndTable();
}

void AsepriteImporterView::drawStatus(){
    ase::AsepriteImporter& importer = ase::AsepriteImporter::get();
    for (const File& file : files){
        const std::shared_ptr<const ase::ImportRecord> record = importer.record(file.assetPath);
        const bool pending = importer.isPending(file.assetPath);
        if (files.size() > 1) ImGui::TextUnformatted(fs::path(file.assetPath).filename().string().c_str());
        if (files.size() > 1) ImGui::SameLine();
        if (pending) ImGui::TextDisabled(ICON_FA_GEAR " Importing...");
        else if (!record) ImGui::TextDisabled("Not imported yet");
        else if (record->ok){
            ImGui::TextWrapped("%s", record->summary.c_str());
            UIProbe::item("asepriteImporter.summary." + file.assetPath);
            // S47 R42: who touched the file last, while this editor remembers it
            if (editor::Project* project = importer.getProject()) {
                if (const std::optional<editor::pixelart::draw::LastEdit> edit = editor::pixelart::draw::SpriteDocCache::get().lastEditOf(project->getAssetsPath() / FileUtils::pathFromUtf8(file.assetPath))) {
                    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    const int64_t seconds = (now - edit->timeMs) / 1000;
                    const std::string ago = seconds < 60 ? std::to_string(seconds) + " s" : std::to_string(seconds / 60) + " min";
                    ImGui::TextDisabled("Last edited by %s, %s ago", edit->by.c_str(), ago.c_str());
                    UIProbe::item("asepriteImporter.lastEdited." + file.assetPath);
                }
            }
        }
        if (record && !record->ok) aseViewProblem(record->error);
        if (record)
            for (const editor::pixelart::PixelProblem& warning : record->warnings)
                aseViewProblem(warning);
        for (const editor::pixelart::PixelProblem& problem : file.problems)
            aseViewProblem(problem);
    }
}

void AsepriteImporterView::drawButtons(){
    std::vector<std::string> assetPaths;
    for (const File& file : files)
        assetPaths.push_back(file.assetPath);
    if (ImGui::Button(ICON_FA_UP_RIGHT_FROM_SQUARE " Open in Aseprite"))
        ase::openInAsepriteOrFallback(project, assetPaths, false);
    UIProbe::item("asepriteImporter.openInAseprite");
    ImGui::SameLine();
    const auto& openPreview = ase::asepriteEditorHooks().openSpritePreview;
    ImGui::BeginDisabled(!openPreview);
    if (ImGui::Button(ICON_FA_EYE " Open Sprite Preview") && openPreview)
        openPreview(files[0].assetPath);
    ImGui::EndDisabled();
    UIProbe::item("asepriteImporter.openSpritePreview");
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Reimport"))
        for (const std::string& path : assetPaths)
            ase::AsepriteImporter::get().reimport(path);
    UIProbe::item("asepriteImporter.reimport");
    ImGui::SameLine();
    size_t clips = 0;
    for (const File& file : files)
        if (const std::shared_ptr<const ase::ImportRecord> record = ase::AsepriteImporter::get().record(file.assetPath))
            clips += record->clips.size();
    ImGui::BeginDisabled(clips == 0);
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Extract Clips..."))
        openExtract = true;
    ImGui::EndDisabled();
    UIProbe::item("asepriteImporter.extractClips");
    if (clips == 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("No clips imported yet");

    if (openExtract){
        ImGui::OpenPopup("Extract Clips##aseprite_extract");
        openExtract = false;
    }
    if (EditorModal::begin("Extract Clips##aseprite_extract", nullptr, ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::Text("Writes %zu .anim file(s) next to the selected file(s), one undo step:", clips);
        std::string listed;
        for (const File& file : files){
            const std::shared_ptr<const ase::ImportRecord> record = ase::AsepriteImporter::get().record(file.assetPath);
            if (!record) continue;
            const std::string stem = FileUtils::pathToUtf8(file.absolute.stem());
            for (const ase::ImportRecord::Clip& clip : record->clips){
                ImGui::BulletText("%s %s.anim", stem.c_str(), clip.name.c_str());
                listed += (listed.empty() ? "" : ", ") + stem + " " + clip.name + ".anim";
            }
        }
        ImGui::TextDisabled("A file that exists already is updated and keeps its GUID.");
        EditorModal::message("Writes " + std::to_string(clips) + " .anim file(s) next to the selected file(s), one undo step: " + listed +
                             ". A file that exists already is updated and keeps its GUID.");
        ImGui::Separator();
        if (EditorModal::button("Extract", "ok", ImVec2(Theme::dpi(100.0f), 0))){
            extractClips();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel", ImVec2(Theme::dpi(100.0f), 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!extractStatus.empty())
        ImGui::TextDisabled("%s", extractStatus.c_str());
}

bool AsepriteImporterView::draw(const std::vector<fs::path>& selection){
    select(selection);
    if (files.empty())
        return false;
    readStructure();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ICON_FA_IMAGE);
    ImGui::SameLine();
    if (files.size() == 1)
        ImGui::TextUnformatted(fs::path(files[0].assetPath).filename().string().c_str());
    else
        ImGui::Text("%zu Aseprite files", files.size());
    ImGui::TextDisabled("Aseprite Importer");
    ImGui::Separator();

    drawStatus();
    ImGui::Separator();

    // a meta written by a newer editor is read with the keys this one knows, and is not written
    bool readOnly = false;
    for (const File& file : files)
        if (file.settings.readOnly()) readOnly = true;
    if (readOnly)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Import settings from a newer editor: read only here.");
    ImGui::BeginDisabled(readOnly);

    auto section = [&](const char* title, auto&& body){
        const bool open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen);
        UIProbe::item(std::string("asepriteImporter.section.") + title);
        if (!open)
            return;
        ImGui::PushID(title);
        body();
        ImGui::PopID();
    };
    auto settingsTable = [&](const char* id, auto&& rows){
        if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)){
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
            rows();
            ImGui::EndTable();
        }
    };
    section("General", [&]{ settingsTable("aseprite_general", [&]{ drawGeneral(); }); });
    section("Layers", [&]{ drawLayers(); });
    section("Animation", [&]{ drawAnimation(); });
    section("Slices", [&]{ drawSlices(); });
    section("Texture", [&]{ drawTexture(); });

    for (const editor::pixelart::PixelProblem& problem : applyProblems)
        aseViewProblem(problem);
    ImGui::Separator();
    ImGui::BeginDisabled(touched.empty());
    if (ImGui::Button("Revert"))
        revert();
    UIProbe::item("asepriteImporter.revert");
    ImGui::SameLine();
    if (ImGui::Button("Apply"))
        apply();
    UIProbe::item("asepriteImporter.apply");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::Separator();
    drawButtons();
    return true;
}
