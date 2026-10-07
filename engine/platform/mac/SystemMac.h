// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// jasperengine::System over WindowMac. It keeps no state of its own, so the editor
// driving WindowMac directly and a game going through here stay in agreement.

#ifndef SystemMac_h
#define SystemMac_h

#include "System.h"

namespace jasperengine {

    class SystemMac: public System {
    public:

        int getScreenWidth() override;
        int getScreenHeight() override;

#if defined(SOKOL_METAL) || defined(SOKOL_VULKAN)
        // The GL backend renders into the default framebuffer, which the base
        // class already describes. Metal and Vulkan have to hand sokol the device
        // and the drawable this frame renders into instead.
        sg_environment getSokolEnvironment() override;
        sg_swapchain getSokolSwapchain() override;
#endif

        bool isFullscreen() override;
        void requestFullscreen() override;
        void exitFullscreen() override;

        bool isWindowMaximized() override;
        void maximizeWindow() override;
        void restoreWindow() override;
        void setWindowSize(int width, int height) override;
        bool isWindowResizable() override;
        void setWindowResizable(bool resizable) override;
        void setWindowTitle(const std::string& title) override;
        void quit() override;

        void setMouseCursor(CursorType type) override;
        void setMouseMode(MouseMode mode) override;
        void setMousePosition(float x, float y) override;

        // NSPasteboard's general pasteboard, plain text
        void setClipboardText(const std::string& utf8) override;
        std::string getClipboardText() override;
        // There is no virtual keyboard on macOS: the text area places the IME
        // candidate window, and hiding ends a composition the engine dropped
        void setTextInputArea(float x, float y, float width, float height) override;
        void hideVirtualKeyboard() override;

        std::string getAssetPath() override;
        std::string getUserDataPath() override;
        std::string getCachePath() override;
        std::string getLuaPath() override;
        std::string getShaderPath() override;
    };

}

#endif /* SystemMac_h */
