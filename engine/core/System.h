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

    class JASPERENGINE_API System {
    private:

        friend class Engine;
        // TEST-ONLY seam (no engine code defines it): a test detaches and restores the instance
        friend struct SystemTestAccess;

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
        // Whether a System instance is set (Engine::systemInit sets it; instance() aborts while none
        // is). It says nothing about the System object being alive: Engine::systemShutdown keeps the
        // instance set. For code that also runs without the engine (editor CLI tools).
        static bool hasInstance() { return systemInstance != nullptr; }

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

        virtual char getDirSeparator();
        
        virtual std::string getAssetPath();
        virtual std::string getUserDataPath();
        // Data the engine can rebuild (the runtime shader cache): the user data directory,
        // except on Apple platforms, which keep caches apart from what the user saves
        virtual std::string getCachePath();
        virtual std::string getLuaPath();
        virtual std::string getShaderPath();
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

        virtual void platformLog(const int type, const char *fmt, va_list args);

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
