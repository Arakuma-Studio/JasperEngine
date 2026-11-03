// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// rulerMajorStep keeps the band and the 1-2-5 progression of OpenEngine's
// SceneViewRulerOverlay::ChooseMajorStepWorld (UI/Controls/Widgets/SceneViewRulerOverlay.cpp),
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md).

#include "SceneViewMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr float PI = 3.14159265358979f;

    float radians(float degrees){
        return degrees * PI / 180.0f;
    }

}

Vector3 sceneview::axisViewDirection(SceneViewAxis axis){
    // the editor's Numpad and view gizmo views: Front looks along +Z, Right along -X
    switch (axis){
        case SceneViewAxis::TOP: return Vector3(0, -1, 0);
        case SceneViewAxis::BOTTOM: return Vector3(0, 1, 0);
        case SceneViewAxis::FRONT: return Vector3(0, 0, 1);
        case SceneViewAxis::BACK: return Vector3(0, 0, -1);
        case SceneViewAxis::RIGHT: return Vector3(-1, 0, 0);
        case SceneViewAxis::LEFT: return Vector3(1, 0, 0);
        case SceneViewAxis::FREE: break;
    }
    return Vector3(0, 0, -1);
}

Vector3 sceneview::axisViewUp(SceneViewAxis axis){
    // looking along Y the world up is the view direction: +Z is up on the screen from the top
    if (axis == SceneViewAxis::TOP) return Vector3(0, 0, 1);
    if (axis == SceneViewAxis::BOTTOM) return Vector3(0, 0, -1);
    return Vector3(0, 1, 0);
}

SceneViewAxis sceneview::alignedAxis(const Vector3& viewDirection){
    const float length = viewDirection.length();
    if (!(length > 0.0f)){
        return SceneViewAxis::FREE;
    }
    const Vector3 direction = viewDirection / length;
    const float threshold = std::cos(radians(AXIS_ALIGN_DEGREES));
    for (SceneViewAxis axis : {SceneViewAxis::TOP, SceneViewAxis::BOTTOM, SceneViewAxis::FRONT, SceneViewAxis::BACK,
                               SceneViewAxis::LEFT, SceneViewAxis::RIGHT}){
        if (direction.dotProduct(axisViewDirection(axis)) >= threshold){
            return axis;
        }
    }
    return SceneViewAxis::FREE;
}

float sceneview::orthoSizeForDistance(float distance, float fovDegrees){
    return std::max(0.0001f, distance * std::tan(radians(fovDegrees) * 0.5f));
}

float sceneview::distanceForOrthoSize(float orthoSize, float fovDegrees){
    const float t = std::tan(radians(fovDegrees) * 0.5f);
    return t > 0.0f ? orthoSize / t : orthoSize;
}

void sceneview::orthoClips(float distanceToTarget, float farClip, float& nearClip, float& farValue){
    nearClip = distanceToTarget - farClip * 0.5f;
    farValue = distanceToTarget + farClip * 0.5f;
}

void sceneview::dynamicClips(float distanceToTarget, float& nearClip, float& farClip){
    const float distance = std::max(distanceToTarget, 0.0001f);
    nearClip = std::max(0.001f, distance * 0.005f);
    farClip = std::max(nearClip * 100000.0f, distance * 1000.0f);
}

