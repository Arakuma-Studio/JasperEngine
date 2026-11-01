// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceProfiles.h"

#include "imgui.h"
#include "util/PlatformRules.h"

#include <algorithm>
#include <cmath>

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

namespace {

    DeviceProfile makeCustomProfile(const CustomDeviceSettings& device){
        const int width = std::clamp(device.width, 16, 16384);
        const int height = std::clamp(device.height, 16, 16384);
        auto clean = [](float value){ return (std::isfinite(value) && value > 0.0f) ? std::round(value) : 0.0f; };
        const EdgeInsets portrait(clean(device.left), clean(device.top), clean(device.right), clean(device.bottom));
        const float corner = std::min(clean(device.cornerRadius), (float)std::min(width, height) * 0.5f);
        // 0: Screen.dpi is the computer's (Platform::getScreenDpi)
        const float dpi = (std::isfinite(device.dpi) && device.dpi > 0.0f) ? std::min(device.dpi, 2000.0f) : 0.0f;
        // a custom screen turns every way: its insets go round with it
        return DeviceProfile{"custom", "Custom", "android", width, height, dpi, 1.0f, corner, CutoutShape::None,
                             Rect(0, 0, 0, 0), portrait, editor::rotateInsets(portrait, ScreenOrientation::LandscapeLeft),
                             editor::rotateInsets(portrait, ScreenOrientation::LandscapeRight),
                             editor::rotateInsets(portrait, ScreenOrientation::PortraitUpsideDown), false, true, "Custom"};
    }

    DeviceProfile& customProfileStorage(){
        static DeviceProfile profile = makeCustomProfile(CustomDeviceSettings());
        return profile;
    }

}

const DeviceProfile* editor::findDeviceProfile(const std::string& id){
    if (id == "custom") return &customProfileStorage();
    for (const DeviceProfile& profile : deviceProfiles()){
        if (id == profile.id) return &profile;
    }
    return nullptr;
}

const DeviceProfile& editor::customDeviceProfile(){
    return customProfileStorage();
}

void editor::setCustomDevice(const CustomDeviceSettings& device){
    customProfileStorage() = makeCustomProfile(device);
}

EdgeInsets editor::rotateInsets(const EdgeInsets& p, ScreenOrientation orientation){
    switch (orientation){
        // turned counter-clockwise: the portrait top is on the left, its right edge on top
        case ScreenOrientation::LandscapeLeft: return EdgeInsets(p.top, p.right, p.bottom, p.left);
        // clockwise: the portrait top is on the right, its left edge on top
        case ScreenOrientation::LandscapeRight: return EdgeInsets(p.bottom, p.left, p.top, p.right);
        case ScreenOrientation::PortraitUpsideDown: return EdgeInsets(p.right, p.bottom, p.left, p.top);
        default: return p;
    }
}

Rect editor::profileCutout(const DeviceProfile& profile, ScreenOrientation orientation){
    const Rect& c = profile.cutoutRect;
    if (profile.cutout == CutoutShape::None || c.getWidth() <= 0.0f || c.getHeight() <= 0.0f) return Rect(0, 0, 0, 0);
    const float w = (float)profile.width;
    const float h = (float)profile.height;
    switch (orientation){
        case ScreenOrientation::LandscapeLeft:
            return Rect(c.getY(), w - c.getX() - c.getWidth(), c.getHeight(), c.getWidth());
        case ScreenOrientation::LandscapeRight:
            return Rect(h - c.getY() - c.getHeight(), c.getX(), c.getHeight(), c.getWidth());
        case ScreenOrientation::PortraitUpsideDown:
            return Rect(w - c.getX() - c.getWidth(), h - c.getY() - c.getHeight(), c.getWidth(), c.getHeight());
        default:
            return c;
    }
}

