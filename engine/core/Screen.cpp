// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Screen.h"

#include "Engine.h"
#include "Log.h"
#include "util/PlatformRules.h"

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
