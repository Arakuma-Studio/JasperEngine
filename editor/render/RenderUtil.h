// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "math/Ray.h"
#include "math/Rect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace jasperengine::editor{

    // keys of what a scene caches for each of the editor's secondary views
    constexpr uint32_t SCENE_VIEW_ID = 1;
    constexpr uint32_t GAME_VIEW_ID = 2;

    // Ray through (x, y), in pixels of a view of viewWidth x viewHeight that viewProjection
    // covers. An orthographic ray spans the whole near-far range.
    inline Ray screenToRayFromMatrix(const Matrix4& viewProjection, bool orthographic, float nearClip, float farClip,
                                     float x, float y, float viewWidth, float viewHeight){
        float normalizedX = ((2.0f * x) / viewWidth) - 1.0f;
        float normalizedY = -(((2.0f * y) / viewHeight) - 1.0f);

        Vector4 nearPointNdc = {normalizedX, normalizedY, -1.0f, 1.0f};
        Vector4 farPointNdc = {normalizedX, normalizedY, 1.0f, 1.0f};

        Matrix4 inverseViewProjection = viewProjection.inverse();
        Vector4 nearPointWorld = inverseViewProjection * nearPointNdc;
        Vector4 farPointWorld = inverseViewProjection * farPointNdc;

        nearPointWorld.divideByW();
        farPointWorld.divideByW();

        Vector3 rayOrigin = {nearPointWorld[0], nearPointWorld[1], nearPointWorld[2]};
        Vector3 rayEnd = {farPointWorld[0], farPointWorld[1], farPointWorld[2]};
        Vector3 rayDirection = rayEnd - rayOrigin;

        if (orthographic){
            float length = farClip - nearClip;
            rayDirection = rayDirection.normalize() * length;
        }

        return Ray(rayOrigin, rayDirection);
    }

    // Ray through (x, y), in pixels of a view of viewWidth x viewHeight that camera draws
    inline Ray screenToRayFromCamera(const CameraComponent& camera, float x, float y, float viewWidth, float viewHeight){
        return screenToRayFromMatrix(camera.viewProjectionMatrix, camera.type == CameraType::CAMERA_ORTHO,
                                     camera.nearClip, camera.farClip, x, y, viewWidth, viewHeight);
    }

    // A scene layered in a view (a child scene over the Scene view, a game's layer scene)
    // as that view shows it: drawn through its camera over rect, a part of a view of
    // viewWidth x viewHeight pixels (y down). Everything below is the camera that covers
    // the whole view instead and puts every world point on the same pixel, so rays, gizmos
    // and projections of the layer work in whole-view pixels like the view's own camera.
    struct LayerView{
        CameraType type = CameraType::CAMERA_ORTHO;
        Matrix4 viewMatrix;
        Vector3 worldPosition;
        Vector3 worldDirection;      // as CameraComponent::worldDirection (the view's +Z)
        float nearClip = 0.0f;
        float farClip = 0.0f;
        // whole-view projection, in the camera's own terms
        float leftClip = 0.0f;
        float rightClip = 0.0f;
        float bottomClip = 0.0f;
        float topClip = 0.0f;
        float yfov = 0.0f;
        float aspect = 1.0f;
        Matrix4 projectionMatrix;
        Matrix4 viewProjectionMatrix;
        // where the layer itself is drawn
        Rect rect;
        float viewWidth = 0.0f;
        float viewHeight = 0.0f;

        bool isOrthographic() const{
            return type != CameraType::CAMERA_PERSPECTIVE;
        }

        bool contains(float x, float y) const{
            return x >= rect.getX() && x <= rect.getX() + rect.getWidth()
                && y >= rect.getY() && y <= rect.getY() + rect.getHeight();
        }

        Ray ray(float x, float y) const{
            return screenToRayFromMatrix(viewProjectionMatrix, isOrthographic(), nearClip, farClip, x, y, viewWidth, viewHeight);
        }

        // world length one pixel of the view covers at worldPoint, in the same terms
        // SceneRender::billboardScreenScale sizes the view's own handles
        float worldPerPixel(const Vector3& worldPoint) const{
            if (viewHeight <= 0.0f){
                return 1.0f;
            }
            if (isOrthographic()){
                return std::fabs(topClip - bottomClip) / viewHeight;
            }
            const float distance = (worldPoint - worldPosition).length();
            const float size = std::tan(yfov) * distance / viewHeight;
            return (std::isfinite(size) && size > 0.0f) ? size : 1.0f;
        }
    };

    // The camera of a layered scene as a view draws it. canvasWidth/Height > 0 is the
    // canvas the view gives an auto-resize camera (Engine::ViewLayer::canvasWidth); rect
    // must be centered in the view for a perspective camera, which has no off-center form.
    inline LayerView makeLayerView(const CameraComponent& camera, const Transform& cameraTransform, const Rect& rect,
                                   float viewWidth, float viewHeight, float canvasWidth = 0.0f, float canvasHeight = 0.0f){
        LayerView view;
        view.type = camera.type;
        view.viewMatrix = camera.viewMatrix;
        view.worldPosition = cameraTransform.worldPosition;
        view.worldDirection = Vector3(camera.viewMatrix[0][2], camera.viewMatrix[1][2], camera.viewMatrix[2][2]);
        view.nearClip = camera.nearClip;
        view.farClip = camera.farClip;
        view.rect = rect;
        view.viewWidth = viewWidth;
        view.viewHeight = viewHeight;

        float left = camera.leftClip;
        float right = camera.rightClip;
        float bottom = camera.bottomClip;
        float top = camera.topClip;
        float aspect = camera.aspect;
        if (canvasWidth > 0.0f && canvasHeight > 0.0f && camera.autoResize && !camera.renderToTexture){
            left = 0.0f;
            bottom = 0.0f;
            right = canvasWidth;
            top = canvasHeight;
            aspect = canvasWidth / canvasHeight;
        }

        const float rectX = rect.getX();
        const float rectY = rect.getY();
        const float rectWidth = std::max(rect.getWidth(), 1.0f);
        const float rectHeight = std::max(rect.getHeight(), 1.0f);

        // The values at NDC -1 and +1 of the rect, stretched to the edges of the view.
        // NDC +1 in y is the top of the rect, and view pixels run downwards.
        auto widenX = [&](float ndcLow, float ndcHigh, float& viewLow, float& viewHigh){
            viewLow = ndcLow - rectX * (ndcHigh - ndcLow) / rectWidth;
            viewHigh = ndcLow + (viewWidth - rectX) * (ndcHigh - ndcLow) / rectWidth;
        };
        auto widenY = [&](float ndcLow, float ndcHigh, float& viewLow, float& viewHigh){
            viewHigh = ndcHigh + rectY * (ndcHigh - ndcLow) / rectHeight;
            viewLow = ndcHigh - (viewHeight - rectY) * (ndcHigh - ndcLow) / rectHeight;
        };

        if (camera.type == CameraType::CAMERA_PERSPECTIVE){
            const float tanY = std::tan(camera.yfov * 0.5f);
            const float tanX = aspect * tanY;
            float nearLeft, nearRight, nearBottom, nearTop;
            widenX(-tanX * camera.nearClip, tanX * camera.nearClip, nearLeft, nearRight);
            widenY(-tanY * camera.nearClip, tanY * camera.nearClip, nearBottom, nearTop);
            view.projectionMatrix = Matrix4::frustumMatrix(nearLeft, nearRight, nearBottom, nearTop, camera.nearClip, camera.farClip);
            const float viewTanY = (nearTop - nearBottom) * 0.5f / camera.nearClip;
            const float viewTanX = (nearRight - nearLeft) * 0.5f / camera.nearClip;
            view.yfov = 2.0f * std::atan(viewTanY);
            view.aspect = (viewTanY != 0.0f) ? viewTanX / viewTanY : aspect;
            view.leftClip = left;
            view.rightClip = right;
            view.bottomClip = bottom;
            view.topClip = top;
        }else{
            // NDC -1 and +1 in y: a UI camera puts its top clip at the bottom of NDC
            const float ndcLow = (camera.type == CameraType::CAMERA_UI) ? top : bottom;
            const float ndcHigh = (camera.type == CameraType::CAMERA_UI) ? bottom : top;
            float viewLeft, viewRight, viewNdcLow, viewNdcHigh;
            widenX(left, right, viewLeft, viewRight);
            widenY(ndcLow, ndcHigh, viewNdcLow, viewNdcHigh);
            view.projectionMatrix = Matrix4::orthoMatrix(viewLeft, viewRight, viewNdcLow, viewNdcHigh, camera.nearClip, camera.farClip);
            view.leftClip = viewLeft;
            view.rightClip = viewRight;
            view.bottomClip = (camera.type == CameraType::CAMERA_UI) ? viewNdcHigh : viewNdcLow;
            view.topClip = (camera.type == CameraType::CAMERA_UI) ? viewNdcLow : viewNdcHigh;
            view.yfov = camera.yfov;
            view.aspect = (viewNdcHigh != viewNdcLow) ? std::fabs((viewRight - viewLeft) / (viewNdcHigh - viewNdcLow)) : aspect;
        }

        view.viewProjectionMatrix = view.projectionMatrix * view.viewMatrix;
        return view;
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

    // The Move/Rotate/Scale gizmos while Physics (G) is on: their colours pulled toward the
    // Physics orange, so the axes still read apart and a highlight stays brighter
    inline Vector4 physicsGizmoColor(const Vector3& color, float alpha, bool physics){
        if (!physics){
            return Vector4(color, alpha);
        }
        const Vector3 orange(0.94f, 0.55f, 0.16f);
        return Vector4(color * 0.45f + orange * 0.55f, alpha);
    }

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

