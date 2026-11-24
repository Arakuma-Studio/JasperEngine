// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Only with the input scripts module (input/InputScriptSwitch.h); InputScriptsOff.cpp answers the
// engine without it
#include "input/InputScriptSwitch.h"
#if JASPERENGINE_INPUT_SCRIPTS_ENABLED

#include "player/InputScriptRun.h"

#include "Engine.h"
#include "Log.h"
#include "input/InputRecorder.h"
#include "input/InputReplay.h"
#include "io/AtomicFile.h"
#include "io/FileData.h"
#include "io/watch/FileWatcher.h"

#include "stb_image_write.h"

#include <memory>
#include <string>

namespace jasperengine::player::InputScriptRun {

namespace {

    struct State {
        LaunchOptions options;
        bool active = false;
        std::shared_ptr<input::ParsedScript> script;
        std::shared_ptr<input::Replay> replay;
        std::string pendingShot;        // a shot step waiting for the next drawn frame
        uint64_t pendingShotFrame = 0;
        int shots = 0;
        bool shotFailed = false;
        bool heldForShot = false;       // the game waits for the shot's frame (it is drawn offscreen from the next one)
    };

    State& state(){
        static State s;
        return s;
    }

    std::string dump(const nlohmann::json& value){
        return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    void failed(int64_t event, const std::string& error){
        Log::print("[input] failed %s", dump({{"event", event}, {"error", error}}).c_str());
        if (state().options.quitAfterInput)
            RunControl::requestQuit(EXIT_INPUT_FAILED);
    }

    void appendPng(void* context, void* data, int size){
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    }

    // the shot folder: a data:// or content path, or a folder of the file system
    std::string shotPath(const std::string& name){
        std::string folder = state().options.inputShots;
        if (!folder.empty() && folder.back() != '/' && folder.back() != '\\')
            folder += '/';
        return FileData::getSystemPath(folder + name + ".png");
    }

    void finished(input::Replay& replay){
        State& s = state();
        const input::ReplayReport& report = replay.report();
        if (report.failed) {
            failed(static_cast<int64_t>(report.failedEvent), replay.errorText());
            return;
        }
        nlohmann::json checkpoints = {{"compared", report.compared}, {"comparable", report.comparable},
                                      {"diverged_at_frame", report.divergedAtFrame >= 0 ? nlohmann::json(report.divergedAtFrame) : nlohmann::json()}};
        Log::print("[input] done %s", dump({{"frames", report.frames}, {"shots", s.shots}, {"checkpoints", checkpoints}}).c_str());
        if (!s.options.quitAfterInput)
            return;
        if (s.shotFailed)
            RunControl::requestQuit(EXIT_INPUT_FAILED);
        else if (s.options.inputStrict && report.divergedAtFrame >= 0)
            RunControl::requestQuit(EXIT_INPUT_DIVERGED);
        else
            RunControl::requestQuit(EXIT_OK);
    }

}

void configure(LaunchOptions& options){
    State& s = state();
    s = State();
    s.options = options;
    s.active = !options.inputScript.empty();
    if (!s.active)
        return;
    // the file is read when the engine can (content paths); until then: its frame rate is 60 unless given
    options.afterReady = true;
}

bool isActive(){
    return state().active;
}

void engineStarted(LaunchOptions& options){
    State& s = state();
    if (!s.active)
        return;
    std::unique_ptr<FileData> file(FileData::newFile(s.options.inputScript.c_str()));
    if (!file || file->length() == 0) {
        failed(-1, "no input file at " + s.options.inputScript);
        return;
    }
    s.script = std::make_shared<input::ParsedScript>();
    std::string error;
    if (!input::parseScript(file->readString(), *s.script, error)) {
        failed(-1, s.options.inputScript + ": " + error);
        s.script.reset();
        return;
    }
    // the file's frame rate unless --fixed-fps is given
    if (options.fixedFps <= 0.0)
        options.fixedFps = s.script->header.fixedFps;
    if (s.script->header.kind == input::ScriptKind::Recording && s.script->header.hasSeed)
        input::setPendingLuaSeed(s.script->header.seed);
    Log::print("[input] begin %s", dump({{"file", s.options.inputScript},
                                          {"kind", s.script->header.kind == input::ScriptKind::Recording ? "recording" : "script"},
                                          {"events", s.script->events}, {"frames", s.script->lastFrame()}}).c_str());
}

void ready(){
    State& s = state();
    if (!s.active || !s.script || s.replay)
        return;
    input::ReplayOptions options;
    options.blockRealInput = !s.options.inputAllowReal;
    options.stopOnDivergence = s.options.inputStrict;
    options.host.shot = [](const std::string& name, uint64_t frame, std::string&) {
        State& run = state();
        run.pendingShot = name;
        run.pendingShotFrame = frame;
        // the step runs inside a frame that already chose not to draw offscreen: the game holds
        // until the next one draws this state offscreen, as the editor's shot holds the game
        if (!Engine::isSimulationHeld()) {
            Engine::setSimulationHeld(true);
            run.heldForShot = true;
        }
        return true;
    };
    options.host.busy = []() { return !state().pendingShot.empty(); };
    options.onFinished = &finished;
    s.replay = input::Replay::start(s.script, std::move(options));
}

bool wantsOffscreenFrame(){
    return !state().pendingShot.empty();
}

void frameDrawn(const FrameReader& reader){
    State& s = state();
    // the frame of the step itself was not drawn offscreen: the next one is
    if (s.pendingShot.empty() || !reader)
        return;
    const std::string name = s.pendingShot;
    s.pendingShot.clear();
    if (s.heldForShot) {
        Engine::setSimulationHeld(false);
        s.heldForShot = false;
    }
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    std::string error;
    const std::string path = shotPath(name);
    if (reader(rgba, width, height, error) && width > 0 && height > 0) {
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        std::string png;
        if (stbi_write_png_to_func(&appendPng, &png, width, height, 4, rgba.data(), width * 4)) {
            const io::WriteResult written = io::writeFileAtomic(watch::pathFromUtf8(path), png);
            if (written.ok()) {
                s.shots++;
                Log::print("[input] shot %s", dump({{"name", name}, {"path", path}, {"frame", s.pendingShotFrame},
                                                     {"width", width}, {"height", height}}).c_str());
                return;
            }
            error = written.message;
        } else {
            error = "cannot encode the PNG";
        }
    }
    if (error.empty())
        error = "the frame could not be read back";
    s.shotFailed = true;
    Log::error("[input] shot %s failed: %s", name.c_str(), error.c_str());
}

bool isFetchedUrl(const std::string& path){
    for (const char* prefix : {"http://", "https://", "/", "./", "../"}) {
        if (path.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

void reset(){
    State& s = state();
    if (s.replay)
        s.replay->stop();
    if (s.heldForShot)
        Engine::setSimulationHeld(false);
    s = State();
}

}

#endif // JASPERENGINE_INPUT_SCRIPTS_ENABLED
