// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCREEN_H
#define SCREEN_H

#include "Export.h"
#include "System.h"

#include <cstdint>

namespace jasperengine {

    // Unity's Screen: the display the game runs on. Orientation (Screen.orientation,
    // Screen.autorotateTo*), screen sleep (Screen.sleepTimeout), dpi and refresh rate. The
    // pixel size stays System::getScreenWidth/Height and the canvas Engine::getCanvasWidth/Height.
    // Main thread. State is global and kept across scene loads; initialize() sets the project's
    // values (the generated main of an export, editor Play start).
    class JASPERENGINE_API Screen {
    private:
        static ScreenOrientation requestedOrientation;
        static uint8_t allowedMask;
        static int sleepTimeout;

    public:
        // The Player settings: Default Orientation, Allowed Orientations for Auto Rotation (a
        // mask of PlatformRules::orientationBit; a fixed default is added to it) and Keep
        // Screen On (the initial sleepTimeout: NeverSleep or SystemSetting)
        static void initialize(ScreenOrientation defaultOrientation, uint8_t allowedMask, bool keepScreenOn);

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
    };

}

#endif // SCREEN_H
