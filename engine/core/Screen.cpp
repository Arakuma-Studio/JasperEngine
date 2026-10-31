// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Screen.h"

#include "Engine.h"
#include "Log.h"
#include "util/PlatformRules.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

ScreenOrientation Screen::requestedOrientation = ScreenOrientation::AutoRotation;
uint8_t Screen::allowedMask = PlatformRules::DEFAULT_ALLOWED_ORIENTATIONS;
int Screen::sleepTimeout = Screen::isMobileSleepPolicy() ? SleepTimeout::NeverSleep : SleepTimeout::SystemSetting;

bool Screen::isMobileSleepPolicy(){
    const Platform platform = Engine::getPlatform();
    return platform == Platform::Android || platform == Platform::iOS;
}

void Screen::initialize(ScreenOrientation defaultOrientation, uint8_t mask, bool keepScreenOn){
    if (!PlatformRules::isOrientation((int)defaultOrientation)){
        defaultOrientation = ScreenOrientation::AutoRotation;
    }
    mask &= PlatformRules::ALL_ORIENTATIONS;
    if (PlatformRules::isFixedOrientation(defaultOrientation)){
        mask |= PlatformRules::orientationBit(defaultOrientation);
    }
    if (mask == 0){
        mask = PlatformRules::DEFAULT_ALLOWED_ORIENTATIONS;
    }
    requestedOrientation = defaultOrientation;
    allowedMask = mask;
    // Unity applies the screen sleep setting on phones and tablets only: a desktop or web game
    // keeps the display awake only when it sets Screen.sleepTimeout = NeverSleep itself
    sleepTimeout = (keepScreenOn && isMobileSleepPolicy()) ? SleepTimeout::NeverSleep : SleepTimeout::SystemSetting;
    applyToPlatform();
}

void Screen::applyToPlatform(){
    if (!System::hasInstance()) return;
    System::instance().applyOrientationPolicy(requestedOrientation, allowedMask);
    System::instance().setKeepScreenOn(sleepTimeout == SleepTimeout::NeverSleep);
}

void Screen::setOrientation(ScreenOrientation orientation){
    if (!PlatformRules::isOrientation((int)orientation)){
        Log::warn("Screen.orientation: %d is not a ScreenOrientation (Portrait 1 .. AutoRotation 5)", (int)orientation);
        return;
    }
    requestedOrientation = orientation;
    if (System::hasInstance()){
        System::instance().applyOrientationPolicy(requestedOrientation, allowedMask);
    }
}

ScreenOrientation Screen::getOrientation(){
    if (!System::hasInstance()) return ScreenOrientation::Portrait;
    ScreenOrientation current = System::instance().getScreenOrientation();
    if (!PlatformRules::isFixedOrientation(current)) current = ScreenOrientation::Portrait;
    return current;
}

ScreenOrientation Screen::getRequestedOrientation(){
    return requestedOrientation;
}

void Screen::setAutorotate(ScreenOrientation orientation, bool allowed){
    if (!PlatformRules::isFixedOrientation(orientation)){
        Log::warn("Screen.autorotateTo: %d is not Portrait, PortraitUpsideDown, LandscapeLeft or LandscapeRight", (int)orientation);
        return;
    }
    const uint8_t bit = PlatformRules::orientationBit(orientation);
    const uint8_t mask = allowed ? (uint8_t)(allowedMask | bit) : (uint8_t)(allowedMask & ~bit);
    if ((mask & PlatformRules::ALL_ORIENTATIONS) == 0){
        Log::warn("Screen.autorotateTo%s = false: at least one orientation must stay allowed for auto-rotation",
                  orientation == ScreenOrientation::Portrait ? "Portrait"
                  : orientation == ScreenOrientation::PortraitUpsideDown ? "PortraitUpsideDown"
                  : orientation == ScreenOrientation::LandscapeLeft ? "LandscapeLeft" : "LandscapeRight");
        return;
    }
    if (mask == allowedMask) return;
    allowedMask = mask;
    // Unity: the autorotate set only matters (and is only applied) while auto-rotating
    if (requestedOrientation == ScreenOrientation::AutoRotation && System::hasInstance()){
        System::instance().applyOrientationPolicy(requestedOrientation, allowedMask);
    }
}

