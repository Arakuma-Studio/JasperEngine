// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelFxKey.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::pixelart {

    namespace {
        // PCG2D (Jarzynski and Olano 2020), as pfxPcg2d in pixel_fx.glsl
        void pcg2d(uint32_t& x, uint32_t& y){
            x = x * 1664525u + 1013904223u;
            y = y * 1664525u + 1013904223u;
            x += y * 1664525u;
            y += x * 1664525u;
            x ^= x >> 16u;
            y ^= y >> 16u;
            x += y * 1664525u;
            y += x * 1664525u;
            x ^= x >> 16u;
            y ^= y >> 16u;
        }
    }

    uint32_t pfxHash(uint32_t x, uint32_t y, uint32_t seed){
        uint32_t a = x ^ seed;
        uint32_t b = y ^ (seed * 2654435769u);
        pcg2d(a, b);
        return a;
    }

    float pfxHash01(uint32_t x, uint32_t y, uint32_t seed){
        return (float)(pfxHash(x, y, seed) >> 8u) * (1.0f / 16777216.0f);
    }

    float pfxValueNoise(float x, float y, float cell, uint32_t seed){
        const float c = std::max(cell, 1.0f);
        const float qx = x / c, qy = y / c;
        const float ix = std::floor(qx), iy = std::floor(qy);
        const float fx = qx - ix, fy = qy - iy;
        const uint32_t cx = (uint32_t)ix, cy = (uint32_t)iy;
        const float a = pfxHash01(cx, cy, seed);
        const float b = pfxHash01(cx + 1u, cy, seed);
        const float d = pfxHash01(cx, cy + 1u, seed);
        const float e = pfxHash01(cx + 1u, cy + 1u, seed);
        const float lo = a + (b - a) * fx;
        const float hi = d + (e - d) * fx;
        return lo + (hi - lo) * fy;
    }

    float pfxValueNoiseGrad(float x, float y, float cell, uint32_t seed, float& gradX, float& gradY){
        const float size = std::max(cell, 1.0f);
        const float qx = x / size, qy = y / size;
        const float ix = std::floor(qx), iy = std::floor(qy);
        const float fx = qx - ix, fy = qy - iy;
        const uint32_t cx = (uint32_t)ix, cy = (uint32_t)iy;
        const float a = pfxHash01(cx, cy, seed);
        const float b = pfxHash01(cx + 1u, cy, seed);
        const float d = pfxHash01(cx, cy + 1u, seed);
        const float e = pfxHash01(cx + 1u, cy + 1u, seed);
        const float lo = a + (b - a) * fx;
        const float hi = d + (e - d) * fx;
        gradX = ((b - a) + ((e - d) - (b - a)) * fy) / size;
        gradY = (hi - lo) / size;
        return lo + (hi - lo) * fy;
    }

    namespace {
        struct DissolveSmooth { float value, gradX, gradY, range, extent; };

        // pfxDissolveSmooth
        DissolveSmooth dissolveSmooth(float px, float py, float sizeX, float sizeY, const PfxDissolve& d){
            DissolveSmooth out{};
            float ngx = 0.0f, ngy = 0.0f;
            const float n = pfxValueNoiseGrad(px + 1024.0f, py + 1024.0f, d.cell, d.seed, ngx, ngy);
            const float len = std::sqrt(d.dirX * d.dirX + d.dirY * d.dirY);
            if (len > 1e-6f){
                const float dx = d.dirX / len, dy = d.dirY / len;
                const float lo = std::min(0.0f, dx * sizeX) + std::min(0.0f, dy * sizeY);
                const float hi = std::max(0.0f, dx * sizeX) + std::max(0.0f, dy * sizeY);
                out.extent = std::max(hi - lo, 1.0f);
                const float amount = 1.5f * std::min(std::max(d.noise, 0.0f), 1.0f) * std::max(d.cell, 1.0f) / out.extent;
                out.range = 1.0f + amount;
                out.gradX = dx / out.extent + ngx * amount;
                out.gradY = dy / out.extent + ngy * amount;
                out.value = (px * dx + py * dy - lo) / out.extent + amount * n;
                return out;
            }
            out.extent = std::max(d.cell, 1.0f);
            out.range = 1.0f;
            out.gradX = ngx;
            out.gradY = ngy;
            out.value = n;
            return out;
        }

        // pfxDissolveJitter
        float dissolveJitter(int x, int y, const PfxDissolve& d){
            const uint32_t seed = d.seed ^ 1759714724u;
            const float len = std::sqrt(d.dirX * d.dirX + d.dirY * d.dirY);
            if (len > 1e-6f){
                const float dx = d.dirX / len, dy = d.dirY / len;
                const float line = std::floor(((float)x + 0.5f) * -dy + ((float)y + 0.5f) * dx + 1024.0f);
                return pfxHash01((uint32_t)line, 0u, seed);
            }
            return pfxHash01((uint32_t)(x + 1024), (uint32_t)(y + 1024), seed);
        }
    }

    float pfxDissolveKey(int x, int y, int frameWidth, int frameHeight, const PfxDissolve& dissolve){
        const DissolveSmooth s = dissolveSmooth((float)x + 0.5f, (float)y + 0.5f, (float)std::max(frameWidth, 1),
                                                (float)std::max(frameHeight, 1), dissolve);
        const float j = std::max(dissolve.jitter, 0.0f);
        return (s.value + j * dissolveJitter(x, y, dissolve)) / (s.range + j);
    }

    float pfxDissolveFront(float progress, float span){
        return progress * (1.0f + span) - span;
    }

    float pfxDissolveTexel(int x, int y, int frameWidth, int frameHeight, const PfxDissolve& dissolve, float progress){
        if (progress <= 0.0f)
            return 1e9f;
        if (progress >= 1.0f)
            return -1e9f;
        const float edge = std::floor(std::max(dissolve.edgeWidth, 0.0f) + 0.5f);
        const float scorch = std::max(dissolve.scorchWidth, 0.0f);
        const float sizeX = (float)std::max(frameWidth, 1), sizeY = (float)std::max(frameHeight, 1);
        const float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
        const DissolveSmooth s = dissolveSmooth(cx, cy, sizeX, sizeY, dissolve);
        const float j = std::max(dissolve.jitter, 0.0f);
        const float scale = 1.0f / (s.range + j);
        const float shift = j * dissolveJitter(x, y, dissolve);
        const float key = (s.value + shift) * scale;
        const float nominal = scale / s.extent;
        const float front = pfxDissolveFront(progress, (edge + scorch) * nominal);
        if (key < front)
            return -1.0f;
        const float len = std::sqrt(dissolve.dirX * dissolve.dirX + dissolve.dirY * dissolve.dirY);
        if (len <= 1e-6f){
            const float gradient = std::sqrt(s.gradX * s.gradX + s.gradY * s.gradY) * scale;
            return (key - front) / std::min(std::max(gradient, 0.45f * nominal), 3.0f * nominal);
        }
        const float dx = dissolve.dirX / len, dy = dissolve.dirY / len;
        if (edge > 0.0f){
            const float back = (dissolveSmooth(cx - dx * edge, cy - dy * edge, sizeX, sizeY, dissolve).value + shift) * scale;
            if (back < front)
                return std::min(std::max((key - front) / std::max(key - back, 1e-6f) * edge, 0.0f), edge - 0.001f);
        }
        if (scorch > 0.0f){
            const float back = (dissolveSmooth(cx - dx * (edge + scorch), cy - dy * (edge + scorch), sizeX, sizeY, dissolve).value + shift) * scale;
            if (back < front)
                return edge;
        }
        return edge + scorch + 1.0f;
    }

    float pfxDissolveBurnFront(int frameWidth, int frameHeight, const PfxDissolve& dissolve, float progress){
        if (progress <= 0.0f)
            return -1e9f;
        if (progress >= 1.0f)
            return 1e9f;
        const float edge = std::floor(std::max(dissolve.edgeWidth, 0.0f) + 0.5f);
        const float scorch = std::max(dissolve.scorchWidth, 0.0f);
        // the range and extent are the frame's, the same at every texel
        const DissolveSmooth s = dissolveSmooth(0.5f, 0.5f, (float)std::max(frameWidth, 1), (float)std::max(frameHeight, 1), dissolve);
        const float scale = 1.0f / (s.range + std::max(dissolve.jitter, 0.0f));
        return pfxDissolveFront(progress, (edge + scorch) * scale / s.extent);
    }

}
