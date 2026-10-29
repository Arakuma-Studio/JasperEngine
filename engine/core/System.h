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
#include <string>
#include <vector>
#include "io/Data.h"
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

    class JASPERENGINE_API System {
    private:

        friend class Engine;
        friend class EarlySystemScope;

        static System* systemInstance;

        // Only Engine is allowed to inject an external System implementation.
        static void setSystemInstance(System* system);

    protected:

        System() {}

        // the base clipboard (setClipboardText/getClipboardText)
        std::string clipboardText;

    public:
        std::vector<std::string> args;

        static System& instance();
        // false before the platform set one (the unit tests, the editor's command line)
        static bool hasInstance();

        virtual ~System() {}

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
        virtual bool syncFileSystem();
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
