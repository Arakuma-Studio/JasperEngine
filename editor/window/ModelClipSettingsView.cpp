// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelClipSettingsView.h"

#include "InspectorWindowClips.h"
#include "InspectorVatBake.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/ClipConverters.h"
#include "animation/ModelClipSettingsFile.h"
#include "command/CommandHistory.h"
#include "command/type/ClipSceneActions.h"
#include "command/type/WriteFilesCmd.h"
#include "external/IconsFontAwesome6.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/UIProbe.h"
#include "window/ImporterFields.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    constexpr const char* MIXED_DASH = "\xE2\x80\x94";
    // a frame's share of the measuring (each row builds its clip and poses it twice)
    constexpr double MEASURE_BUDGET_MS = 6.0;

    const ClipWrapMode wrapChoices[] = {ClipWrapMode::Once, ClipWrapMode::Loop, ClipWrapMode::PingPong, ClipWrapMode::ClampForever};

    std::string clipNameLower(std::string text){
        for (char& c : text) c = (char)std::tolower((unsigned char)c);
        return text;
    }

    bool sameClipSetting(const anim::ModelClipSetting* a, const anim::ModelClipSetting* b){
        const bool aEmpty = !a || a->empty();
        const bool bEmpty = !b || b->empty();
        if (aEmpty || bEmpty) return aEmpty == bEmpty;
        return *a == *b;
    }

    // "Auto, Bake Y": the bone (Auto: the Animator's choice) and what stays in the pose
    std::string clipRootMotionText(const anim::ClipRootMotion& rootMotion){
        std::string text = rootMotion.bone.empty() ? std::string("Auto") : rootMotion.bone.substr(rootMotion.bone.find_last_of('/') + 1);
        std::string baked;
        if (rootMotion.bakeRotation) baked += " Rotation";
        if (rootMotion.bakePositionY) baked += " Y";
        if (rootMotion.bakePositionXZ) baked += " XZ";
        return baked.empty() ? text : text + ", Bake" + baked;
    }

    // a file name of a clip name (names come from the model: '/' and the like are not ours)
    std::string clipFileNameOf(const std::string& clip){
        std::string name = clip;
        for (char& c : name){
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        }
        return name.empty() ? std::string("Clip") : name;
    }

    // a cell the row's selectable spans: its text is an item, so find_ui_item reads it
    void clipCell(const std::string& text, const std::string& id, bool subtle){
        ImGui::TableNextColumn();
        if (subtle) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::Selectable((text + "##" + id).c_str(), false, ImGuiSelectableFlags_None);
        if (subtle) ImGui::PopStyleColor();
    }

    void clipSettingLabel(const char* label, const char* tooltip){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::TableSetColumnIndex(1);
    }

}

ModelClipSettingsView::ModelClipSettingsView(Project* project): project(project) {
    // the prompt for edits not applied when the model, the editor or Play moves on (S53 R17's)
    ImporterPrompt::add(this, {[this](){ return hasPendingEdits(); },
                               [this](){ return fs::path(model).filename().string() + " (clips)"; },
                               [this](){ apply(); }, [this](){ revert(); }});
}

ModelClipSettingsView::~ModelClipSettingsView(){
    ImporterPrompt::remove(this);
}

bool ModelClipSettingsView::namesACycle(const std::string& clip){
    const std::string name = clipNameLower(clip);
    for (const char* word : {"loop", "idle", "walk", "run", "cycle"}){
        if (name.find(word) != std::string::npos) return true;
    }
    return false;
}

ModelClipSettingsView::Stamp ModelClipSettingsView::stampOf(const fs::path& path) const{
    Stamp stamp;
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return stamp;
    stamp.size = fs::file_size(path, ec);
    if (ec) return Stamp();
    stamp.time = (int64_t)time.time_since_epoch().count();
    stamp.exists = true;
    return stamp;
}

const anim::ModelClipSetting* ModelClipSettingsView::storedSetting(const std::string& clip) const{
    return stored.find(clip);
}

anim::ModelClipSetting ModelClipSettingsView::workingSetting(const std::string& clip) const{
    const auto found = working.find(clip);
    return found == working.end() ? anim::ModelClipSetting() : found->second;
}

ClipWrapMode ModelClipSettingsView::wrapOf(const std::string& clip) const{
    return workingSetting(clip).wrap.value_or(ClipWrapMode::Once);
}

anim::ClipRootMotion ModelClipSettingsView::rootMotionOf(const std::string& clip) const{
    return workingSetting(clip).rootMotion.value_or(anim::ClipRootMotion());
}

