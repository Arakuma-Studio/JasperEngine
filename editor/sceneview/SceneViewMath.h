// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The pure maths of the Scene view's camera and tools (docs/specs/S26-scene-view.md R17-R25):
// axis views, the orthographic size that keeps a perspective framing, the eased camera moves,
// the fly acceleration, dynamic clipping and the rulers' steps. No ImGui, no engine state:
// SceneWindow and the MCP tools drive them, the editor tests check them.

#include "sceneview/SceneViewState.h"
#include "math/Vector3.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor::sceneview {

    // within this angle of an axis a view counts as looking along it (Unity's automatic
    // orthographic for axis views)
    inline constexpr float AXIS_ALIGN_DEGREES = 0.5f;

    // the direction a camera looks along for an axis view (Top looks down -Y), and its up
    Vector3 axisViewDirection(SceneViewAxis axis);
    Vector3 axisViewUp(SceneViewAxis axis);
    // the axis view a direction is aligned to, FREE when none
    SceneViewAxis alignedAxis(const Vector3& viewDirection);

    // the orthographic half height that shows at the target what a perspective camera of
    // fovDegrees shows from `distance` (R17: distance * tan(fov / 2)), and back
    float orthoSizeForDistance(float distance, float fovDegrees);
    float distanceForOrthoSize(float orthoSize, float fovDegrees);

    // near and far of an orthographic view: +-far/2 around the target, so zooming in never clips
    void orthoClips(float distanceToTarget, float farClip, float& nearClip, float& farValue);
    // Dynamic Clipping: near and far from the distance to the target
    void dynamicClips(float distanceToTarget, float& nearClip, float& farClip);

    // Camera Easing: 0.25 s ease-out cubic
    float easeOutCubic(float t);
    // between two poses: the target and distance linearly, the view direction along the
    // shorter arc, the up re-orthogonalized; projection values linearly
    SceneCameraPose interpolatePose(const SceneCameraPose& from, const SceneCameraPose& to, float t);

    // Camera Acceleration: the share of the fly speed after `seconds` of continuous flight
    // (25% to 100% over 0.6 s)
    float flyAccelerationFactor(float seconds, bool acceleration);
    // the wheel during a fly: the speed one notch up or down, kept in [speedMin, speedMax]
    float stepFlySpeed(float speed, float wheel, const SceneCameraSettings& settings);

    // R25: the rulers' major step in world units for a view showing `unitsPerPixel`: 1, 2 or
    // 5 times a power of ten whose spacing is within [minPixels, maxPixels]; 2.5 times one
    // where neither 2 nor 5 lands in the band (their ratio is wider than the band's)
    double rulerMajorStep(double unitsPerPixel, double minPixels = 64.0, double maxPixels = 130.0);
    inline constexpr int RULER_MINOR_TICKS = 4;
    // a ruler label: as short as the step allows
    std::string rulerLabel(double value, double step);

    // "4.000 m", "120 px": a measure's length as the pill shows it
    std::string formatLength(double length, bool metres);

}
