// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RunControl.h"
#include "player/InputScriptRun.h"
#include "TestRunnerConfig.h"
#if JASPERENGINE_TEST_RUNNER
#include "player/PlayerTests.h"
#endif
#include "debug/ConsoleCommands.h"
#include "audio/AudioCapture.h"
#include "manager/SceneManager.h"
#include "subsystem/AudioSystem.h"
#include "PlatformConfig.h"

#include "Engine.h"
#include "Log.h"
#include "System.h"
#include "io/AtomicFile.h"
#include "pool/ShaderPool.h"
#include "pool/TextureDataPool.h"
#include "profiler/PerfSampler.h"
#include "log/LogSink.h"
#include "log/Logger.h"

#include "stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

using namespace jasperengine;

namespace jasperengine::player {

namespace {

    struct State {
        bool audioCaptureBegun = false;
        LaunchOptions options;
        bool active = false;
        bool counting = false;          // frames are being counted (from the ready frame with --after-ready)
        uint64_t counted = 0;
        uint64_t drawn = 0;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        bool quit = false;
        int quitCode = EXIT_OK;
        bool screenshotWritten = false;
        // --after-ready with --fixed-fps: the game's update events and clocks (scripts, timers)
        // start at the ready frame, so a run's frames are the same every time (how many frames
        // come before it depends on how long shaders take). Loading and drawing go on meanwhile.
        bool heldUntilReady = false;
        // the screenshot's frame came with shaders still compiling at runtime, shaders or
        // pipelines still being created off the render thread (spec S32), or textures still
        // loading: the game is held and the capture waits for them (as the editor's screenshots
        // do), up to CAPTURE_WAIT_SECONDS; then it fails, naming them (never a stand-in image)
        bool captureWaiting = false;
        bool captureHeldGame = false;
        std::chrono::steady_clock::time_point captureWaitStarted;
        std::atomic<bool> errorLogged{false};
        Logger::SinkId errorSink = 0;
        bool testsBegun = false;
        // --command: the next line to run, and whether the gate was checked (at the first frame,
        // after the game's init set it)
        size_t nextCommand = 0;
        bool commandsChecked = false;
        bool perfWindowWritten = false;
    };

    State& state() {
        static State* s = new State();
        return *s;
    }

    constexpr double CAPTURE_WAIT_SECONDS = 120.0;

    bool (*gpuCreationIdleProbe)() = nullptr;

    // --perf-summary (R12): the JSON through the atomic writer, or on the web a console line
    void writePerfSummaryFile(const PerfSummary& summary) {
        const std::filesystem::path& path = state().options.perfSummary;
        if (path.empty()) return;
        const std::string json = PerfSampler::toJson(summary);
#if defined(__EMSCRIPTEN__)
        std::printf("JASPERENGINE PERF: %s\n", json.c_str());
        std::fflush(stdout);
#else
        const io::WriteResult written = io::writeFileAtomic(path, json + "\n");
        if (!written.ok())
            Log::error("--perf-summary: %s", written.message.c_str());
#endif
    }

    void writePerfWindow(const PerfSummary& summary) {
        writePerfSummaryFile(summary);
        state().perfWindowWritten = true;
    }

    // nothing drawn as a stand-in: no shader or pipeline being made, no texture loading
    bool gpuCreationIdle() {
        return gpuCreationIdleProbe ? gpuCreationIdleProbe() : ShaderPool::isCreationIdle() && TextureDataPool::getLoadingCount() == 0;
    }

    std::string describeCreation() {
        std::string out;
        if (!ShaderPool::isCreationIdle()) out = "shaders or pipelines being made";
        if (const size_t textures = TextureDataPool::getLoadingCount()) {
            std::string list;
            for (const std::string& id : TextureDataPool::getLoadingIds(8)) list += (list.empty() ? "" : ", ") + id;
            out += (out.empty() ? "" : "; ") + std::to_string(textures) + " texture load(s): " + list;
        }
        return out.empty() ? std::string("nothing named (a test probe)") : out;
    }

