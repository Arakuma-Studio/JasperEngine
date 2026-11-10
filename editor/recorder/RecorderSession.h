// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A recording of the Game view (docs/specs/S27-editor-tools.md R26-R30, §6.6): Idle -> Arming ->
// Recording -> Finalising -> Idle. While it records, the game runs at the fixed timestep
// 1/fps (Engine::setFixedDeltatime, restored after), the Game view renders at exactly the output
// size, VSync is off and every editor frame renders one game frame. Each game frame is read back
// at the start of the next editor frame (what the frame drew, submitted on every backend) and
// pushed into a queue of at most 8 frames: when it is full the main thread waits, so no frame is
// ever dropped. PNG sequences are encoded on background jobs (stbi_write_png, level 3); movies
// stream raw RGBA frames to FFmpeg's stdin from a writer thread (ProcessStream). Audio, when asked
// for, is mixed offline in step with the frames (SoLoud's null driver) into a WAV that a second
// FFmpeg pass muxes in. The Recorder window and MCP's record_game drive this one session. The
// bounded queues follow OpenEngine's GameViewMovieCapture.cpp (MIT, NOTICE.md).
//
// A recording that reports success is a good one (docs/specs/S62-recorder-integrity.md). A frame
// drawn with anything still loading is drawn again with the game held, until a whole frame is
// drawn with everything ready. The output is written under temporary names and takes its real one
// only once complete. The first write that fails stops the recording (Play goes on): the result
// says what and why, the output path holds nothing, and what was recorded is kept as
// <name>.partial.<ext> only if it decodes.

#include "recorder/RecorderSettings.h"

#include "mcp/McpJson.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine {
    class Framebuffer;
    class Scene;
}

namespace jasperengine::editor::recorder {

    // What the session needs of the editor (App)
    struct Host {
        std::function<bool()> isPlaying;                  // a play session updates (PLAYING or PAUSED)
        std::function<bool()> isPlayStarting;             // a play session is loading
        std::function<bool(std::string&)> startPlay;
        std::function<void()> stopPlay;
        std::function<void()> resetUiDocuments;         // fresh Play: CSS starts at the recording's clock origin
        std::function<Framebuffer*()> gameFramebuffer;
        std::function<Scene*()> gameScene;                // the running game's (null when none)
        std::function<void(int, int)> setGameRenderSize;  // 0, 0: the view's own
        std::function<void(int&, int&)> gameViewSize;
        std::function<std::string()> sceneName;
        std::function<std::filesystem::path()> projectPath;
        std::function<int()> maxTextureSize;
    };

    enum class State : uint8_t { Idle, Arming, Recording, Finalising };

    struct Result {
        bool ok = false;
        std::string error;                // what failed and why, and what was kept
        std::string failedStep;           // audio, video, mux, png, publish, readback, loading ("": no write failed)
        std::filesystem::path partial;    // what was recorded before the failure, when it decodes
        int64_t partialFrames = 0;
        uint64_t estimatedBytes = 0;      // the free-space check at the start
        int64_t freeBytes = -1;
        int64_t frames = 0;
        int fps = 0;
        int width = 0;
        int height = 0;
        std::string encoder;              // "png" for a sequence
        std::string scene;
        int take = 0;
        int64_t startFrame = 0;
        std::vector<std::string> warnings;
        double wallSeconds = 0.0;
        size_t maxQueued = 0;
        std::filesystem::path output;     // the movie, or the sequence's folder (where it would be, on a failure)
        std::vector<std::filesystem::path> files;
        std::filesystem::path sidecar;
        double overriddenFixedFps = 0.0;  // MCP play's fixed_fps the recording replaced (0: none)

        mcp::Json sidecarJson() const;
    };

    // FFmpeg as found for movies (probed off the main thread, cached per executable)
    struct FfmpegInfo {
        bool probed = false;
        std::string path;                 // "" when none
        std::string why;
        std::string h264;                 // the H.264 encoder picked ("" when none)
        bool proRes = false;
    };

    class RecorderSession {
    public:
        static RecorderSession& get();

        void setHost(Host host);