bool Screen::getAutorotate(ScreenOrientation orientation){
    if (!PlatformRules::isFixedOrientation(orientation)) return false;
    return (allowedMask & PlatformRules::orientationBit(orientation)) != 0;
}

uint8_t Screen::getAllowedMask(){
    return allowedMask;
}

void Screen::setSleepTimeout(int value){
    if (value != SleepTimeout::NeverSleep){
        value = SleepTimeout::SystemSetting;
    }
    sleepTimeout = value;
    if (System::hasInstance()){
        System::instance().setKeepScreenOn(sleepTimeout == SleepTimeout::NeverSleep);
    }
}

int Screen::getSleepTimeout(){
    return sleepTimeout;
}

float Screen::getDpi(){
    return System::hasInstance() ? System::instance().getScreenDpi() : 0.0f;
}

float Screen::getRefreshRate(){
    return System::hasInstance() ? System::instance().getRefreshRate() : 60.0f;
}

// ------------------------------------------------------------------ safe area

EdgeInsets Screen::platformInsets;
std::vector<Rect> Screen::platformCutouts;
EdgeInsets Screen::extraInsets;
bool Screen::extraInPixels = false;
int Screen::editorCanvasWidth = 0;
int Screen::editorCanvasHeight = 0;
int Screen::editorScreenWidth = 0;
int Screen::editorScreenHeight = 0;
int Screen::editorScaling = 0;
SafeAreaResult Screen::safeArea;
bool Screen::safeAreaComputed = false;
uint64_t Screen::safeAreaRevision = 0;
bool Screen::safeAreaEventPending = false;
bool Screen::warnedEmptySafeArea = false;
bool Screen::warnedExtraInsets = false;

namespace {

    EdgeInsets nonNegative(const EdgeInsets& insets){
        auto clean = [](float value){ return (std::isfinite(value) && value > 0.0f) ? value : 0.0f; };
        return EdgeInsets(clean(insets.left), clean(insets.top), clean(insets.right), clean(insets.bottom));
    }

    // values are compared after rounding to 1/1000 canvas unit (float noise is no change)
    float rounded(float value){
        return std::round(value * 1000.0f) / 1000.0f;
    }

    Rect roundedRect(const Rect& rect){
        return Rect(rounded(rect.getX()), rounded(rect.getY()), rounded(rect.getWidth()), rounded(rect.getHeight()));
    }

    EdgeInsets roundedInsets(const EdgeInsets& insets){
        return EdgeInsets(rounded(insets.left), rounded(insets.top), rounded(insets.right), rounded(insets.bottom));
    }

    bool sameRect(const Rect& a, const Rect& b){
        return a.getX() == b.getX() && a.getY() == b.getY() && a.getWidth() == b.getWidth() && a.getHeight() == b.getHeight();
    }

    bool sameResult(const SafeAreaResult& a, const SafeAreaResult& b){
        if (!sameRect(a.safeArea, b.safeArea) || !sameRect(a.deviceSafeArea, b.deviceSafeArea) ||
            !sameRect(a.safeAreaPixels, b.safeAreaPixels) || a.extraInsets != b.extraInsets ||
            a.cutouts.size() != b.cutouts.size()){
            return false;
        }
        for (size_t i = 0; i < a.cutouts.size(); i++){
            if (!sameRect(a.cutouts[i], b.cutouts[i])) return false;
        }
        return true;
    }

    // [left, top, right, bottom] edges of a rectangle
    struct Edges {
        float left, top, right, bottom;
        float width() const { return right - left; }
        float height() const { return bottom - top; }
        Rect rect() const { return Rect(left, top, right - left, bottom - top); }
    };

