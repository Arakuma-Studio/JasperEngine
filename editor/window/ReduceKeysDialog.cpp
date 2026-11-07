// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ReduceKeysDialog.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipOptimize.h"
#include "animation/ClipReduction.h"
#include "animation/ClipReport.h"
#include "animation/ModelSubClips.h"
#include "command/type/ClipSceneActions.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    constexpr double kDebounceSeconds = 0.25;

    // One run on a worker. The dialog keeps the latest; a run it dropped sees `cancel` and ends.
    struct Run{
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        anim::ClipRig rig;
        std::string rigError;
        anim::ClipOptimizeResult result;
        bool verified = false;
        std::string verifyError;
    };

    struct DialogState{
        bool requested = false;
        std::string reference;      // project-relative
        std::string poolReference;  // what the pool takes
        bool subClip = false;
        float toleranceMm = 1.0f;
        float skinDistanceMm = 100.0f;
        char output[512] = "";
        bool replace = false;
        bool overwrite = false;
        std::string error;
        std::shared_ptr<Run> run;
        std::chrono::steady_clock::time_point changedAt;
        bool pending = false;
        std::string writtenTo;
    };

    DialogState& state(){
        static DialogState instance;
        return instance;
    }

    double secondsSince(std::chrono::steady_clock::time_point at){
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - at).count();
    }

    void startPreview(DialogState& s){
        if (s.run) s.run->cancel = true;
        auto run = std::make_shared<Run>();
        s.run = run;
        s.pending = false;
        const std::string reference = s.poolReference;
        const std::string display = s.reference;
        const double tolerance = s.toleranceMm, skin = s.skinDistanceMm;
        const bool inPlace = s.replace;
        std::thread([run, reference, display, tolerance, skin, inPlace](){
            std::string error;
            const std::string model = anim::clipRigModelOf(reference);
            if (model.empty()){
                run->rigError = "This clip names no model to measure on. Set its Preview Model in Settings first.";
            }else if (!anim::clipRigFromModel(model, run->rig, &error)){
                run->rigError = error;
            }else{
                anim::ClipOptimizeRequest request;
                request.reference = reference;
                request.displayName = display;
                request.rig = &run->rig;
                request.reduction.toleranceMm = tolerance;
                request.reduction.skinDistanceMm = skin;
                request.inPlace = inPlace;
                request.cancel = &run->cancel;
                run->result = anim::runClipOptimize(request);
            }
            run->done = true;
        }).detach();
    }

    std::string defaultOutput(const std::string& reference){
        anim::SubClipReference parts;
        if (!anim::parseSubClipReference(reference, parts)){
            const fs::path file = FileUtils::pathFromUtf8(reference);
            return FileUtils::pathToGenericUtf8(file.parent_path() / FileUtils::pathFromUtf8(FileUtils::pathToUtf8(file.stem()) + ".opt.anim"));
        }
        const fs::path model = FileUtils::pathFromUtf8(parts.model);
        std::string name = FileUtils::pathToUtf8(model.stem()) + "_" + parts.clip;
        for (char& c : name){
            if (std::strchr("/\\:|?*\"<>", c)) c = '_';
        }
        return FileUtils::pathToGenericUtf8(model.parent_path() / FileUtils::pathFromUtf8(name + ".opt.anim"));
    }

    // label, then its value as a selectable so tools can read the drawn row (find_ui_item)
    void row(const char* key, const std::string& value, const char* id){
        const anim::ClipReportField* field = anim::findClipReportField(key);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(field ? field->label : key);
        ImGui::TableNextColumn();
        ImGui::Selectable((value + "##" + id).c_str(), false);
        UIProbe::item(id);
    }

    // writes what the current run measured: a project step; the source only when Replace is on
    bool writeResult(Project* project, DialogState& s, std::string& error){
        const fs::path root = project->getProjectPath();
        const std::string output = s.replace ? s.reference : std::string(s.output);
        if (output.size() < 5 || output.compare(output.size() - 5, 5, ".anim") != 0){
            error = "Output must be a project-relative .anim";
            return false;
        }
        const fs::path absolute = (root / FileUtils::pathFromUtf8(output)).lexically_normal();
        std::error_code ec;
        const bool exists = fs::exists(absolute, ec);
        if (exists && !s.replace && !s.overwrite){
            error = "'" + output + "' exists: choose another name, or tick Overwrite";
            return false;
        }
        ClipData data = s.run->result.reduced;
        if (exists){
            std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, output, &error);
            if (!document) return false;
            data.guid = document->data().guid;
            if (!document->commit(data, "Reduce Keys", "", &error)) return false;
        }else{
            data.guid.clear();
            if (!AnimationClipDocument::create(project, output, data, "Reduce Keys", &error)) return false;
        }
        // from the file, off this thread: the number shown is what the file holds
        std::shared_ptr<Run> run = s.run;
        run->done = false;
        s.writtenTo = output;
        std::thread([run, absolute](){
            run->verified = anim::verifyWrittenClip(absolute, run->rig, run->result, run->verifyError);
            run->done = true;
        }).detach();
        return true;
    }

}

