// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "component/CameraComponent.h"
#include "math/Ray.h"

#include <cstdint>

namespace jasperengine::editor{

    // keys of what a scene caches for each of the editor's secondary views
    constexpr uint32_t SCENE_VIEW_ID = 1;
    constexpr uint32_t GAME_VIEW_ID = 2;

    // Ray through (x, y), in pixels of a view of viewWidth x viewHeight that camera draws
    inline Ray screenToRayFromCamera(const CameraComponent& camera, float x, float y, float viewWidth, float viewHeight){
        float normalizedX = ((2.0f * x) / viewWidth) - 1.0f;
        float normalizedY = -(((2.0f * y) / viewHeight) - 1.0f);

        Vector4 nearPointNdc = {normalizedX, normalizedY, -1.0f, 1.0f};
        Vector4 farPointNdc = {normalizedX, normalizedY, 1.0f, 1.0f};

        Matrix4 inverseViewProjection = camera.viewProjectionMatrix.inverse();
        Vector4 nearPointWorld = inverseViewProjection * nearPointNdc;
        Vector4 farPointWorld = inverseViewProjection * farPointNdc;

        nearPointWorld.divideByW();
        farPointWorld.divideByW();

        Vector3 rayOrigin = {nearPointWorld[0], nearPointWorld[1], nearPointWorld[2]};
        Vector3 rayEnd = {farPointWorld[0], farPointWorld[1], farPointWorld[2]};
        Vector3 rayDirection = rayEnd - rayOrigin;

        // for orthographic cameras the ray spans the whole near-far range
        if (camera.type == CameraType::CAMERA_ORTHO){
            float length = camera.farClip - camera.nearClip;
            rayDirection = rayDirection.normalize() * length;
        }

        return Ray(rayOrigin, rayDirection);
    }

    enum class CursorSelected{
        POINTER,
        HAND
    };

    enum class GizmoSelected{
        TRANSLATE,
        ROTATE,
        SCALE,
        OBJECT2D
    };

    enum class GizmoSideSelected{
        NONE,
        X,
        Y,
        Z,
        XY,
        XZ,
        YZ,
        XYZ
    };

    enum class Gizmo2DSideSelected{
        NONE,
        NX_NY, // -X -Y
        NX, // -X
        NX_PY, // -X +Y
        PY, // +Y
        PX_PY, // +X +Y
        PX, // +X
        PX_NY, // +X -Y
        NY, // -Y
        CENTER
    };

}

