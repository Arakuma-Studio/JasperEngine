// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// audio_session (docs/specs/S35-agent-audio.md R19-R25): an agent cannot hear, so it reads a Play's
// capture back as numbers and a picture, and turns it into verdicts.

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Generator.h"
#include "Project.h"
#include "audio/AudioAnalysis.h"
#include "audio/AudioBaseline.h"
#include "audio/AudioCapture.h"
#include "audio/AudioImages.h"
#include "audio/AudioMixer.h"
#include "audio/AudioQa.h"
#include "audio/AudioSessionReader.h"
#include "audio/AudioTimelineImage.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "settings/SettingsRegistry.h"
#include "subsystem/AudioSystem.h"
#include "util/ProjectFileWrites.h"
#include "util/QaChecks.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

constexpr int DEFAULT_WIDTH = 1024;
constexpr int DEFAULT_EVENTS = 200;
constexpr int MAX_EVENTS = 2000;
constexpr int MAX_EXPECT = 64;
constexpr double WAIT_SECONDS = 60.0;

// consecutive FAILs of a check on a scene in this editor session
qa::FailRounds& failRounds() {
    static qa::FailRounds rounds;
    return rounds;
}

const char* outputModeName(AudioOutputMode mode) {
    switch (mode) {
        case AudioOutputMode::Device: return "device";
        case AudioOutputMode::NoOutput: return "no_output";
        case AudioOutputMode::Deterministic: return "deterministic";
    }
    return "no_output";
}

std::string numberText(const Json& value, const char* none = "none") {
    if (!value.is_number()) return none;
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value.get<double>());
    return text;
}

void statusAction(const CallPtr& call) {
    Project* project = call->ctx().project();
    const std::string live = AudioCapture::sessionId();
    const std::vector<AudioAnalysis::Capture> sessions = AudioAnalysis::listCaptures(AudioAnalysis::captureRoot(project->getProjectPath()), live);
    Json list = Json::array();
    std::string lines;
    for (auto it = sessions.rbegin(); it != sessions.rend(); ++it) {
        Json entry = AudioSessionReader::statusEntry(*it);
        lines += "\n" + it->id + ": " + entry.value("state", "") + ", " + entry.value("scene", "") + ", " + numberText(entry["duration_s"], "0") + " s, " +
                 std::to_string(entry.value("events", 0)) + " events, " + std::to_string(entry.value("markers", 0)) + " markers";
        list.push_back(std::move(entry));
    }
    const Project::AudioCaptureArm& arm = project->getAudioCaptureArm();
    const double seconds = AudioCapture::isActive() ? (double)AudioCapture::cursor() / 48000.0 : 0.0;
    Json data = {{"output_mode", outputModeName(AudioSystem::getOutputMode())}, {"backend", AudioSystem::getBackendName()},
                 {"capturing", AudioCapture::isActive()}, {"session", live.empty() ? Json(nullptr) : Json(live)},
                 {"cursor", AudioCapture::cursor()}, {"duration_s", seconds}, {"voices", AudioSystem::getVoiceCount()},
                 {"armed", arm.armed ? Json{{"capture", arm.capture}, {"max_seconds", arm.maxSeconds}, {"stems", arm.stems}} : Json(nullptr)},
                 {"sessions", list}};
    if (!sessions.empty() && !AudioCapture::isActive()) data["next"] = "audio_session report";
    call->ok("Output " + data["output_mode"].get<std::string>() + (AudioCapture::isActive() ? ", capturing " + live : std::string(", not capturing")) +
             (arm.armed ? std::string(", next Play armed") : std::string()) + ", " + std::to_string(sessions.size()) + " session(s)." + lines, data);
}

void armAction(const CallPtr& call) {
    const Json& args = call->args();
    Project::AudioCaptureArm arm;
    arm.capture = args.value("capture", true);
    arm.maxSeconds = args.value("max_seconds", 0);
    if (args.contains("max_seconds") && (arm.maxSeconds < 1 || arm.maxSeconds > 3600)) {
        call->fail("max_seconds is from 1 to 3600.");
        return;
    }
    if (args.contains("stems")) arm.stems = args.value("stems", false) ? 1 : 0;
    call->ctx().project()->armAudioCapture(arm);
    call->ok(arm.capture ? "The next play start or restart captures its audio on the deterministic driver: the device is muted." : "The next Play does not capture.",
             Json{{"armed", {{"capture", arm.capture}, {"max_seconds", arm.maxSeconds}, {"stems", arm.stems}}}});
}

