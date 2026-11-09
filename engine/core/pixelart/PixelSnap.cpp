// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelSnap.h"

namespace jasperengine::pixelart {

    // each line is the same operation as in includes/pixel_snap.glsl, in its order

    Vector2 toPixels(const Vector4& clip, const SnapGrid& grid){
        const SnapTerms terms = snapTerms(grid);
        const float ndcX = clip.x / clip.w * 1.0f;
        const float ndcY = clip.y / clip.w * grid.flipY;
        return Vector2(ndcX * terms.halfWidth + terms.centerX, ndcY * terms.halfHeight + terms.centerY);
    }

    Vector4 fromPixels(const Vector2& pixels, const Vector4& clip, const SnapGrid& grid){
        const SnapTerms terms = snapTerms(grid);
        const float ndcX = (pixels.x - terms.centerX) / terms.halfWidth * 1.0f;
        const float ndcY = (pixels.y - terms.centerY) / terms.halfHeight * grid.flipY;
        return Vector4(ndcX * clip.w, ndcY * clip.w, clip.z, clip.w);
    }

    Vector4 snapClip(const Vector4& clip, const SnapGrid& grid){
        const Vector2 p = toPixels(clip, grid);
        return fromPixels(Vector2(snap(p.x), snap(p.y)), clip, grid);
    }

    Vector4 snapCenterDelta(const Vector4& clipCenter, const SnapGrid& grid){
        const Vector2 p = toPixels(clipCenter, grid);
        const Vector4 snapped = fromPixels(Vector2(snap(p.x), snap(p.y)), clipCenter, grid);
        return Vector4(snapped.x - clipCenter.x, snapped.y - clipCenter.y, snapped.z - clipCenter.z, snapped.w - clipCenter.w);
    }

    bool isGridAligned(const Matrix4& mvp, const SnapGrid& grid){
        // a projective draw (its w depends on x or y) has no fixed pixel size
        if (mvp[0][3] != 0.0f || mvp[1][3] != 0.0f || mvp[3][3] == 0.0f)
            return false;
        // the 2D linear part in pixels: d(px, py) / d(x, y)
        const float sx = 0.5f * grid.width / mvp[3][3];
        const float sy = 0.5f * grid.height * grid.flipY / mvp[3][3];
        const float a00 = std::fabs(mvp[0][0] * sx);
        const float a01 = std::fabs(mvp[1][0] * sx);
        const float a10 = std::fabs(mvp[0][1] * sy);
        const float a11 = std::fabs(mvp[1][1] * sy);
        const float diagonal = a00 + a11;
        const float cross = a01 + a10;
        constexpr float tolerance = 1e-4f;
        if (diagonal > 0.0f && a00 > 0.0f && a11 > 0.0f && cross <= tolerance * diagonal)
            return true;
        return cross > 0.0f && a01 > 0.0f && a10 > 0.0f && diagonal <= tolerance * cross;
    }

    void exactGridZeros(Matrix4& m){
        const float a00 = std::fabs(m[0][0]), a01 = std::fabs(m[1][0]);
        const float a10 = std::fabs(m[0][1]), a11 = std::fabs(m[1][1]);
        constexpr float tolerance = 1e-6f;
        if (a00 > 0.0f && a11 > 0.0f && a01 + a10 <= tolerance * (a00 + a11)){
            m[1][0] = 0.0f;
            m[0][1] = 0.0f;
        }else if (a01 > 0.0f && a10 > 0.0f && a00 + a11 <= tolerance * (a01 + a10)){
            m[0][0] = 0.0f;
            m[1][1] = 0.0f;
        }
    }

    float snapWorld(float world, int pixelsPerUnit){
        const float ppu = (float)(pixelsPerUnit > 0 ? pixelsPerUnit : 1);
        return snap(world * ppu) / ppu;
    }

    Vector3 snapWorld(const Vector3& world, int pixelsPerUnit){
        return Vector3(snapWorld(world.x, pixelsPerUnit), snapWorld(world.y, pixelsPerUnit), world.z);
    }

}
