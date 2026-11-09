// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The shape rules port pixelfx's drawing code (src/core/sim.js), MIT License, Copyright (c) 2026
// Patch, which adapts Sprite Fusion's pixel art VFX, MIT License, Copyright (c) 2026 Hugo Duprez.
// See NOTICE.md.

#include "ParticlePixel.h"

#include "particles/ParticleProgram.h"
#include "pixelart/PixelSnap.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr float kTau = 6.28318530718f;

    // a disc of radius r around (cx, cy) holds the point (qx, qy) (no division, no root)
    bool inDisc(float qx, float qy, float cx, float cy, float r){
        const float dx = qx - cx, dy = qy - cy;
        return dx * dx + dy * dy <= r * r;
    }

}

PixelShapeSpec PixelShapeSpec::fromRenderer(const RendererModule& r){
    PixelShapeSpec s;
    s.shape = r.renderMode == RenderMode::Pixel ? (int)r.pixelShape : -1;
    s.streakLengthScale = std::max(r.streakLengthScale, 0.0f);
    s.streakMaxLength = std::clamp(r.streakMaxLength, 1.0f, 64.0f);
    s.streakHeadFraction = std::clamp(r.streakHeadFraction, 0.0f, 1.0f);
    s.streakWidth = std::clamp(r.streakWidth, 1, 3);
    s.ringWidth = std::clamp(r.ringWidth, 1, 8);
    s.glowBands = std::clamp(r.glowBands, 1, 4);
    s.rotationSteps = std::clamp(r.rotationSteps, 1, 360);
    s.discRim = r.discRim;
    s.dotShadeBottom = r.dotShadeBottom;
    return s;
}

PixelShapeSpec PixelShapeSpec::fromParams(const PixelParams& p){
    PixelShapeSpec s;
    s.shape = p.shapes ? (int)p.shape : -1;
    s.streakLengthScale = p.streakLengthScale;
    s.streakMaxLength = p.streakMaxLength;
    s.streakHeadFraction = p.streakHeadFraction;
    s.streakWidth = (int)p.streakWidth;
    s.ringWidth = (int)p.ringWidth;
    s.glowBands = (int)p.glowBands;
    s.rotationSteps = (int)p.rotationSteps;
    s.discRim = p.discRim != 0;
    s.dotShadeBottom = p.dotShadeBottom != 0;
    return s;
}

Vector2 ParticlePixel::drawnSize(Vector2 size, bool integerSize){
    Vector2 n(std::fabs(size.x), std::fabs(size.y));
    if (integerSize)
        return Vector2(std::max(1.0f, std::floor(n.x + 0.5f)), std::max(1.0f, std::floor(n.y + 0.5f)));
    return Vector2(std::max(1.0f, n.x), std::max(1.0f, n.y));
}

int ParticlePixel::floorDiv(int a, int b){
    const int q = a / b;
    return (q * b > a) ? q - 1 : q;
}

Vector2 ParticlePixel::axis(float angle){
    return Vector2(std::floor(std::cos(angle) * 65536.0f + 0.5f) / 65536.0f, std::floor(std::sin(angle) * 65536.0f + 0.5f) / 65536.0f);
}

bool ParticlePixel::onLine(int px, int py, int hx, int hy, int tx, int ty, int width, int& along, int& steps){
    const int dx = tx - hx, dy = ty - hy;
    const bool xMajor = std::abs(dx) >= std::abs(dy);
    const int major = xMajor ? dx : dy;
    const int minor = xMajor ? dy : dx;
    const int pm = xMajor ? px - hx : py - hy;
    const int pn = xMajor ? py - hy : px - hx;
    steps = std::abs(major);
    const int s = major < 0 ? -1 : 1;
    along = pm * s;
    if (along < 0 || along > steps)
        return false;
    const int expected = steps == 0 ? 0 : floorDiv(2 * along * minor + steps, 2 * steps);
    const int lo = -((width - 1) / 2);
    const int hi = width / 2;
    return pn - expected >= lo && pn - expected <= hi;
}