    Edges edgesOf(const Rect& rect){
        return Edges{rect.getX(), rect.getY(), rect.getX() + rect.getWidth(), rect.getY() + rect.getHeight()};
    }

    Edges intersect(const Edges& a, const Edges& b){
        return Edges{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
    }

}

SafeAreaResult Screen::computeSafeArea(const SafeAreaInput& input){
    SafeAreaResult result;
    const float cw = std::max(0.0f, input.canvasWidth);
    const float ch = std::max(0.0f, input.canvasHeight);
    const Edges canvas{0.0f, 0.0f, cw, ch};

    const float viewWidth = input.viewRect.getWidth();
    const float viewHeight = input.viewRect.getHeight();
    const bool hasView = input.screenWidth > 0 && input.screenHeight > 0 && viewWidth > 0.0f && viewHeight > 0.0f;
    // canvas units per view pixel: the inverse of Engine::transformCoordPos
    const float kx = hasView ? cw / viewWidth : 1.0f;
    const float ky = hasView ? ch / viewHeight : 1.0f;
    const float viewX = hasView ? input.viewRect.getX() : 0.0f;
    const float viewY = hasView ? input.viewRect.getY() : 0.0f;
    auto toCanvas = [&](const Edges& px){
        return Edges{(px.left - viewX) * kx, (px.top - viewY) * ky, (px.right - viewX) * kx, (px.bottom - viewY) * ky};
    };

    // the device's safe rectangle on the screen, in pixels, then on the canvas: letterbox bars
    // absorb the insets that fall inside them, a cropped part of the canvas is outside it
    Edges device = canvas;
    if (hasView){
        const float w = (float)input.screenWidth;
        const float h = (float)input.screenHeight;
        const EdgeInsets insets = nonNegative(input.insetsPixels);
        Edges pixels{std::min(insets.left, w), std::min(insets.top, h), std::max(0.0f, w - insets.right), std::max(0.0f, h - insets.bottom)};
        if (pixels.width() <= 0.0f || pixels.height() <= 0.0f){
            result.deviceRectEmpty = true;
            pixels = Edges{0.0f, 0.0f, w, h};
        }
        device = intersect(toCanvas(pixels), canvas);
        if (device.width() < 0.0f) device.right = device.left = std::clamp((device.left + device.right) * 0.5f, 0.0f, cw);
        if (device.height() < 0.0f) device.bottom = device.top = std::clamp((device.top + device.bottom) * 0.5f, 0.0f, ch);

        for (const Rect& cutout : input.cutoutsPixels){
            const Edges mapped = intersect(toCanvas(edgesOf(cutout)), canvas);
            if (mapped.width() > 0.0f && mapped.height() > 0.0f) result.cutouts.push_back(roundedRect(mapped.rect()));
        }
    }

    // the app's extras, in canvas units (pixels converted at the current scale)
    EdgeInsets extras = nonNegative(input.extraInsets);
    if (input.extraInPixels){
        extras = EdgeInsets(extras.left * kx, extras.top * ky, extras.right * kx, extras.bottom * ky);
    }
    Edges safe{device.left + extras.left, device.top + extras.top, device.right - extras.right, device.bottom - extras.bottom};
    // less than 1x1 left: 1x1, centred on what remains
    if (safe.width() < 1.0f){
        result.extrasTooLarge = true;
        const float centre = (safe.left + safe.right) * 0.5f;
        safe.left = centre - 0.5f;
        safe.right = centre + 0.5f;
    }
    if (safe.height() < 1.0f){
        result.extrasTooLarge = true;
        const float centre = (safe.top + safe.bottom) * 0.5f;
        safe.top = centre - 0.5f;
        safe.bottom = centre + 0.5f;
    }

    result.deviceSafeArea = roundedRect(device.rect());
    result.safeArea = roundedRect(safe.rect());
    result.extraInsets = roundedInsets(extras);
    result.deviceSafeAreaInsets = roundedInsets(EdgeInsets(device.left, device.top, cw - device.right, ch - device.bottom));
    result.safeAreaInsets = roundedInsets(EdgeInsets(safe.left, safe.top, cw - safe.right, ch - safe.bottom));
    // back to view pixels (Engine::transformCoordPos's direction)
    const float px = hasView ? 1.0f / kx : 1.0f;
    const float py = hasView ? 1.0f / ky : 1.0f;
    result.safeAreaPixels = roundedRect(Rect(safe.left * px + viewX, safe.top * py + viewY, safe.width() * px, safe.height() * py));
    return result;
}

bool Screen::parseSimulatedSafeArea(const char* text, EdgeInsets& insets){
    if (!text) return false;
    int values[4];
    int count = 0;
    const char* cursor = text;
    while (count < 4){
        while (*cursor == ' ') cursor++;
        if (*cursor < '0' || *cursor > '9') return false;
        long value = 0;
        while (*cursor >= '0' && *cursor <= '9'){
            value = value * 10 + (*cursor - '0');
            if (value > 100000) return false;
            cursor++;
        }
        values[count++] = (int)value;
        while (*cursor == ' ') cursor++;
        if (count < 4){
            if (*cursor != ',') return false;
            cursor++;
        }
    }
    if (*cursor != '\0') return false;
    // top, right, bottom, left (CSS order)
    insets = EdgeInsets((float)values[3], (float)values[0], (float)values[1], (float)values[2]);
    return true;
}

SafeAreaInput Screen::currentSafeAreaInput(){
    SafeAreaInput input;
    if (editorCanvasWidth > 0 && editorCanvasHeight > 0){
        // as a Play at the Game view's size would lay it out, scaled to the reference canvas:
        // the mapping keeps the fractions of the canvas each edge covers
        input.screenWidth = editorScreenWidth;
        input.screenHeight = editorScreenHeight;
        int playCanvasWidth = 0, playCanvasHeight = 0;
        Rect viewRect;
        if (editorScreenWidth > 0 && editorScreenHeight > 0){
            Engine::computeViewLayout(editorScreenWidth, editorScreenHeight, editorCanvasWidth, editorCanvasHeight,
                                      (Scaling)editorScaling, playCanvasWidth, playCanvasHeight, viewRect);
        }
        input.viewRect = viewRect;
        input.canvasWidth = (float)editorCanvasWidth;
        input.canvasHeight = (float)editorCanvasHeight;
    }else{
        if (System::hasInstance()){
            input.screenWidth = System::instance().getScreenWidth();
            input.screenHeight = System::instance().getScreenHeight();
        }
        input.viewRect = Engine::getViewRect();
        input.canvasWidth = (float)Engine::getCanvasWidth();
        input.canvasHeight = (float)Engine::getCanvasHeight();
    }
    input.insetsPixels = platformInsets;
    input.cutoutsPixels = platformCutouts;
    input.extraInsets = extraInsets;
    input.extraInPixels = extraInPixels;
    return input;
}

const SafeAreaResult& Screen::currentSafeArea(){
    // before the first update (init() of a game): the values of now, as a baseline
    if (!safeAreaComputed) updateSafeArea();
    return safeArea;
}

void Screen::updateSafeArea(){
    SafeAreaResult result = computeSafeArea(currentSafeAreaInput());
    if (result.deviceRectEmpty && !warnedEmptySafeArea){
        warnedEmptySafeArea = true;
        Log::warn("Screen.safeArea: the platform's safe-area insets cover the whole screen; the whole screen is used");
    }
    if (result.extrasTooLarge && !warnedExtraInsets){
        warnedExtraInsets = true;
        Log::warn("Screen.setExtraInsets: the extra insets are larger than the safe area; Screen.safeArea is 1x1");
    }
    const bool baseline = !safeAreaComputed;
    if (!baseline && sameResult(result, safeArea)) return;
    safeArea = std::move(result);
    safeAreaComputed = true;
    safeAreaRevision++;
    if (!baseline) safeAreaEventPending = true;
}

void Screen::dispatchSafeAreaEvent(){
    if (!safeAreaEventPending) return;
    safeAreaEventPending = false;
    Engine::onSafeAreaChanged.call();
}

void Screen::setPlatformSafeArea(const EdgeInsets& insetsPixels, const std::vector<Rect>& cutoutsPixels){
    platformInsets = nonNegative(insetsPixels);
    platformCutouts = cutoutsPixels;
    updateSafeArea();
}

EdgeInsets Screen::getPlatformInsetsPixels(){
    return platformInsets;
}

void Screen::setExtraInsets(const EdgeInsets& insets, bool pixels){
    extraInsets = nonNegative(insets);
    extraInPixels = pixels;
    warnedExtraInsets = false;
    updateSafeArea();
}

void Screen::clearExtraInsets(){
    setExtraInsets(EdgeInsets(), false);
}

EdgeInsets Screen::getExtraInsets(){
    return currentSafeArea().extraInsets;
}

Rect Screen::getSafeArea(){
    return currentSafeArea().safeArea;
}

Rect Screen::getDeviceSafeArea(){
    return currentSafeArea().deviceSafeArea;
}

EdgeInsets Screen::getSafeAreaInsets(){
    return currentSafeArea().safeAreaInsets;
}

EdgeInsets Screen::getDeviceSafeAreaInsets(){
    return currentSafeArea().deviceSafeAreaInsets;
}

Rect Screen::getSafeAreaPixels(){
    return currentSafeArea().safeAreaPixels;
}

std::vector<Rect> Screen::getCutouts(){
    return currentSafeArea().cutouts;
}

float Screen::getSafeAreaCanvasWidth(){
    if (editorCanvasWidth > 0 && editorCanvasHeight > 0) return (float)editorCanvasWidth;
    return (float)Engine::getCanvasWidth();
}

float Screen::getSafeAreaCanvasHeight(){
    if (editorCanvasWidth > 0 && editorCanvasHeight > 0) return (float)editorCanvasHeight;
    return (float)Engine::getCanvasHeight();
}

uint64_t Screen::getSafeAreaRevision(){
    currentSafeArea();
    return safeAreaRevision;
}

void Screen::setEditorReference(int canvasWidth, int canvasHeight, int screenWidth, int screenHeight, Scaling scaling){
    if (canvasWidth == editorCanvasWidth && canvasHeight == editorCanvasHeight && screenWidth == editorScreenWidth &&
        screenHeight == editorScreenHeight && (int)scaling == editorScaling && safeAreaComputed){
        return;
    }
    editorCanvasWidth = std::max(0, canvasWidth);
    editorCanvasHeight = std::max(0, canvasHeight);
    editorScreenWidth = std::max(0, screenWidth);
    editorScreenHeight = std::max(0, screenHeight);
    editorScaling = (int)scaling;
    updateSafeArea();
}

void Screen::clearEditorReference(){
    if (editorCanvasWidth == 0 && editorCanvasHeight == 0) return;
    editorCanvasWidth = editorCanvasHeight = editorScreenWidth = editorScreenHeight = 0;
    updateSafeArea();
}

bool Screen::hasEditorReference(){
    return editorCanvasWidth > 0 && editorCanvasHeight > 0;
}

void Screen::resetSafeAreaState(){
    extraInsets = EdgeInsets();
    extraInPixels = false;
    editorCanvasWidth = editorCanvasHeight = editorScreenWidth = editorScreenHeight = 0;
    warnedEmptySafeArea = false;
    warnedExtraInsets = false;
    safeAreaEventPending = false;
    // the next computation is where the game starts, not a change; the revision keeps counting
    safeAreaComputed = false;
}
