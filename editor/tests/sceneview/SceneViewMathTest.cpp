// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26: the Scene view's camera and ruler maths (R17, R18, R21, R25; AC16 RulerStep.NiceSteps)

#include "sceneview/SceneViewMath.h"
#include "component/CameraComponent.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(RulerStep, NiceSteps) {
    // 1e-4 .. 1e6 world units per pixel, 40 per decade: the major step is 1, 2 or 5 (2.5 where
    // neither lands) times a power of ten, and its spacing stays within the 64-130 px band
    for (int i = -160; i <= 240; i++) {
        const double unitsPerPixel = std::pow(10.0, i / 40.0);
        const double step = sceneview::rulerMajorStep(unitsPerPixel);
        const double pixels = step / unitsPerPixel;
        EXPECT_GE(pixels, 64.0 * (1.0 - 1e-9)) << "units/px " << unitsPerPixel << " step " << step;
        EXPECT_LE(pixels, 130.0 * (1.0 + 1e-9)) << "units/px " << unitsPerPixel << " step " << step;
        const double exponent = std::floor(std::log10(step) + 1e-9);
        const double mantissa = step / std::pow(10.0, exponent);
        const bool nice = std::fabs(mantissa - 1.0) < 1e-6 || std::fabs(mantissa - 2.0) < 1e-6 || std::fabs(mantissa - 2.5) < 1e-6 ||
                          std::fabs(mantissa - 5.0) < 1e-6;
        EXPECT_TRUE(nice) << "step " << step << " (mantissa " << mantissa << ")";
    }
    // the plain 1-2-5 choice wherever it fits the band
    EXPECT_DOUBLE_EQ(sceneview::rulerMajorStep(1.0 / 100.0), 1.0);
    EXPECT_DOUBLE_EQ(sceneview::rulerMajorStep(1.0 / 50.0), 2.0);
    EXPECT_DOUBLE_EQ(sceneview::rulerMajorStep(0.05), 5.0);
}

TEST(RulerStep, Labels) {
    EXPECT_EQ(sceneview::rulerLabel(0.0, 1.0), "0");
    EXPECT_EQ(sceneview::rulerLabel(20.0, 10.0), "20");
    EXPECT_EQ(sceneview::rulerLabel(0.25, 0.25), "0.25");
    EXPECT_EQ(sceneview::rulerLabel(-1.5, 0.5), "-1.5");
    EXPECT_EQ(sceneview::formatLength(4.0, true), "4.000 m");
}

TEST(SceneViewMath, OrthoSizeKeepsTheFraming) {
    // R17: half height = distance * tan(fov / 2), and back
    EXPECT_NEAR(sceneview::orthoSizeForDistance(10.0f, 90.0f), 10.0f, 1e-4f);
    EXPECT_NEAR(sceneview::orthoSizeForDistance(10.0f, 60.0f), 10.0f * std::tan(0.5235987756f), 1e-4f);
    for (float fov : {20.0f, 60.0f, 110.0f}) {
        EXPECT_NEAR(sceneview::distanceForOrthoSize(sceneview::orthoSizeForDistance(37.0f, fov), fov), 37.0f, 1e-3f);
    }
    // near and far +-far/2 around the target
    float nearClip = 0.0f, farClip = 0.0f;
    sceneview::orthoClips(5.0f, 1000.0f, nearClip, farClip);
    EXPECT_FLOAT_EQ(nearClip, -495.0f);
    EXPECT_FLOAT_EQ(farClip, 505.0f);
}

TEST(SceneViewMath, AxisViews) {
    for (SceneViewAxis axis : {SceneViewAxis::TOP, SceneViewAxis::BOTTOM, SceneViewAxis::FRONT, SceneViewAxis::BACK, SceneViewAxis::LEFT,
                               SceneViewAxis::RIGHT}) {
        const Vector3 direction = sceneview::axisViewDirection(axis);
        EXPECT_EQ(sceneview::alignedAxis(direction), axis);
        // the up of an axis view is never along it
        EXPECT_LT(std::fabs(direction.dotProduct(sceneview::axisViewUp(axis))), 1e-6f);
    }
    // within half a degree counts, a degree off does not
    const float small = std::tan(0.3f * 3.14159265f / 180.0f);
    EXPECT_EQ(sceneview::alignedAxis(Vector3(small, -1.0f, 0.0f)), SceneViewAxis::TOP);
    const float off = std::tan(1.0f * 3.14159265f / 180.0f);
    EXPECT_EQ(sceneview::alignedAxis(Vector3(off, -1.0f, 0.0f)), SceneViewAxis::FREE);
    EXPECT_EQ(sceneview::alignedAxis(Vector3(1.0f, -1.0f, 0.3f)), SceneViewAxis::FREE);
}

