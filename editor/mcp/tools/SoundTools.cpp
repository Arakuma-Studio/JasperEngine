// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sound Events (docs/specs/S37-sound-events.md R20): edit_sound_event makes or changes a .soundevent
// as one undo step and reports its problems and offline renditions of its next plays.
#include "McpTools.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "audio/SoundEventDocument.h"
#include "audio/SoundEventRender.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

constexpr int SOUND_EVENT_WORK_TIMEOUT_MS = 120000;

struct SoundEventJob {
    SoundEventDef def;
    fs::path assetsRoot;
    std::string key;
    uint64_t seed = 1;
    int audition = 0;
    bool matchLevels = false;
    // the results
    std::string error;
    std::vector<std::string> problems;
    std::vector<SoundEventRender::Rendition> renditions;
    std::atomic<bool> finished{false};
};

// where the call's `path` is: inside the assets folder, with its extension
bool soundEventFile(Project* project, const std::string& text, fs::path& file, std::string& error) {
    if (!project) {
        error = "No project is open.";
        return false;
    }
    std::string named = text;
    if (!Util::isSoundEventFile(named)) named += ".soundevent";
    std::error_code ec;
    fs::path path = project->getProjectPath() / FileUtils::pathFromUtf8(named);
    if (!fs::exists(path, ec) && !project->isInsideAssetsPath(path)) path = project->resolveAssetPath(FileUtils::pathFromUtf8(named));
    file = path.lexically_normal();
    if (!project->isInsideAssetsPath(file)) {
        error = "`path` must be a .soundevent inside the assets folder (\"sounds/steps.soundevent\").";
        return false;
    }
    return true;
}

Json renditionsJson(const SoundEventJob& job) {
    Json out = Json::array();
    for (size_t i = 0; i < job.renditions.size(); i++) {
        const SoundEventRender::Rendition& rendition = job.renditions[i];
        Json picks = Json::array();
        for (const SoundEventRender::Voice& voice : rendition.voices) {
            Json pick = {{"variation", voice.variation}, {"clip", voice.clip}, {"pitch_semitones", voice.pitchSemitones},
                         {"volume_db", voice.volumeDb}, {"delay_ms", voice.delayMs}};
            if (!voice.error.empty()) pick["error"] = voice.error;
            picks.push_back(std::move(pick));
        }
        out.push_back({{"n", i + 1}, {"picks", picks}, {"lufs", rendition.lufs}, {"peak_dbtp", rendition.peakDbtp}});
    }
    return out;
}

std::string guidOf(Project* project, const fs::path& file) {
    return project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(file)).str();
}

// after the write: what decoding finds, and the renditions, off the main thread
void finishEdit(const CallPtr& call, Project* project, const fs::path& file, const std::string& rel, bool created, const SoundEventDef& def, bool written, int audition) {
    const std::string guid = guidOf(project, file);
    auto job = std::make_shared<SoundEventJob>();
    job->def = def;
    job->assetsRoot = fs::absolute(project->getAssetsPath());
    job->key = guid.empty() ? project->normalizeToAssetsRelative(file).generic_string() : guid;
    job->seed = (uint64_t)project->getAudioSettings().seed;
    job->audition = audition;
    std::vector<std::string> problems = SoundEventDocument::problems(project, def);
    std::thread([job]() {
        job->problems = SoundEventRender::analyze(job->def, job->assetsRoot);
        if (job->audition > 0 && !job->def.clips.empty()) {
            SoundEventRender::render(job->def, job->key, job->assetsRoot, job->audition, job->seed, job->renditions, job->error);
        }
        job->finished = true;
    }).detach();
    call->waitUntil([job]() { return job->finished.load(); }, SOUND_EVENT_WORK_TIMEOUT_MS,
                    [call, job, problems, rel, created, written, guid](bool timedOut) mutable {
        if (timedOut) {
            call->fail("Decoding the event's clips took over " + std::to_string(SOUND_EVENT_WORK_TIMEOUT_MS / 1000) + " s. The edit is written; call again without audition.");
            return;
        }
        problems.insert(problems.end(), job->problems.begin(), job->problems.end());
        if (!job->error.empty()) problems.push_back(job->error);
        Json data = {{"event", rel}, {"guid", guid}, {"values", SoundEventDocument::describe(job->def)},
                     {"problems", problems}, {"renditions", renditionsJson(*job)}, {"seed", job->seed}};
        std::string text = std::string(!written ? "Unchanged " : created ? "Created " : "Changed ") + rel + ": " +
                           SoundEventDef::modeName(job->def.mode) + ", " + std::to_string(job->def.clips.size()) + " clip(s).";
        if (written) text += " One undo step.";
        text += problems.empty() ? " No problems." : " " + std::to_string(problems.size()) + " problem(s): " + problems.front() + (problems.size() > 1 ? " ..." : "");
        if (!job->renditions.empty()) text += " " + std::to_string(job->renditions.size()) + " rendition(s) of the next plays under audio.seed.";
        call->ok(text, data);
    });
}