    // the frame a screenshot is taken at: --screenshot-frame, else --quit-after's last
    // (0: none known, the frame the game quits on)
    uint64_t screenshotTarget() {
        const LaunchOptions& o = state().options;
        if (o.screenshotFrame > 0) return o.screenshotFrame;
        return o.quitAfter;
    }

    class ErrorWatchSink : public LogSink {
    public:
        ErrorWatchSink() { minLevel.store(LogLevel::Error); }
        void write(const LogRecord& record) override {
            if (record.level >= LogLevel::Error) RunControl::noteError();
        }
        Delivery delivery() const override { return Delivery::Inline; }
        const char* name() const override { return "fail-on-error"; }
    };

    void appendPng(void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    }

    bool writePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, int width, int height, std::string& error) {
        if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4) {
            error = "no frame pixels";
            return false;
        }
        // opaque: the swap chain shows no alpha, and a cleared target's alpha would show through
        std::vector<uint8_t> opaque(rgba);
        for (size_t i = 3; i < opaque.size(); i += 4) opaque[i] = 255;
        std::string png;
        if (!stbi_write_png_to_func(&appendPng, &png, width, height, 4, opaque.data(), width * 4)) {
            error = "cannot encode " + path.string();
            return false;
        }
        // the path may hold an earlier run's image: it stays whole if this write fails
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        options.createDirectories = false;
        const io::WriteResult written = io::writeFileAtomic(path, png, options);
        if (!written.ok()) {
            error = written.message;
            return false;
        }
        return true;
    }

}

void RunControl::reset() {
    State& s = state();
    if (s.errorSink != 0) {
        Logger::removeSink(s.errorSink);
        s.errorSink = 0;
    }
    s.options = LaunchOptions();
    s.active = false;
    s.counting = false;
    s.counted = 0;
    s.drawn = 0;
    s.started = std::chrono::steady_clock::now();
    s.quit = false;
    s.quitCode = EXIT_OK;
    s.screenshotWritten = false;
    if (s.heldUntilReady || s.captureHeldGame) Engine::pauseGameEvents(false);
    s.heldUntilReady = false;
    s.captureWaiting = false;
    s.captureHeldGame = false;
    gpuCreationIdleProbe = nullptr;
    s.errorLogged.store(false);
    InputScriptRun::reset();
    s.testsBegun = false;
    s.nextCommand = 0;
    s.commandsChecked = false;
    s.perfWindowWritten = false;
}

void RunControl::configure(const LaunchOptions& options) {
    reset();
    State& s = state();
    s.options = options;
    InputScriptRun::configure(s.options);
    s.active = s.options.quitAfter > 0 || s.options.afterReady || !s.options.screenshot.empty() || s.options.failOnError ||
               !s.options.commands.empty();
    // the commands wait for the ready frame, and the frames count after them
    s.counting = !s.options.afterReady && s.options.commands.empty();
}

const LaunchOptions& RunControl::options() {
    return state().options;
}

void RunControl::engineStarted() {
    State& s = state();
    s.started = std::chrono::steady_clock::now();
    InputScriptRun::engineStarted(s.options);
#if JASPERENGINE_TEST_RUNNER
    if (s.options.runTests) PlayerTests::launch(s.options);
#endif
    if (s.options.fixedFps > 0.0)
        Engine::setFixedDeltatime(1.0 / s.options.fixedFps);
    if (s.options.fixedFps > 0.0 && s.options.afterReady) {
        Engine::pauseGameEvents(true);
        s.heldUntilReady = true;
    }
    // a test run owns the errors (project-test-runner R11, R27): --fail-on-error waits
    if (s.options.failOnError && !s.options.runTests && s.errorSink == 0 && Logger::isInitialized())
        s.errorSink = Logger::addSink(std::make_unique<ErrorWatchSink>());
}

