// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A build without the input scripts module (input/InputScriptSwitch.h): the calls the engine, the
// Lua state and a player's run control make, answered as when no script, recording or replay runs.
// An --input-script is refused at start, as a file that does not load is (R26: [input] failed and,
// with --quit-after-input, exit code 10), naming a build that has the module.

#include "input/InputScriptSwitch.h"
#if !JASPERENGINE_INPUT_SCRIPTS_ENABLED

#include "Log.h"
#include "input/InputRecorder.h"
#include "input/InputReplay.h"
#include "input/InputScriptRunner.h"
#include "player/InputScriptRun.h"

namespace jasperengine::input {

    bool InputRecorder::record(const RecordedEvent&, std::string_view) { return true; }
    InputRecorder::EngineMade::EngineMade() = default;
    InputRecorder::EngineMade::~EngineMade() = default;
    void InputRecorder::afterUpdate() {}
    bool takePendingLuaSeed(uint32_t&) { return false; }

    bool ScriptRunner::isActive() { return false; }
    void ScriptRunner::stopAll() {}
    void ScriptRunner::beforeFrame(bool) {}
    void ScriptRunner::afterUpdate() {}

    void Replay::stopAll() {}

}

namespace jasperengine::player::InputScriptRun {

    void configure(LaunchOptions&) {}
    bool isActive() { return false; }
    void engineStarted(LaunchOptions& options) {
        if (options.inputScript.empty()) return;
        Log::print("[input] failed {\"event\":-1,\"error\":\"this build has no input scripts: run a Development Build, or a "
                   "release build of a project whose scripts use InputRecording or InputReplay\"}");
        if (options.quitAfterInput) RunControl::requestQuit(EXIT_INPUT_FAILED);
    }
    void ready() {}
    bool wantsOffscreenFrame() { return false; }
    void frameDrawn(const FrameReader&) {}
    void reset() {}
    // nothing to fetch: engineStarted refuses the flag
    bool isFetchedUrl(const std::string&) { return false; }

}

#endif // !JASPERENGINE_INPUT_SCRIPTS_ENABLED
