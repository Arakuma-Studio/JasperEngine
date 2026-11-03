// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "CanvasScaler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

bool CanvasScalerSettings::operator==(const CanvasScalerSettings& other) const{
    return mode == other.mode && scaleFactor == other.scaleFactor && referenceResolution == other.referenceResolution &&
           screenMatchMode == other.screenMatchMode && matchWidthOrHeight == other.matchWidthOrHeight &&
           physicalUnit == other.physicalUnit && fallbackScreenDPI == other.fallbackScreenDPI;
}

static bool positive(float value){
    return std::isfinite(value) && value > 0.0f;
}

CanvasScalerResult CanvasScaler::compute(const CanvasScalerSettings& settings, float viewWidth, float viewHeight,
                                         float gameCanvasWidth, float gameCanvasHeight, float screenDpi,
                                         const CanvasScalerResult& last){
    if (!positive(viewWidth) || !positive(viewHeight))
        return last;

    CanvasScalerResult result;

    if (settings.mode == UIScaleMode::FollowGameCanvas){
        if (!positive(gameCanvasWidth) || !positive(gameCanvasHeight))
            return last;
        result.uiWidth = gameCanvasWidth;
        result.uiHeight = gameCanvasHeight;
        result.scaleFactor = viewWidth / gameCanvasWidth;
        result.scaleFactorY = viewHeight / gameCanvasHeight;
        result.scaled = false;
        return result;
    }

    float scale = 1.0f;

    if (settings.mode == UIScaleMode::ConstantPixelSize){
        if (!std::isfinite(settings.scaleFactor))
            return last;
        scale = std::max(settings.scaleFactor, 0.01f);

    }else if (settings.mode == UIScaleMode::ScaleWithScreenSize){
        const float referenceWidth = settings.referenceResolution.x;
        const float referenceHeight = settings.referenceResolution.y;
        if (!positive(referenceWidth) || !positive(referenceHeight))
            return last;

        const float widthRatio = viewWidth / referenceWidth;
        const float heightRatio = viewHeight / referenceHeight;

        switch (settings.screenMatchMode){
            case ScreenMatchMode::MatchWidthOrHeight: {
                // a log-space blend, so 0.5 between a doubled width and a halved height is 1
                const float match = std::isfinite(settings.matchWidthOrHeight)
                    ? std::clamp(settings.matchWidthOrHeight, 0.0f, 1.0f) : 0.0f;
                const float logWidth = std::log2(widthRatio);
                const float logHeight = std::log2(heightRatio);
                scale = std::pow(2.0f, logWidth + (logHeight - logWidth) * match);
                break;
            }
            case ScreenMatchMode::Expand:
                scale = std::min(widthRatio, heightRatio);
                break;
            case ScreenMatchMode::Shrink:
                scale = std::max(widthRatio, heightRatio);
                break;
        }

    }else if (settings.mode == UIScaleMode::ConstantPhysicalSize){
        const float dpi = positive(screenDpi) ? screenDpi : settings.fallbackScreenDPI;
        float unitsPerInch = 72.0f;
        switch (settings.physicalUnit){
            case PhysicalUnit::Centimeters: unitsPerInch = 2.54f; break;
            case PhysicalUnit::Millimeters: unitsPerInch = 25.4f; break;
            case PhysicalUnit::Inches:      unitsPerInch = 1.0f; break;
            case PhysicalUnit::Points:      unitsPerInch = 72.0f; break;
            case PhysicalUnit::Picas:       unitsPerInch = 6.0f; break;
        }
        scale = dpi / unitsPerInch;
    }

    if (!positive(scale))
        return last;

    result.uiWidth = viewWidth / scale;
    result.uiHeight = viewHeight / scale;
    result.scaleFactor = scale;
    result.scaleFactorY = scale;
    result.scaled = true;
    return result;
}

namespace {

    template<typename T, size_t N>
    const char* nameOf(const char* const (&names)[N], T value){
        const size_t index = static_cast<size_t>(value);
        return index < N ? names[index] : names[0];
    }

    template<typename T, size_t N>
    bool parseName(const char* const (&names)[N], const char* name, T& value){
        if (!name)
            return false;
        for (size_t i = 0; i < N; i++){
            if (std::strcmp(names[i], name) == 0){
                value = static_cast<T>(i);
                return true;
            }
        }
        return false;
    }

    const char* const modeNames[] = {"follow_game_canvas", "constant_pixel_size", "scale_with_screen_size", "constant_physical_size"};
    const char* const matchModeNames[] = {"match_width_or_height", "expand", "shrink"};
    const char* const unitNames[] = {"centimeters", "millimeters", "inches", "points", "picas"};

}

const char* CanvasScaler::modeName(UIScaleMode mode){
    return nameOf(modeNames, mode);
}

bool CanvasScaler::parseMode(const char* name, UIScaleMode& mode){
    return parseName(modeNames, name, mode);
}

const char* CanvasScaler::matchModeName(ScreenMatchMode mode){
    return nameOf(matchModeNames, mode);
}

bool CanvasScaler::parseMatchMode(const char* name, ScreenMatchMode& mode){
    return parseName(matchModeNames, name, mode);
}

const char* CanvasScaler::unitName(PhysicalUnit unit){
    return nameOf(unitNames, unit);
}

bool CanvasScaler::parseUnit(const char* name, PhysicalUnit& unit){
    return parseName(unitNames, name, unit);
}