PixelLayout ParticlePixel::layout(const PixelShapeSpec& spec, Vector2 centre, Vector2 n, Vector2 velocityPx, float rotation, bool snap){
    PixelLayout out;
    out.n = n.x;
    auto corner = [&](float boxW, float boxH){
        const float cx = centre.x - boxW * 0.5f, cy = centre.y - boxH * 0.5f;
        out.boxW = boxW;
        out.boxH = boxH;
        out.cornerX = snap ? pixelart::snap(cx) : cx;
        out.cornerY = snap ? pixelart::snap(cy) : cy;
    };
    const int shape = spec.shape;
    if (shape == (int)PixelShape::Streak){
        const float speed = std::sqrt(velocityPx.x * velocityPx.x + velocityPx.y * velocityPx.y);
        const float len = std::clamp(1.0f + speed * spec.streakLengthScale, 1.0f, spec.streakMaxLength);
        const float dirX = speed > 1e-6f ? velocityPx.x / speed : 0.0f;
        const float dirY = speed > 1e-6f ? velocityPx.y / speed : 0.0f;
        const float dx = std::floor(-dirX * (len - 1.0f) + 0.5f);
        const float dy = std::floor(-dirY * (len - 1.0f) + 0.5f);
        const float headX = snap ? pixelart::snap(centre.x - 0.5f) : centre.x - 0.5f;
        const float headY = snap ? pixelart::snap(centre.y - 0.5f) : centre.y - 0.5f;
        const int width = spec.streakWidth;
        const bool xMajor = std::fabs(dx) >= std::fabs(dy);
        const float low = (float)((width - 1) / 2);
        out.boxW = std::fabs(dx) + 1.0f + (xMajor ? 0.0f : (float)(width - 1));
        out.boxH = std::fabs(dy) + 1.0f + (xMajor ? (float)(width - 1) : 0.0f);
        out.cornerX = headX + std::min(dx, 0.0f) - (xMajor ? 0.0f : low);
        out.cornerY = headY + std::min(dy, 0.0f) - (xMajor ? low : 0.0f);
        out.headX = (int)(headX - out.cornerX);
        out.headY = (int)(headY - out.cornerY);
        out.tailX = (int)(headX + dx - out.cornerX);
        out.tailY = (int)(headY + dy - out.cornerY);
        return out;
    }
    const float size = n.x;
    switch (shape){
        case (int)PixelShape::Dot: corner(std::ceil(n.x), std::ceil(n.y)); break;
        case (int)PixelShape::Cross: {
            const float odd = std::floor(size) + 1.0f - std::fmod(std::floor(size), 2.0f);
            corner(odd, odd);
            break;
        }
        case (int)PixelShape::Puff: corner(std::ceil(1.6f * size), std::ceil(1.6f * size)); break;
        case (int)PixelShape::Shard: corner(std::ceil(size) + 2.0f, std::ceil(size) + 2.0f); break;
        case (int)PixelShape::Chunk: corner(std::ceil(1.3f * size) + 2.0f, std::ceil(1.3f * size) + 2.0f); break;
        default:
            if (shape > 0)
                corner(std::ceil(size), std::ceil(size));
            else
                corner(n.x, n.y);
            break;
    }
    if (shape == (int)PixelShape::Shard || shape == (int)PixelShape::Chunk){
        const float steps = (float)spec.rotationSteps;
        out.angle = -std::floor(rotation / kTau * steps + 0.5f) * kTau / steps;
        if (shape == (int)PixelShape::Shard){
            const Vector2 dir = axis(out.angle);
            const float halfLength = (size - 1.0f) * 0.5f;
            const float cx = out.boxW * 0.5f, cy = out.boxH * 0.5f;
            out.headX = (int)std::floor(cx - dir.x * halfLength);
            out.headY = (int)std::floor(cy - dir.y * halfLength);
            out.tailX = (int)std::floor(cx + dir.x * halfLength);
            out.tailY = (int)std::floor(cy + dir.y * halfLength);
        }
    }
    return out;
}