std::vector<std::string> ModelClipSettingsView::selectedInOrder() const{
    std::vector<std::string> names;
    for (const Row& row : rows){
        if (selected.count(row.info.name)) names.push_back(row.info.name);
    }
    return names;
}

std::string ModelClipSettingsView::shownClip() const{
    const std::vector<std::string> names = selectedInOrder();
    return names.empty() ? std::string() : names.front();
}

bool ModelClipSettingsView::hasPendingEdits() const{
    for (const auto& entry : working){
        if (!sameClipSetting(&entry.second, storedSetting(entry.first))) return true;
    }
    for (const auto& entry : stored.clips){
        if (!working.count(entry.first) && !entry.second.empty()) return true;
    }
    return false;
}

void ModelClipSettingsView::readStored(){
    readError.clear();
    if (!readModelClipSettingsFile(file, stored, readError)) stored = anim::ModelClipSettings();
    working = stored.clips;
    sidecarStamp = stampOf(FileUtils::pathFromUtf8(anim::modelClipSettingsPath(FileUtils::pathToUtf8(file))));
}

void ModelClipSettingsView::show(const fs::path& newFile, const std::string& newModel, const std::vector<anim::ModelClipInfo>& clips){
    std::string key;
    for (const anim::ModelClipInfo& info : clips) key += info.name + "\n";
    const Stamp nowModel = stampOf(newFile);
    const Stamp nowSidecar = stampOf(FileUtils::pathFromUtf8(anim::modelClipSettingsPath(FileUtils::pathToUtf8(newFile))));
    const bool otherModel = newFile != file;
    if (otherModel && hasPendingEdits()){
        // the prompt decides; the edited model stays shown until it is answered
        if (promptedModel != newModel){
            promptedModel = newModel;
            ImporterPrompt::guard(nullptr);
        }
        return;
    }
    promptedModel.clear();
    if (!otherModel && key == clipsKey && nowModel == modelStamp && nowSidecar == sidecarStamp) return;

    const bool sameModel = !otherModel;
    const bool modelChanged = !(nowModel == modelStamp);
    file = newFile;
    model = newModel;
    clipsKey = key;
    modelStamp = nowModel;
    // the settings again (an undo, another tool, a hand edit), unless they are being edited
    if (!sameModel || !hasPendingEdits()) readStored();
    else sidecarStamp = nowSidecar;
    rows.clear();
    for (const anim::ModelClipInfo& info : clips){
        Row row;
        row.info = info;
        rows.push_back(row);
    }
    if (!sameModel){
        selected.clear();
        anchor.clear();
        actionError.clear();
        rig.reset();
        rigRead = false;
    }else if (modelChanged){
        rig.reset();
        rigRead = false;
    }
    std::set<std::string> kept;
    for (const Row& row : rows){
        if (selected.count(row.info.name)) kept.insert(row.info.name);
    }
    selected = kept;
    if (selected.empty() && !rows.empty()) selected.insert(rows.front().info.name);
}

void ModelClipSettingsView::measure(){
    if (!rigRead){
        rigRead = true;
        auto made = std::make_unique<anim::ClipRig>();
        std::string error;
        if (anim::clipRigFromModel(FileUtils::pathToGenericUtf8(file), *made, &error)) rig = std::move(made);
    }
    const auto start = std::chrono::steady_clock::now();
    for (Row& row : rows){
        if (row.measured) continue;
        anim::ClipReportRequest request;
        request.reference = anim::formatSubClipReference(FileUtils::pathToGenericUtf8(file), row.info.name);
        request.rig = rig.get();
        request.loopSeam = true;
        row.report = anim::analyzeClip(request);
        row.measured = true;
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > MEASURE_BUDGET_MS) break;
    }
}

std::vector<std::string> ModelClipSettingsView::suggestedLoops() const{
    // Mixamo names every clip "mixamo.com" and the action after the '@' of the file name
    // ("hero@Running.fbx"): that part names the file's longest clip (the others are T-poses)
    const std::string stem = FileUtils::pathToUtf8(file.stem());
    const size_t at = stem.find('@');
    const std::string fileAction = at == std::string::npos ? std::string() : stem.substr(at + 1);
    const Row* longest = nullptr;
    for (const Row& row : rows){
        if (!longest || row.info.duration > longest->info.duration) longest = &row;
    }
    std::vector<std::string> names;
    for (const Row& row : rows){
        if (!selected.count(row.info.name)) continue;
        const std::string& name = row.info.name;
        const anim::ModelClipSetting* setting = storedSetting(name);
        if (setting && !setting->empty()) continue;
        if (!sameClipSetting(working.count(name) ? &working.at(name) : nullptr, setting)) continue;
        const bool cycle = namesACycle(name) || (&row == longest && namesACycle(fileAction));
        if (!cycle || !row.measured || !row.report.loopSeam) continue;
        if (row.report.loopSeam->maxErrorMm < LOOP_SUGGESTION_MM) names.push_back(name);
    }
    return names;
}

