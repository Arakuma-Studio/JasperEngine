// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef DEBUGOVERLAY_H
#define DEBUGOVERLAY_H

#include "Export.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine {

    class Scene;

    // How the overlay starts (Project Settings > Debug; the generated init of an export, the
    // editor at Play start)
    struct JASPERENGINE_API DebugOverlaySettings {
        // resolved from Project Settings > Debug > Overlay (never / development / always) and
        // the build type; DebugOverlay::setEnabled overrides it at runtime in any build
        bool enabled = false;
        std::string toggleKey = "backquote";    // a key name (InputControlNames::parseKey: "`", "f1"...); "" = none
        std::string statsKey = "f3";            // cycles the stats: off -> fps -> full
        enum class Gesture { ThreeFingerHold, FourFingerTap, Off };   // not None/Always: X11 macros
        Gesture gesture = Gesture::ThreeFingerHold;
        // the bubble with the warning and error counts that opens the console
        enum class Popup { Never, TouchDevices, AllDevices };
        Popup popup = Popup::TouchDevices;
        bool openOnError = false;               // the first error opens the console
        enum class Stats { Off, Fps, Full };
        Stats stats = Stats::Off;               // the stats shown at start
        enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };
        Corner statsCorner = Corner::TopLeft;
        bool allowLua = false;                  // the console's `lua <code>` command
    };

    // The on-device debug overlay (Unity's IngameDebugConsole and Graphy): a log console with
    // filters, search, collapse, stack traces, copy, save and commands; a popup with the
    // warning and error counts; an FPS / stats panel. Engine-owned: an HTML UI document embedded
    // in the engine (engine://debug/overlay.rml) in a Scene of its own that is not a
    // SceneManager scene (never in the Hierarchy, Engine.getSceneLayers or MCP scene lists),
    // updated and drawn after every scene and offered input first. It needs nothing of the
    // project (no assets, fonts, CSS or Lua), so it works when the game's UI is broken.
    // Not enabled, it costs a branch per input event: no Scene exists.
    // Main thread only.
    class JASPERENGINE_API DebugOverlay {
    public:
        // the settings a game starts with: the console closed, the stats as set
        static void configure(const DebugOverlaySettings& settings);
        // Settings changed while the game runs (the editor's Project Settings during Play): what
        // changed applies, the rest of the state (console open, stats picked) stays
        static void updateSettings(const DebugOverlaySettings& settings);
        static const DebugOverlaySettings& getSettings();

        static void setEnabled(bool enabled);
        static bool isEnabled();

        // no-ops (warned once) while not enabled
        static void showConsole();
        static void hideConsole();
        static void toggleConsole();
        static bool isConsoleVisible();

        // Off, Fps or Full (another value is refused with a warning)
        static void setStatsMode(DebugOverlaySettings::Stats mode);
        static DebugOverlaySettings::Stats getStatsMode();
        // off -> fps -> full -> off
        static void cycleStatsMode();

        // A console command: `name args...` typed in the command field (quotes group words).
        // name is [a-z0-9_-]+ (lower case); adding a name again replaces it. ownerTag: the
        // subscription tag of what added it (a Lua script's "_<address>_"), removed with it
        // (removeCommandsByTag); "" = until removeCommand
        static bool addCommand(const std::string& name, const std::string& description,
                               std::function<void(const std::vector<std::string>&)> function, const std::string& ownerTag = "");
        static void removeCommand(const std::string& name);
        static bool hasCommand(const std::string& name);
        // name and description of every command, built-in ones first
        static std::vector<std::pair<std::string, std::string>> getCommands();
        // runs a command line as the command field does (its output is logged)
        static void executeCommand(const std::string& line);
        // commands whose owner tag contains `substring` (Engine::removeSubscriptionsByTag)
        static void removeCommandsByTag(const std::string& substring);
        // every command added with an owner tag (scripts): Play stop, the Lua state going
        static void removeOwnedCommands();

        // The insets the panels keep clear of, in canvas px (left, top, right, bottom): feature
        // safe-area gives Screen's safe area insets here (whichever of the two merges second
        // wires it: Engine's startup sets a function reading Screen::getSafeAreaInsets()
        // converted to canvas px); null = none
        using InsetsFunction = void (*)(float& left, float& top, float& right, float& bottom);
        static void setSafeAreaInsetsProvider(InsetsFunction provider);

        // ---- engine hooks
        // Engine::systemKeyDown/Up and systemTouch*, before anything else; never consume. The
        // keys do nothing while a text field of the game has the keyboard.
        // phase: 0 down, 1 move, 2 up, 3 cancel (screen pixels, as the platform gives them)
        static void handleKey(int key, int mods, bool down, bool repeat);
        static void handleTouch(int pointer, float x, float y, int phase);
        // every finger the gestures track lets go (Engine::cancelAllUIPointers: all touches
        // cancelled, pause, focus loss)
        static void cancelGestures();
        // Engine::runFrame, after the scenes updated (also while the game is paused by the time
        // scale or held by the editor), with the unscaled frame time; cheap when not active
        static bool needsUpdate();
        static void update(double unscaledDelta);
        // Engine::runFrame, after every scene drew (inside the composite target)
        static void draw();
        // something of it is drawn this frame (the Vulkan composite counts it as a layer then)
        static bool isDrawn();
        // the overlay Scene (null when not shown or not enabled)
        static Scene* getScene();
        // the document entity of the overlay Scene (NULL_ENTITY without one)
        static Entity getDocumentEntity();
        // the console's search or command field has the keyboard (keys go to the overlay first)
        static bool hasTextFocus();
        // the view went away (Engine::systemViewDestroyed): the Scene goes, the state stays
        static void viewDestroyed();
        // Engine::systemShutdown and the editor's Play stop: everything goes (commands of
        // scripts too), the log buffer stays
        static void shutdown();
    };

}

#endif // DEBUGOVERLAY_H
