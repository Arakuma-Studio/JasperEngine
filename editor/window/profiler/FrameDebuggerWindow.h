// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "render/RenderStats.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace jasperengine::editor {

    class App;

    // Window > Analysis > Frame Debugger (spec S15 R32, Unity's Frame Debugger over our sokol
    // passes: there is no render graph). Enable shows the live frame of the chosen view (Game,
    // Scene view), Freeze holds the last one. Left: the ordered passes, repeats folded
    // ("shadow 2048x2048 x4"), each pass's draws when the draw list is on. Right: the selected
    // pass's targets, actions, counts and CPU/GPU time, and the preview of one of its
    // attachments (Color, Alpha, Depth, R, G, B), refreshed at most 10 times a second.
    class FrameDebuggerWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Frame Debugger";

        explicit FrameDebuggerWindow(App* app);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return windowFocused; }

        void setEnabled(bool enabled);
        bool isEnabled() const { return enabled; }
        void setFrozen(bool frozen) { this->frozen = frozen; }
        void setViewTag(uint32_t tag) { viewTag = tag; selectedPass = -1; }
        void selectPass(int index) { selectedPass = index; }
        const PassList& getPassList() const { return passes; }

    private:
        App* app = nullptr;
        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequested = false;
        bool enabled = false;
        bool frozen = false;
        bool drawList = false;
        uint32_t viewTag = 0xFFFFFFFFu;
        PassList passes;
        int selectedPass = -1;
        int attachment = 0;
        int mode = 0;
        bool linearDepth = false;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        float depthMin = 0.0f;
        float depthMax = 1.0f;
        std::chrono::steady_clock::time_point lastPreview{};

        std::string viewName(uint32_t tag) const;
        void drawPassTree();
        void drawPassDetail(const PassRecord& pass);
        void drawPreview(const PassRecord& pass);
    };

}
