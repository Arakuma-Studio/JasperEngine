// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// jasperengine::System over WindowWin. It keeps no state of its own, so the editor
// driving WindowWin directly and a game going through here stay in agreement.

#ifndef SystemWin_h
#define SystemWin_h

#include "System.h"

namespace jasperengine {

    class WinInputRouter;

    // UTF-16 from the wide Win32 entry points (clipboard, IME) to UTF-8.
    // Unpaired surrogates become U+FFFD.
    std::string winWideToUtf8(const wchar_t* text, int length);

    class SystemWin: public System {
    public:

        // The router tracks the position the engine sees, which a warp has to
        // stay in step with; null when no router is installed.
        explicit SystemWin(WinInputRouter* router = nullptr);

        int getScreenWidth() override;
        int getScreenHeight() override;

#if defined(SOKOL_VULKAN) || defined(SOKOL_D3D11)
        // The GL backend renders into the default framebuffer, which the base
        // class already describes. Vulkan and D3D11 have to hand sokol the
        // device and the views this frame draws into instead.
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

        // No on-screen keyboard on the desktop; a text field losing focus
        // also drops what the IME is still composing for it.
        void hideVirtualKeyboard() override;

        // CF_UNICODETEXT on the system clipboard, owned by the game window
        void setClipboardText(const std::string& utf8) override;
        std::string getClipboardText() override;
        // Kept by the router, which places the IME windows there again when
        // a composition starts.
        void setTextInputArea(float x, float y, float width, float height) override;

        std::string getAssetPath() override;
        std::string getUserDataPath() override;
        std::string getLuaPath() override;
        std::string getShaderPath() override;

        // platform services (HostServicesWin)
        std::vector<std::string> getPreferredLocales() override;
        std::string getRegionOverride() override;
        std::string getOperatingSystem() override;
        std::string getOperatingSystemVersion() override;
        std::string getDeviceModel() override;
        DeviceType getDeviceType() override;
        int getSystemMemorySize() override;
        NetworkReachability getInternetReachability() override;
        float getScreenDpi() override;
        float getRefreshRate() override;
        void setKeepScreenOn(bool keepOn) override;
        void applyFramePacing(const FramePacing& pacing) override;
        int getMaxSwapInterval() override;

    protected:
        bool platformOpenURL(const std::string& url) override;

    private:
        WinInputRouter* router;
    };

}

#endif /* SystemWin_h */
