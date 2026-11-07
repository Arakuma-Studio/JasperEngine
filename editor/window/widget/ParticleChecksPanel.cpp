// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleChecksPanel.h"

#include "Backend.h"
#include "Log.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticlePreviewSource.h"
#include "particles/ParticleSheetBake.h"
#include "render/TextureRender.h"
#include "texture/TextureData.h"
#include "util/ParticleEffectEdit.h"
#include "util/ProjectFileWrites.h"
#include "util/UIProbe.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    ImVec4 statusColor(const std::string& status){
        if (status == "FAIL") return ImVec4(0.92f, 0.33f, 0.30f, 1.0f);
        if (status == "WARN") return ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
        if (status == "PASS") return ImVec4(0.40f, 0.78f, 0.45f, 1.0f);
        return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    }

    std::string valueText(const qa::Json& value){
        if (value.is_null()) return "-";
        if (value.is_string()) return value.get<std::string>();
        if (value.is_number_float()){
            char text[32];
            std::snprintf(text, sizeof(text), "%.3g", value.get<double>());
            return text;
        }
        if (value.is_object()){
            std::string text;
            for (auto it = value.begin(); it != value.end(); ++it){
                if (it.key() == "estimate") continue;
                text += (text.empty() ? "" : ", ") + it.key() + " " + valueText(it.value());
            }
            return text;
        }
        return value.dump();
    }

}

ParticleChecksPanel::ParticleChecksPanel() = default;

ParticleChecksPanel::~ParticleChecksPanel(){
    run.reset();
    recording.reset();
    filmstrip.destroy();
    for (Texture& texture : retired) texture.destroy();
}

void ParticleChecksPanel::setAsset(const std::string& assetPath){
    if (asset == assetPath && root == NULL_ENTITY) return;
    asset = assetPath;
    sceneId = 0;
    root = NULL_ENTITY;
    report = qa::Json();
    rows.clear();
    message.clear();
    recordPath[0] = 0;
}

void ParticleChecksPanel::setSubtree(uint32_t scene, Entity entity){
    if (sceneId == scene && root == entity) return;
    asset.clear();
    sceneId = scene;
    root = entity;
    report = qa::Json();
    rows.clear();
    message.clear();
    recordPath[0] = 0;
}

bool ParticleChecksPanel::buildRequest(Project* project, ParticlePreviewRequest& request, std::string& error) const{
    request = ParticlePreviewRequest();
    if (!asset.empty()){
        if (!ParticlePreviewSource::fromAsset(asset, request.systems, error)) return false;
    }else{
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        if (!sceneProject || !sceneProject->scene || root == NULL_ENTITY){
            error = "Nothing to check: open an effect or a GameObject with Particle Systems.";
            return false;
        }
        if (!ParticlePreviewSource::fromSubtree(sceneProject->scene, root, request.systems, error)) return false;
    }
    // the tool's arguments, as the window sets them
    qa::Json args = {{"frames", frames}, {"platform", platform}, {"overdraw_map", overdrawMap}};
    if (seed != 0) args["seed"] = seed;
    if (!ParticlePreviewSource::applyArgs(args, request, error)) return false;
    if (project) request.qa = project->getParticleQaSettings();
    error = ParticleEffectPreview::validate(request);
    return error.empty();
}

bool ParticleChecksPanel::start(Project* project, std::string& error){
    ParticlePreviewRequest request;
    if (!buildRequest(project, request, error)) return false;
    run = std::make_unique<ParticleEffectPreview>(std::move(request));
    run->start();
    message = "Running...";
    return true;
}

std::vector<ParticleChecksPanel::Row> ParticleChecksPanel::rowsOf(const qa::Json& report){
    std::vector<Row> out;
    if (!report.is_object() || !report.contains("checks")) return out;
    for (const qa::Json& check : report["checks"]){
        Row row;
        row.id = check.value("id", "");
        row.status = check.value("status", "");
        row.measured = valueText(check["measured"]);
        row.threshold = valueText(check["threshold"]);
        if (check["limited_by"].is_string()) row.detail = check["limited_by"].get<std::string>();
        if (check["fix_hint"].is_string()) row.detail += (row.detail.empty() ? "" : " ") + check["fix_hint"].get<std::string>();
        const qa::Json& fixes = report.value("fixes", qa::Json::array());
        for (size_t k = 0; k < fixes.size(); k++){
            if (fixes[k].value("check", "") == row.id) row.fix = (int)k;
        }
        out.push_back(std::move(row));
    }
    return out;
}

