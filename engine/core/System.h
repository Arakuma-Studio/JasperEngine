// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SYSTEM_H
#define SYSTEM_H

#define D_LOG_VERBOSE 1
#define D_LOG_DEBUG 2
#define D_LOG_WARN 3
#define D_LOG_ERROR 4

#include "Export.h"
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "io/Data.h"
#include "math/EdgeInsets.h"
#include "sokol_gfx.h"

namespace jasperengine {
    class Engine;

    enum class AdMobRating{
        General,
        ParentalGuidance,
        Teen,
        MatureAudience
    };

    enum class CursorType{
        ARROW, // default
        IBEAM,
        CROSSHAIR,
        POINTING_HAND,
        RESIZE_EW,
        RESIZE_NS,
        RESIZE_NWSE,
        RESIZE_NESW,
        RESIZE_ALL,
        NOT_ALLOWED,
        // appended for the CSS cursor keywords of HTML UI documents (wait, progress, help, grab, grabbing)
        WAIT,
        PROGRESS,
        HELP,
        GRAB,
        GRABBING
    };

    // The name the HTML UI specs use (phase-1 spec §3.6.3): one enum for component UI, scripts and
    // documents. POINTER is POINTING_HAND, TEXT is IBEAM and MOVE is RESIZE_ALL.
    using MouseCursor = CursorType;

    // How the mouse cursor behaves. A single authoritative state replaces the old
    // independent "show cursor" and "mouse locked" flags: visibility and motion
    // confinement are two axes of one mode, so only the valid combinations exist.
    enum class MouseMode{
        NORMAL,   // visible, moves freely (default)
        HIDDEN,   // hidden, moves freely (absolute position still reported)
        CAPTURED, // hidden and locked to the window; relative motion only (mouse-look)
        CONFINED  // visible, but cannot leave the window bounds
    };

    class System;

    // A System for code that runs before the platform made its own (a prebuilt player reads its
    // content and game.json before the window exists): installed only while none is set, and
    // removed again when the scope ends
    class JASPERENGINE_API EarlySystemScope {
    public:
        explicit EarlySystemScope(System* system);
        ~EarlySystemScope();
        EarlySystemScope(const EarlySystemScope&) = delete;
        EarlySystemScope& operator=(const EarlySystemScope&) = delete;

    private:
        System* installed = nullptr;
    };

    // Unity's SystemLanguage: the same names and values (Application.systemLanguage)
    enum class SystemLanguage{
        Afrikaans = 0,
        Arabic,
        Basque,
        Belarusian,
        Bulgarian,
        Catalan,
        Chinese,
        Czech,
        Danish,
        Dutch,
        English,
        Estonian,
        Faroese,
        Finnish,
        French,
        German,
        Greek,
        Hebrew,
        Hungarian,
        Icelandic,
        Indonesian,
        Italian,
        Japanese,
        Korean,
        Latvian,
        Lithuanian,
        Norwegian,
        Polish,
        Portuguese,
        Romanian,
        Russian,
        SerboCroatian,
        Slovak,
        Slovenian,
        Spanish,
        Swedish,
        Thai,
        Turkish,
        Ukrainian,
        Vietnamese,
        ChineseSimplified,
        ChineseTraditional,
        Unknown,
        Hindi
    };

    // Unity's ScreenOrientation (Screen.orientation), the same values. LandscapeLeft is the
    // device turned counter-clockwise from portrait (its top edge on the left): Android
    // SCREEN_ORIENTATION_LANDSCAPE, iOS UIInterfaceOrientationLandscapeRight.
    enum class ScreenOrientation{
        Portrait = 1,
        PortraitUpsideDown = 2,
        LandscapeLeft = 3,
        LandscapeRight = 4,
        AutoRotation = 5
    };

    // Unity's DeviceOrientation (Input.deviceOrientation): how the device is held
    enum class DeviceOrientation{
        Unknown = 0,
        Portrait,
        PortraitUpsideDown,
        LandscapeLeft,
        LandscapeRight,
        FaceUp,
        FaceDown
    };

    // Unity's DeviceType (SystemInfo.deviceType)
    enum class DeviceType{
        Unknown = 0,
        Handheld,
        Console,
        Desktop
    };

    // Unity's NetworkReachability: whether a network is there, not whether a server answers
    enum class NetworkReachability{
        NotReachable = 0,
        ReachableViaCarrierDataNetwork,
        ReachableViaLocalAreaNetwork
    };