struct SessionWork {
    AudioAnalysis::Capture capture;
    double start = 0.0;
    double end = -1.0;
    int eventsLimit = DEFAULT_EVENTS;
    bool image = false;
    int width = DEFAULT_WIDTH;
    AudioQa::Request request;
    // check: a session to compare with, when the baseline names one
    bool baselineIsSession = false;
    AudioAnalysis::Capture baselineSession;
    std::string baselineName;
    std::string saveBaseline;

    std::string error;
    Json data;
    Json baselineToSave;
    std::string png;
};

// the picture and its metadata into work.data
void renderTimeline(SessionWork& work, const AudioSessionData& session, const AudioTimelineImage::Options& options) {
    if (!work.image) return;
    const AudioTimelineImage::Result result = AudioTimelineImage::render(session, options, work.width);
    AudioImages::encodePng(result.image, work.png);
    work.data["image"] = {{"width", result.image.width}, {"height", result.image.height}, {"lanes", result.lanes},
                          {"marker_labels", result.image.markerLabels}, {"event_labels", result.image.eventLabels},
                          {"hidden_labels", result.image.hiddenLabels}, {"listen_labels", result.image.listenLabels}};
}

void runReport(SessionWork& work) {
    AudioSessionData session;
    if (!AudioSessionReader::load(work.capture, work.start, work.end, session, work.error)) return;
    AudioTimelineImage::Options options;
    work.data = AudioSessionReader::report(session, work.eventsLimit, options.markers, options.events);
    work.data["guidance"] = "Report these numbers; never claim a sound plays without a capture.";
    work.data["next"] = "audio_session check";
    renderTimeline(work, session, options);
}

void runCheck(SessionWork& work) {
    AudioSessionData session;
    if (!AudioSessionReader::load(work.capture, work.start, work.end, session, work.error)) return;
    if (work.baselineIsSession) {
        AudioSessionData then;
        if (!AudioSessionReader::load(work.baselineSession, 0.0, -1.0, then, work.error)) return;
        work.request.baseline = AudioBaseline::build(then, work.request.qa.audibilityMarginDb);
    }
    work.data = AudioQa::run(session, work.request);
    if (!work.saveBaseline.empty()) work.baselineToSave = AudioBaseline::build(session, work.request.qa.audibilityMarginDb);

    // the moments on the picture
    AudioTimelineImage::Options options;
    Json report = AudioSessionReader::report(session, MAX_EVENTS, options.markers, options.events);
    for (const Json& check : work.data["checks"]) {
        const std::string status = check.value("status", "");
        if (status != "FAIL" && status != "WARN") continue;
        for (const Json& at : check["at"]) {
            (status == "FAIL" ? options.failures : options.warnings).push_back({at.get<double>(), check.value("id", "")});
        }
    }
    int n = 0;
    for (const Json& moment : work.data["listen"]) options.listen.push_back({moment.value("t", 0.0), "L" + std::to_string(++n)});
    renderTimeline(work, session, options);

    const io::WriteResult written = io::writeFileAtomic(work.capture.directory / "check.json", work.data.dump(2) + "\n");
    if (!written.ok()) work.data["check_json_error"] = written.message;
}

ToolResult reportResult(const SessionWork& work) {
    if (!work.error.empty()) return ToolResult::error(work.error);
    const Json& data = work.data;
    const Json& loudness = data["loudness"];
    const Json& peak = data["peak"];
    const Json& voices = data["voices"];
    std::string text = "Capture " + work.capture.id + " (" + data["capture"].value("state", "") + "): " + numberText(data["range"]["start"], "0") + "-" + numberText(data["range"]["end"], "0") + " s.";
    text += "\nLoudness: integrated " + numberText(loudness["integrated_lufs"]) + " LUFS, momentary max " + numberText(loudness["momentary_max"]) + ", short-term max " + numberText(loudness["short_term_max"]) + ", LRA " + numberText(loudness["lra"]) + ".";
    text += "\nPeak: " + numberText(peak["sample_dbfs"]) + " dBFS, true peak " + numberText(peak["true_peak_dbtp"]) + " dBTP, " + std::to_string(peak.value("clipped", 0)) + " clipped.";
    text += "\nVoices: " + std::to_string(voices.value("started", 0)) + " started, " + std::to_string(voices.value("stolen", 0)) + " stolen, " + std::to_string(voices.value("rejected", 0)) + " rejected, peak " + std::to_string(voices.value("peak", 0)) + ".";
    for (auto it = data["groups"].begin(); it != data["groups"].end(); ++it) text += "\nGroup " + it.key() + ": peak " + numberText(it.value()["peak_db"]) + " dB.";
    text += "\n" + std::to_string(data.value("events_total", 0)) + " events, " + std::to_string(data.value("markers_total", 0)) + " markers. " + data.value("guidance", "");
    ToolResult result = ToolResult::success(text, data);
    if (!work.png.empty()) result.images.push_back({work.png, "image/png"});
    return result;
}