void ParticleChecksPanel::finishRun(Project* project){
    (void)project;
    if (run->isFailed()){
        message = run->getError();
        run.reset();
        return;
    }
    report = run->getReport();
    rows = rowsOf(report);
    message = "Verdict " + report.value("verdict", "") + ", seed " + (report["seeds"].empty() ? std::string("-") : report["seeds"][0]["seed"].dump()) +
              ", state_hash " + report.value("state_hash", "") + ". Cost is an estimate, not measured on a device.";
    const PoseImage& image = run->getImage();
    if (!image.isEmpty()){
        pixels = std::make_shared<PoseImage>(image);
        TextureData data(pixels->getWidth(), pixels->getHeight(), (unsigned int)pixels->getPixels().size(), ColorFormat::RGBA, 4, pixels->data());
        Texture next;
        next.setData("editor:particle-filmstrip:" + std::to_string(++textureGeneration), data);
        TextureRender* render = next.getRender();
        if (render && render->isCreated()){
            if (!filmstrip.empty()) retired.push_back(filmstrip);
            filmstrip = next;
        }else{
            next.destroy();
            Log::warn("Particle Effect: the filmstrip's texture could not be created");
        }
    }
    run.reset();
}

std::string ParticleChecksPanel::defaultRecordPath() const{
    namespace fs = std::filesystem;
    std::string base = asset.empty() ? std::string("fx/effect") : (fs::path(asset).parent_path() / fs::path(asset).stem()).generic_string();
    return "assets/" + base + (recordKind == 0 ? "_sheet" : "_frames");
}

void ParticleChecksPanel::finishRecording(Project* project){
    std::unique_ptr<ParticleEffectPreview> done = std::move(recording);
    if (done->isFailed()){
        message = "Record: " + done->getError();
        return;
    }
    std::vector<WriteFilesCmd::Item> items;
    std::vector<std::string> written;
    std::string error;
    bool ok = false;
    if (recordingSheet){
        bool additive = true, looping = false;
        ParticlePreviewRequest request;
        std::string ignored;
        if (buildRequest(project, request, ignored)){
            for (const ParticlePreviewSystem& s : request.systems){
                additive = additive && s.document.renderer.blend == particles::ParticleBlend::Additive;
                looping = looping || s.document.looping;
            }
        }
        const PoseImage sheet = ParticleSheetBake::composeSheet(done->getCells(), columns, rowsCount);
        const particles::ParticleEffectDocument effect =
            ParticleSheetBake::sheetEffect(std::string(), columns, rowsCount, done->getReport().value("seconds", 0.0), done->getFrameWorldSize(), looping, additive);
        ok = ParticleSheetBake::sheetFiles(project, recordOutput, sheet, effect, false, items, written, error);
    }else{
        const std::filesystem::path out(recordOutput);
        ok = ParticleSheetBake::sequenceFiles(project, out.parent_path().generic_string(), out.filename().string(), done->getCells(), false, items, written, error);
    }
    if (ok) ok = ProjectFileWrites::writeUndoable(project, std::move(items), error);
    message = ok ? "Recorded " + std::to_string(written.size()) + " file(s) to " + recordOutput + " (one undo step)." : "Record: nothing written: " + error;
}

void ParticleChecksPanel::applyFix(Project* project, int index){
    const qa::Json fix = report["fixes"][index];
    const nlohmann::ordered_json patch = nlohmann::ordered_json::parse(fix["patch"].dump());
    std::string path = fix.value("path", "");
    Command* command = nullptr;
    std::vector<particles::Diagnostic> diagnostics;
    const auto edit = [&](particles::ParticleEffectDocument& document){
        return particles::ParticleEffectCodec::applyMergePatch(document, patch, diagnostics);
    };
    CommandHistory* history = nullptr;
    if (!path.empty()){
        command = ParticleEffectEdit::buildAsset(project, path, edit);
        history = project->getProjectCommandHistory();
    }else if (fix.value("system", -1) >= 0 && root != NULL_ENTITY){
        SceneProject* sceneProject = project->getScene(sceneId);
        if (sceneProject && sceneProject->scene){
            const std::vector<Entity> systems = ParticlePreviewSource::subtreeSystems(sceneProject->scene, root);
            const int system = fix.value("system", -1);
            if (system < (int)systems.size()){
                command = ParticleEffectEdit::build(project, sceneId, {systems[system]}, edit);
                history = CommandHandle::get(sceneId);
            }
        }
    }
    if (!command || !history){
        message = "The fix could not be applied" + (diagnostics.empty() ? std::string(".") : ": " + particles::ParticleEffectCodec::format(diagnostics));
        return;
    }
    history->addCommand(command);
    if (const UndoEntry* top = history->top()) top->cmd->setNoMerge();
    message = "Applied the fix of " + fix.value("check", "") + " (one undo step). Preview again to check it.";
}

