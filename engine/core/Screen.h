// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCREEN_H
#define SCREEN_H

#include "Export.h"
#include "System.h"
#include "math/EdgeInsets.h"
#include "math/Rect.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    enum class Scaling;

    // What the safe area is computed from (Screen::computeSafeArea): the screen in pixels, the
    // view the engine draws into on it, the canvas it maps to, and the insets
    struct SafeAreaInput {
        int screenWidth = 0;          // view pixels
        int screenHeight = 0;
        Rect viewRect;                // Engine::getViewRect(): where the canvas lies on the screen
        float canvasWidth = 0.0f;     // the canvas units the result is in
        float canvasHeight = 0.0f;
        EdgeInsets insetsPixels;      // the platform's, top-left origin
        std::vector<Rect> cutoutsPixels;
        EdgeInsets extraInsets;       // the app's (Screen.setExtraInsets)
        bool extraInPixels = false;   // extraInsets in view pixels instead of canvas units
    };

    struct SafeAreaResult {
        Rect safeArea;                // canvas, extras included
        Rect deviceSafeArea;          // canvas, without extras
        EdgeInsets safeAreaInsets;    // canvas
        EdgeInsets deviceSafeAreaInsets;
        EdgeInsets extraInsets;       // canvas
        Rect safeAreaPixels;          // view pixels, extras included
        std::vector<Rect> cutouts;    // canvas, clipped to it
        bool deviceRectEmpty = false; // the platform insets left nothing: the whole screen is used
        bool extrasTooLarge = false;  // the extras left less than 1x1: a 1x1 rect is kept
    };

    // Unity's Screen: the display the game runs on. Orientation (Screen.orientation,
    // Screen.autorotateTo*), screen sleep (Screen.sleepTimeout), dpi, refresh rate and the
    // safe area (Screen.safeArea, the notch and rounded corners). The pixel size stays
    // System::getScreenWidth/Height and the canvas Engine::getCanvasWidth/Height.
    // Main thread. State is global and kept across scene loads; initialize() sets the project's
    // values (the generated main of an export, editor Play start).
    class JASPERENGINE_API Screen {
    private:
        static ScreenOrientation requestedOrientation;
        static uint8_t allowedMask;
        static int sleepTimeout;

        // safe area (safe-area spec 6.1): what the platform and the app gave
        static EdgeInsets platformInsets;
        static std::vector<Rect> platformCutouts;
        static EdgeInsets extraInsets;
        static bool extraInPixels;
        // the editor's edit mode: the canvas and screen the Scene and Game views lay UI out for
        static int editorCanvasWidth;
        static int editorCanvasHeight;
        static int editorScreenWidth;
        static int editorScreenHeight;
        static int editorScaling;
        // what was computed last, and whether that was the baseline (no event for it)
        static SafeAreaResult safeArea;
        static bool safeAreaComputed;
        static uint64_t safeAreaRevision;
        static bool safeAreaEventPending;
        static bool safeAreaHeld;
        static bool warnedEmptySafeArea;
        static bool warnedExtraInsets;

        static SafeAreaInput currentSafeAreaInput();
        static const SafeAreaResult& currentSafeArea();

    public:
        // The Player settings: Default Orientation, Allowed Orientations for Auto Rotation (a
        // mask of PlatformRules::orientationBit; a fixed default is added to it) and Keep
        // Screen On (the initial sleepTimeout on Android and iOS: NeverSleep or SystemSetting;
        // elsewhere it starts at SystemSetting, as Unity's)
        static void initialize(ScreenOrientation defaultOrientation, uint8_t allowedMask, bool keepScreenOn);
        // whether Keep Screen On applies here (Android, iOS)
        static bool isMobileSleepPolicy();

        // A fixed orientation, or AutoRotation within the autorotateTo* set. Values outside
        // ScreenOrientation are ignored with a warning.
        static void setOrientation(ScreenOrientation orientation);
        // The orientation the interface is shown in now, never AutoRotation
        static ScreenOrientation getOrientation();
        // What setOrientation asked for (AutoRotation included)
        static ScreenOrientation getRequestedOrientation();

        // Screen.autorotateToPortrait / PortraitUpsideDown / LandscapeLeft / LandscapeRight:
        // the orientations AutoRotation may turn to. Turning the last one off is refused (a
        // warning). Applied right away while the request is AutoRotation; a device that shows
        // an orientation turned off rotates to an allowed one.
        static void setAutorotate(ScreenOrientation orientation, bool allowed);
        static bool getAutorotate(ScreenOrientation orientation);
        static uint8_t getAllowedMask();

        // Unity's Screen.sleepTimeout: SleepTimeout::NeverSleep (-1) keeps the screen on,
        // SleepTimeout::SystemSetting (-2) lets it dim as the device is set to. A number of
        // seconds counts as SystemSetting (Unity's mobile platforms do the same).
        static void setSleepTimeout(int value);
        static int getSleepTimeout();

        // dots per inch, 0 when unknown
        static float getDpi();
        // Hz
        static float getRefreshRate();

        // Sends the orientation policy and screen sleep to the platform again (the view was
        // created; the editor's Play started)
        static void applyToPlatform();

        // ---- safe area: the part of the screen no notch, punch hole, rounded corner or home
        // indicator covers, minus what the app reserves (a banner ad). Canvas coordinates, top-left
        // origin (the space of the UI and Input), clipped to the canvas.

        // Platforms (engine thread): insets and cutout rects in view pixels, top-left origin,
        // relative to the view the engine draws into. Recomputes; the change event follows. While a
        // quarter turn waits for its view size (Engine::isOrientationPending) they are only stored:
        // they are the new orientation's, and the size change uses them (one event, the final value).
        static void setPlatformSafeArea(const EdgeInsets& insetsPixels, const std::vector<Rect>& cutoutsPixels = {});
        static EdgeInsets getPlatformInsetsPixels();

        // Space the app keeps free per edge (a banner's height), on top of the device insets and
        // included by every consumer (safeArea, the Safe Area component, HTML env()). Canvas units,
        // or view pixels (kept in pixels: converted again on every canvas change). Negatives are 0.
        // Kept across scene loads; editor Play stop clears them.
        static void setExtraInsets(const EdgeInsets& insets, bool pixels = false);
        static void clearExtraInsets();
        // canvas units
        static EdgeInsets getExtraInsets();

        static Rect getSafeArea();                    // canvas, extras included
        static Rect getDeviceSafeArea();              // canvas, without extras
        static EdgeInsets getSafeAreaInsets();        // canvas, extras included
        static EdgeInsets getDeviceSafeAreaInsets();  // canvas
        static Rect getSafeAreaPixels();              // view pixels, extras included
        static std::vector<Rect> getCutouts();        // canvas; empty when none or unknown
        // the canvas size the values above are in (the Engine's canvas; the editor's reference
        // canvas in edit mode)
        static float getSafeAreaCanvasWidth();
        static float getSafeAreaCanvasHeight();
        // +1 on every change of the values above
        static uint64_t getSafeAreaRevision();

        // Recomputes; on a change the revision goes up and Engine::onSafeAreaChanged fires once,
        // at the end of Engine::systemViewChanged or at the start of the next frame
        static void updateSafeArea();
        // Engine: fires onSafeAreaChanged if a change is waiting
        static void dispatchSafeAreaEvent();
        // Engine: a quarter turn stopped waiting for its size (timeout or undone): the insets
        // setPlatformSafeArea held meanwhile are applied
        static void releaseHeldSafeArea();
        // Editor edit mode: UI is laid out for the project canvas (UISystem::setAnchorReferenceSize),
        // not the Engine's view (the Scene view). The safe area is computed as a Play at that
        // screen size would, then scaled to the canvas. clear: back to the Engine's view (Play).
        static void setEditorReference(int canvasWidth, int canvasHeight, int screenWidth, int screenHeight, Scaling scaling);
        static void clearEditorReference();
        static bool hasEditorReference();
        // editor Play start and stop: no extras, no editor reference, the next value a baseline
        static void resetSafeAreaState();

        // The computation itself (pure, for tests): see the safe-area spec 6.1
        static SafeAreaResult computeSafeArea(const SafeAreaInput& input);
        // JASPERENGINE_SIMULATE_SAFE_AREA's "top,right,bottom,left" (integer view pixels, >= 0);
        // false when malformed
        static bool parseSimulatedSafeArea(const char* text, EdgeInsets& insets);
    };

}

#endif // SCREEN_H
