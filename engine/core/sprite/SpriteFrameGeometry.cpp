// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteFrameGeometry.h"

#include <algorithm>
#include <cmath>

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

    Rect spriteFrameLocalRect(const SpriteFrame& frame, const SpriteFrameGeometryInput& input, const Rect& pixels){
        // the quad of a frame trimmed to exactly `pixels` is that rect's place
        SpriteFrame cut = frame;
        cut.offset = Vector2(pixels.getX(), pixels.getY());
        cut.rect = Rect(0.0f, 0.0f, pixels.getWidth(), pixels.getHeight());
        SpriteFrameGeometryInput in = input;
        in.pageWidth = 0;
        in.pageHeight = 0;
        const SpriteFrameQuad quad = spriteFrameQuad(cut, in);
        return Rect(std::min(quad.x0, quad.x1), std::min(quad.y0, quad.y1), std::abs(quad.x1 - quad.x0), std::abs(quad.y1 - quad.y0));
    }

    bool spriteSliceLocalRect(const SpriteComponent& sprite, const SpriteSheetData& sheet, bool uiCamera,
                              std::string_view name, Rect& bounds, Rect* center, Vector2* pivot){
        const int index = sheet.findSlice(name);
        if (index < 0 || sheet.frames.empty()) return false;
        const int frameIndex = std::clamp(sprite.frame, 0, sheet.frameCount() - 1);
        const SpriteSliceKey* key = sheet.slices[static_cast<size_t>(index)].keyAt(frameIndex);
        if (!key) return false;
        const SpriteFrame& frame = sheet.frames[static_cast<size_t>(frameIndex)];
        SpriteFrameGeometryInput input;
        input.width = sprite.width;
        input.height = sprite.height;
        input.pivot = sprite.pivotPreset;
        // as the quad (MeshSystem): the Sprite's own Pixels Per Unit over its sheet's
        input.pixelsPerUnit = sprite.pixelsPerUnit > 0.0f ? sprite.pixelsPerUnit : sheet.pixelsPerUnit;
        input.pixelRatio = sheet.pixelRatio;
        input.flipY = sprite.flipY;
        input.uiCamera = uiCamera;
        bounds = spriteFrameLocalRect(frame, input, key->bounds);
        if (center){
            *center = key->hasCenter ? spriteFrameLocalRect(frame, input, Rect(key->bounds.getX() + key->center.getX(), key->bounds.getY() + key->center.getY(),
                                                                              key->center.getWidth(), key->center.getHeight()))
                                     : Rect(0.0f, 0.0f, 0.0f, 0.0f);
        }
        if (pivot){
            const Vector2 at = key->hasPivot ? Vector2(key->bounds.getX() + key->pivot.x, key->bounds.getY() + key->pivot.y)
                                             : Vector2(key->bounds.getX(), key->bounds.getY());
            const Rect point = spriteFrameLocalRect(frame, input, Rect(at.x, at.y, 0.0f, 0.0f));
            *pivot = Vector2(point.getX(), point.getY());
        }
        return true;
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


    bool spriteFrameHasBorder(const SpriteFrame& frame){
        return frame.border.x > 0.0f || frame.border.y > 0.0f || frame.border.z > 0.0f || frame.border.w > 0.0f;
    }

    namespace{
        // source pixels along one axis -> output units: [0, a] keeps its native size (au), [n - b, n]
        // too (bu), the middle stretches over what is left
        struct SliceAxis{
            float n = 0, a = 0, b = 0;      // source size and borders, pixels
            float out = 0, au = 0, bu = 0;  // output size and borders, units

            float map(float p) const{
                if (p <= a) return a > 0.0f ? p / a * au : 0.0f;
                if (p >= n - b) return b > 0.0f ? out - (n - p) / b * bu : out;
                const float middle = n - a - b;
                return middle > 0.0f ? au + (p - a) / middle * (out - au - bu) : au;
            }
        };

        SliceAxis sliceAxis(float n, float a, float b, float out, float unitsPerPixel){
            SliceAxis axis;
            axis.n = n;
            axis.a = std::clamp(a, 0.0f, n);
            axis.b = std::clamp(b, 0.0f, n - axis.a);
            axis.out = out;
            axis.au = axis.a * unitsPerPixel;
            axis.bu = axis.b * unitsPerPixel;
            // a sprite smaller than its two borders: the corners shrink together
            if (axis.au + axis.bu > out && axis.au + axis.bu > 0.0f){
                const float k = out / (axis.au + axis.bu);
                axis.au *= k;
                axis.bu *= k;
            }
            return axis;
        }
    }

    SpriteSlicedMesh spriteFrameSliced(const SpriteFrame& frame, const SpriteFrameGeometryInput& input){
        SpriteSlicedMesh mesh;
        const float w = frame.sourceSize.x;
        const float h = frame.sourceSize.y;
        if (w <= 0.0f || h <= 0.0f) return mesh;
        const float ppu = input.pixelsPerUnit > 0.0f ? input.pixelsPerUnit : 1.0f;
        const float ratio = input.pixelRatio.y > 0.0f ? input.pixelRatio.x / input.pixelRatio.y : 1.0f;
        const float ux = ratio / ppu;
        const float uy = 1.0f / ppu;
        mesh.width = input.width > 0 ? static_cast<float>(input.width) : w * ux;
        mesh.height = input.height > 0 ? static_cast<float>(input.height) : h * uy;
        const SliceAxis ax = sliceAxis(w, frame.border.x, frame.border.z, mesh.width, ux);
        const SliceAxis ay = sliceAxis(h, frame.border.y, frame.border.w, mesh.height, uy);

        const float ox = frame.offset.x, oy = frame.offset.y;
        const float rw = frame.rect.getWidth(), rh = frame.rect.getHeight();
        mesh.empty = rw <= 0.0f || rh <= 0.0f;
        if (mesh.empty) return mesh;
        // the lattice lines in source pixels (y down), clamped to the trimmed rect
        const float cols[4] = {ox, std::clamp(ax.a, ox, ox + rw), std::clamp(w - ax.b, ox, ox + rw), ox + rw};
        const float rows[4] = {oy, std::clamp(ay.a, oy, oy + rh), std::clamp(h - ay.b, oy, oy + rh), oy + rh};

        // the pivot in output units, y up from the bottom: presets on the drawn size, the frame's
        // own pivot through the slicing
        const Vector2 sourcePivot = spriteFramePivot(frame, input.pivot);
        Vector2 pivot;
        if (input.pivot == PivotPreset::FRAME){
            pivot = Vector2(ax.map(sourcePivot.x), mesh.height - ay.map(h - sourcePivot.y));
        }else{
            pivot = Vector2(sourcePivot.x / w * mesh.width, sourcePivot.y / h * mesh.height);
        }
        const bool upright = input.uiCamera ? !input.flipY : input.flipY;
        float ys[4];
        for (int i = 0; i < 4; i++){
            const float down = ay.map(rows[i]);   // from the drawn top
            if (!input.uiCamera){
                ys[i] = (upright ? mesh.height - down : down) - pivot.y;
            }else{
                ys[i] = (upright ? down : mesh.height - down) - (mesh.height - pivot.y);
            }
            mesh.v[i] = (rows[i] - oy) / rh;
        }
        for (int i = 0; i < 4; i++){
            mesh.x[i] = ax.map(cols[i]) - pivot.x;
            mesh.u[i] = (cols[i] - ox) / rw;
        }
        // rows bottom-up in y, so the cells wind as the quad's
        const bool reversed = ys[0] > ys[3];
        for (int i = 0; i < 4; i++){
            const int from = reversed ? 3 - i : i;
            mesh.y[i] = ys[from];
        }
        if (reversed){
            std::swap(mesh.v[0], mesh.v[3]);
            std::swap(mesh.v[1], mesh.v[2]);
        }
        if (input.pageWidth > 0 && input.pageHeight > 0){
            const float pw = static_cast<float>(input.pageWidth);
            const float ph = static_cast<float>(input.pageHeight);
            mesh.textureRect = Rect(frame.rect.getX() / pw, frame.rect.getY() / ph, rw / pw, rh / ph);
        }
        return mesh;
    }

    const uint16_t* spriteSlicedIndices(){
        static const uint16_t indices[SPRITE_SLICED_INDICES] = {
            0, 1, 5, 0, 5, 4,      1, 2, 6, 1, 6, 5,      2, 3, 7, 2, 7, 6,
            4, 5, 9, 4, 9, 8,      5, 6, 10, 5, 10, 9,    6, 7, 11, 6, 11, 10,
            8, 9, 13, 8, 13, 12,   9, 10, 14, 9, 14, 13,  10, 11, 15, 10, 15, 14,
        };
        return indices;
    }

}
