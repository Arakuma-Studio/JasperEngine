// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "System.h"
#include "math/EdgeInsets.h"
#include "math/Rect.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    enum class CutoutShape { None, Notch, Island, PunchHole };

    // A phone or tablet the Game view simulates (the Device menu, MCP configure_game_view
    // device): its screen, the model and dpi System and Screen report in Play, and the
    // safe-area data the device overlay draws. Editor only, never in builds.
    struct DeviceProfile {
        const char* id;
        const char* name;
        const char* platform;        // "ios" | "android"
        int width, height;           // portrait pixels
        float dpi;
        float scale;                 // pixels per point (iOS) / per dp (Android)
        float cornerRadius;          // pixels
        CutoutShape cutout;
        Rect cutoutRect;             // portrait pixels (PunchHole: the circle's bounding box)
        // pixels, each in that orientation's frame
        EdgeInsets portrait, landscapeLeft, landscapeRight, portraitUpsideDown;
        bool homeIndicator;          // the iOS bar at the bottom
        bool supportsUpsideDown;     // iPhones with a notch do not turn upside down
        const char* model;           // System.getDeviceModel() in Play
    };

    const std::vector<DeviceProfile>& deviceProfiles();
    // nullptr when there is no profile of that id
    const DeviceProfile* findDeviceProfile(const std::string& id);
    // the orientations the device can show (PlatformRules::orientationBit mask)
    uint8_t profileOrientationMask(const DeviceProfile& profile);
    // the screen in an orientation: width and height swapped in landscape
    void profileSize(const DeviceProfile& profile, ScreenOrientation orientation, int& width, int& height);
    // the insets in an orientation's frame, pixels
    EdgeInsets profileInsets(const DeviceProfile& profile, ScreenOrientation orientation);
    // the next orientation of the Rotate button: portrait -> landscape left -> landscape right
    // (-> upside down where the device has it) -> portrait, skipping what `allowed` leaves out;
    // `current` when nothing else is allowed
    ScreenOrientation nextProfileOrientation(const DeviceProfile& profile, ScreenOrientation current, uint8_t allowed);

}
