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
#include "audio/AudioAnalysis.h"
#include "audio/AudioImages.h"
#include "audio/AudioProvenance.h"
#include "audio/SoundEventDocument.h"
#include "audio/SoundEventIO.h"
#include "audio/SoundEventRender.h"
#include "audio/SoundSearch.h"
#include "audio/SoundSlicer.h"
#include "audio/SoundTypes.h"
#include "command/type/SetAssetMetaCmd.h"
#include "mcp/McpRefs.h"
#include "util/Sha256.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

constexpr int SOUND_EVENT_WORK_TIMEOUT_MS = 120000;

struct SoundEventJob {
    SoundEventDef def;
    SoundEventRender::Input input;
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
void finishEdit(const CallPtr& call, Project* project, const fs::path& file, const std::string& rel, bool created, const SoundEventDef& def, bool written, int audition,
                const Json& slices = nullptr, const std::vector<std::string>& sliceProblems = {}) {
    // a new file has no entry until the database next looks, and the reply names its GUID
    if (created) project->refreshAssetDatabase(true);
    const std::string guid = guidOf(project, file);
    auto job = std::make_shared<SoundEventJob>();
    job->def = def;
    job->input = SoundEventRender::prepare(*project, def);
    job->key = guid.empty() ? project->normalizeToAssetsRelative(file).generic_string() : guid;
    job->seed = (uint64_t)project->getAudioSettings().seed;
    job->audition = audition;
    std::vector<std::string> problems = SoundEventDocument::problems(project, def);
    problems.insert(problems.end(), sliceProblems.begin(), sliceProblems.end());
    std::thread([job]() {
        int analysed = 0;
        job->problems = SoundSearch::eventProblems(job->def, job->input.clipFiles, job->input.cacheDirectory, analysed);
        if (job->audition > 0 && !job->def.clips.empty()) {
            SoundEventRender::render(job->def, job->key, job->input, job->audition, job->seed, job->renditions, job->error);
        }
        job->finished = true;
    }).detach();
    call->waitUntil([job]() { return job->finished.load(); }, SOUND_EVENT_WORK_TIMEOUT_MS,
                    [call, job, problems, rel, created, written, guid, slices](bool timedOut) mutable {
        if (timedOut) {
            call->fail("Decoding the event's clips took over " + std::to_string(SOUND_EVENT_WORK_TIMEOUT_MS / 1000) + " s. The edit is written; call again without audition.");
            return;
        }
        problems.insert(problems.end(), job->problems.begin(), job->problems.end());
        if (!job->error.empty()) problems.push_back(job->error);
        Json data = {{"event", rel}, {"guid", guid}, {"values", SoundEventDocument::describe(job->def)},
                     {"problems", problems}, {"renditions", renditionsJson(*job)}, {"seed", job->seed}};
        if (!slices.is_null()) data["slices"] = slices;
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

// -- slice: a multi-take file into a new shuffle event (spec S36 M9)

struct SliceJob {
    fs::path source;
    std::string sourceRel;
    fs::path eventFile;
    SoundSlicer::Options options;
    std::vector<int> pick;
    bool preview = false;
    // the results
    std::string error;
    std::string kind;
    std::string type;
    bool licensed = false;
    AudioAnalysis::Signal signal;
    std::vector<SoundSlicer::Slice> slices;
    std::vector<SoundSlicer::File> files;
    std::string png;
    Json imageInfo = nullptr;
    std::atomic<bool> finished{false};
};

void runSlice(SliceJob& job) {
    std::ifstream in(job.source, std::ios::binary);
    if (!in) {
        job.error = job.sourceRel + " cannot be read.";
        return;
    }
    std::stringstream read;
    read << in.rdbuf();
    const std::string bytes = read.str();
    Json cached;
    if (!AudioAnalysis::analyzeBytes(bytes, cached, job.error, &job.signal)) return;
    const AudioAnalysis::Labels labels = AudioAnalysis::labelsOf(job.source);
    std::string kindSource;
    AudioAnalysis::resolveKind(cached.value("measured_kind", std::string(SoundTypes::TRANSIENT)), labels, job.kind, kindSource);
    job.type = labels.type;
    job.licensed = labels.licence.is_object() && labels.licence.value("id", std::string()) != AudioProvenance::LICENCE_UNKNOWN &&
                   !labels.licence.value("id", std::string()).empty();
    if (!SoundSlicer::detect(job.signal, job.options, job.kind, job.slices, job.error)) return;
    if (job.preview) {
        std::vector<AudioImages::SliceMark> marks;
        for (const SoundSlicer::Slice& slice : job.slices) {
            marks.push_back({(double)slice.startFrame / job.signal.sampleRate, (double)slice.endFrame / job.signal.sampleRate, slice.fadeInMs > 0.0, slice.fadeOutMs > 0.0});
        }
        const AudioImages::Image image = AudioImages::renderSlices(job.signal, job.sourceRel, marks, 768);
        if (AudioImages::encodePng(image, job.png)) job.imageInfo = {{"mode", "slices"}, {"width", image.width}, {"height", image.height}};
        return;
    }
    Sha256 sha;
    sha.update(bytes);
    SoundSlicer::plan(job.source, job.sourceRel, Sha256::toHex(sha.finish()), job.signal, job.slices, job.pick, job.eventFile, job.files, job.error);
}

void sliceSoundEvent(const CallPtr& call, Project* project, const fs::path& file, const std::string& rel, const Json& slice, int audition) {
    static const std::vector<std::string> keys = {"source", "gate_db", "min_gap_ms", "pre_ms", "max_ms", "pick", "variation", "preview"};
    for (auto it = slice.begin(); it != slice.end(); ++it) {
        if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
            call->fail("slice." + it.key() + " is unknown: source, gate_db, min_gap_ms, pre_ms, max_ms, pick, variation, preview.");
            return;
        }
    }
    auto job = std::make_shared<SliceJob>();
    job->sourceRel = slice.value("source", std::string());
    std::string error;
    if (job->sourceRel.empty() || !McpRefs::resolveProjectPath(project, job->sourceRel, job->source, error, true)) {
        call->fail("slice.source is a project audio file" + (error.empty() ? std::string(".") : ": " + error) + " Import a built-in clip with import_files first.");
        return;
    }
    if (!project->isInsideAssetsPath(job->source)) {
        call->fail("slice.source " + job->sourceRel + " is outside the assets folder.");
        return;
    }
    for (const char* key : {"gate_db", "min_gap_ms", "pre_ms", "max_ms"}) {
        if (slice.contains(key) && !slice[key].is_number()) {
            call->fail(std::string("slice.") + key + " is a number.");
            return;
        }
    }
    job->options.gateDb = slice.value("gate_db", job->options.gateDb);
    job->options.minGapMs = slice.value("min_gap_ms", job->options.minGapMs);
    job->options.preMs = slice.value("pre_ms", job->options.preMs);
    job->options.maxMs = slice.value("max_ms", job->options.maxMs);
    if (slice.contains("pick")) {
        if (!slice["pick"].is_array()) {
            call->fail("slice.pick is a list of slice numbers, from 1.");
            return;
        }
        for (const Json& n : slice["pick"]) {
            if (!n.is_number_integer()) {
                call->fail("slice.pick is a list of slice numbers, from 1.");
                return;
            }
            job->pick.push_back(n.get<int>());
        }
    }
    job->preview = slice.value("preview", false);
    job->eventFile = file;
    const bool variation = slice.value("variation", false);
    std::thread([job]() {
        runSlice(*job);
        job->finished = true;
    }).detach();
    call->waitUntil([job]() { return job->finished.load(); }, SOUND_EVENT_WORK_TIMEOUT_MS, [call, job, project, file, rel, variation, audition](bool timedOut) {
        if (timedOut) {
            call->fail("Slicing took over " + std::to_string(SOUND_EVENT_WORK_TIMEOUT_MS / 1000) + " s. Nothing was written.");
            return;
        }
        if (!job->error.empty()) {
            call->fail("slice: " + job->error + " Nothing was written.");
            return;
        }
        const unsigned int rate = job->signal.sampleRate;
        if (job->preview) {
            Json slices = Json::array();
            for (size_t i = 0; i < job->slices.size(); i++) slices.push_back(SoundSlicer::toJson(job->slices[i], (int)i + 1, rate));
            Json data = {{"slices", slices}, {"source", {{"path", job->sourceRel}, {"duration_s", AudioAnalysis::rounded(job->signal.seconds(), 0.001)}, {"licensed", job->licensed}}}, {"image", job->imageInfo}};
            ToolResult result = ToolResult::success(std::to_string(job->slices.size()) + " slice(s) of " + job->sourceRel + ". Nothing was written: call again without preview, with pick to choose.", data);
            if (!job->png.empty()) result.images.push_back({job->png, "image/png"});
            call->reply(std::move(result));
            return;
        }
        SoundEventDef def;
        def.mode = SoundEventMode::Shuffle;
        def.avoidRepeat = true;
        def.type = job->type;
        if (variation) {
            def.pitchMin = -1.0f;
            def.pitchMax = 1.0f;
            def.volumeMin = -1.5f;
            def.volumeMax = 0.0f;
        }
        std::vector<WriteFilesCmd::Item> items;
        Json slices = Json::array();
        for (size_t i = 0; i < job->files.size(); i++) {
            const SoundSlicer::File& made = job->files[i];
            items.push_back({made.wav, made.wavBytes});
            items.push_back({SetAssetMetaCmd::metaPathOf(made.wav), made.metaText});
            SoundEventClip clip;
            clip.clip = AssetRef(project->normalizeToAssetsRelative(made.wav).generic_string(), Guid::parse(YAML::Load(made.metaText)["guid"].as<std::string>("")));
            def.clips.push_back(std::move(clip));
            const int n = job->pick.empty() ? (int)i + 1 : job->pick[i];
            Json item = SoundSlicer::toJson(job->slices[(size_t)n - 1], n, rate);
            item["path"] = w4::projectRelative(project, made.wav);
            slices.push_back(std::move(item));
        }
        const std::vector<std::string> invalid = SoundEventIO::validate(def);
        if (!invalid.empty()) {
            call->fail("slice: " + invalid.front() + ". Nothing was written.");
            return;
        }
        items.push_back({file, SoundEventIO::write(def)});
        std::string writeError;
        if (!w4::writeFilesUndoable(call->ctx(), std::move(items), writeError, {}, "Slice into Sound Event")) {
            call->fail(writeError);
            return;
        }
        std::vector<std::string> problems;
        if (!job->licensed) problems.push_back("source has no licence: the slices' licence is unknown");
        finishEdit(call, project, file, rel, true, def, true, audition, slices, problems);
    });
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
    if (args.contains("slice")) {
        if (!create) {
            call->fail("slice makes a new event: pass create: true with a new path.");
            return;
        }
        if (!args["slice"].is_object()) {
            call->fail("slice is an object: {source, gate_db, min_gap_ms, pre_ms, max_ms, pick, variation, preview}.");
            return;
        }
        if (args.contains("values") || args.value("match_levels", false)) {
            call->fail("slice makes the whole event: change it with values or match_levels in a second call.");
            return;
        }
        sliceSoundEvent(call, project, file, rel, args["slice"], args.value("audition", 0));
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
    job->input = SoundEventRender::prepare(*project, def);
    std::thread([job]() {
        SoundEventRender::matchLevels(job->def, job->input, job->error);
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

void registerSoundEventTools(McpRegistry& registry) {
    using namespace schema;

    ToolDef tool;
    tool.name = "edit_sound_event";
    tool.title = "Edit Sound Event";
    tool.description =
        "Create or change a .soundevent (Unity's Audio Random Container): clips played by weight, shuffle, sequence or as "
        "layers, with pitch, volume and delay ranges, a cooldown and an instance limit. Scenes refer to it by GUID, so "
        "move_asset it freely; an AudioSource plays it through set_properties `event`. One undo step; during Play the next "
        "plays use it. Result: problems, and with audition the picks and loudness of the next plays. Keys: jasper://docs/sound. "
        "slice: cut a multi-take file into clips.";
    tool.inputSchema = object({
        {"path", assetPath("A .soundevent in the assets folder.")},
        {"create", withDefault(boolean("Make the file; refused when it exists."), false)},
        {"values", anyObject("mode, clips (paths, GUIDs or {clip, weight, volume_db, pitch_semitones, delay_ms, loops}; the whole list), "
                             "avoid_repeat, pitch_semitones, volume_db, delay_ms ([min, max]), output_group (a name), max_instances, "
                             "steal, cooldown_ms, priority, loop, spatial, type. null resets a key.")},
        {"match_levels", withDefault(boolean("After values: every clip's volume_db so all play equally loud, none past -1 dBTP. Not for layer."), false)},
        {"audition", withDefault(integer("Offline renditions of the next plays.", 0, SoundEventRender::MAX_COUNT), 0)},
        {"slice", anyObject("With create: {source, gate_db, min_gap_ms, pre_ms, max_ms, pick, variation, preview}.")}
    }, {"path"});
    tool.annotations.idempotent = true;
    tool.activity = ToolActivity::Edit;
    tool.allowedWhilePlaying = true;
    tool.writesFiles = [](const Json&) { return true; };
    tool.handler = editSoundEventTool;
    registry.addTool(std::move(tool));
}

}
