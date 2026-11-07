// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "System.h"
#include "Project.h"
#include <cstdarg>
#include <deque>
#include <string>
#include <vector>

namespace jasperengine::editor{

    struct DeviceProfile;

    // The Game view's device simulation (editor only: real devices and builds never see it):
    // the simulated phone or tablet and how it is turned, and the simulated language
    struct PlatformSimulation{
        // nullptr: no device, the editor behaves as a desktop build
        const DeviceProfile* device = nullptr;
        // where the simulated device is turned (the Game view's Rotate, scripts' requests)
        ScreenOrientation orientation = ScreenOrientation::Portrait;
        // BCP 47 tag; "" = the host's languages
        std::string language;
        // the Game view's fixed resolution's dpi (0: none, the host's); a device's own wins
        float dpi = 0.0f;
    };

    // A System.openURL call made in the editor (MCP get_play_state.platform.opened_urls)
    struct OpenedUrl{
        std::string url;
        double time = 0.0;      // Engine::getSystemTime() of the call
        bool launched = false;  // the host's browser was started (not in headless runs)
    };

    class Platform : public System{
    private:
        Project* project;

        static int width;
        static int height;

        static PlatformSimulation simulation;
        // the orientation the Game view's last layout shows (Screen.orientation with a device)
        static ScreenOrientation shownOrientation;
        // the device orientation the Game view toolbar chose (restored at Play stop)
        static ScreenOrientation settingsOrientation;
        // Screen.orientation / autorotateTo* as the game asked for them
        static ScreenOrientation requestedOrientation;
        static uint8_t allowedMask;
        static bool warnedUnsupportedOrientation;
        static std::deque<OpenedUrl> openedUrls;
        static int openedUrlCount;
        static int vibrationCount;
        static int hapticCount;
        static bool hostFocused;
        static bool hostFocusKnown;
        // send_input {type: "app", action: "hide"}: the game updates without drawing
        static bool gameHiddenSimulated;

    protected:
        bool platformOpenURL(const std::string& url) override;

    public:
        Platform(Project* project);

        // the project this Platform resolves paths for (the editor's, the CLI's)
        Project* getProject() const { return project; }

        static bool setSizes(int width, int height);

        // the Game view's device simulation: a profile (nullptr = none) turned as its toolbar says
        static void setSimulatedDevice(const DeviceProfile* device, ScreenOrientation orientation);
        // turns the simulated device (Rotate, MCP configure_game_view / send_input orientation)
        static void setSimulatedOrientation(ScreenOrientation orientation);
        // "" = the host's languages
        static void setSimulatedLanguage(const std::string& language);
        // Screen.dpi of a Game view resolution preset (0 = the host's)
        static void setSimulatedDpi(float dpi);
        static const PlatformSimulation& getSimulation();
        // the orientation of the layout the engine's view has now (App, before setSizes)
        static void setShownOrientation(ScreenOrientation orientation);
        // what the game asked for (the Rotate button skips what it excludes)
        static ScreenOrientation getRequestedOrientation();
        static uint8_t getAllowedMask();

        // Play start and stop: no URLs or vibrations recorded, the toolbar's device orientation,
        // no orientation request
        static void resetPlayRecords();
        static void endPlaySimulation();
        static const std::deque<OpenedUrl>& getOpenedUrls();
        static int getOpenedUrlCount();
        static int getVibrationCount();
        static int getHapticCount();

        // The editor window's OS focus as Engine::systemFocus while a Play session runs (its
        // changes only: Play starts focused); headless MCP runs have no user to focus anything
        static void forwardHostFocus(bool focused, bool playSessionActive);
        // The editor window's visibility as Engine::systemVisibilityChanged while a Play session
        // runs: hidden while the window can't be seen (minimized, unmapped, covered on macOS) or
        // the game is hidden by send_input. Also headless: an Xvfb window is mapped, and a test
        // unmaps it on purpose
        static void forwardHostVisibility(bool windowVisible, bool playSessionActive);
        // send_input {type: "app", action: "hide" / "show"}: the running game updates without
        // drawing (its Game view keeps the last frame) while the window stays visible; ends with Play
        static void setGameHiddenSimulated(bool hidden);
        static bool isGameHiddenSimulated();

        // the host browser is never started: MCP --headless, or Linux without a display
        static bool isHeadless();

        int getScreenWidth() override;
        int getScreenHeight() override;

        std::string getAssetPath() override;
        std::string getLuaPath() override;
        std::string getShaderPath() override;
        std::string getUserDataPath() override;
        std::string getImportPath() override;

        sg_environment getSokolEnvironment() override;
        sg_swapchain getSokolSwapchain() override;

        bool isTouchDevice() override;

        void setMouseMode(MouseMode mode) override;

        // the editor's system clipboard (its X11 / Win32 / NSPasteboard backends, through ImGui)
        void setClipboardText(const std::string& utf8) override;
        std::string getClipboardText() override;

        void quit() override;

        // Starts a program and does not wait for it: argv[0] is looked up on PATH, the
        // arguments are passed as they are (no shell, so paths with spaces stay one argument),
        // and the program outlives the editor. False (and why) when it could not start.
        static bool launchDetached(const std::vector<std::string>& argv, std::string* error = nullptr);
        // Opens a file with the program the system associates with it (xdg-open, open,
        // ShellExecute)
        static bool openWithSystemDefault(const std::string& path, std::string* error = nullptr);

        // Platform services in Play: the host's (HostServices), the simulated device and
        // language of the Game view, the project's Player settings as the application
        std::vector<std::string> getPreferredLocales() override;
        std::string getRegionOverride() override;
        std::string getAppVersion() override;
        int64_t getAppBuild() override;
        std::string getAppIdentifier() override;
        std::string getAppName() override;
        std::string getOperatingSystem() override;
        std::string getOperatingSystemVersion() override;
        std::string getDeviceModel() override;
        DeviceType getDeviceType() override;
        int getSystemMemorySize() override;
        bool isEditor() override;
        NetworkReachability getInternetReachability() override;
        // recorded (MCP), never felt: false
        bool vibrate(int milliseconds) override;
        bool hapticFeedback(HapticFeedback type) override;
        float getScreenDpi() override;
        float getRefreshRate() override;
        void applyOrientationPolicy(ScreenOrientation requested, uint8_t allowedMask) override;
        ScreenOrientation getScreenOrientation() override;
        DeviceOrientation getDeviceOrientation() override;

    };

}
