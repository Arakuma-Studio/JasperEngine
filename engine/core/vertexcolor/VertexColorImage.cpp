// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R33, R41: bilinear lookups into the images ops carry (stencils,
// from_texture sources)

#include "vertexcolor/VertexColorOps.h"

#include <cmath>

namespace jasperengine::vertexcolor {

    namespace {

        // texel coordinate of `t` in [0, size): pixel centres at (i + 0.5) / size, v = 0 is row 0
        void imageTexels(float t, int size, bool wrap, int& i0, int& i1, float& fraction) {
            if (!std::isfinite(t)) t = 0.0f;
            if (wrap) t -= std::floor(t);
            float x = t * (float)size - 0.5f;
            if (!wrap) x = std::fmin(std::fmax(x, 0.0f), (float)(size - 1));
            const float base = std::floor(x);
            fraction = x - base;
            i0 = (int)base;
            i1 = i0 + 1;
            if (wrap) {
                i0 = ((i0 % size) + size) % size;
                i1 = i1 % size;
            } else {
                if (i1 > size - 1) i1 = size - 1;
            }
        }

    }

    Vector4 Image::sample(float u, float v) const {
        if (width <= 0 || height <= 0 || pixels.size() != (size_t)width * (size_t)height) return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        int x0, x1, y0, y1;
        float fx, fy;
        imageTexels(u, width, wrap, x0, x1, fx);
        imageTexels(v, height, wrap, y0, y1, fy);
        const Vector4& a = pixels[(size_t)y0 * (size_t)width + (size_t)x0];
        const Vector4& b = pixels[(size_t)y0 * (size_t)width + (size_t)x1];
        const Vector4& c = pixels[(size_t)y1 * (size_t)width + (size_t)x0];
        const Vector4& d = pixels[(size_t)y1 * (size_t)width + (size_t)x1];
        const Vector4 top = a + (b - a) * fx;
        const Vector4 bottom = c + (d - c) * fx;
        return top + (bottom - top) * fy;
    }

}