void ParticleChecksPanel::draw(Project* project){
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
    if (run && run->isFinished()) finishRun(project);
    if (recording && recording->isFinished()) finishRecording(project);

    // ---- the run's settings and Preview
    static const char* const platforms[] = {"mobile", "web", "desktop"};
    int platformIndex = (int)(std::find(std::begin(platforms), std::end(platforms), platform) - std::begin(platforms));
    if (platformIndex > 2) platformIndex = 0;
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("Platform##particleChecks", &platformIndex, platforms, 3)) platform = platforms[platformIndex];
    UIProbe::item("particleChecks.platform");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderInt("Frames##particleChecks", &frames, 0, ParticleEffectPreview::MAX_FRAMES);
    ImGui::SameLine();
    ImGui::Checkbox("Overdraw##particleChecks", &overdrawMap);
    ImGui::SameLine();
    ImGui::BeginDisabled(isRunning());
    if (ImGui::Button("Preview##particleChecks")){
        std::string error;
        if (!start(project, error)) message = error;
    }
    UIProbe::item("particleChecks.preview");
    ImGui::EndDisabled();
    if (isRunning()){
        ImGui::SameLine();
        ImGui::TextDisabled("%s (%d%%)", run->getStage().c_str(), (int)(run->getProgress() * 100.0));
    }
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());

    // ---- Filmstrip
    if (!filmstrip.empty() && pixels){
        if (TextureRender* render = filmstrip.getRender()){
            const float available = ImGui::GetContentRegionAvail().x;
            const float scale = std::min(1.0f, available / (float)pixels->getWidth());
            ImGui::Image(Backend::getImGuiTexture(render), ImVec2(pixels->getWidth() * scale, pixels->getHeight() * scale));
            UIProbe::item("particleChecks.filmstrip");
        }
    }

    // ---- Checks: the tool's rows
    if (!rows.empty() && ImGui::BeginTable("##particleChecksRows", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("Check");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Measured");
        ImGui::TableSetupColumn("Threshold");
        ImGui::TableSetupColumn("Why / Fix", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();
        int fixClicked = -1;
        for (const Row& row : rows){
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.id.c_str());
            UIProbe::item("particleChecks.row." + row.id);
            ImGui::TableNextColumn();
            ImGui::TextColored(statusColor(row.status), "%s", row.status.c_str());
            UIProbe::item("particleChecks.status." + row.id + "." + row.status);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.measured.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.threshold.c_str());
            ImGui::TableNextColumn();
            if (row.fix >= 0){
                ImGui::PushID(row.id.c_str());
                if (ImGui::SmallButton("Fix")) fixClicked = row.fix;
                UIProbe::item("particleChecks.fix." + row.id);
                ImGui::PopID();
                ImGui::SameLine();
            }
            ImGui::TextWrapped("%s", row.detail.c_str());
        }
        ImGui::EndTable();
        if (fixClicked >= 0) applyFix(project, fixClicked);
    }

    // ---- Record: a sprite sheet with its effect, or a PNG sequence, to the path below
    ImGui::SeparatorText("Record");
    ImGui::RadioButton("Sprite Sheet##particleRecord", &recordKind, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Sequence##particleRecord", &recordKind, 1);
    if (recordKind == 0){
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("Columns##particleRecord", &columns);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("Rows##particleRecord", &rowsCount);
        columns = std::clamp(columns, 1, ParticleSheetBake::MAX_TILES);
        rowsCount = std::clamp(rowsCount, 1, ParticleSheetBake::MAX_TILES);
    }else{
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("Frames##particleRecordFrames", &columns);
        columns = std::clamp(columns, 1, ParticleEffectPreview::MAX_FRAMES);
        rowsCount = 1;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("Cell Size##particleRecord", &cellSize, 16);
    cellSize = std::clamp(cellSize, 16, 1024);
    if (recordPath[0] == 0) std::snprintf(recordPath, sizeof(recordPath), "%s", defaultRecordPath().c_str());
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("##particleRecordPath", recordPath, sizeof(recordPath));
    UIProbe::item("particleRecord.path");
    ImGui::SameLine();
    ImGui::BeginDisabled(recording != nullptr);
    if (ImGui::Button("Record##particleRecord")){
        ParticlePreviewRequest base;
        std::string error = recordKind == 0 ? ParticleSheetBake::validate(columns, rowsCount, cellSize) : std::string();
        if (error.empty() && buildRequest(project, base, error)){
            base.view = "front";
            ParticlePreviewRequest request = ParticleSheetBake::sheetRequest(base, columns, rowsCount, cellSize);
            error = ParticleEffectPreview::validate(request);
            if (error.empty()){
                recordingSheet = recordKind == 0;
                recordOutput = recordPath;
                recording = std::make_unique<ParticleEffectPreview>(std::move(request));
                recording->start();
                message = "Recording...";
            }
        }
        if (!error.empty()) message = "Record: " + error;
    }
    UIProbe::item("particleRecord.record");
    ImGui::EndDisabled();
}
