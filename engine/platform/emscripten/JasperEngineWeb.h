// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JasperEngineWeb_h
#define JasperEngineWeb_h

#include <emscripten/html5.h>

#include "System.h"

#include <string>
#include <vector>

#define JASPERENGINE_WEB_MAX_GAMEPADS 16

class JasperEngineWeb: public jasperengine::System{

private:

    typedef struct GamepadState{
        bool connected = false;
        bool buttons[64] = {false};
        float axes[64] = {0.0f};
        float triggers[2] = {0.0f};
    } GamepadState;

    static std::string canvas;

    static int syncWaitTime;
    static bool enabledIDB;
    static bool loadedIDB;
    // the project waits for the IndexedDB restore (at most 3 s from startTime, ms)
    static bool projectStarted;
    static double startTime;

    static int screenWidth;
    static int screenHeight;

    static double mousePosX;
    static double mousePosY;

    static int sampleCount;

    static bool quitRequested;

    // pause sources (Engine::systemPause while any is on): the page hidden, a CrazyGames ad,
    // the WebGL context lost
    static bool pageHidden;
    static bool adPlaying;
    static bool contextLost;
    // the window's focus (applied once the project has started)
    static bool windowFocused;
    // requestAnimationFrame intervals of the first frames (Screen.refreshRate)
    static double lastFrameTime;
    static std::vector<double> frameIntervals;
    static float measuredRefreshRate;

    static GamepadState gamepads[JASPERENGINE_WEB_MAX_GAMEPADS];

    static EM_BOOL key_callback(int eventType, const EmscriptenKeyboardEvent *e, void *userData);
    static EM_BOOL mouse_callback(int eventType, const EmscriptenMouseEvent *e, void *userData);
    static EM_BOOL wheel_callback(int eventType, const EmscriptenWheelEvent *e, void *userData);
    static EM_BOOL touch_callback(int emsc_type, const EmscriptenTouchEvent* emsc_event, void* user_data);

    static EM_BOOL canvas_resize(int eventType, const void *reserved, void *userData);
    static EM_BOOL visibility_callback(int eventType, const EmscriptenVisibilityChangeEvent* e, void* userData);
    static EM_BOOL webgl_context_callback(int emsc_type, const void* reserved, void* user_data);
    static EM_BOOL pointerlock_callback(int eventType, const EmscriptenPointerlockChangeEvent* e, void* userData);
    static EM_BOOL focus_callback(int eventType, const EmscriptenFocusEvent* e, void* userData);

    static EM_BOOL renderLoop(double time, void* userdata);

    static void unregisterCallbacks();
    static void syncFileSystemNow();

    static wchar_t toCodepoint(const std::string &u);
    static std::string toUTF8(wchar_t cp);
    static int jasperengine_mouse_button(int button);
    static int jasperengine_gamepad_button(int button);

    static void pollGamepads();
    // the web's memory heuristic (spec S56 R7): the wasm heap near its maximum or, where the
    // browser has performance.memory, the JS heap near its limit is a low-memory warning
    static void pollMemoryPressure();

public:

    JasperEngineWeb();

    static void setEnabledIDB(bool enabledIDB);
    static bool hasProjectStarted() { return projectStarted; }

    static int init(int argc, char **argv);
    static void changeCanvasSize(int width, int height);
    // a pause source turned on or off (CrazyGames ads, the WebGL context)
    static void setAdPlaying(bool playing);
    static void setContextLost(bool lost);
    static void updatePause();

    virtual int getScreenWidth() override;
    virtual int getScreenHeight() override;

    virtual int getSampleCount() override;

    virtual bool isTouchDevice() override;

    virtual bool isFullscreen() override;
    virtual void requestFullscreen() override;
    virtual void exitFullscreen() override;

    // Sets the browser tab title; the other window APIs keep the base
    // no-ops since the canvas has no OS window
    virtual void setWindowTitle(const std::string& title) override;
    virtual void quit() override;

    virtual void showVirtualKeyboard(std::wstring text) override;
    virtual void hideVirtualKeyboard() override;

    // async browser clipboard: writes go to navigator.clipboard, reads return the text of the
    // last copy or paste event
    virtual void setClipboardText(const std::string& utf8) override;
    virtual std::string getClipboardText() override;
    virtual void setTextInputArea(float x, float y, float width, float height) override;
    // the paste event handler
    static void rememberClipboardText(const std::string& text);

    virtual void setMouseCursor(jasperengine::CursorType type) override;
    virtual void setMouseMode(jasperengine::MouseMode mode) override;

    virtual std::string getUserDataPath() override;
    virtual std::string getCrashReportPath() override;

    virtual bool syncFileSystem() override;
    virtual bool flushFileSystem() override;
    virtual bool isUserDataLoaded() override;

    virtual std::string getStringForKey(const char *key, const std::string& defaultValue) override;
    virtual void setStringForKey(const char* key, const std::string& value) override;
    virtual void removeKey(const char *key) override;
    virtual bool hasKey(const char *key) override;
    virtual void removeAllKeys() override;
    virtual void saveUserSettings() override;

    // platform services (navigator, screen.orientation, the wake lock)
    virtual std::vector<std::string> getPreferredLocales() override;
    virtual std::string getOperatingSystem() override;
    virtual std::string getOperatingSystemVersion() override;
    virtual std::string getDeviceModel() override;
    virtual jasperengine::DeviceType getDeviceType() override;
    virtual int getSystemMemorySize() override;
    virtual std::string getGpuRendererName() override;
    virtual jasperengine::NetworkReachability getInternetReachability() override;
    virtual bool vibrate(int milliseconds) override;
    virtual bool hapticFeedback(jasperengine::HapticFeedback type) override;
    virtual float getScreenDpi() override;
    virtual float getRefreshRate() override;
    virtual void setKeepScreenOn(bool keepOn) override;
    virtual void applyOrientationPolicy(jasperengine::ScreenOrientation requested, uint8_t allowedMask) override;
    virtual jasperengine::ScreenOrientation getScreenOrientation() override;

    virtual void initializeCrazyGamesSDK() override;
    virtual void showCrazyGamesAd(const std::string& type) override;
    virtual void happytimeCrazyGames() override;
    virtual void gameplayStartCrazyGames() override;
    virtual void gameplayStopCrazyGames() override;
    virtual void loadingStartCrazyGames() override;
    virtual void loadingStopCrazyGames() override;

protected:
    virtual bool platformOpenURL(const std::string& url) override;
};


#endif /* JasperEngineWeb_h */
