// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Transparency Sort Mode by projection (docs/specs/S94-rendering-anim-gaps.md F5, AC5): the
// distance a 3D pass sorts transparent draws by (far first). Perspective is the distance to the
// camera, Orthographic the depth along its view axis, Default the view depth for an
// orthographic camera and the distance otherwise (Unity's rule). An orthographic camera scrolling
// past two sprites a little apart in depth keeps their order whatever its x.

#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "registry/SortingLayers.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace jasperengine;

namespace {

    // A at (50, 0, -1), B at (0, 0, -2), seen by a camera at (x, 0, 10) looking down -Z: the
    // order the transparent queue draws them in ("AB": A first)
    std::string drawOrder(TransparencySortMode mode, CameraType type, float cameraX){
        SortingLayers::setTransparencySortMode(mode);
        // outside a registry nothing deletes the framebuffer the camera's initialiser allocates
        CameraComponent camera;
        delete camera.framebuffer;
        camera.framebuffer = nullptr;
        camera.type = type;
        camera.worldDirection = Vector3(0.0f, 0.0f, 1.0f); // the view's backward axis
        Transform cameraTransform;
        cameraTransform.worldPosition = Vector3(cameraX, 0.0f, 10.0f);
        std::vector<RenderSystem::TransparentRenderData> queue(2);
        const Vector3 positions[2] = {Vector3(50.0f, 0.0f, -1.0f), Vector3(0.0f, 0.0f, -2.0f)};
        for (uint32_t i = 0; i < 2; i++){
            queue[i] = {};
            queue[i].distanceToCamera = RenderSystem::transparentSortDistance(camera, cameraTransform, positions[i]);
            queue[i].waterSortDistance = queue[i].distanceToCamera;
            queue[i].order = i;
        }
        std::sort(queue.begin(), queue.end(), RenderSystem::TransparentRenderComparison());
        std::string order;
        for (const auto& entry : queue)
            order += entry.order == 0 ? "A" : "B";
        return order;
    }

}

TEST(RenderSorting, OrthographicSortsByViewDepth){
    for (float x = -100.0f; x <= 100.0f; x += 10.0f){
        SCOPED_TRACE("camera x " + std::to_string(x));
        // B is farther along the view: drawn first, A after it, wherever the camera is
        EXPECT_EQ(drawOrder(TransparencySortMode::DEFAULT, CameraType::CAMERA_ORTHO, x), "BA");
        EXPECT_EQ(drawOrder(TransparencySortMode::ORTHOGRAPHIC, CameraType::CAMERA_ORTHO, x), "BA");
        EXPECT_EQ(drawOrder(TransparencySortMode::ORTHOGRAPHIC, CameraType::CAMERA_PERSPECTIVE, x), "BA");
        EXPECT_EQ(drawOrder(TransparencySortMode::DEFAULT, CameraType::CAMERA_UI, x), "BA");
    }
    // Perspective (and Default with a perspective camera) keep the distance: the camera at
    // x 0 has A (50 away) farther, at x 50 B
    EXPECT_EQ(drawOrder(TransparencySortMode::PERSPECTIVE, CameraType::CAMERA_ORTHO, 0.0f), "AB");
    EXPECT_EQ(drawOrder(TransparencySortMode::PERSPECTIVE, CameraType::CAMERA_ORTHO, 50.0f), "BA");
    EXPECT_EQ(drawOrder(TransparencySortMode::DEFAULT, CameraType::CAMERA_PERSPECTIVE, 0.0f), "AB");
    EXPECT_EQ(drawOrder(TransparencySortMode::CUSTOM_AXIS, CameraType::CAMERA_ORTHO, 0.0f), "AB");
    SortingLayers::reset();
}

TEST(RenderSorting, ModeTokensRoundTrip){
    for (TransparencySortMode mode : {TransparencySortMode::DEFAULT, TransparencySortMode::CUSTOM_AXIS,
                                      TransparencySortMode::PERSPECTIVE, TransparencySortMode::ORTHOGRAPHIC}){
        TransparencySortMode read = TransparencySortMode::DEFAULT;
        EXPECT_TRUE(SortingLayers::transparencySortModeFromToken(SortingLayers::transparencySortModeToken(mode), read));
        EXPECT_EQ(read, mode);
    }
    TransparencySortMode read = TransparencySortMode::PERSPECTIVE;
    EXPECT_FALSE(SortingLayers::transparencySortModeFromToken("isometric", read));
    EXPECT_EQ(read, TransparencySortMode::PERSPECTIVE);
}
