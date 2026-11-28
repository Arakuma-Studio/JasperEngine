// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "recorder/InputCapture.h"

#include "Out.h"
#include "recorder/RecordInput.h"
#include "recorder/RecorderSession.h"
#include "util/FileUtils.h"

#include "input/InputReplay.h"
#include "io/AtomicFile.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::recordinput {

InputCapture& InputCapture::get() {
    static InputCapture instance;
    return instance;
}

void InputCapture::setHost(Host newHost) {
    host = std::move(newHost);
}

fs::path InputCapture::folderOf(size_t run) const {
    return request.sizes.empty() ? request.output : request.output / request.sizes[run].name();
}

bool InputCapture::start(CaptureRequest newRequest, std::string& error) {
    if (running) {
        error = "A capture is running: cancel it or wait for it";
        return false;
    }
    if (recorder::RecorderSession::get().active()) {
        error = "A recording is running (the Recorder window or record_game): stop it first";
        return false;
    }
    if (host.playStopped && !host.playStopped()) {
        error = "Play is running: a capture replays the file in its own sessions (stop Play first)";
        return false;
    }
    if (!newRequest.script) {
        error = "no input file";
        return false;
    }
    request = std::move(newRequest);
    result = Result();
    index = 0;
    cancelling = false;
    running = true;
    if (!startRun(error)) {
        running = false;
        if (host.setGameRenderSize) host.setGameRenderSize(0, 0);
        return false;
    }
    return true;
}

bool InputCapture::startRun(std::string& error) {
    const fs::path folder = folderOf(index);
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = "cannot create " + FileUtils::pathToGenericUtf8(folder) + ": " + ec.message();
        return false;
    }
    const bool sized = !request.sizes.empty();
    if (host.setGameRenderSize) host.setGameRenderSize(sized ? request.sizes[index].width : 0, sized ? request.sizes[index].height : 0);
    if (!RecordInput::get().startSession(request.script, request.input, folder, request.shots, error)) return false;
    recorderStarted = false;
    if (!request.shots) {
        recorder::Settings settings = request.recorder;
        if (sized) {
            settings.resolution = recorder::Resolution::Custom;
            settings.customWidth = request.sizes[index].width;
            settings.customHeight = request.sizes[index].height;
        }
        settings.outputFolder = FileUtils::pathToUtf8(folder);
        settings.sequenceName = "frame_<Frame>";
        // the run stops Play itself, once the replay's report is read
        settings.exitPlayMode = false;
        settings.recordOnPlay = false;
        recorderSerial = recorder::RecorderSession::get().resultSerial();
        if (!recorder::RecorderSession::get().startWithPlay(settings, error)) {
            if (host.stopPlay) host.stopPlay();
            return false;
        }
        recorderStarted = true;
    }
    step = Step::Playing;
    return true;
}

void InputCapture::endRun() {
    const RecordInput& input = RecordInput::get();
    const bool sized = !request.sizes.empty();
    Json run = {{"folder", FileUtils::pathToGenericUtf8(folderOf(index))}};
    if (sized) run["resolution"] = request.sizes[index].name();
    Json files = Json::array();
    std::string runError;
    if (request.shots) {
        for (const Shot& shot : input.shots()) {
            if (!shot.error.empty()) {
                runError = "shot \"" + shot.name + "\": " + shot.error;
                continue;
            }
            files.push_back(FileUtils::pathToGenericUtf8(shot.path));
            result.files.push_back(shot.path);
        }
        if (files.empty() && runError.empty()) runError = "the file took no shot (a shot step names each image: {\"type\": \"shot\", \"name\": \"01_menu\"})";
    } else {
        const recorder::Result& recorded = recorder::RecorderSession::get().lastResult();
        for (const fs::path& file : recorded.files) {
            files.push_back(FileUtils::pathToGenericUtf8(file));
            result.files.push_back(file);
        }
        if (recorded.files.empty() && !recorded.output.empty() && recorded.ok) {
            files.push_back(FileUtils::pathToGenericUtf8(recorded.output));
            result.files.push_back(recorded.output);
        }
        run["recording"] = recorded.sidecarJson();
        if (!recorded.ok) runError = recorded.error;
    }
    run["files"] = files;
    if (const input::Replay* replay = input.currentReplay(); !replay || replay->isStopped())
        runError = "Play stopped before the replay ended";
    Json replay = input.replayingJson();
    if (replay.is_object()) {
        if (replay.contains("error")) runError = runError.empty() ? replay["error"].get<std::string>() : runError;
        run["replay"] = {{"compared", replay.value("compared", 0)}, {"comparable", replay.value("comparable", false)},
                         {"diverged_at_frame", replay.value("diverged_at_frame", Json(nullptr))},
                         {"differences", replay.value("differences", Json::array())}};
    }
    if (!runError.empty()) run["error"] = runError;
    result.runs.push_back(run);
    const bool last = index + 1 >= std::max<size_t>(1, request.sizes.size());
    if (!runError.empty() || !last || request.exitPlay) {
        if (host.stopPlay) host.stopPlay();
        step = Step::Stopping;
        if (!runError.empty()) cancelling = true;
        return;
    }
    finish("");
}