void reportAction(const CallPtr& call, std::shared_ptr<SessionWork> work) {
    JobPtr job = call->ctx().jobs().create("audio_session", "Report on audio capture " + work->capture.id);
    jobs::submit([work, job]() {
        try {
            runReport(*work);
        } catch (const std::exception& e) {
            work->error = std::string("Reading the capture failed: ") + e.what();
        }
        if (work->error.empty()) job->succeed("Reported.", work->data);
        else job->fail(work->error);
    }, {jobs::Lane::Background, jobs::GateClass::Default, "audio-session-report"});
    replyWhenJobDone(call, job, true, WAIT_SECONDS, [work](const McpJob&) { return reportResult(*work); });
}

// S38's targets, when its settings exist
double targetSetting(SettingsContext& context, const char* key, double fallback) {
    const SettingRow* row = SettingsRegistry::get().find(key);
    if (!row || !row->get) return fallback;
    const Json value = row->get(context);
    return value.is_number() ? value.get<double>() : fallback;
}

bool validateExpect(const Json& rules, std::string& why) {
    if (!rules.is_array() || (int)rules.size() > MAX_EXPECT) {
        why = "expect is a list of at most " + std::to_string(MAX_EXPECT) + " rules.";
        return false;
    }
    for (const Json& rule : rules) {
        if (!rule.is_object() || !rule.contains("on") || !rule["on"].is_string()) {
            why = "each expect rule needs on: a marker kind (ui_click, collision_3d...) or an Audio.mark label.";
            return false;
        }
        for (auto it = rule.begin(); it != rule.end(); ++it) {
            static const std::vector<std::string> keys = {"on", "entity", "sound", "within_ms", "offset_ms", "align"};
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
                why = "expect has no key " + it.key() + ": on, entity, sound, within_ms, offset_ms, align.";
                return false;
            }
        }
        if (rule.contains("within_ms") && (!rule["within_ms"].is_number() || rule["within_ms"].get<double>() < 0 || rule["within_ms"].get<double>() > 500)) {
            why = "expect within_ms is from 0 to 500.";
            return false;
        }
        if (rule.contains("offset_ms") && (!rule["offset_ms"].is_number() || std::abs(rule["offset_ms"].get<double>()) > 10000)) {
            why = "expect offset_ms is from -10000 to 10000.";
            return false;
        }
        const std::string align = rule.value("align", "onset");
        if (align != "onset" && align != "peak" && align != "end") {
            why = "expect align is onset, peak or end.";
            return false;
        }
    }
    return true;
}

