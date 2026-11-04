// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteFrameGeometry.h"

namespace jasperengine{

    Vector2 spriteFramePivot(const SpriteFrame& frame, PivotPreset preset){
        const float w = frame.sourceSize.x;
        const float h = frame.sourceSize.y;
        switch (preset){
            case PivotPreset::BOTTOM_LEFT: return Vector2(0.0f, 0.0f);
            case PivotPreset::BOTTOM_RIGHT: return Vector2(w, 0.0f);
            case PivotPreset::TOP_RIGHT: return Vector2(w, h);
            case PivotPreset::TOP_LEFT: return Vector2(0.0f, h);
            case PivotPreset::TOP_CENTER: return Vector2(w / 2.0f, h);
            case PivotPreset::BOTTOM_CENTER: return Vector2(w / 2.0f, 0.0f);
            case PivotPreset::RIGHT_CENTER: return Vector2(w, h / 2.0f);
            case PivotPreset::LEFT_CENTER: return Vector2(0.0f, h / 2.0f);
            case PivotPreset::CENTER: return Vector2(w / 2.0f, h / 2.0f);
            case PivotPreset::FRAME: return Vector2(frame.pivot.x, h - frame.pivot.y);
        }
        return Vector2(0.0f, 0.0f);
    }

    SpriteFrameQuad spriteFrameQuad(const SpriteFrame& frame, const SpriteFrameGeometryInput& input){
        SpriteFrameQuad quad;
        const float w = frame.sourceSize.x;
        const float h = frame.sourceSize.y;
        if (w <= 0.0f || h <= 0.0f) return quad;
        const float ppu = input.pixelsPerUnit > 0.0f ? input.pixelsPerUnit : 1.0f;
        const float ratio = input.pixelRatio.y > 0.0f ? input.pixelRatio.x / input.pixelRatio.y : 1.0f;
        const float sx = input.width > 0 ? static_cast<float>(input.width) / w : ratio / ppu;
        const float sy = input.height > 0 ? static_cast<float>(input.height) / h : 1.0f / ppu;
        quad.width = w * sx;
        quad.height = h * sy;

        const float rx = frame.offset.x;
        const float ry = frame.offset.y;
        const float rw = frame.rect.getWidth();
        const float rh = frame.rect.getHeight();
        quad.empty = rw <= 0.0f || rh <= 0.0f;

        const Vector2 pivot = spriteFramePivot(frame, input.pivot);
        // the trimmed rows' place inside the source frame, along the camera's y (up in the
        // world, down in the UI); a mirroring flip mirrors it about the frame's middle
        const bool upright = input.uiCamera ? !input.flipY : input.flipY;
        float low, pivotY;
        if (!input.uiCamera){
            low = upright ? h - ry - rh : ry;
            pivotY = pivot.y;
        }else{
            low = upright ? ry : h - ry - rh;
            pivotY = h - pivot.y;
        }
        quad.x0 = (rx - pivot.x) * sx;
        quad.x1 = (rx + rw - pivot.x) * sx;
        quad.y0 = (low - pivotY) * sy;
        quad.y1 = (low + rh - pivotY) * sy;

        if (input.pageWidth > 0 && input.pageHeight > 0){
            const float pw = static_cast<float>(input.pageWidth);
            const float ph = static_cast<float>(input.pageHeight);
            quad.textureRect = Rect(frame.rect.getX() / pw, frame.rect.getY() / ph, rw / pw, rh / ph);
        }
        return quad;
    }

}
