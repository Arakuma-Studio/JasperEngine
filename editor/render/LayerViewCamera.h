// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LAYERVIEWCAMERA_H
#define LAYERVIEWCAMERA_H

#include "RenderUtil.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::editor{

    // Ray through (x, y), in pixels of a view of viewWidth x viewHeight that camera draws
    inline Ray screenToRayFromCamera(const CameraComponent& camera, float x, float y, float viewWidth, float viewHeight){
        return screenToRayFromMatrix(camera.viewProjectionMatrix, camera.type == CameraType::CAMERA_ORTHO,
                                     camera.nearClip, camera.farClip, x, y, viewWidth, viewHeight);
    }

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

}

#endif /* LAYERVIEWCAMERA_H */