// a camera aimed straight down or up (set_scene_camera, a game camera) takes the Top / Bottom
// views' up, so it frames like those views (agent-authoring)
TEST(SceneViewMath, StraightUpOrDownUsesTheAxisViewUp) {
    EXPECT_EQ(cameraUpFor(sceneview::axisViewDirection(SceneViewAxis::TOP), Vector3(0, 1, 0)), sceneview::axisViewUp(SceneViewAxis::TOP));
    EXPECT_EQ(cameraUpFor(sceneview::axisViewDirection(SceneViewAxis::BOTTOM), Vector3(0, 1, 0)), sceneview::axisViewUp(SceneViewAxis::BOTTOM));
}

TEST(SceneViewMath, EasedMoves) {
    EXPECT_FLOAT_EQ(sceneview::easeOutCubic(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(sceneview::easeOutCubic(1.0f), 1.0f);
    EXPECT_GT(sceneview::easeOutCubic(0.5f), 0.5f) << "ease-out: fast first";

    SceneCameraPose from;
    from.position = Vector3(0, 0, 10);
    from.target = Vector3(0, 0, 0);
    from.orthoSize = 2.0f;
    SceneCameraPose to;
    to.position = Vector3(20, 0, 0);
    to.target = Vector3(10, 0, 0);
    to.orthoSize = 6.0f;
    const SceneCameraPose start = sceneview::interpolatePose(from, to, 0.0f);
    EXPECT_NEAR((start.position - from.position).length(), 0.0f, 1e-4f);
    const SceneCameraPose end = sceneview::interpolatePose(from, to, 1.0f);
    EXPECT_NEAR((end.position - to.position).length(), 0.0f, 1e-5f);
    const SceneCameraPose middle = sceneview::interpolatePose(from, to, 0.5f);
    EXPECT_NEAR(middle.target.x, 5.0f, 1e-4f);
    EXPECT_NEAR((middle.position - middle.target).length(), 10.0f, 1e-3f) << "the distance is interpolated, not cut short";
    EXPECT_NEAR(middle.orthoSize, 4.0f, 1e-4f);
    EXPECT_LT(std::fabs(middle.up.dotProduct((middle.target - middle.position).normalized())), 1e-4f);
}

TEST(SceneViewMath, FlySpeed) {
    // Camera Acceleration: 25% at once, full after 0.6 s; off: full at once
    EXPECT_FLOAT_EQ(sceneview::flyAccelerationFactor(0.0f, true), 0.25f);
    EXPECT_FLOAT_EQ(sceneview::flyAccelerationFactor(0.6f, true), 1.0f);
    EXPECT_FLOAT_EQ(sceneview::flyAccelerationFactor(5.0f, true), 1.0f);
    EXPECT_NEAR(sceneview::flyAccelerationFactor(0.3f, true), 0.625f, 1e-5f);
    EXPECT_FLOAT_EQ(sceneview::flyAccelerationFactor(0.0f, false), 1.0f);
    // the wheel steps the speed, kept within Speed Min/Max
    SceneCameraSettings settings;
    EXPECT_GT(sceneview::stepFlySpeed(10.0f, 1.0f, settings), 10.0f);
    EXPECT_LT(sceneview::stepFlySpeed(10.0f, -1.0f, settings), 10.0f);
    EXPECT_FLOAT_EQ(sceneview::stepFlySpeed(settings.speedMax, 10.0f, settings), settings.speedMax);
    EXPECT_FLOAT_EQ(sceneview::stepFlySpeed(settings.speedMin, -10.0f, settings), settings.speedMin);
    // dynamic clipping follows the distance
    float nearClip = 0.0f, farClip = 0.0f;
    sceneview::dynamicClips(100.0f, nearClip, farClip);
    float nearFar = 0.0f, farFar = 0.0f;
    sceneview::dynamicClips(1000.0f, nearFar, farFar);
    EXPECT_GT(nearFar, nearClip);
    EXPECT_GT(farFar, farClip);
    EXPECT_LT(nearClip, 100.0f);
    EXPECT_GT(farClip, 100.0f);
}

TEST(SceneViewMath, CameraSettingsClamp) {
    SceneCameraSettings settings;
    settings.fieldOfView = 400.0f;
    settings.speed = 5000.0f;
    settings.nearClip = 50.0f;
    settings.farClip = 10.0f;
    settings.clamp();
    EXPECT_FLOAT_EQ(settings.fieldOfView, SceneCameraSettings::MAX_FOV);
    EXPECT_LE(settings.speed, settings.speedMax);
    EXPECT_LT(settings.nearClip, settings.farClip);
}
