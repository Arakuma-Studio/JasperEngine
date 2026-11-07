// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "App.h"
#include "renderer/Renderer.h"
#include "util/FrameLimiter.h"

#include <string>

namespace jasperengine::editor{

    struct EditorFrameState{
        bool& redrawRequested;
        double framePeriod = 1.0 / 60.0;
        bool forceRedraw = false;
        // the window can't be seen: minimized, unmapped or _NET_WM_STATE_HIDDEN (Linux)
        bool minimized = false;
        bool focused = false;
        int width = 0;
        int height = 0;
    };

    // Frame loop shared by the native backends, including the idle pacing
    class EditorFrame{
        private:
            Renderer* renderer = nullptr;
            App* app = nullptr;
            void (*platformNewFrame)() = nullptr;
            double lastActivityTime = 0.0;
            // when the next unfocused or minimized frame is due
            double frameDeadline = 0.0;
            // Engine.targetFrameRate while a game plays without vsync
            FrameLimiter playLimiter;

            bool detectActivity(bool& redrawRequested);

        public:
            void init(Renderer& renderer, App& app, void (*platformNewFrame)());

            static std::string formatWindowTitle(const std::string& projectName);

            bool isIdle() const;

            // Returns false when the render target is lost and the editor must close.
            bool run(EditorFrameState& state);
    };

}
