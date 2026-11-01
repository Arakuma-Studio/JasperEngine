// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceProfiles.h"

#include "util/PlatformRules.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // iPhones: symmetric landscape insets (both landscapes), no upside-down portrait
    DeviceProfile iphone(const char* id, const char* name, int width, int height, float dpi, float corner,
                         CutoutShape cutout, Rect cutoutRect, float top, float sideInset, const char* model){
        const EdgeInsets portrait(0, top, 0, 102);
        const EdgeInsets landscape(sideInset, 0, sideInset, 63);
        return DeviceProfile{id, name, "ios", width, height, dpi, 3.0f, corner, cutout, cutoutRect,
                             portrait, landscape, landscape, portrait, true, false, model};
    }

    // Android phones: the cutout is at the top of portrait, so on the left in Unity's
    // LandscapeLeft (turned counter-clockwise) and at the bottom upside down
    DeviceProfile android(const char* id, const char* name, int width, int height, float dpi, float scale, float corner,
                          CutoutShape cutout, Rect cutoutRect, float top, const char* model){
        return DeviceProfile{id, name, "android", width, height, dpi, scale, corner, cutout, cutoutRect,
                             EdgeInsets(0, top, 0, 0), EdgeInsets(top, 0, 0, 0), EdgeInsets(0, 0, top, 0),
                             EdgeInsets(0, 0, 0, top), false, true, model};
    }

}

const std::vector<DeviceProfile>& editor::deviceProfiles(){
    // Apple HIG layout and device specifications, Android device cutout reports (safe-area spec 6.6)
    static const std::vector<DeviceProfile> profiles = {
        iphone("iphone_15_pro", "iPhone 15 Pro", 1179, 2556, 460.0f, 165.0f, CutoutShape::Island,
               Rect(400, 33, 378, 111), 177, 177, "iPhone16,1"),
        iphone("iphone_15_pro_max", "iPhone 15 Pro Max", 1290, 2796, 460.0f, 165.0f, CutoutShape::Island,
               Rect(456, 33, 378, 111), 177, 177, "iPhone16,2"),
        iphone("iphone_13", "iPhone 13 / 14", 1170, 2532, 460.0f, 142.0f, CutoutShape::Notch,
               Rect(342, 0, 486, 99), 141, 141, "iPhone14,5"),
        iphone("iphone_x", "iPhone X / XS / 11 Pro", 1125, 2436, 458.0f, 120.0f, CutoutShape::Notch,
               Rect(249, 0, 627, 90), 132, 132, "iPhone10,3"),
        DeviceProfile{"iphone_se", "iPhone SE (3rd gen)", "ios", 750, 1334, 326.0f, 2.0f, 0.0f, CutoutShape::None,
                      Rect(0, 0, 0, 0), EdgeInsets(), EdgeInsets(), EdgeInsets(), EdgeInsets(), false, false, "iPhone14,6"},
        DeviceProfile{"ipad_pro_11", "iPad Pro 11\"", "ios", 1668, 2388, 264.0f, 2.0f, 36.0f, CutoutShape::None,
                      Rect(0, 0, 0, 0), EdgeInsets(0, 0, 0, 40), EdgeInsets(0, 0, 0, 40), EdgeInsets(0, 0, 0, 40),
                      EdgeInsets(0, 0, 0, 40), true, true, "iPad8,1"},
        android("pixel_8", "Google Pixel 8", 1080, 2400, 428.0f, 2.625f, 100.0f, CutoutShape::PunchHole,
                Rect(498, 44, 84, 84), 128, "Google Pixel 8"),
        android("pixel_3_xl", "Google Pixel 3 XL", 1440, 2960, 523.0f, 3.5f, 90.0f, CutoutShape::Notch,
                Rect(440, 0, 560, 171), 171, "Google Pixel 3 XL"),
        android("galaxy_s23", "Samsung Galaxy S23", 1080, 2340, 425.0f, 2.8125f, 90.0f, CutoutShape::PunchHole,
                Rect(500, 34, 80, 80), 110, "samsung SM-S911B"),
    };
    return profiles;
}

const DeviceProfile* editor::findDeviceProfile(const std::string& id){
    for (const DeviceProfile& profile : deviceProfiles()){
        if (id == profile.id) return &profile;
    }
    return nullptr;
}

uint8_t editor::profileOrientationMask(const DeviceProfile& profile){
    uint8_t mask = PlatformRules::orientationBit(ScreenOrientation::Portrait) |
                   PlatformRules::orientationBit(ScreenOrientation::LandscapeLeft) |
                   PlatformRules::orientationBit(ScreenOrientation::LandscapeRight);
    if (profile.supportsUpsideDown) mask |= PlatformRules::orientationBit(ScreenOrientation::PortraitUpsideDown);
    return mask;
}

void editor::profileSize(const DeviceProfile& profile, ScreenOrientation orientation, int& width, int& height){
    const bool landscape = orientation == ScreenOrientation::LandscapeLeft || orientation == ScreenOrientation::LandscapeRight;
    width = landscape ? profile.height : profile.width;
    height = landscape ? profile.width : profile.height;
}

EdgeInsets editor::profileInsets(const DeviceProfile& profile, ScreenOrientation orientation){
    switch (orientation){
        case ScreenOrientation::LandscapeLeft: return profile.landscapeLeft;
        case ScreenOrientation::LandscapeRight: return profile.landscapeRight;
        case ScreenOrientation::PortraitUpsideDown: return profile.portraitUpsideDown;
        default: return profile.portrait;
    }
}

ScreenOrientation editor::nextProfileOrientation(const DeviceProfile& profile, ScreenOrientation current, uint8_t allowed){
    const ScreenOrientation cycle[] = {ScreenOrientation::Portrait, ScreenOrientation::LandscapeLeft,
                                       ScreenOrientation::LandscapeRight, ScreenOrientation::PortraitUpsideDown};
    const uint8_t mask = profileOrientationMask(profile) & allowed;
    int index = 0;
    for (int i = 0; i < 4; i++){
        if (cycle[i] == current) index = i;
    }
    for (int step = 1; step <= 4; step++){
        const ScreenOrientation candidate = cycle[(index + step) % 4];
        if (mask & PlatformRules::orientationBit(candidate)) return candidate;
    }
    return current;
}
