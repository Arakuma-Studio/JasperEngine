// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "Project.h"
#include "AppSettings.h"
#include "window/PlayInput.h"
#include "window/DeviceProfiles.h"
#include "window/GameResolutions.h"
#include "device/Device.h"

#include <set>

namespace jasperengine::editor {

    // Unity-style Game view. While a scene plays the game renders here: the engine
    // draws it into this view's framebuffer at this view's size, even while the tab
    // is hidden behind another, and the view forwards game input while it is hovered
    // or focused. When stopped it shows the selected scene through its game camera
    // (a secondary view of the Scene view's frame, see App::engineRender).
    class GameWindow {
    private:
        Project* project;
        PlayInput* playInput;

        bool windowOpen = true;
        bool focusRequested = false;
        // Drawn in the last show(): the view the engine should size the game for
        bool visible = false;
        bool maximizedShown = false;
        // Maximize On Play's hint (the key that shows the editor again) fades out by then
        double maximizeHintUntil = 0.0;
        bool focused = false;
        bool cursorInView = false;
        bool hostedLastFrame = false;
        // Frames waited for a Scene view to dock next to on first launch
        int dockWaitFrames = 0;

        GameViewSettings settings;
        bool settingsLoaded = false;

        // Game image size in engine pixels, and the scale from ImGui to engine pixels
        int width = 0;
        int height = 0;
        // the simulated device's orientation the size above is laid out for
        ScreenOrientation layoutOrientation = ScreenOrientation::Portrait;
        // the Device menu's Custom language field
        char customLanguage[32] = {0};
        bool customLanguageOpen = false;
        // the Device menu's Custom... device fields
        CustomDeviceSettings customDeviceEdit;
        bool customDeviceOpen = false;
        // what the last syncSafeArea pushed to Screen (the simulated insets)
        struct SafeAreaPush {
            const DeviceProfile* device = nullptr;
            ScreenOrientation orientation = ScreenOrientation::Portrait;
            int width = -1, height = -1;
            CustomDeviceSettings custom;
        };
        SafeAreaPush safeAreaPushed;
        uint64_t editSafeAreaRevision = 0;
        // scenes whose Scene view was hidden when the safe area changed: they redraw once shown
        // (a hidden view left dirty would keep the Game view's preview drawing every frame)
        std::set<uint32_t> safeAreaRedrawPending;
        ImVec2 engineScale = ImVec2(1.0f, 1.0f);
        // the Recorder's output size (S27 R27): the game renders at it, shown scaled to fit
        int overrideWidth = 0;
        int overrideHeight = 0;
        // the game renders at a fixed resolution (a preset or a device) shown at fitScale
        bool fixed = false;
        float fitScale = 1.0f;
        // the "Add custom..." popup's fields
        char customName[64] = {0};
        int customWidth = 1920;
        int customHeight = 1080;
        float customDpi = 0.0f;
        bool customPopupRequested = false;
        void drawCustomResolutionPopup();

        // Scene drawn by the edit-mode pass and whether it needs drawing again
        uint32_t editPassSceneId = NULL_PROJECT_SCENE;
        bool editPassDirty = true;
        // the played scene whose RmlUi debugger the toolbar showed (hidden again on stop)
        uint32_t htmlUiDebuggerSceneId = NULL_PROJECT_SCENE;
        bool editPassRendered = false;
        // what the view shows: the running game, or the edit-mode pass
        Framebuffer framebuffer;
        // the Transition menu's preview values (Project Settings' defaults until changed)
        bool transitionPreviewSet = false;
        float transitionPreviewDuration = 0.6f;
        Vector4 transitionPreviewColor;
        char transitionPreviewRamp[512] = {0};

        void loadSettings();
        void saveSettings();
        // the device simulation of the settings into editor::Platform (System and Screen in Play)
        void applySimulation();
        // the simulated device's insets to Screen (scaled to the render size) when they change,
        // and in edit mode the project canvas the editor scenes lay UI out for (Scene view too)
        void syncSafeArea();
        // the orientation the render size is laid out for
        ScreenOrientation renderOrientation() const;
        // the safe rectangle drawn over an image of w x h: Screen's in Play, the device's stopped
        Rect safeAreaInImage(float w, float h) const;
        void drawDeviceMenu();
        // Device: Auto | Low | Medium | High | Ultra, its platform family and thermal state (S56)
        void drawDeviceTierMenu();
        void drawToolbar();
        // Transition ▾ (S48 R40): plays a screen transition over the view, edit mode or Play
        void drawTransitionMenu();
        void drawContent(uint32_t sceneId);
        void drawStats(ImVec2 imageMin, ImVec2 imageMax, const SceneProject* sceneProject);
        bool computeAspect(float& aspect) const;
        uint32_t getTargetSceneId() const;
        // S41: the shown scene's main camera draws through a Pixel Perfect Camera (its request)
        bool shownPixelPerfect(PixelPerfectViewRequest& request) const;
        // the toolbar's Pixel Scale (Auto, 1x .. 8x; scales that do not fit are disabled) and readout
        void drawPixelScale(const PixelPerfectViewRequest& request);