void ModelClipSettingsView::draw(const fs::path& newFile, const std::string& newModel, const std::vector<anim::ModelClipInfo>& clips){
    show(newFile, newModel, clips);
    measure();

    ImGui::SeparatorText("Animation Clips");
    if (rows.empty()){
        ImGui::TextDisabled("The model has no animations.");
        return;
    }
    drawTable();
    drawSettings();
    drawActions();
}

void ModelClipSettingsView::drawTable(){
    const bool bakeColumn = vatbakeentry::available();
    const int columns = bakeColumn ? 8 : 7;
    // the Inspector's width, no horizontal scroll: a row is a whole item there (find_ui_item, clicks)
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY;
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().CellPadding.y * 2.0f;
    const float height = std::min((float)(rows.size() + 1) * rowHeight + ImGui::GetStyle().ScrollbarSize, 12.0f * rowHeight);
    if (!ImGui::BeginTable("model_clip_settings_table", columns, flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Length");
    ImGui::TableSetupColumn("Wrap Mode");
    ImGui::TableSetupColumn("Root Motion");
    ImGui::TableSetupColumn("Memory");
    ImGui::TableSetupColumn("Keys");
    ImGui::TableSetupColumn("Loop Match");
    if (bakeColumn) ImGui::TableSetupColumn("Bake");
    ImGui::TableHeadersRow();
    const bool playing = project->isAnyScenePlaying();
    for (size_t r = 0; r < rows.size(); r++){
        const Row& row = rows[r];
        const std::string& name = row.info.name;
        const std::string id = std::to_string(row.info.index);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool isSelected = selected.count(name) > 0;
        if (ImGui::Selectable((name + "##model_clip." + id).c_str(), isSelected,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)){
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && !anchor.empty()){
                // the rows from the anchor to this one
                size_t from = r;
                for (size_t i = 0; i < rows.size(); i++){
                    if (rows[i].info.name == anchor) from = i;
                }
                if (!io.KeyCtrl) selected.clear();
                for (size_t i = std::min(from, r); i <= std::max(from, r); i++) selected.insert(rows[i].info.name);
            }else if (io.KeyCtrl){
                if (isSelected && selected.size() > 1) selected.erase(name);
                else selected.insert(name);
                anchor = name;
            }else{
                selected = {name};
                anchor = name;
            }
        }
        UIProbe::item("modelClips.row." + id);
        clipCell(inspectorclips::formatSeconds(row.info.duration), "model_clip.length." + id, true);
        clipCell(inspectorclips::wrapLabel(wrapOf(name)), "model_clip.wrap." + id, false);
        clipCell(clipRootMotionText(rootMotionOf(name)), "model_clip.root_motion." + id, false);
        const bool known = row.measured && row.report.ok;
        clipCell(known ? anim::formatClipField("runtime_bytes", (double)row.report.runtimeBytes) : std::string(row.measured ? "-" : "..."),
             "model_clip.memory." + id, true);
        clipCell(known ? anim::formatClipField("keys", (double)row.report.keys) : std::string(row.measured ? "-" : "..."),
             "model_clip.keys." + id, true);
        const bool seam = known && row.report.loopSeam.has_value();
        clipCell(seam ? anim::formatClipField("loop_seam.max_error_mm", row.report.loopSeam->maxErrorMm) : std::string(row.measured ? "-" : "..."),
             "model_clip.loop_match." + id, true);
        if (seam && ImGui::IsItemHovered() && !row.report.loopSeam->worstJoint.empty()){
            ImGui::SetTooltip("The pose at the end against the start, the root's travel taken out: worst at %s",
                              row.report.loopSeam->worstJoint.c_str());
        }
        if (bakeColumn){
            // Bake ▾ › Vertex Animation… of this clip (no scene: the model file is the source)
            ImGui::TableNextColumn();
            ImGui::PushID(row.info.index);
            VatBakeEntry entry;
            entry.kind = VatBakeEntry::Kind::ModelClip;
            entry.asset = FileUtils::pathToUtf8(file);
            entry.clip = name;
            vatbakeentry::bakeButton(project, entry, !playing, "model_clip_bake", "modelAsset.bake." + id, true);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void ModelClipSettingsView::drawSettings(){
    const std::vector<std::string> names = selectedInOrder();
    if (names.empty()) return;
    if (names.size() > 1) ImGui::TextDisabled("%zu clips selected", names.size());
    if (!readError.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", readError.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!readError.empty());

    // each field's value over the selection: mixed when the clips differ
    const ClipWrapMode wrap = wrapOf(names.front());
    const anim::ClipRootMotion rootMotion = rootMotionOf(names.front());
    bool wrapMixed = false, boneMixed = false, rotationMixed = false, yMixed = false, xzMixed = false;
    for (const std::string& name : names){
        const anim::ClipRootMotion other = rootMotionOf(name);
        wrapMixed = wrapMixed || wrapOf(name) != wrap;
        boneMixed = boneMixed || other.bone != rootMotion.bone;
        rotationMixed = rotationMixed || other.bakeRotation != rootMotion.bakeRotation;
        yMixed = yMixed || other.bakePositionY != rootMotion.bakePositionY;
        xzMixed = xzMixed || other.bakePositionXZ != rootMotion.bakePositionXZ;
    }
    auto editRootMotion = [&](auto change){
        for (const std::string& name : names){
            anim::ClipRootMotion value = rootMotionOf(name);
            value.present = true;
            change(value);
            working[name].rootMotion = value;
        }
    };

    if (ImGui::BeginTable("model_clip_settings_fields", 2, ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Root Transform Position (XZ)").x +
                                ImGui::GetStyle().ItemSpacing.x);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        clipSettingLabel("Wrap Mode", "How the clip plays past its end. Once: the model's own (glTF has no loop flag)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##model_clip_wrap", wrapMixed ? MIXED_DASH : inspectorclips::wrapLabel(wrap))){
            for (ClipWrapMode choice : wrapChoices){
                if (ImGui::Selectable(inspectorclips::wrapLabel(choice), !wrapMixed && choice == wrap)){
                    for (const std::string& name : names) working[name].wrap = choice;
                }
            }
            ImGui::EndCombo();
        }

        clipSettingLabel("Root Motion Node", "The bone the root motion is read from. Auto: the Animator's Root Motion bone, the hips, "
                                         "then the shallowest bone the clip moves");
        char bone[256];
        std::snprintf(bone, sizeof(bone), "%s", boneMixed ? "" : rootMotion.bone.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##model_clip_root_motion_bone", boneMixed ? MIXED_DASH : "Auto", bone, sizeof(bone),
                                     ImGuiInputTextFlags_EnterReturnsTrue)){
            const std::string value = bone;
            editRootMotion([&](anim::ClipRootMotion& changed){ changed.bone = value; });
        }

        struct BakeRow {
            const char* label;
            const char* id;
            bool anim::ClipRootMotion::* member;
            bool mixed;
            const char* tooltip;
        };
        const BakeRow bakes[] = {
            {"Root Transform Rotation", "Bake Into Pose##model_clip_bake_rotation", &anim::ClipRootMotion::bakeRotation, rotationMixed,
             "Keep the heading in the pose: no turning root motion"},
            {"Root Transform Position (Y)", "Bake Into Pose##model_clip_bake_y", &anim::ClipRootMotion::bakePositionY, yMixed,
             "Keep the height in the pose"},
            {"Root Transform Position (XZ)", "Bake Into Pose##model_clip_bake_xz", &anim::ClipRootMotion::bakePositionXZ, xzMixed,
             "Keep the horizontal motion in the pose"},
        };
        for (const BakeRow& row : bakes){
            clipSettingLabel(row.label, row.tooltip);
            bool value = rootMotion.*row.member;
            if (row.mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            const bool changed = ImGui::Checkbox(row.id, &value);
            if (row.mixed) ImGui::PopItemFlag();
            if (changed) editRootMotion([&](anim::ClipRootMotion& edited){ edited.*row.member = value; });
        }
        ImGui::EndTable();
    }

    // R30: shown, never applied by itself
    const std::vector<std::string> loops = suggestedLoops();
    if (!loops.empty()){
        ImGui::AlignTextToFramePadding();
        if (loops.size() == 1) ImGui::TextColored(Theme::Colors::WarningText, "Suggestion: looks like a loop");
        else ImGui::TextColored(Theme::Colors::WarningText, "Suggestion: %zu clips look like loops", loops.size());
        ImGui::SetItemTooltip("A cycle's name and a Loop Match under %.0f mm: Use sets Wrap Mode to Loop (then Apply)", LOOP_SUGGESTION_MM);
        ImGui::SameLine();
        if (ImGui::SmallButton("Use##model_clip_suggestion")){
            for (const std::string& name : loops) working[name].wrap = ClipWrapMode::Loop;
        }
    }

    const bool pending = hasPendingEdits();
    ImGui::BeginDisabled(!pending);
    if (ImGui::Button("Revert##model_clip_settings")) revert();
    ImGui::SameLine();
    if (ImGui::Button("Apply##model_clip_settings")) apply();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void ModelClipSettingsView::drawActions(){
    const std::vector<std::string> names = selectedInOrder();
    const bool playing = project->isAnyScenePlaying();
    ImGui::BeginDisabled(playing || names.empty());
    const std::string count = names.size() > 1 ? " (" + std::to_string(names.size()) + ")" : std::string();
    if (ImGui::Button((ICON_FA_FILE_EXPORT " Extract to .anim..." + count + "##model_clips_extract").c_str())) extractSelected();
    ImGui::SetItemTooltip("The selected clips as editable .anim files, in one folder (one undo step); the model is unchanged");
    ImGui::SameLine();
    if (ImGui::Button((ICON_FA_DIAGRAM_PROJECT " Add to Animator..." + count + "##model_clips_add").c_str())){
        // the project's controllers, listed once as the menu opens
        controllers.clear();
        std::error_code ec;
        for (fs::recursive_directory_iterator it(project->getAssetsPath(), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)){
            if (it->is_regular_file(ec) && clipNameLower(it->path().extension().string()) == ".controller"){
                controllers.push_back(FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(it->path())));
            }
        }
        std::sort(controllers.begin(), controllers.end());
        ImGui::OpenPopup("##model_clips_add_menu");
    }
    ImGui::SetItemTooltip("A state per selected clip in an Animator Controller's first layer (one undo step)");
    if (ImGui::BeginPopup("##model_clips_add_menu")){
        for (const std::string& controller : controllers){
            if (ImGui::MenuItem(controller.c_str())) addSelectedToAnimator(controller);
        }
        if (!controllers.empty()) ImGui::Separator();
        if (ImGui::MenuItem("New Animator Controller...")) addSelectedToAnimator("");
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    if (!actionError.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", actionError.c_str());
        ImGui::PopStyleColor();
    }
}

void ModelClipSettingsView::revert(){
    readStored();
}

void ModelClipSettingsView::apply(){
    if (!hasPendingEdits()) return;
    anim::ModelClipSettings settings;
    std::string error;
    // the file as it is now (unknown keys, other clips' entries), with the edited clips
    if (!readModelClipSettingsFile(file, settings, error)){
        readError = error;
        return;
    }
    for (const auto& entry : working){
        if (sameClipSetting(&entry.second, storedSetting(entry.first))) continue;
        if (entry.second.empty()) settings.clips.erase(entry.first);
        else settings.clips[entry.first] = entry.second;
    }
    for (const auto& entry : stored.clips){
        if (!working.count(entry.first)) settings.clips.erase(entry.first);
    }
    if (!writeModelClipSettingsFile(project, file, settings, "Set Model Clip Settings", error)){
        actionError = error;
        Out::error("%s", error.c_str());
        return;
    }
    actionError.clear();
    readStored();
}

void ModelClipSettingsView::extractSelected(){
    actionError.clear();
    const std::vector<std::string> names = selectedInOrder();
    if (names.empty()) return;
    const fs::path start = file.parent_path();
    const std::string chosen = FileDialogs::openFileDialog(FileUtils::pathToGenericUtf8(start), FILE_DIALOG_ALL, true);
    if (chosen.empty()) return;
    const fs::path folder = FileUtils::pathFromUtf8(chosen).lexically_normal();
    if (!project->isInsideAssetsPath(folder)){
        actionError = "Extract to .anim: choose a folder inside the project's assets";
        return;
    }
    std::vector<WriteFilesCmd::Item> items;
    std::set<fs::path> taken;
    for (const std::string& name : names){
        const std::string reference = anim::formatSubClipReference(model, name);
        ClipData data;
        std::vector<ClipWarning> warnings;
        std::string error;
        if (!ClipConverters::extractModelSubClip(reference, data, warnings, &error)){
            actionError = "Extract to .anim: " + error;
            return;
        }
        data.previewModel = model;
        // a new file each, never over an existing one
        fs::path target = folder / FileUtils::pathFromUtf8(clipFileNameOf(name) + ".anim");
        std::error_code ec;
        for (int k = 2; (fs::exists(target, ec) || taken.count(target)) && k < 1000; k++){
            target = folder / FileUtils::pathFromUtf8(clipFileNameOf(name) + " " + std::to_string(k) + ".anim");
        }
        taken.insert(target);
        items.push_back({target, ClipSceneActions::newClipText(project, data)});
    }
    auto* command = new WriteFilesCmd(std::move(items), [project = project](const std::vector<fs::path>& paths){
        ProjectFileWrites::filesWritten(project, paths);
    });
    command->setName(names.size() == 1 ? "Extract to .anim" : "Extract " + std::to_string(names.size()) + " Clips to .anim");
    command->setNoMerge();
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)){
        actionError = "Extract to .anim: " + (failure->empty() ? std::string("the files could not be written") : *failure);
        return;
    }
    Out::info("Extracted %zu clip(s) of %s to %s (one project undo step)", names.size(), model.c_str(),
              FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(folder)).c_str());
}

void ModelClipSettingsView::addSelectedToAnimator(const std::string& controller){
    actionError.clear();
    const std::vector<std::string> names = selectedInOrder();
    if (names.empty()) return;
    std::shared_ptr<AnimatorAssetDocument> document;
    anim::ControllerDef def;
    fs::path target;
    std::string error;
    if (controller.empty()){
        const std::string chosen = FileDialogs::saveFileDialog(FileUtils::pathToGenericUtf8(file.parent_path()),
                                                               FileUtils::pathToUtf8(file.stem()) + ".controller", FILE_DIALOG_ANIMATOR_CONTROLLER);
        if (chosen.empty()) return;
        target = FileUtils::pathFromUtf8(chosen).lexically_normal();
        if (target.extension() != ".controller") target += ".controller";
        std::error_code ec;
        if (!project->isInsideAssetsPath(target) || fs::exists(target, ec)){
            actionError = "Add to Animator: choose a new .controller inside the project's assets";
            return;
        }
        def.guid = anim::generateClipGuid();
    }else{
        document = AnimatorAssetDocument::open(project, controller, &error);
        if (!document){
            actionError = "Add to Animator: " + error;
            return;
        }
        def = document->controller();
    }
    if (def.layers.empty()){
        anim::LayerDef layer;
        layer.name = "Base Layer";
        def.layers.push_back(layer);
    }
    anim::StateMachineDef& machine = def.layers[0].machine;
    const bool hadStates = !machine.states.empty();
    size_t cell = machine.states.size();
    std::string first;
    for (const std::string& name : names){
        // a name of its own in the layer ("Walk", "Walk 2"...)
        std::string stateName = name;
        for (int k = 2; std::any_of(machine.states.begin(), machine.states.end(), [&](const anim::StateDef& s){ return s.name == stateName; }); k++){
            stateName = name + " " + std::to_string(k);
        }
        anim::StateDef state;
        state.name = stateName;
        state.motion.clip.path = anim::formatSubClipReference(model, name);
        state.position = Vector2(240.0f + 220.0f * (float)(cell % 4), 120.0f + 90.0f * (float)(cell / 4));
        cell++;
        machine.states.push_back(state);
        if (first.empty()) first = stateName;
    }
    if (!hadStates && machine.defaultState.empty()) machine.defaultState = first;
    const std::string undoName = names.size() == 1 ? "Add to Animator" : "Add " + std::to_string(names.size()) + " States to Animator";
    if (document){
        if (!document->commit(def, undoName, "", &error)){
            actionError = "Add to Animator: " + error;
            return;
        }
        Out::info("Added %zu state(s) to %s (one project undo step)", names.size(), controller.c_str());
        return;
    }
    anim::normalizeController(def);
    auto* command = new WriteFilesCmd({{target, anim::writeController(def)}}, [project = project](const std::vector<fs::path>& paths){
        ProjectFileWrites::filesWritten(project, paths);
    });
    command->setName(undoName);
    command->setNoMerge();
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)){
        actionError = "Add to Animator: " + (failure->empty() ? std::string("the file could not be written") : *failure);
        return;
    }
    Out::info("Created %s with %zu state(s) (one project undo step)",
              FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(target)).c_str(), names.size());
}