    // System.hapticFeedback: the iOS feedback generators' kinds, approximated elsewhere. Lua
    // names the last three Success, Warning and Error (X11 defines a Success macro).
    enum class HapticFeedback{
        Light = 0,
        Medium,
        Heavy,
        Selection,
        NotificationSuccess,
        NotificationWarning,
        NotificationError
    };

    // Unity's SleepTimeout constants for Screen.sleepTimeout (an int: SleepTimeout::NeverSleep)
    enum SleepTimeout : int{
        NeverSleep = -1,
        SystemSetting = -2
    };

    // What System.getAppName/Identifier/Version/Build return where the platform has no
    // installed-package information of its own (desktops, the web, the editor): the exported
    // project's Player settings (generated main) or the open project's (editor Play)
    struct ApplicationInfo{
        std::string name;
        std::string identifier;
        std::string version;
        int64_t build = 0;
    };

    // How frames are paced (Engine.targetFrameRate, Engine.vSyncCount), from
    // PlatformRules::computeFramePacing: the swap interval of the presenter, a CPU limiter
    // after each present (0 = none) and the frame rate of mobile frame pacing
    struct FramePacing{
        int swapInterval = 1;
        double limiterFps = 0.0;
        int mobileFps = 60;

        bool operator==(const FramePacing& other) const {
            return swapInterval == other.swapInterval && limiterFps == other.limiterFps && mobileFps == other.mobileFps;
        }
        bool operator!=(const FramePacing& other) const { return !(*this == other); }
    };

    class JASPERENGINE_API System {
    private:

        friend class Engine;
        friend class EarlySystemScope;
        // TEST-ONLY seam (no engine code defines it): a test detaches and restores the instance
        friend struct SystemTestAccess;
        friend struct SafeAreaTestAccess;

        static System* systemInstance;
        static ApplicationInfo applicationInfo;

        // Only Engine is allowed to inject an external System implementation.
        static void setSystemInstance(System* system);

    protected:

        System() {}

        // the base clipboard (setClipboardText/getClipboardText)
        std::string clipboardText;

        // openURL's platform part, for a URL openURL has checked: true when the system took it
        virtual bool platformOpenURL(const std::string& url);

    public:
        std::vector<std::string> args;

        static System& instance();
        // Whether a System instance is set: false before the platform set one (the unit tests,
        // the editor's command line; instance() aborts while none is). It says nothing about the
        // System object being alive: Engine::systemShutdown keeps the instance set.
        static bool hasInstance();

        // What the UserSettings store of this System has unsaved is written
        virtual ~System();

        // *******
        // Used for user and Lua
        // *******
        virtual int getScreenWidth() = 0;
        virtual int getScreenHeight() = 0;

        virtual int getSampleCount();

        // Touch is the main input, as on phones and tablets; a laptop with a touch
        // screen and a mouse is not a touch device
        virtual bool isTouchDevice();

        virtual void showVirtualKeyboard(std::wstring text = L"");
        virtual void hideVirtualKeyboard();

        // The system clipboard, UTF-8 text. The base implementation keeps an in-process
        // string (headless runs, tests); every platform layer replaces it.
        virtual void setClipboardText(const std::string& utf8);
        virtual std::string getClipboardText();
        // Where text is being typed, in screen pixels from the top-left of the view: places
        // the IME candidate window or the platform's text field. The base does nothing.
        virtual void setTextInputArea(float x, float y, float width, float height);

        virtual bool isFullscreen();
        virtual void requestFullscreen();
        virtual void exitFullscreen();

        // Desktop window control. Maximize/resize/resizable are no-ops where
        // the platform offers no such control (mobile, web, sokol app backend);
        // setWindowTitle also applies on web (tab title) and sokol.
        // quit() closes the app, except on iOS where it is a no-op.
        virtual bool isWindowMaximized();
        virtual void maximizeWindow();
        virtual void restoreWindow();
        virtual void setWindowSize(int width, int height);
        virtual bool isWindowResizable();
        virtual void setWindowResizable(bool resizable);
        virtual void setWindowTitle(const std::string& title);
        virtual void quit();

        // *******
        // Platform services: URLs, locale, application and device information, display.
        // Main thread. Device information is cached by the platforms after the first call,
        // except getInternetReachability (cheap, but not for every frame).
        // *******

        // Opens the URL with the system's handler (browser, store, mail app): true when the
        // system took the request. javascript:, data:, file: and vbscript: URLs, URLs
        // without a scheme and URLs with control characters are refused (false, an error
        // logged). Web: a popup the browser blocks opens on the next click, key or touch
        // end on the page instead, and this returns false.
        bool openURL(const std::string& url);