    public:
        static constexpr const char* WINDOW_NAME = "Game";

        GameWindow(Project* project, PlayInput* playInput);

        void show();

        void setOpen(bool open);
        bool isOpen() const;
        // Brings the Game tab to front, opening it if needed (play start)
        void requestFocus();

        // sceneId is the running game: it renders into this view's framebuffer at
        // getRenderWidth() x getRenderHeight(), whether the view is shown or not
        bool hostsScene(uint32_t sceneId) const;
        // sceneId is stopped and needs the edit-mode pass through its game camera
        bool wantsEditPass(uint32_t sceneId) const;
        bool isEditPassDirty() const;
        void markEditPassRendered(uint32_t sceneId);
        Framebuffer* getFramebuffer();

        // drawn in the last show()
        bool isVisible() const;
        // size of the game image as last laid out (0 before the view was ever shown)
        int getWidth() const;
        int getHeight() const;
        // size the game renders at: the laid out one, or the project canvas before any
        int getRenderWidth() const;
        int getRenderHeight() const;
        // the game renders at exactly w x h (the Recorder), the view showing it scaled to fit;
        // 0, 0 goes back to the view's own size
        void setRenderSizeOverride(int w, int h);
        bool isGameCursorInView() const;
        // The game image has keyboard focus (keys go to the game while playing)
        bool hasKeyboardFocus() const;

        void resetProjectState();

        // The toolbar's settings (aspect, scale, Maximize On Play, Stats); applySettings()
        // sets them as if picked there (MCP configure_game_view)
        const GameViewSettings& getSettings();
        void applySettings(const GameViewSettings& newSettings);
        // Maximize On Play (F11, Edit > Maximize On Play, the toolbar): the saved setting, acting
        // at once during Play
        void toggleMaximizeOnPlay();
        bool isMaximizeOnPlay();
        // the Game view fills the editor now (maximized in the last show())
        bool isMaximizedNow() const { return maximizedShown; }
        // what Play simulates of the device (Device::setSimulation, applied each frame of Play):
        // the toolbar's Device Tier, platform family (Auto: Mobile with a simulated phone) and
        // thermal state
        DeviceSimulation getDeviceSimulation() const;
        static int getAspectCount();
        // the simulated device's safe rectangle in a game image of w x h pixels (MCP screenshot
        // safe_area_in_image); false without a device
        bool getSafeAreaInImage(float w, float h, Rect& rect) const;

        // the orientation of the simulated device the game image is laid out for (the engine's
        // Screen.orientation with a device; App hands it over with the size)
        ScreenOrientation getLayoutOrientation() const;
        // Rotate: the simulated device's next orientation the game allows (Play) or any (stopped).
        // False when it cannot turn (no device, or the game locks one orientation); error says why.
        bool rotateDevice(std::string* error = nullptr);
        // turns the simulated device to an orientation (the toolbar's setting too); false when
        // there is no device or it cannot show it now (getRotatableOrientations)
        bool turnDevice(ScreenOrientation orientation);
        // The orientations the simulated device may turn to now: the device's, within what a
        // playing game allows
        uint8_t getRotatableOrientations() const;
        // the Device menu's languages (BCP 47), System first as ""
        static const std::vector<std::string>& languagePresets();

        // A fixed resolution the game renders at: the simulated device's screen, else the
        // resolution preset; false in aspect mode (the view's own size)
        bool getFixedResolution(int& width, int& height, float& dpi) const;
        // a preset or custom name, "WxH", "WxH@dpi"; "" or "free" back to aspect mode
        bool setResolution(const std::string& name, std::string* error = nullptr);
        // the project's custom resolutions (editor workspace); a name already used is replaced
        bool addCustomResolution(const GameResolution& resolution, std::string* error = nullptr);
        void removeCustomResolution(const std::string& name);
    };

}