void RunControl::beginAudioCapture() {
#if JASPERENGINE_AUDIO_CAPTURE
    State& s = state();
    if (s.audioCaptureBegun || s.options.audioCapture.empty()) return;
    s.audioCaptureBegun = true;
    AudioCaptureOptions options;
    options.root = s.options.audioCapture;
    options.project = platformConfig().title;
    options.scene = SceneManager::getCurrentSceneName();
    options.seed = AudioSystem::getConfig().seed;
    // no project settings here: the run's --quit-after ends it
    options.maxSeconds = 3600;
    options.keep = 20;
    std::string error;
    if (!AudioCapture::begin(options, &error)) Log::error("--audio-capture: %s", error.c_str());
#endif
}

bool RunControl::isHeadless() {
    return state().options.headless;
}

bool RunControl::isActive() {
    return state().active;
}

bool RunControl::wantsReadySignal() {
    return state().options.afterReady || !state().options.commands.empty() || !state().options.perfSummary.empty();
}

void RunControl::engineReady() {
    State& s = state();
    if (s.options.perfSummary.empty()) return;
    PerfSampler::setWindowWriter(&writePerfWindow);
    if (!PerfSampler::wasSetByGame()) {
        PerfSampler::setSampling(true);
        Log::print("Performance sampling: on (--perf-summary %s)", s.options.perfSummary.string().c_str());
    }
}

void RunControl::writePerfSummary() {
    State& s = state();
    if (s.options.perfSummary.empty()) return;
    const PerfSummary summary = PerfSampler::summary();
    if (s.perfWindowWritten && summary.seconds <= 0.0) return;
    writePerfSummaryFile(summary);
}

bool RunControl::wantsScreenshot() {
    const State& s = state();
    return s.active && !s.options.screenshot.empty() && !s.screenshotWritten;
}

bool RunControl::wantsOffscreenFrame() {
    const State& s = state();
    if (s.active && InputScriptRun::wantsOffscreenFrame()) return true;
#if JASPERENGINE_TEST_RUNNER
    // a test case's capture reads the composite of the next frame
    if (s.active && s.options.runTests && PlayerTests::wantsFrame()) return true;
#endif
    if (!s.active || s.options.screenshot.empty() || s.screenshotWritten) return false;
    if (s.captureWaiting) return true;
    const uint64_t target = screenshotTarget();
    if (target == 0) return true;   // the frame the game quits on, not known before it
    if (!s.counting) return target == 1;   // the ready frame could be the first counted one
    return s.counted + 1 == target;
}

void RunControl::frameDrawn(bool readySignalled, const FrameReader& reader) {
    State& s = state();
    if (!s.active) return;
    s.drawn++;

    InputScriptRun::frameDrawn(reader);
    if (!s.options.commands.empty() && !s.commandsChecked) {
        s.commandsChecked = true;
        if (!console::commandsEnabled()) {
            Log::error("%s", commandsOffRefusal());
            requestQuit(EXIT_BAD_ARGUMENTS);
            return;
        }
    }

    if (!s.counting) {
        if (readySignalled) {
            if (s.heldUntilReady) {
                Engine::pauseGameEvents(false);
                s.heldUntilReady = false;
            }
            // one --command a frame (a scene load or a spawn it asks for lands before the next)
            if (s.nextCommand < s.options.commands.size()) {
                const std::string& line = s.options.commands[s.nextCommand++];
                const console::Result result = console::execute(line, console::Source::LaunchOption);
                if (result.ok())
                    Log::print("[command] %s: ok", line.c_str());
                else
                    Log::error("[command] %s: %s (%s)", line.c_str(), console::statusName(result.status), result.error.c_str());
                return;
            }
            s.counting = true;
            // the input script starts where the frames count: after the --command lines
            InputScriptRun::ready();
        } else {
            const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.started).count();
            if (waited > s.options.readyTimeout) {
                Log::error("Not ready after %.0f s (--ready-timeout): no frame logged \"Engine ready:\". Waiting for: %s",
                           s.options.readyTimeout, Engine::describeNotLoaded().c_str());
                requestQuit(EXIT_NOT_READY);
            }
            return;
        }
    }
    // the capture: its frame's image, read from the offscreen frame just drawn
    auto capture = [&s, &reader]() {
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        std::string error;
        if (reader && reader(rgba, width, height, error) && writePng(s.options.screenshot, rgba, width, height, error)) {
            s.screenshotWritten = true;
            Log::print("Screenshot: %s (%dx%d, frame %llu)", s.options.screenshot.string().c_str(), width, height,
                       (unsigned long long)s.counted);
        } else {
            Log::error("Screenshot %s failed: %s", s.options.screenshot.string().c_str(), reader ? error.c_str() : "the frame did not draw offscreen");
            requestQuit(EXIT_SCREENSHOT_FAILED);
        }
    };