void checkAction(const CallPtr& call, std::shared_ptr<SessionWork> work) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();

    AudioQa::Request& request = work->request;
    if (args.contains("checks")) {
        for (const Json& id : args["checks"]) {
            const std::vector<std::string>& ids = AudioQa::checkIds();
            if (!id.is_string() || std::find(ids.begin(), ids.end(), id.get<std::string>()) == ids.end()) {
                std::string list;
                for (const std::string& name : ids) list += (list.empty() ? "" : ", ") + name;
                call->fail("No check " + id.dump() + ". Checks: " + list + ".");
                return;
            }
            request.checks.push_back(id.get<std::string>());
        }
    }
    if (args.contains("expect")) {
        std::string why;
        if (!validateExpect(args["expect"], why)) {
            call->fail(why);
            return;
        }
        request.expect = args["expect"];
    }
    const std::string failOn = args.value("fail_on", std::string());
    if (!failOn.empty() && failOn != "fail" && failOn != "warn") {
        call->fail("fail_on is fail or warn.");
        return;
    }

    const AudioSettings& settings = project->getAudioSettings();
    request.qa = settings.qa;
    request.maxVoices = settings.maxVoices;
    SettingsContext context{project, ctx.consoleWindow(), ctx.isPlaying()};
    request.targets.targetLufs = targetSetting(context, "audio.loudness_target_lufs", request.targets.targetLufs);
    request.targets.toleranceLu = targetSetting(context, "audio.loudness_tolerance_lu", request.targets.toleranceLu);
    request.targets.ceilingDbtp = targetSetting(context, "audio.true_peak_ceiling_dbtp", request.targets.ceilingDbtp);
    request.projectDirectory = project->getProjectPath();
    request.assetsDirectory = project->getAssetsPath();
    for (const AudioMixerGroupDef& group : AudioMixer::getDef().groups) {
        for (const AudioMixerEffectDef& effect : group.effects) {
            if (effect.type != AudioEffectType::Duck || effect.bypass || effect.sidechainGroup.empty()) continue;
            AudioQa::DuckEffect duck;
            duck.group = group.name;
            duck.sidechain = effect.sidechainGroup;
            auto level = effect.params.find("level");
            if (level != effect.params.end()) duck.level = level->second;
            request.ducks.push_back(std::move(duck));
        }
    }

    const std::string scene = work->capture.session.value("scene", std::string());
    request.rounds = failRounds().previous(scene);

    if (args.contains("baseline") && args["baseline"].is_string()) {
        const std::string name = args["baseline"].get<std::string>();
        std::string why;
        const fs::path file = AudioBaseline::isName(name) ? AudioBaseline::file(project->getProjectPath(), name) : fs::path();
        std::error_code ec;
        if (!file.empty() && fs::exists(file, ec)) {
            if (!AudioBaseline::read(file, request.baseline, why)) {
                call->fail(why);
                return;
            }
        } else if (AudioAnalysis::findCapture(AudioAnalysis::captureRoot(project->getProjectPath()), name, AudioCapture::sessionId(), work->baselineSession, why)) {
            work->baselineIsSession = true;
        } else {
            call->fail("baseline \"" + name + "\" is neither baselines/audio/" + name + ".json nor a session: save one with save_baseline.");
            return;
        }
    }
    if (args.contains("save_baseline")) {
        work->saveBaseline = args.value("save_baseline", std::string());
        if (!AudioBaseline::isName(work->saveBaseline)) {
            call->fail("save_baseline is a name without path separators: it writes baselines/audio/<name>.json.");
            return;
        }
    }

    JobPtr job = ctx.jobs().create("audio_session", "Check audio capture " + work->capture.id);
    jobs::submit([work, job]() {
        try {
            runCheck(*work);
        } catch (const std::exception& e) {
            work->error = std::string("Checking the capture failed: ") + e.what();
        }
        if (work->error.empty()) job->succeed("Checked.", work->data);
        else job->fail(work->error);
    }, {jobs::Lane::Background, jobs::GateClass::Default, "audio-session-check"});
    replyWhenJobDone(call, job, true, WAIT_SECONDS, [work, failOn, scene, project](const McpJob&) {
        if (!work->error.empty()) return ToolResult::error(work->error);
        Json& data = work->data;
        std::string text = "Verdict " + data.value("verdict", "") + " on capture " + work->capture.id + ".";
        failRounds().record(scene, data["checks"]);
        for (const Json& check : data["checks"]) text += "\n" + qa::rowLine(check);
        if (!data["thresholds"]["changed"].empty()) text += "\nThresholds changed from their defaults: " + data["thresholds"]["changed"].dump() + ".";
        text += "\n" + data.value("guidance", "");

        if (!work->saveBaseline.empty()) {
            const fs::path file = AudioBaseline::file(project->getProjectPath(), work->saveBaseline);
            auto* command = new WriteFilesCmd({{file, work->baselineToSave.dump(2) + "\n"}}, [project](const std::vector<fs::path>& changed) {
                ProjectFileWrites::filesWritten(project, changed);
            });
            command->setName("Save Audio Baseline");
            command->setNoMerge();
            auto failure = std::make_shared<std::string>();
            command->reportFailureTo(failure);
            if (!project->getProjectCommandHistory()->addCommand(command)) return ToolResult::error("Writing " + file.generic_string() + " failed: " + *failure, data);
            data["baseline_saved"] = "baselines/audio/" + work->saveBaseline + ".json";
            text += "\nBaseline saved: " + data["baseline_saved"].get<std::string>() + ", one undo step.";
        }

        const std::string verdict = data.value("verdict", "");
        const bool failed = (failOn == "fail" && verdict == "FAIL") || (failOn == "warn" && (verdict == "FAIL" || verdict == "WARN"));
        ToolResult result = failed ? ToolResult::error(text, data) : ToolResult::success(text, data);
        if (!work->png.empty()) result.images.push_back({work->png, "image/png"});
        return result;
    });
}

void audioSessionTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string action = args.value("action", std::string());
    if (action == "status") {
        statusAction(call);
        return;
    }
    if (action == "arm") {
        armAction(call);
        return;
    }
    if (action != "report" && action != "check") {
        call->fail("action is status, arm, report or check.");
        return;
    }

    auto work = std::make_shared<SessionWork>();
    work->start = args.value("start", 0.0);
    work->end = args.value("end", -1.0);
    if (work->end >= 0.0 && work->end <= work->start) {
        call->fail("end must be after start.");
        return;
    }
    work->eventsLimit = args.value("events_limit", DEFAULT_EVENTS);
    work->image = args.value("image", action == "report");
    work->width = args.value("max_width", DEFAULT_WIDTH);
    if (work->start < 0.0) {
        call->fail("start is 0 or more, in seconds.");
        return;
    }
    if (work->eventsLimit < 0 || work->eventsLimit > MAX_EVENTS) {
        call->fail("events_limit is from 0 to " + std::to_string(MAX_EVENTS) + ".");
        return;
    }
    if (work->width < AudioImages::MIN_WIDTH || work->width > AudioImages::MAX_WIDTH) {
        call->fail("max_width is from " + std::to_string(AudioImages::MIN_WIDTH) + " to " + std::to_string(AudioImages::MAX_WIDTH) + ".");
        return;
    }
    Project* project = call->ctx().project();
    std::string error;
    if (!AudioAnalysis::findCapture(AudioAnalysis::captureRoot(project->getProjectPath()), args.value("session", std::string("last")),
                                    AudioCapture::sessionId(), work->capture, error)) {
        call->fail(error);
        return;
    }
    if (action == "report") reportAction(call, work);
    else checkAction(call, work);
}

} // namespace

void registerAudioSessionTools(McpRegistry& registry) {
    using namespace schema;

    // the rules and the vocabularies are in the guide; the handler validates
    Json expect = array(object({
        {"on", string("")},
        {"entity", string("")},
        {"sound", string("")},
        {"within_ms", number("")},
        {"offset_ms", number("")},
        {"align", string("")}
    }, {"on"}), "What must sound.");

    ToolDef tool;
    tool.name = "audio_session";
    tool.title = "Audio Session";
    tool.description =
        "You can't hear: a Play's capture as numbers. status; arm: whether the next Play captures; report: loudness, "
        "peaks, groups, numbered events and markers, an image; check: verdicts, what limits each, the fix. jasper://docs/sound.";
    tool.inputSchema = object({
        {"action", stringEnum("", {"status", "arm", "report", "check"})},
        {"session", string("Default last; current, an id or a directory.")},
        {"start", number("s.")},
        {"end", number("s.")},
        {"events_limit", integer("Default 200.")},
        {"image", boolean("Default report yes, check no.")},
        {"max_width", integer("")},
        {"capture", boolean("arm.")},
        {"max_seconds", integer("arm.")},
        {"stems", boolean("arm.")},
        {"checks", array(string(""), "Default all.")},
        {"expect", expect},
        {"baseline", string("A name or a session.")},
        {"save_baseline", string("A name.")},
        {"fail_on", stringEnum("Error at this verdict or worse.", {"fail", "warn"})}
    }, {"action"});
    tool.writesFiles = [](const Json& args) { return args.contains("save_baseline"); };
    tool.activity = ToolActivity::Read;
    tool.allowedWhilePlaying = true;
    tool.handler = audioSessionTool;
    registry.addTool(std::move(tool));
}

}