Rect editor::profileSafeRect(const DeviceProfile& profile, ScreenOrientation orientation){
    int width = 0, height = 0;
    profileSize(profile, orientation, width, height);
    const EdgeInsets insets = profileInsets(profile, orientation);
    return Rect(insets.left, insets.top, std::max(0.0f, (float)width - insets.left - insets.right),
                std::max(0.0f, (float)height - insets.top - insets.bottom));
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

// ------------------------------------------------------------------ device overlay

namespace {

    constexpr float kPi = 3.14159265358979f;

    // The overlay's shapes in image pixels (both the rasterizer and the ImGui drawing use them)
    struct OverlayShapes {
        float width = 0.0f, height = 0.0f;
        float corner = 0.0f;
        CutoutShape cutout = CutoutShape::None;
        Rect cutoutRect;
        float cutoutRounding = 0.0f;
        // the side of a notch away from the screen edge it hangs from (its rounded corners)
        enum class Side { Top, Right, Bottom, Left } innerSide = Side::Bottom;
        bool homeIndicator = false;
        Rect homeBar;
        Rect safe;
    };

    OverlayShapes overlayShapes(float w, float h, const DeviceProfile& profile, ScreenOrientation orientation, const Rect* safeRect){
        OverlayShapes shapes;
        shapes.width = w;
        shapes.height = h;
        int deviceWidth = 0, deviceHeight = 0;
        editor::profileSize(profile, orientation, deviceWidth, deviceHeight);
        // image pixels per device pixel (the image has the device's aspect)
        const float s = deviceWidth > 0 ? w / (float)deviceWidth : 1.0f;
        shapes.corner = std::min(profile.cornerRadius * s, std::min(w, h) * 0.5f);
        const Rect c = editor::profileCutout(profile, orientation);
        if (profile.cutout != CutoutShape::None && c.getWidth() > 0.0f){
            shapes.cutout = profile.cutout;
            shapes.cutoutRect = Rect(c.getX() * s, c.getY() * s, c.getWidth() * s, c.getHeight() * s);
            const float thin = std::min(shapes.cutoutRect.getWidth(), shapes.cutoutRect.getHeight());
            shapes.cutoutRounding = profile.cutout == CutoutShape::Island ? thin * 0.5f : thin * 0.4f;
            switch (orientation){
                case ScreenOrientation::LandscapeLeft: shapes.innerSide = OverlayShapes::Side::Right; break;
                case ScreenOrientation::LandscapeRight: shapes.innerSide = OverlayShapes::Side::Left; break;
                case ScreenOrientation::PortraitUpsideDown: shapes.innerSide = OverlayShapes::Side::Top; break;
                default: shapes.innerSide = OverlayShapes::Side::Bottom; break;
            }
        }
        if (profile.homeIndicator){
            // iOS: 134 x 5 points, 8 points above the bottom of the screen as it is turned
            const float pt = profile.scale * s;
            const float barWidth = 134.0f * pt;
            const float barHeight = std::max(1.0f, 5.0f * pt);
            shapes.homeIndicator = true;
            shapes.homeBar = Rect((w - barWidth) * 0.5f, h - 8.0f * pt - barHeight, barWidth, barHeight);
        }
        if (safeRect){
            shapes.safe = *safeRect;
        }else{
            const Rect safe = editor::profileSafeRect(profile, orientation);
            shapes.safe = Rect(safe.getX() * s, safe.getY() * s, safe.getWidth() * s, safe.getHeight() * s);
        }
        return shapes;
    }

    // inside a rectangle with rounded corners of radius r
    bool insideRounded(float x, float y, float left, float top, float right, float bottom, float r){
        if (x < left || x > right || y < top || y > bottom) return false;
        if (r <= 0.0f) return true;
        const float cx = std::clamp(x, left + r, right - r);
        const float cy = std::clamp(y, top + r, bottom - r);
        const float dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= r * r;
    }

    bool insideCutout(const OverlayShapes& shapes, float x, float y){
        const Rect& c = shapes.cutoutRect;
        const float left = c.getX(), top = c.getY(), right = c.getX() + c.getWidth(), bottom = c.getY() + c.getHeight();
        if (x < left || x > right || y < top || y > bottom) return false;
        switch (shapes.cutout){
            case CutoutShape::PunchHole: {
                const float rx = c.getWidth() * 0.5f, ry = c.getHeight() * 0.5f;
                const float dx = (x - left - rx) / rx, dy = (y - top - ry) / ry;
                return dx * dx + dy * dy <= 1.0f;
            }
            case CutoutShape::Island:
                return insideRounded(x, y, left, top, right, bottom, shapes.cutoutRounding);
            case CutoutShape::Notch: {
                // square where it meets the screen edge, rounded on the inner side
                const float r = shapes.cutoutRounding;
                float l = left, t = top, rr = right, b = bottom;
                switch (shapes.innerSide){
                    case OverlayShapes::Side::Bottom: t -= r; break;
                    case OverlayShapes::Side::Top: b += r; break;
                    case OverlayShapes::Side::Right: l -= r; break;
                    case OverlayShapes::Side::Left: rr += r; break;
                }
                return insideRounded(x, y, l, t, rr, b, r);
            }
            default:
                return false;
        }
    }

    // a dash of 4 pixels every 8 along the outline
    bool onDashedOutline(const Rect& rect, int px, int py){
        const int left = (int)std::round(rect.getX());
        const int top = (int)std::round(rect.getY());
        const int right = (int)std::round(rect.getX() + rect.getWidth()) - 1;
        const int bottom = (int)std::round(rect.getY() + rect.getHeight()) - 1;
        if (right < left || bottom < top) return false;
        const bool vertical = (px == left || px == right) && py >= top && py <= bottom;
        const bool horizontal = (py == top || py == bottom) && px >= left && px <= right;
        if (horizontal && ((px - left) % 8) < 4) return true;
        if (vertical && ((py - top) % 8) < 4) return true;
        return false;
    }

}

void editor::rasterizeDeviceOverlay(uint8_t* rgba, int w, int h, const DeviceProfile& profile, ScreenOrientation orientation,
                                    bool safeAreaOutline, const Rect* safeRect, bool screen){
    if (!rgba || w <= 0 || h <= 0) return;
    const OverlayShapes shapes = overlayShapes((float)w, (float)h, profile, orientation, safeRect);
    const float barRounding = shapes.homeBar.getHeight() * 0.5f;
    for (int py = 0; py < h; py++){
        const float y = (float)py + 0.5f;
        for (int px = 0; px < w; px++){
            const float x = (float)px + 0.5f;
            uint8_t* p = rgba + ((size_t)py * (size_t)w + (size_t)px) * 4;
            if (screen && (!insideRounded(x, y, 0.0f, 0.0f, (float)w, (float)h, shapes.corner) ||
                           (shapes.cutout != CutoutShape::None && insideCutout(shapes, x, y)))){
                p[0] = p[1] = p[2] = 0;
                p[3] = 255;
                continue;
            }
            if (screen && shapes.homeIndicator &&
                insideRounded(x, y, shapes.homeBar.getX(), shapes.homeBar.getY(), shapes.homeBar.getX() + shapes.homeBar.getWidth(),
                              shapes.homeBar.getY() + shapes.homeBar.getHeight(), barRounding)){
                // light gray at 60%
                for (int i = 0; i < 3; i++) p[i] = (uint8_t)std::lround(p[i] * 0.4f + 230.0f * 0.6f);
            }
            if (safeAreaOutline && onDashedOutline(shapes.safe, px, py)){
                p[0] = 0;
                p[1] = 255;
                p[2] = 0;
                p[3] = 255;
            }
        }
    }
}

void editor::drawDeviceOverlay(ImDrawList* drawList, ImVec2 min, ImVec2 max, const DeviceProfile& profile, ScreenOrientation orientation,
                               bool safeAreaOutline, const Rect* safeRect, bool screen){
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    if (!drawList || w <= 0.0f || h <= 0.0f) return;
    const OverlayShapes shapes = overlayShapes(w, h, profile, orientation, safeRect);
    const ImU32 black = IM_COL32(0, 0, 0, 255);
    auto at = [&](float x, float y){ return ImVec2(min.x + x, min.y + y); };

    // the rounded corners: what is between each corner and its arc, as a fan from the corner
    if (screen && shapes.corner > 0.5f){
        const float r = shapes.corner;
        const int segments = std::clamp((int)(r * 0.5f), 6, 32);
        const struct { float cx, cy, x, y, a0; } corners[] = {
            {r, r, 0.0f, 0.0f, kPi},
            {w - r, r, w, 0.0f, kPi * 1.5f},
            {w - r, h - r, w, h, 0.0f},
            {r, h - r, 0.0f, h, kPi * 0.5f}};
        for (const auto& corner : corners){
            ImVec2 previous = at(corner.cx + std::cos(corner.a0) * r, corner.cy + std::sin(corner.a0) * r);
            for (int i = 1; i <= segments; i++){
                const float a = corner.a0 + (kPi * 0.5f) * (float)i / (float)segments;
                const ImVec2 next = at(corner.cx + std::cos(a) * r, corner.cy + std::sin(a) * r);
                drawList->AddTriangleFilled(at(corner.x, corner.y), previous, next, black);
                previous = next;
            }
        }
    }

    if (screen && shapes.cutout != CutoutShape::None){
        const Rect& c = shapes.cutoutRect;
        const ImVec2 cMin = at(c.getX(), c.getY());
        const ImVec2 cMax = at(c.getX() + c.getWidth(), c.getY() + c.getHeight());
        if (shapes.cutout == CutoutShape::PunchHole){
            drawList->AddCircleFilled(ImVec2((cMin.x + cMax.x) * 0.5f, (cMin.y + cMax.y) * 0.5f),
                                      std::min(cMax.x - cMin.x, cMax.y - cMin.y) * 0.5f, black, 32);
        }else if (shapes.cutout == CutoutShape::Island){
            drawList->AddRectFilled(cMin, cMax, black, shapes.cutoutRounding);
        }else{
            ImDrawFlags flags = ImDrawFlags_RoundCornersBottom;
            switch (shapes.innerSide){
                case OverlayShapes::Side::Top: flags = ImDrawFlags_RoundCornersTop; break;
                case OverlayShapes::Side::Right: flags = ImDrawFlags_RoundCornersRight; break;
                case OverlayShapes::Side::Left: flags = ImDrawFlags_RoundCornersLeft; break;
                default: break;
            }
            drawList->AddRectFilled(cMin, cMax, black, shapes.cutoutRounding, flags);
        }
    }

    if (screen && shapes.homeIndicator){
        const Rect& bar = shapes.homeBar;
        drawList->AddRectFilled(at(bar.getX(), bar.getY()), at(bar.getX() + bar.getWidth(), bar.getY() + bar.getHeight()),
                                IM_COL32(230, 230, 230, 153), bar.getHeight() * 0.5f);
    }

    if (safeAreaOutline){
        const Rect& safe = shapes.safe;
        const ImU32 green = IM_COL32(0, 255, 0, 255);
        const float left = std::floor(safe.getX()) + 0.5f, top = std::floor(safe.getY()) + 0.5f;
        const float right = std::floor(safe.getX() + safe.getWidth()) - 0.5f, bottom = std::floor(safe.getY() + safe.getHeight()) - 0.5f;
        auto dashed = [&](float x0, float y0, float x1, float y1){
            const float length = std::hypot(x1 - x0, y1 - y0);
            if (length <= 0.0f) return;
            for (float d = 0.0f; d < length; d += 8.0f){
                const float e = std::min(d + 4.0f, length);
                drawList->AddLine(at(x0 + (x1 - x0) * d / length, y0 + (y1 - y0) * d / length),
                                  at(x0 + (x1 - x0) * e / length, y0 + (y1 - y0) * e / length), green, 1.0f);
            }
        };
        dashed(left, top, right, top);
        dashed(right, top, right, bottom);
        dashed(right, bottom, left, bottom);
        dashed(left, bottom, left, top);
    }
}
