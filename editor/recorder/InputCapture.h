// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A .input.json replayed while the Game view is captured (docs/specs/input-recorder-and-input-scripts.md
// R23, R38): the whole replay once per resolution, each in a new session (the recording's seed and
// scene, the game held until its scene runs), the Game view rendered at that size. Shots writes
// only the frames of the file's `shot` steps as <shot>.png; a PNG sequence or a movie is the
// Recorder's (RecorderSession) for the frames asked. Output: <output>/<WxH>/ per resolution (the
// output itself without resolutions), and <output>/input-capture.json listing, per resolution,
// the files and the replay's divergence. MCP's record_game {input} and the Capture Screenshots
// dialog drive this one run.

#include "recorder/RecorderSettings.h"

#include "input/InputScript.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::recordinput {

    namespace fs = std::filesystem;

    struct CaptureSize {
        int width = 0;
        int height = 0;
        std::string name() const { return std::to_string(width) + "x" + std::to_string(height); }
    };

    struct CaptureRequest {
        std::string input;                          // project-relative or absolute
        std::shared_ptr<input::ParsedScript> script;
        std::vector<CaptureSize> sizes;             // empty: one run at the Game view's size
        fs::path output;                            // absolute
        bool shots = true;                          // false: the Recorder's frames (recorder settings)
        recorder::Settings recorder;                // the frames, fps, format of a sequence or a movie
        bool exitPlay = true;                       // the last run stops Play when done
    };

    class InputCapture {
    public:
        static InputCapture& get();

        struct Host {
            std::function<void(int, int)> setGameRenderSize;    // 0, 0: the view's own
            std::function<void()> stopPlay;
            std::function<bool()> playStopped;                  // no session, not even stopping
        };
        void setHost(Host host);

        // starts the first run; false with error (busy, Play running)
        bool start(CaptureRequest request, std::string& error);
        void cancel();
        bool active() const { return running; }
        // App, every frame
        void tick();

        // "1280x720 (2/3): frame 120/600"
        std::string progressText() const;
        double progress() const;

        // the finished run: ok, the error of the run that failed, and the sidecar's content
        struct Result {
            bool ok = false;
            bool cancelled = false;
            std::string error;
            input::ScriptJson runs = input::ScriptJson::array();   // per resolution: resolution, folder, files, replay, recording
            fs::path sidecar;
            std::vector<fs::path> files;
        };
        const Result& lastResult() const { return result; }
        void setOnFinished(std::function<void(const Result&)> callback) { onFinished = std::move(callback); }

    private:
        enum class Step : uint8_t { Start, Playing, Stopping };

        Host host;
        CaptureRequest request;
        Result result;
        bool running = false;
        bool cancelling = false;
        size_t index = 0;
        Step step = Step::Start;
        uint64_t recorderSerial = 0;
        bool recorderStarted = false;
        std::function<void(const Result&)> onFinished;

        fs::path folderOf(size_t run) const;
        bool startRun(std::string& error);
        void endRun();
        void finish(const std::string& error);
    };

}