void ReduceKeysDialog::open(Project* project, const std::string& reference){
    DialogState& s = state();
    if (s.run) s.run->cancel = true;
    s = DialogState();
    s.requested = true;
    s.reference = reference;
    anim::SubClipReference parts;
    s.subClip = anim::parseSubClipReference(reference, parts);
    const fs::path root = project->getProjectPath();
    if (s.subClip){
        s.poolReference = FileUtils::pathToGenericUtf8((root / FileUtils::pathFromUtf8(parts.model)).lexically_normal()) + "#" + parts.clip;
    }else{
        s.poolReference = FileUtils::pathToGenericUtf8((root / FileUtils::pathFromUtf8(reference)).lexically_normal());
    }
    std::snprintf(s.output, sizeof(s.output), "%s", defaultOutput(reference).c_str());
    startPreview(s);
}

void ReduceKeysDialog::draw(Project* project){
    DialogState& s = state();
    if (s.requested){
        s.requested = false;
        ImGui::OpenPopup("Reduce Keys###ReduceKeys");
    }
    if (!EditorModal::begin("Reduce Keys###ReduceKeys", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(s.reference.c_str());
    ImGui::TextColored(Theme::Colors::SubtleText, "Removes keys a skeleton does not need, and measures what that does. The gain\n"
                       "is often small, and it can be none: read the numbers before you write.");

    const bool written = !s.writtenTo.empty();
    auto changed = [&s](){
        s.pending = true;
        s.changedAt = std::chrono::steady_clock::now();
    };
    ImGui::BeginDisabled(written);
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    if (EditorModal::field("tolerance_mm", s.toleranceMm)) changed();
    if (ImGui::DragFloat("Error Tolerance##reduce_tolerance", &s.toleranceMm, 0.02f, 0.01f, 100.0f, "%.2f mm", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)){
        changed();
    }
    ImGui::SetItemTooltip("The most any joint, or a point at Skin Distance from it, may end up from where it was, measured through the runtime sampler");
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    if (EditorModal::field("skin_distance_mm", s.skinDistanceMm)) changed();
    if (ImGui::DragFloat("Skin Distance##reduce_skin", &s.skinDistanceMm, 1.0f, (float)anim::kClipReductionSkinFloorMm, 1000.0f, "%.0f mm", ImGuiSliderFlags_AlwaysClamp)){
        changed();
    }
    ImGui::SetItemTooltip("How far from each joint the error is measured: where the mesh it moves is");
    ImGui::EndDisabled();
    // an agent's value goes through the same ranges as a drag's
    s.toleranceMm = std::clamp(s.toleranceMm, 0.01f, 100.0f);
    s.skinDistanceMm = std::clamp(s.skinDistanceMm, (float)anim::kClipReductionSkinFloorMm, 1000.0f);
    if (s.pending && secondsSince(s.changedAt) >= kDebounceSeconds) startPreview(s);

    const std::shared_ptr<Run> run = s.run;
    const bool busy = s.pending || !run || !run->done.load();
    const bool ready = !busy && run->rigError.empty() && run->result.ok;
    std::string problem;
    if (!busy && !run->rigError.empty()){
        problem = run->rigError;
    }else if (!busy && !run->result.ok && run->result.errorKind != "cancelled"){
        problem = run->result.error + (run->result.fix.empty() ? std::string() : ". " + run->result.fix);
    }
    if (!problem.empty()) ImGui::TextColored(Theme::Colors::ErrorText, "%s", problem.c_str());

    if (ImGui::BeginTable("reduce_numbers", 2, ImGuiTableFlags_BordersInnerV)){
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(280.0f));
        if (ready){
            const anim::ClipOptimizeResult& r = run->result;
            row("bytes_before", anim::formatClipField("bytes_before", (double)r.before.runtimeBytes) + " -> " + anim::formatClipField("bytes_after", (double)r.after.runtimeBytes), "reduce_memory");
            row("keys_before", anim::formatClipField("keys_before", (double)r.before.keys) + " -> " + anim::formatClipField("keys_after", (double)r.after.keys), "reduce_keys");
            row("max_error_mm", anim::formatClipField("max_error_mm", r.measured.maxErrorMm), "reduce_max_error");
            row("mean_error_mm", anim::formatClipField("mean_error_mm", r.measured.meanErrorMm), "reduce_mean_error");
            row("worst_joint", r.measured.worstJoint + " at " + anim::formatClipField("worst_time_s", r.measured.worstTimeSeconds), "reduce_worst_joint");
        }else{
            for (const char* key : {"bytes_before", "keys_before", "max_error_mm", "mean_error_mm", "worst_joint"}){
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", anim::findClipReportField(key)->label);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", busy ? "measuring..." : "-");
            }
        }
        ImGui::EndTable();
    }
    std::string note;
    if (ready){
        if (run->result.keptAllKeys){
            note = "No key can go within this tolerance.";
        }else if (run->result.after.runtimeBytes >= run->result.before.runtimeBytes){
            note = "No memory saved: keep the source.";
        }
        if (!note.empty()) ImGui::TextColored(Theme::Colors::WarningText, "%s", note.c_str());
        if (run->result.skipped.total() > 0){
            ImGui::TextColored(Theme::Colors::SubtleText, "%zu tracks keep all their keys (eases, tangents, morph and property tracks, unbound paths)", run->result.skipped.total());
        }
    }

    ImGui::Separator();
    ImGui::BeginDisabled(written);
    ImGui::BeginDisabled(s.replace);
    ImGui::SetNextItemWidth(Theme::dpi(360.0f));
    EditorModal::pathField("output", s.output, sizeof(s.output));
    ImGui::InputText("Output##reduce_output", s.output, sizeof(s.output));
    ImGui::SetItemTooltip("The new .anim (project-relative). A model file is never changed");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(s.subClip);
    // a model's clip stays in the model: Replace cannot apply, whoever asks
    if (EditorModal::checkbox("Replace This Clip##reduce_replace", "replace", &s.replace)) changed();
    if (s.subClip) s.replace = false;
    ImGui::SetItemTooltip(s.subClip ? "A model's clip stays in the model: it writes a new .anim" : "Write over this clip's file (one undo step puts it back)");
    ImGui::EndDisabled();
    std::error_code ec;
    if (!s.replace && fs::exists(project->getProjectPath() / FileUtils::pathFromUtf8(s.output), ec)){
        ImGui::SameLine();
        EditorModal::checkbox("Overwrite##reduce_overwrite", "overwrite", &s.overwrite);
    }
    ImGui::EndDisabled();
    if (!s.error.empty()) ImGui::TextColored(Theme::Colors::ErrorText, "%s", s.error.c_str());

    // what an agent reads of it (editor_status.dialog): the numbers the rows show
    std::string message = "Reduce the keys of " + s.reference + ". ";
    if (written){
        const bool verifying = !run->done.load();
        if (verifying){
            ImGui::TextDisabled("Reading %s back...", s.writtenTo.c_str());
            message += "Reading " + s.writtenTo + " back.";
        }else if (run->verified){
            const std::string line = "Wrote " + s.writtenTo + " (one undo step). Numbers above are from the file.";
            ImGui::TextUnformatted(line.c_str());
            message += line;
        }else{
            ImGui::TextColored(Theme::Colors::ErrorText, "%s", run->verifyError.c_str());
        }
    }else{
        message += busy ? "Measuring." : ready ? "Nothing is written until Reduce." : "";
    }
    if (ready){
        const anim::ClipOptimizeResult& r = run->result;
        message += " Memory " + anim::formatClipField("bytes_before", (double)r.before.runtimeBytes) + " -> " + anim::formatClipField("bytes_after", (double)r.after.runtimeBytes) +
                   ", Keys " + anim::formatClipField("keys_before", (double)r.before.keys) + " -> " + anim::formatClipField("keys_after", (double)r.after.keys) + ", " +
                   anim::clipErrorLine(r.measured) + ", Worst Joint " + r.measured.worstJoint + ". " + note;
    }
    EditorModal::message(message);
    EditorModal::error(written && run->done.load() && !run->verified ? run->verifyError : !s.error.empty() ? s.error : problem);

    if (!written){
        ImGui::BeginDisabled(!ready || project->isAnyScenePlaying());
        if (EditorModal::button("Reduce##reduce_write", "ok")){
            s.error.clear();
            if (!writeResult(project, s, s.error) && s.error.empty()) s.error = "The clip could not be written";
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (EditorModal::button(written ? "Close##reduce_close" : "Cancel##reduce_close", "cancel")){
        if (s.run) s.run->cancel = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