void writeEdit(const CallPtr& call, Project* project, const fs::path& file, const std::string& rel, bool created, const SoundEventDef& before, const SoundEventDef& def, int audition) {
    std::string error;
    const bool changed = created || !(def == before);
    if (changed && !SoundEventDocument::commit(project, file, def, "Edit Sound Event " + file.stem().string(), "", &error)) {
        call->fail(rel + ": " + error);
        return;
    }
    if (changed) w4::syncFiles(call->ctx(), {file}, false);
    finishEdit(call, project, file, rel, created, def, changed, audition);
}

void editSoundEventTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    fs::path file;
    std::string error;
    if (!soundEventFile(project, args.value("path", ""), file, error)) {
        call->fail(error);
        return;
    }
    const std::string rel = w4::projectRelative(project, file);
    const bool create = args.value("create", false);
    std::error_code ec;
    const bool exists = fs::is_regular_file(file, ec);
    if (create && exists) {
        call->fail(rel + " already exists: call again without `create` to change it.");
        return;
    }
    if (!create && !exists) {
        call->fail(rel + " does not exist: pass create: true to make it.");
        return;
    }
    SoundEventDef def;
    if (create) def.mode = SoundEventMode::Shuffle;
    if (exists && !SoundEventDocument::read(file, def, &error)) {
        call->fail(rel + ": " + error);
        return;
    }
    const SoundEventDef before = def;
    if (args.contains("values") && !SoundEventDocument::applyValues(project, def, args["values"], &error)) {
        call->fail(error);
        return;
    }
    const int audition = args.value("audition", 0);
    if (!args.value("match_levels", false)) {
        writeEdit(call, project, file, rel, create, before, def, audition);
        return;
    }

    // Match Levels decodes every clip: off the main thread, then the one write of the call
    auto job = std::make_shared<SoundEventJob>();
    job->def = def;
    job->assetsRoot = fs::absolute(project->getAssetsPath());
    std::thread([job]() {
        SoundEventRender::matchLevels(job->def, job->assetsRoot, job->error);
        job->finished = true;
    }).detach();
    call->waitUntil([job]() { return job->finished.load(); }, SOUND_EVENT_WORK_TIMEOUT_MS,
                    [call, job, project, file, rel, create, before, audition](bool timedOut) {
        if (timedOut) {
            call->fail("Measuring the clips took over " + std::to_string(SOUND_EVENT_WORK_TIMEOUT_MS / 1000) + " s. Nothing was written.");
            return;
        }
        if (!job->error.empty()) {
            call->fail("match_levels: " + job->error + ". Nothing was written.");
            return;
        }
        writeEdit(call, project, file, rel, create, before, job->def, audition);
    });
}

} // namespace

Json soundEventInfo(Project* project, const fs::path& file) {
    SoundEventDef def;
    std::string error;
    if (!SoundEventDocument::read(file, def, &error)) {
        return {{"mode", "unreadable"}, {"clips", Json::array()}, {"problems", Json::array({error})}};
    }
    Json out = SoundEventDocument::describe(def);
    out["problems"] = SoundEventDocument::problems(project, def);
    return out;
}

void registerSoundTools(McpRegistry& registry) {
    using namespace schema;

    ToolDef tool;
    tool.name = "edit_sound_event";
    tool.title = "Edit Sound Event";
    tool.description =
        "Create or change a .soundevent (Unity's Audio Random Container): clips played by weight, shuffle, sequence or as "
        "layers, with pitch, volume and delay ranges, a cooldown and an instance limit. Scenes refer to it by GUID, so "
        "move_asset it freely; an AudioSource plays it through set_properties `event`. One undo step; during Play the next "
        "plays use it. Result: problems, and with audition the picks and loudness of the next plays. Keys: jasper://docs/audio.";
    tool.inputSchema = object({
        {"path", assetPath("A .soundevent in the assets folder.")},
        {"create", withDefault(boolean("Make the file; refused when it exists."), false)},
        {"values", anyObject("mode, clips (paths, GUIDs or {clip, weight, volume_db, pitch_semitones, delay_ms}; the whole list), "
                             "avoid_repeat, pitch_semitones, volume_db, delay_ms ([min, max]), output_group (a name), max_instances, "
                             "steal, cooldown_ms, priority, loop, spatial, type. null resets a key.")},
        {"match_levels", withDefault(boolean("After values: every clip's volume_db so all play equally loud, none past -1 dBTP. Not for layer."), false)},
        {"audition", withDefault(integer("Offline renditions of the next plays.", 0, SoundEventRender::MAX_COUNT), 0)}
    }, {"path"});
    tool.annotations.idempotent = true;
    tool.activity = ToolActivity::Edit;
    tool.allowedWhilePlaying = true;
    tool.writesFiles = [](const Json&) { return true; };
    tool.handler = editSoundEventTool;
    registry.addTool(std::move(tool));
}

}