void InputCapture::tick() {
    if (!running) return;
    const bool stopped = !host.playStopped || host.playStopped();
    if (step == Step::Playing) {
        RecordInput& input = RecordInput::get();
        if (cancelling) {
            if (recorderStarted && recorder::RecorderSession::get().active()) recorder::RecorderSession::get().stop();
            if (host.stopPlay) host.stopPlay();
            step = Step::Stopping;
            return;
        }
        // (Play ending under the run, a script error or a stop, ends it too: endRun says so)
        const bool done = request.shots ? (!input.isReplaying() && (input.replayStarted() || stopped))
                                        : recorder::RecorderSession::get().resultSerial() != recorderSerial;
        if (done) endRun();
        return;
    }
    if (step == Step::Stopping) {
        if (!stopped) return;
        if (cancelling) {
            const Json& lastRun = result.runs.empty() ? Json() : result.runs.back();
            std::string why = lastRun.is_object() && lastRun.contains("error")
                ? (lastRun.contains("resolution") ? lastRun["resolution"].get<std::string>() + ": " : std::string()) + lastRun["error"].get<std::string>()
                : std::string();
            result.cancelled = why.empty();
            finish(why.empty() ? "cancelled" : why);
            return;
        }
        index++;
        if (index >= std::max<size_t>(1, request.sizes.size())) {
            finish("");
            return;
        }
        std::string error;
        if (!startRun(error)) finish(request.sizes[index].name() + ": " + error);
    }
}

void InputCapture::cancel() {
    if (running) cancelling = true;
}

std::string InputCapture::progressText() const {
    if (!running) return std::string();
    const size_t count = std::max<size_t>(1, request.sizes.size());
    std::string text = request.sizes.empty() ? std::string() : request.sizes[index].name() + " ";
    text += "(" + std::to_string(index + 1) + "/" + std::to_string(count) + ")";
    const input::Replay* replay = RecordInput::get().currentReplay();
    const input::ParsedScript* script = RecordInput::get().replayScript();
    if (replay && script) text += ": frame " + std::to_string(std::max<int64_t>(replay->frame(), 0)) + "/" + std::to_string(script->lastFrame());
    else text += ": starting";
    return text;
}

double InputCapture::progress() const {
    if (!running) return 0.0;
    const size_t count = std::max<size_t>(1, request.sizes.size());
    double within = 0.0;
    const input::Replay* replay = RecordInput::get().currentReplay();
    const input::ParsedScript* script = RecordInput::get().replayScript();
    if (replay && script && script->lastFrame() > 0)
        within = std::clamp(static_cast<double>(std::max<int64_t>(replay->frame(), 0)) / script->lastFrame(), 0.0, 1.0);
    return (static_cast<double>(index) + within) / count;
}

void InputCapture::finish(const std::string& error) {
    running = false;
    if (host.setGameRenderSize) host.setGameRenderSize(0, 0);
    result.ok = error.empty();
    result.error = error;
    Json sidecar = {{"input", request.input}, {"format", request.shots ? "shots" : "recorder"}, {"runs", result.runs}};
    if (!error.empty()) sidecar["error"] = error;
    const fs::path file = request.output / "input-capture.json";
    const io::WriteResult written = io::writeFileAtomic(file, sidecar.dump(2) + "\n");
    if (written.ok()) result.sidecar = file;
    else Out::warning("Capture: the sidecar %s was not written: %s", FileUtils::pathToGenericUtf8(file).c_str(), written.message.c_str());
    if (result.ok) Out::success("Captured %zu file(s) into %s", result.files.size(), FileUtils::pathToGenericUtf8(request.output).c_str());
    else if (!result.cancelled) Out::error("Capture failed: %s", error.c_str());
    if (onFinished) {
        auto callback = std::move(onFinished);
        onFinished = nullptr;
        callback(result);
    }
}

}  // namespace jasperengine::editor::recordinput