        // The user's languages as BCP 47 tags ("es-MX"), most preferred first; never empty
        // (the base returns {"en-US"})
        virtual std::vector<std::string> getPreferredLocales();
        // getPreferredLocales()[0], normalized ("es-MX")
        std::string getLocale();
        // "es": the language subtag of getLocale(), lower case
        std::string getLanguageCode();
        // "MX": the platform's region setting (getRegionOverride) when it has one, else the
        // region subtag of getLocale(), upper case; "" when neither has one
        std::string getRegionCode();
        // A region set apart from the language (iOS/macOS region, Android default locale
        // country, Windows home location); "" when the platform has none
        virtual std::string getRegionOverride();
        // Unity's Application.systemLanguage, from getLocale()
        SystemLanguage getSystemLanguage();

        // The values the base app getters return (generated main, editor Play start)
        static void setApplicationInfo(const ApplicationInfo& info);
        static const ApplicationInfo& getApplicationInfo();
        // Android: the installed package (versionName, versionCode, package name, label);
        // Apple: the main bundle; elsewhere setApplicationInfo's values
        virtual std::string getAppVersion();
        // the integer build (Android versionCode, CFBundleVersion's first number); 0 unknown
        virtual int64_t getAppBuild();
        virtual std::string getAppIdentifier();
        virtual std::string getAppName();

        // Unity's SystemInfo.operatingSystem format: "Android OS 14 / API-34 (UP1A.231005.007/10754064)",
        // "iOS 17.1", "Mac OS X 14.1.0", "Windows 11 (10.0.22631) 64bit", "Linux 6.8.0 Ubuntu 24.04 LTS"
        virtual std::string getOperatingSystem();
        // "14", "17.1", "10.0.22631", "24.04"
        virtual std::string getOperatingSystemVersion();
        // Build.VERSION.SDK_INT on Android, 0 elsewhere
        virtual int getAndroidApiLevel();
        // "Google Pixel 8", "iPhone16,1", "MacBookPro18,3", the PC's product name, "PC"
        virtual std::string getDeviceModel();
        virtual DeviceType getDeviceType();
        // physical memory in MiB, 0 when unknown
        virtual int getSystemMemorySize();
        // the GPU's name where the render device's own is masked (the web: WebGL's unmasked
        // renderer, WEBGL_debug_renderer_info); "" elsewhere, where Device reads the device name
        virtual std::string getGpuRendererName();
        // logical processors, at least 1
        int getProcessorCount();
        // true only in the editor's Play mode
        virtual bool isEditor();
        virtual NetworkReachability getInternetReachability();
        // false where the device cannot vibrate; iOS ignores the length
        virtual bool vibrate(int milliseconds);
        virtual bool hapticFeedback(HapticFeedback type);

        // Desktop builds: fixed safe-area insets (view pixels) to test a game's notch layout
        // without a phone, from JASPERENGINE_SIMULATE_SAFE_AREA="top,right,bottom,left" at start
        // (Engine::systemViewLoaded pushes them). False when unset or malformed (a warning).
        bool getSimulatedSafeArea(EdgeInsets& insets);

        // Display: dots per inch of the screen (0 unknown) and its refresh rate in Hz
        virtual float getScreenDpi();
        virtual float getRefreshRate();
        // Screen.sleepTimeout's platform part: the screen never dims while keepOn
        virtual void setKeepScreenOn(bool keepOn);
        // Screen.orientation / autorotateTo*: a fixed orientation, or AutoRotation within
        // allowedMask (bit 1 << ScreenOrientation of each allowed one). Stored until the
        // view exists where the platform needs it.
        virtual void applyOrientationPolicy(ScreenOrientation requested, uint8_t allowedMask);
        // The orientation the interface is shown in, never AutoRotation; the base takes it
        // from the screen size (wider than high: LandscapeLeft)
        virtual ScreenOrientation getScreenOrientation();
        // How the device is held; Unknown where nothing reports it (desktops)
        virtual DeviceOrientation getDeviceOrientation();
        // Engine.targetFrameRate / vSyncCount: Engine computes the pacing, the platform applies
        // the swap interval and mobile frame rate (its loop reads Engine::getFramePacing for the limiter)
        virtual void applyFramePacing(const FramePacing& pacing);
        // The longest swap interval the presenter takes (GL and D3D11 present every Nth
        // vblank; Vulkan, Metal and the editor only 0 or 1): longer ones use the limiter
        virtual int getMaxSwapInterval();
        // false where vsync cannot be turned off (the macOS MTKView backend)
        virtual bool canDisableVSync();

        virtual char getDirSeparator();