#if JASPERENGINE_TEST_RUNNER
    // --run-tests (project-test-runner R23): the run starts at the ready frame and ends the process
    if (s.options.runTests && !s.quit) {
        if (!s.testsBegun) {
            s.testsBegun = true;
            PlayerTests::begin(s.options);
        } else {
            PlayerTests::frame(reader);
        }
        return;
    }
#endif
    if (s.captureWaiting) {
        // frames drawn while the capture waits for GPU creation: the game held, none counted
        const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.captureWaitStarted).count();
        const bool idle = gpuCreationIdle();
        if (!idle && waited < CAPTURE_WAIT_SECONDS) return;
        s.captureWaiting = false;
        if (idle) {
            capture();
        } else {
            Log::error("Screenshot %s not taken: still loading after %.0f s: %s", s.options.screenshot.string().c_str(),
                       CAPTURE_WAIT_SECONDS, describeCreation().c_str());
            requestQuit(EXIT_SCREENSHOT_FAILED);
        }
        if (s.captureHeldGame) {
            Engine::pauseGameEvents(false);
            s.captureHeldGame = false;
        }
    } else {
        s.counted++;
        if (!s.options.screenshot.empty() && !s.screenshotWritten) {
            const uint64_t target = screenshotTarget();
            const bool due = target != 0 ? s.counted == target : s.quit;
            // (a game quitting by itself, with no frame named, is captured as it ends)
            if (due && target != 0 && !gpuCreationIdle()) {
                s.captureWaiting = true;
                s.captureWaitStarted = std::chrono::steady_clock::now();
                if (!Engine::areGameEventsPaused()) {
                    Engine::pauseGameEvents(true);
                    s.captureHeldGame = true;
                }
                Log::print("Screenshot: frame %llu waits for %s", (unsigned long long)s.counted, describeCreation().c_str());
                return;
            }
            if (due) capture();
        }
    }

    if (s.options.quitAfter > 0 && s.counted >= s.options.quitAfter && !s.quit) {
        Log::print("Quit after %llu frames (game time %.9f s)", (unsigned long long)s.counted, Engine::getGameTime());
        requestQuit(EXIT_OK);
    }
}

void RunControl::requestQuit(int code) {
    State& s = state();
    if (!s.quit) {
        s.quit = true;
        s.quitCode = code;
    }
    if (System::hasInstance())
        System::instance().quit();
}

bool RunControl::quitRequested() {
    return state().quit;
}

int RunControl::finish() {
    State& s = state();
    if (s.quit && s.quitCode != EXIT_OK) return s.quitCode;
    if (s.options.failOnError && !s.options.runTests && s.errorLogged.load()) return EXIT_ERROR_LOGGED;
    if (!s.options.screenshot.empty() && !s.screenshotWritten) {
        std::fprintf(stderr, "Screenshot %s was not written: the game ended before the frame it names\n",
                     s.options.screenshot.string().c_str());
        return EXIT_SCREENSHOT_FAILED;
    }
    return s.quitCode;
}

void RunControl::setGpuCreationIdleProbe(bool (*probe)()) {
    gpuCreationIdleProbe = probe;
}

uint64_t RunControl::countedFrames() {
    return state().counted;
}

void RunControl::noteError() {
    state().errorLogged.store(true);
}

}  // namespace jasperengine::player