float sceneview::easeOutCubic(float t){
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

SceneCameraPose sceneview::interpolatePose(const SceneCameraPose& from, const SceneCameraPose& to, float t){
    t = std::clamp(t, 0.0f, 1.0f);
    if (t >= 1.0f){
        return to;
    }
    SceneCameraPose pose = to;
    pose.target = from.target + (to.target - from.target) * t;
    Vector3 a = from.position - from.target;
    Vector3 b = to.position - to.target;
    const float lengthA = a.length();
    const float lengthB = b.length();
    const float distance = lengthA + (lengthB - lengthA) * t;
    Vector3 direction;
    if (lengthA > 0.0f && lengthB > 0.0f){
        a = a / lengthA;
        b = b / lengthB;
        const float cosine = std::clamp(a.dotProduct(b), -1.0f, 1.0f);
        const float angle = std::acos(cosine);
        if (angle < 1e-4f){
            direction = a + (b - a) * t;
        }else if (angle > PI - 1e-3f){
            // opposite views: around the up axis
            Vector3 axis = from.up.crossProduct(a);
            if (axis.squaredLength() < 1e-8f) axis = a.perpendicular();
            axis.normalize();
            const float half = angle * t;
            direction = a * std::cos(half) + axis * std::sin(half);
        }else{
            const float sine = std::sin(angle);
            direction = a * (std::sin((1.0f - t) * angle) / sine) + b * (std::sin(t * angle) / sine);
        }
        direction.normalize();
    }else{
        direction = (lengthB > 0.0f) ? b / lengthB : Vector3(0, 0, 1);
    }
    pose.position = pose.target + direction * distance;
    Vector3 up = from.up + (to.up - from.up) * t;
    // perpendicular to the view, so the camera never rolls through a degenerate up
    up = up - direction * up.dotProduct(direction);
    pose.up = up.squaredLength() > 1e-8f ? up.normalized() : to.up;
    pose.orthoSize = from.orthoSize + (to.orthoSize - from.orthoSize) * t;
    pose.fieldOfView = from.fieldOfView + (to.fieldOfView - from.fieldOfView) * t;
    pose.orthographic = to.orthographic;
    return pose;
}

float sceneview::flyAccelerationFactor(float seconds, bool acceleration){
    if (!acceleration){
        return 1.0f;
    }
    const float t = std::clamp(seconds / SceneCameraSettings::ACCELERATION_SECONDS, 0.0f, 1.0f);
    return SceneCameraSettings::ACCELERATION_START + (1.0f - SceneCameraSettings::ACCELERATION_START) * t;
}

float sceneview::stepFlySpeed(float speed, float wheel, const SceneCameraSettings& settings){
    // a notch is 10%: the wheel covers the range quickly at any speed
    const float stepped = speed * std::pow(1.1f, wheel);
    return std::clamp(stepped, settings.speedMin, settings.speedMax);
}

double sceneview::rulerMajorStep(double unitsPerPixel, double minPixels, double maxPixels){
    if (!(unitsPerPixel > 0.0) || !std::isfinite(unitsPerPixel)){
        return 1.0;
    }
    const double low = minPixels * unitsPerPixel;
    const double high = maxPixels * unitsPerPixel;
    const double exponent = std::floor(std::log10(low));
    // the smallest 1, 2 or 5 step inside the band
    for (int k = -1; k <= 1; k++){
        const double power = std::pow(10.0, exponent + k);
        for (double mantissa : {1.0, 2.0, 5.0}){
            const double step = mantissa * power;
            if (step >= low && step <= high){
                return step;
            }
        }
    }
    // between 2 and 5 (a 2.5 ratio, the band's is narrower): 2.5 keeps it
    for (int k = -1; k <= 1; k++){
        const double step = 2.5 * std::pow(10.0, exponent + k);
        if (step >= low && step <= high){
            return step;
        }
    }
    return std::pow(10.0, exponent + 1.0);
}

std::string sceneview::rulerLabel(double value, double step){
    if (std::fabs(value) < step * 1e-6){
        return "0";
    }
    int decimals = 0;
    for (double s = step; decimals < 8 && std::fabs(s - std::round(s)) > s * 1e-6; s *= 10.0){
        decimals++;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

std::string sceneview::formatLength(double length, bool metres){
    char text[64];
    if (metres){
        std::snprintf(text, sizeof(text), "%.3f m", length);
    }else{
        std::snprintf(text, sizeof(text), "%.1f", length);
    }
    return text;
}