        virtual std::string getAssetPath();
        virtual std::string getUserDataPath();
        // Data the engine can rebuild (the runtime shader cache): the user data directory,
        // except on Apple platforms, which keep caches apart from what the user saves
        virtual std::string getCachePath();
        // Where a game's crash and hang reports go (diag/CrashHandler.h): <user data>/crash
        // (Android: the internal files dir; iOS: Library/Caches/crash). Empty: no reports (the
        // web, whose crashes the page hooks report, and the unit tests)
        virtual std::string getCrashReportPath();
        virtual std::string getLuaPath();
        virtual std::string getShaderPath();
        // The root of a shipped game's content (content://): game.json, content-manifest.json,
        // the cooked scenes and prefabs, with assets/ and lua/ below it. A player's content
        // root (PlatformConfig), else the directory above the asset path
        virtual std::string getContentPath();
        // Where the editor keeps the models it imports (FBX converted to .glb); empty when
        // each import sits next to its source, as in an exported game
        virtual std::string getImportPath();

        // *******
        // Used only for engine
        // *******
        virtual sg_environment getSokolEnvironment();
        virtual sg_swapchain getSokolSwapchain();

        virtual void setMouseCursor(CursorType type);
        virtual void setMouseMode(MouseMode mode);
        virtual void setMousePosition(float x, float y);

        virtual FILE* platformFopen(const char* fname, const char* mode);
        // The user data files reach persistent storage soon (the web build: IndexedDB, half a
        // second after the last write); elsewhere they are there already
        virtual bool syncFileSystem();
        // ... now (Lua System.syncFileSystem)
        virtual bool flushFileSystem();
        // The user data directory holds what earlier runs saved there. The web build
        // restores it from IndexedDB after it starts; what is written before that is lost.
        virtual bool isUserDataLoaded();

        // The engine logs through Logger (log/Logger.h), not through this: it is only called
        // by a PlatformSink, which an override of installLogSinks adds. The default prints
        // "(ERROR): msg" to stdout.
        virtual void platformLog(const int type, const char *fmt, va_list args);
        // Adds the game's log sinks when the engine starts (Engine::systemInit): by default
        // the platform's (installPlatformLogSinks: the console, logcat, the browser console,
        // os_log, the debugger, and the project's player.log when enabled). A System that
        // overrides platformLog adds a PlatformSink here to keep receiving messages.
        virtual void installLogSinks();

        // resident memory of the process in bytes (the stats overlay); 0 when the platform
        // does not tell. Linux/Android /proc/self/statm, Windows the working set, Apple the
        // physical footprint, Web the WebAssembly heap (debug/ProcessMemory.cpp)
        virtual uint64_t getProcessMemoryBytes();

        virtual bool getBoolForKey(const char *key, bool defaultValue);
        virtual int getIntegerForKey(const char *key, int defaultValue);
        virtual long getLongForKey(const char *key, long defaultValue);
        virtual float getFloatForKey(const char *key, float defaultValue);
        virtual double getDoubleForKey(const char *key, double defaultValue);
        virtual Data getDataForKey(const char *key, const Data& defaultValue);
        virtual std::string getStringForKey(const char *key, const std::string& defaultValue);

        virtual void setBoolForKey(const char *key, bool value);
        virtual void setIntegerForKey(const char *key, int value);
        virtual void setLongForKey(const char *key, long value);
        virtual void setFloatForKey(const char *key, float value);
        virtual void setDoubleForKey(const char *key, double value);
        virtual void setDataForKey(const char *key, Data& value);
        virtual void setStringForKey(const char* key, const std::string& value);

        virtual void removeKey(const char *key);

        // UserSettings (PlayerPrefs): the key is stored (an empty value too)
        virtual bool hasKey(const char *key);
        // UserSettings: every key of this game goes
        virtual void removeAllKeys();
        // UserSettings: what is unsaved is written to disk now (the platform's synchronous write)
        virtual void saveUserSettings();

        // Google AdMob SDK
        virtual void initializeAdMob(bool tagForChildDirectedTreatment = false, bool tagForUnderAgeOfConsent = false);
        virtual void setMaxAdContentRating(AdMobRating rating);
        virtual void loadInterstitialAd(const std::string& adUnitID);
        virtual bool isInterstitialAdLoaded();
        virtual void showInterstitialAd();

        // CrazyGames SDK
        virtual void initializeCrazyGamesSDK();
        virtual void showCrazyGamesAd(const std::string& type);
        virtual void happytimeCrazyGames();
        virtual void gameplayStartCrazyGames();
        virtual void gameplayStopCrazyGames();
        virtual void loadingStartCrazyGames();
        virtual void loadingStopCrazyGames();
    };

}


#endif //SYSTEM_H
