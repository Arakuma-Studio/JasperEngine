// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What a run with the launch options does frame by frame (docs/specs/S30-shipping.md R17-R20):
// counting drawn frames (from the ready frame with --after-ready), the screenshot frame, the
// quit, the ready timeout and the exit code. Engine::runFrame asks it whether the next frame
// draws offscreen (the one a screenshot reads) and tells it each drawn frame; a run without
// these options never gets past isActive().

#include "Export.h"
#include "player/LaunchOptions.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::player {

    // R19
    enum ExitCode : int {
        EXIT_OK = 0,
        EXIT_BAD_ARGUMENTS = 2,
        EXIT_ERROR_LOGGED = 3,
        EXIT_SCREENSHOT_FAILED = 4,
        EXIT_CONTENT_MISMATCH = 5,
        EXIT_NOT_READY = 6,
        EXIT_NO_DISPLAY = 7
    };

    // the composited frame: width * height RGBA8, rows top first; false when it cannot be read
    using FrameReader = std::function<bool(std::vector<uint8_t>& rgba, int& width, int& height, std::string& error)>;

    class JASPERENGINE_API RunControl {
    public:
        // the parsed options, before Engine::systemInit (clears what an earlier run left)
        static void configure(const LaunchOptions& options);
        static const LaunchOptions& options();

        // Engine::systemInit, after its logger started: the error watch, the fixed frame time,
        // the scripts' arguments
        static void engineStarted();

        // Engine::runFrame, once: --audio-capture's session starts with the start scene's first frame
        static void beginAudioCapture();

        static bool isHeadless();
        // a frame option is in use (quit, ready wait, screenshot, error watch)
        static bool isActive();
        // the ready signal must run whatever the environment says
        static bool wantsReadySignal();

        // Engine::runFrame, before the draw: the frame draws into the engine's composite
        // framebuffer, so its pixels can be read after it
        static bool wantsOffscreenFrame();
        // a screenshot is still to be written (the run draws offscreen from its first frame)
        static bool wantsScreenshot();
        // after the frame drew and committed: readSignalled is the engine's ready state, reader
        // reads the composite (null when the frame was not offscreen)
        static void frameDrawn(bool readySignalled, const FrameReader& reader);

        // Engine::quit and the timeouts: ends the run with this code (the first one wins)
        static void requestQuit(int code);
        static bool quitRequested();
        // the process's exit code once the main loop ended: the requested one, else 3 for a
        // logged error (--fail-on-error), else 4 for a screenshot asked for and not written
        static int finish();

        // frames counted so far (from the ready frame with --after-ready)
        static uint64_t countedFrames();

        // a log sink calls this for every error-level record (--fail-on-error)
        static void noteError();

        // test support: forget everything
        static void reset();
        // test support: what tells whether shaders and pipelines are still being made (compiled
        // at runtime, or created off the render thread) or textures still loading: the
        // screenshot waits for them; null: ShaderPool::isCreationIdle and no texture load
        static void setGpuCreationIdleProbe(bool (*probe)());
    };

}
