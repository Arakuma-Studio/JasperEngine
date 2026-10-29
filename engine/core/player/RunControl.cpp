// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RunControl.h"

#include "Engine.h"
#include "Log.h"
#include "System.h"
#include "log/LogSink.h"
#include "log/Logger.h"

#include "stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::player;

namespace {

    struct State {
        LaunchOptions options;
        bool active = false;
        bool counting = false;          // frames are being counted (from the ready frame with --after-ready)
        uint64_t counted = 0;
        uint64_t drawn = 0;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        bool quit = false;
        int quitCode = EXIT_OK;
        bool screenshotWritten = false;
        // --after-ready with --fixed-fps: the simulation waits for the ready frame, so a run's
        // frames are the same every time (the frames before it depend on how long shaders take)
        bool heldUntilReady = false;
        std::atomic<bool> errorLogged{false};
        Logger::SinkId errorSink = 0;
    };

    State& state() {
        static State* s = new State();
        return *s;
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

    bool writePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, int width, int height, std::string& error) {
        if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4) {
            error = "no frame pixels";
            return false;
        }
        // opaque: the swap chain shows no alpha, and a cleared target's alpha would show through
        std::vector<uint8_t> opaque(rgba);
        for (size_t i = 3; i < opaque.size(); i += 4) opaque[i] = 255;
        if (!stbi_write_png(path.string().c_str(), width, height, 4, opaque.data(), width * 4)) {
            error = "cannot write " + path.string();
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
    if (s.heldUntilReady) Engine::setSimulationHeld(false);
    s.heldUntilReady = false;
    s.errorLogged.store(false);
}

void RunControl::configure(const LaunchOptions& options) {
    reset();
    State& s = state();
    s.options = options;
    s.active = options.quitAfter > 0 || options.afterReady || !options.screenshot.empty() || options.failOnError;
    s.counting = !options.afterReady;
}

const LaunchOptions& RunControl::options() {
    return state().options;
}

void RunControl::engineStarted() {
    State& s = state();
    s.started = std::chrono::steady_clock::now();
    if (s.options.fixedFps > 0.0)
        Engine::setFixedDeltatime(1.0 / s.options.fixedFps);
    if (s.options.fixedFps > 0.0 && s.options.afterReady) {
        Engine::setSimulationHeld(true);
        s.heldUntilReady = true;
    }
    if (s.options.failOnError && s.errorSink == 0 && Logger::isInitialized())
        s.errorSink = Logger::addSink(std::make_unique<ErrorWatchSink>());
}

bool RunControl::isHeadless() {
    return state().options.headless;
}

bool RunControl::isActive() {
    return state().active;
}

bool RunControl::wantsReadySignal() {
    return state().options.afterReady;
}

bool RunControl::wantsScreenshot() {
    const State& s = state();
    return s.active && !s.options.screenshot.empty() && !s.screenshotWritten;
}

bool RunControl::wantsOffscreenFrame() {
    const State& s = state();
    if (!s.active || s.options.screenshot.empty() || s.screenshotWritten) return false;
    const uint64_t target = screenshotTarget();
    if (target == 0) return true;   // the frame the game quits on, not known before it
    if (!s.counting) return target == 1;   // the ready frame could be the first counted one
    return s.counted + 1 == target;
}

void RunControl::frameDrawn(bool readySignalled, const FrameReader& reader) {
    State& s = state();
    if (!s.active) return;
    s.drawn++;

    if (!s.counting) {
        if (readySignalled) {
            s.counting = true;
            if (s.heldUntilReady) {
                Engine::setSimulationHeld(false);
                s.heldUntilReady = false;
            }
        } else {
            const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.started).count();
            if (waited > s.options.readyTimeout) {
                Log::error("Not ready after %.0f s (--ready-timeout): no frame logged \"Engine ready:\"", s.options.readyTimeout);
                requestQuit(EXIT_NOT_READY);
            }
            return;
        }
    }
    s.counted++;

    if (!s.options.screenshot.empty() && !s.screenshotWritten && reader) {
        const uint64_t target = screenshotTarget();
        const bool due = target != 0 ? s.counted == target : s.quit;
        if (due) {
            std::vector<uint8_t> rgba;
            int width = 0, height = 0;
            std::string error;
            if (reader(rgba, width, height, error) && writePng(s.options.screenshot, rgba, width, height, error)) {
                s.screenshotWritten = true;
                Log::print("Screenshot: %s (%dx%d, frame %llu)", s.options.screenshot.string().c_str(), width, height,
                           (unsigned long long)s.counted);
            } else {
                Log::error("Screenshot %s failed: %s", s.options.screenshot.string().c_str(), error.c_str());
                requestQuit(EXIT_SCREENSHOT_FAILED);
            }
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
    if (s.options.failOnError && s.errorLogged.load()) return EXIT_ERROR_LOGGED;
    if (!s.options.screenshot.empty() && !s.screenshotWritten) {
        std::fprintf(stderr, "Screenshot %s was not written: the game ended before the frame it names\n",
                     s.options.screenshot.string().c_str());
        return EXIT_SCREENSHOT_FAILED;
    }
    return s.quitCode;
}

uint64_t RunControl::countedFrames() {
    return state().counted;
}

void RunControl::noteError() {
    state().errorLogged.store(true);
}