        // Starts with these settings: enters Play when it is not running (the recording then
        // starts with its first frame). False with `error` when it cannot (busy, no FFmpeg for a
        // movie, sizes...). `warnings` gets what was changed (audio off mid-play...)
        bool start(const Settings& settings, std::string& error);
        // stops and finalises (Stop Recording, F10, MCP cancel_job)
        void stop();
        // the editor about to start Play: Record on Play arms here (and the audio capture)
        void onPlayStarting(const Settings& settings);

        // App, at the start of an editor frame (engineRender, before the game updates): the frame
        // the game drew is read back and queued
        void captureFrame();
        // App, immediately before the running game's engine frame, including paused Play.
        void prepareFrame();
        // App, every frame: the state, the end of the frames asked for, Play stopped, finalising
        void update();

        State state() const { return currentState; }
        bool active() const { return currentState != State::Idle; }
        bool recording() const { return currentState == State::Arming || currentState == State::Recording; }
        int64_t framesRecorded() const { return recorded; }
        int64_t gameFrame() const { return frameIndex; }
        const Settings& settings() const { return current; }
        // "REC 00:12 · frame 734" (empty when not recording): the Game view's overlay
        std::string overlayText() const;
        bool audioMuted() const;
        // the start's warning for this recording ("" when none): it may not fit the disk
        const std::string& startWarningText() const { return startWarning; }

        // the last finished recording, and a serial that grows with each
        const Result& lastResult() const { return result; }
        uint64_t resultSerial() const { return serial; }

        const FfmpegInfo& ffmpeg();
        void refreshFfmpeg();

        // called once (main thread) when the recording in progress has finished (MCP record_game)
        void setOnFinished(std::function<void(const Result&)> callback) { onFinished = std::move(callback); }

    private:
        struct Sink;
        RecorderSession() = default;

        Host host;
        Settings current;
        State currentState = State::Idle;
        Result result;
        Result working;
        uint64_t serial = 0;
        std::unique_ptr<Sink> sink;
        std::future<void> finalising;
        int64_t first = 0;
        int64_t last = -1;
        int64_t frameIndex = -1;          // the game frame of the recording last read (-1 none yet)
        uint64_t gameFramesAtStart = 0;
        uint64_t gameFramesSeen = 0;
        int64_t recorded = 0;
        int width = 0;
        int height = 0;
        bool startedPlay = false;
        bool audio = false;
        // the audio tap's subscription while it runs
        int audioTap = 0;
        double previousFixedDelta = 0.0;
        std::optional<double> previousFixedUiTime;
        double previousUiTime = 0.0;
        double recordingUiStart = 0.0;
        bool animationClocksAligned = false;
        std::chrono::steady_clock::time_point startTime;
        FfmpegInfo ffmpegInfo;
        std::future<FfmpegInfo> ffmpegProbe;
        std::mutex mutex;
        std::function<void(const Result&)> onFinished;

        std::string startWarning;

        // what is known of the frame last drawn, read at each capture: the frame between two reads
        // is good when both say all was ready and nothing moved between them
        struct FrameMarks {
            bool assetsIdle = false;      // no shader, pipeline, texture or model being made
            bool settled = false;         // the scene's objects report loaded
            uint32_t waitingDraws = 0;    // draws that found their pipeline being made (ObjectRender)
            uint32_t standIns = 0;        // bindings that got a stand-in texture (Texture)
            uint32_t loadingAnswers = 0;  // bindings of an image still loading (Texture)
            uint32_t textureEpoch = 0;    // images that became ready (TexturePool)
            uint64_t created = 0;         // shaders, pipelines and warm draws finished (S32)
        };
        FrameMarks drawn;
        // a frame not drawn so is drawn again, the game held
        bool holdingFrame = false;
        bool heldBeforeHold = false;
        bool settleGivenUp = false;
        std::chrono::steady_clock::time_point holdStart;
        std::chrono::steady_clock::time_point settleStart;

        bool arm(const Settings& settings, bool fromPlayStart, std::string& error);
        void warnAboutFreeSpace();
        bool assetsLoading() const;
        bool contentSettled() const;
        std::string describeLoading() const;
        FrameMarks readMarks() const;
        bool drawnWithAllReady(const FrameMarks& before, const FrameMarks& after) const;
        void releaseHold();
        void checkHold(const FrameMarks& before);
        void recordFrame();
        void beginRecording();
        void finish();
        void restoreEngine();
    };

}