PixelSample ParticlePixel::sample(const PixelShapeSpec& spec, const PixelLayout& l, int x, int y, float age, int rampCount){
    PixelSample r;
    const int last = rampCount - 1;
    const float n = l.n;
    const float qx = (float)x + 0.5f, qy = (float)y + 0.5f;
    switch (spec.shape){
        case (int)PixelShape::Dot:
            r.drawn = true;
            if (spec.dotShadeBottom && y == 0 && n >= 2.0f)
                r.tone = 1;
            return r;
        case (int)PixelShape::Cross: {
            const int size = (int)l.boxW;
            const int c = (size - 1) / 2;
            const int a = c;
            const int dx = std::abs(x - c), dy = std::abs(y - c);
            if (dx == 0 && dy == 0){
                r.drawn = true;
                r.tone = rampCount > 0 ? 2 + last : 0;
            }else if (dx == 0 || dy == 0){
                r.drawn = true;
                r.tone = (float)std::max(dx, dy) > 0.6f * (float)a ? 1 : 0;
            }else if (a >= 3 && dx == 1 && dy == 1){
                r.drawn = true;
            }else if (a >= 6 && dx == 2 && dy == 2){
                r.drawn = true;
                r.tone = 1;
            }
            return r;
        }
        case (int)PixelShape::Disc:
        case (int)PixelShape::Ring:
        case (int)PixelShape::Glow: {
            const float rr = n * 0.5f;
            const float cx = l.boxW * 0.5f, cy = l.boxH * 0.5f;
            if (!inDisc(qx, qy, cx, cy, rr))
                return r;
            if (spec.shape == (int)PixelShape::Ring){
                const float inner = rr - (float)spec.ringWidth;
                r.drawn = inner <= 0.0f || !inDisc(qx, qy, cx, cy, inner);
                return r;
            }
            r.drawn = true;
            if (spec.shape == (int)PixelShape::Glow){
                const int bands = spec.glowBands;
                const float dx = qx - cx, dy = qy - cy;
                const float d2 = (dx * dx + dy * dy) * (float)(bands * bands);
                int band = bands - 1;
                for (int i = 0; i < 4 && i < bands; i++){
                    const float edge = rr * (float)(i + 1);
                    if (d2 <= edge * edge){
                        band = i;
                        break;
                    }
                }
                r.cover = 1.0f - (float)band / (float)bands;
                return r;
            }
            if (spec.discRim && (!inDisc(qx + 1.0f, qy, cx, cy, rr) || !inDisc(qx - 1.0f, qy, cx, cy, rr) ||
                                 !inDisc(qx, qy + 1.0f, cx, cy, rr) || !inDisc(qx, qy - 1.0f, cx, cy, rr)))
                r.tone = 1;
            return r;
        }
        case (int)PixelShape::Streak:
        case (int)PixelShape::Shard: {
            int along = 0, steps = 0;
            const int width = spec.shape == (int)PixelShape::Streak ? spec.streakWidth : 1;
            if (!onLine(x, y, l.headX, l.headY, l.tailX, l.tailY, width, along, steps))
                return r;
            r.drawn = true;
            if (spec.shape == (int)PixelShape::Streak)
                r.tone = along <= (int)std::floor(spec.streakHeadFraction * (float)steps) ? 0 : 1;
            else
                r.tone = along == steps ? 1 : 0;
            return r;
        }
        case (int)PixelShape::Puff: {
            const float rr = n * 0.5f;
            const float cx = l.boxW * 0.5f, cy = l.boxH * 0.5f;
            const bool body = inDisc(qx, qy, cx, cy, rr);
            const bool core = inDisc(qx, qy, cx + 0.05f * rr, cy + -0.06f * rr, 0.74f * rr);
            const bool cap = age < 0.5f && inDisc(qx, qy, cx + -0.22f * rr, cy + 0.3f * rr, 0.5f * rr);
            if (!body && !core && !cap)
                return r;
            r.drawn = true;
            const float under = core ? 0.62f : (body ? 0.45f : 0.0f);
            if (cap){
                r.tone = 1;
                r.cover = 0.4f * (1.0f - 2.0f * age);
                r.tone2 = under > 0.0f ? 0 : -1;
                r.cover2 = under;
            }else{
                r.cover = under;
            }
            return r;
        }
        case (int)PixelShape::Chunk: {
            const float s = n * 0.5f;
            const float dx = qx - l.boxW * 0.5f, dy = qy - l.boxH * 0.5f;
            const Vector2 a = axis(l.angle);
            const float u = dx * a.x + dy * a.y;
            const float v = -dx * a.y + dy * a.x;
            const float m = 0.62f * std::fabs(u) + std::fabs(v);
            if (m > 0.806f * s)
                return r;
            r.drawn = true;
            const bool rim = m > 0.62f * s;
            if (rampCount > 0){
                if (rim)
                    r.tone = 2;
                else if (v > 0.0f)
                    r.tone = 2 + (u < 0.0f ? last : std::max(last - 1, 0));
                else
                    r.tone = 2 + (u < 0.0f ? std::max(last - 2, 0) : std::max(std::min(1, last), last - 3));
            }else{
                r.tone = rim ? 1 : 0;
            }
            return r;
        }
        default:
            r.drawn = true;
            return r;
    }
}

void ParticlePixel::rasterize(const PixelShapeSpec& spec, const PixelLayout& l, float age, int rampCount, std::vector<PixelCell>& out){
    const int w = (int)std::ceil(l.boxW), h = (int)std::ceil(l.boxH);
    const int ox = (int)std::floor(l.cornerX), oy = (int)std::floor(l.cornerY);
    for (int y = 0; y < h; y++){
        for (int x = 0; x < w; x++){
            const PixelSample s = sample(spec, l, x, y, age, rampCount);
            if (!s.drawn || (s.cover <= 0.0f && s.cover2 <= 0.0f))
                continue;
            PixelCell c;
            c.x = ox + x;
            c.y = oy + y;
            c.tone = s.cover > 0.0f ? s.tone : s.tone2;
            c.cover = s.cover > 0.0f ? s.cover : s.cover2;
            out.push_back(c);
        }
    }
}
