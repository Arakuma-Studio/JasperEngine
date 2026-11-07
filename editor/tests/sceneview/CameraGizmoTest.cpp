// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Scene view's camera gizmo (docs/superpowers/specs/2026-11-04-render-correctness-design.md
// 6.10; Doriax 570a023cf): the compact gizmo was 2 units deep for every camera, so a wide
// orthographic camera drew as a flat slab; it is as deep as half its view's larger side now, and a
// selected camera draws near to far (Unity). A perspective camera renders with 0 < near < far.

#include "render/SceneRender.h"
#include "component/CameraComponent.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    CameraComponent gizmoOrthoCamera(float width, float height, float nearClip, float farClip){
        CameraComponent camera;
        camera.type = CameraType::CAMERA_ORTHO;
        camera.leftClip = -width / 2.0f;
        camera.rightClip = width / 2.0f;
        camera.bottomClip = -height / 2.0f;
        camera.topClip = height / 2.0f;
        camera.nearClip = nearClip;
        camera.farClip = farClip;
        return camera;
    }

    // the gizmo's depth: from its near plane to its far plane
    float gizmoDepthOf(const std::array<Vector3, 8>& corners){
        return std::abs(corners[4].z - corners[0].z);
    }

}

TEST(CameraGizmo, OrthoCameraGizmoHasDepth){
    const CameraComponent camera = gizmoOrthoCamera(20.0f, 10.0f, 0.0f, 100.0f);
    const std::array<Vector3, 8> corners = cameraGizmoCorners(camera, false);
    EXPECT_FLOAT_EQ(gizmoDepthOf(corners), 10.0f) << "the compact gizmo of a 20-wide orthographic camera is flat";
    // the near and far rectangles are the view's
    EXPECT_FLOAT_EQ(corners[0].x, -10.0f);
    EXPECT_FLOAT_EQ(corners[5].x, 10.0f);
    EXPECT_FLOAT_EQ(corners[0].y, 5.0f);
    // never past the far plane
    EXPECT_FLOAT_EQ(gizmoDepthOf(cameraGizmoCorners(gizmoOrthoCamera(20.0f, 10.0f, 0.0f, 4.0f), false)), 4.0f);
}

TEST(CameraGizmo, SelectedCameraShowsTheFullFrustum){
    CameraComponent perspective;
    perspective.type = CameraType::CAMERA_PERSPECTIVE;
    perspective.yfov = 1.0f;
    perspective.aspect = 1.5f;
    perspective.nearClip = 0.5f;
    perspective.farClip = 200.0f;
    EXPECT_FLOAT_EQ(gizmoDepthOf(cameraGizmoCorners(perspective, false)), 2.0f);
    const std::array<Vector3, 8> full = cameraGizmoCorners(perspective, true);
    EXPECT_FLOAT_EQ(full[0].z, -0.5f);
    EXPECT_FLOAT_EQ(full[4].z, -200.0f);
    EXPECT_NEAR(full[4].y, 200.0f * std::tan(0.5f), 1e-3f);

    EXPECT_FLOAT_EQ(gizmoDepthOf(cameraGizmoCorners(gizmoOrthoCamera(20.0f, 10.0f, 1.0f, 300.0f), true)), 299.0f);
}

TEST(CameraGizmo, ADegenerateViewDrawsNoNaN){
    const std::array<Vector3, 8> corners = cameraGizmoCorners(gizmoOrthoCamera(0.0f, 0.0f, 0.0f, 100.0f), false);
    for (const Vector3& corner : corners){
        EXPECT_TRUE(std::isfinite(corner.x) && std::isfinite(corner.y) && std::isfinite(corner.z));
    }
    EXPECT_FLOAT_EQ(gizmoDepthOf(corners), 0.0f);
}

TEST(CameraGizmo, PerspectiveNearIsClamped){
    CameraComponent camera;
    camera.type = CameraType::CAMERA_PERSPECTIVE;
    camera.nearClip = 0.0f;
    camera.farClip = 0.0f;
    const Vector2 nearFar = RenderSystem::getCameraNearFar(camera);
    EXPECT_FLOAT_EQ(nearFar.x, 0.001f);
    EXPECT_GT(nearFar.y, nearFar.x);
    camera.nearClip = -1.0f;
    EXPECT_FLOAT_EQ(RenderSystem::getCameraNearFar(camera).x, 0.001f);
    // a small positive near is the user's: kept
    camera.nearClip = 0.0001f;
    camera.farClip = 100.0f;
    EXPECT_FLOAT_EQ(RenderSystem::getCameraNearFar(camera).x, 0.0001f);
    EXPECT_FLOAT_EQ(RenderSystem::getCameraNearFar(camera).y, 100.0f);
    // orthographic near and far stay as they are (a negative near is valid there)
    const Vector2 ortho = RenderSystem::getCameraNearFar(gizmoOrthoCamera(10.0f, 10.0f, -5.0f, 5.0f));
    EXPECT_FLOAT_EQ(ortho.x, -5.0f);
    EXPECT_FLOAT_EQ(ortho.y, 5.0f);
}
