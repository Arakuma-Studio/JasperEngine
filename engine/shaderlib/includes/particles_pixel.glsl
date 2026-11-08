// Pixel particles (docs/specs/S49-pixel-particles.md R4, R6, R9, R11): the fragment part of the
// PARTICLE_PIXEL variant ('Pxl'). The vertex stage (includes/particles_vs.glsl) puts each
// particle's pixel box on the base-pixel grid with S41's rule and hands the box, the shape's
// line and the particle's data over as flat varyings; this file evaluates the shape per whole
// base pixel, picks its tone (primary, secondary or a ramp step) and fades it with the family's
// one Bayer table (includes/bayer.glsl, S43). The CPU twin, which the tests and the inspector's
// shape icons use, is engine/core/particles/ParticlePixel.cpp: the same rules, the same order.
//
// The shape rules port pixelfx's drawing code (src/core/sim.js: sparkle, the SPARK streak,
// SMOKE's three discs, Bits' SHARD and CHUNK, Lights' glow), MIT License, Copyright (c) 2026
// Patch; sim.js adapts Sprite Fusion's pixel art VFX, MIT License, Copyright (c) 2026 Hugo
// Duprez. See NOTICE.md.
//
// Local pixel coordinates: (0, 0) is the box's lower-left pixel, x right, y up. Tones: 0 the
// particle's colour, 1 the secondary colour, 2 + k step k of the ramp (particlesFsParams.pixelRamp).
// Only integer maths, floor and multiplications reach a comparison, so the CPU and every
// backend agree to the pixel (no texture fetches, no derivatives).

#ifndef PARTICLES_PIXEL_GLSL
#define PARTICLES_PIXEL_GLSL

#include "includes/bayer.glsl"

flat in vec4 v_pixelBox;     // x, y the box (pixels), z n (the size), w the shape's quantised angle (radians, counter-clockwise)
flat in vec4 v_pixelLine;    // Streak and Shard: xy the head (first) pixel, zw the tail (last) pixel, local
flat in vec4 v_pixelData;    // x the ramp step (-1: none), y normalized age, z the particle's alpha (fade over lifetime in), w 1 at rest
in vec2 v_pixelLocal;        // the fragment in the box, pixels
in vec2 v_pixelGrid;         // the fragment on the grid, base pixels (Screen dither)

const int PIXEL_DOT = 0;
const int PIXEL_CROSS = 1;
const int PIXEL_DISC = 2;
const int PIXEL_RING = 3;
const int PIXEL_STREAK = 4;
const int PIXEL_PUFF = 5;
const int PIXEL_SHARD = 6;
const int PIXEL_CHUNK = 7;
const int PIXEL_GLOW = 8;

// a / b rounded down, b > 0 (GLSL's / truncates toward zero)
int pixelFloorDiv(int a, int b){
    int q = a / b;
    return (q * b > a) ? q - 1 : q;
}

// R11: on the pixel-perfect line from h to t (one pixel per step of the major axis, the minor
// offset floor(t * slope + 0.5) in integers), widened to `width` across the minor axis; `along`
// the step from h, `steps` the line's last step
bool pixelOnLine(ivec2 p, ivec2 h, ivec2 t, int width, out int along, out int steps){
    ivec2 d = t - h;
    bool xMajor = abs(d.x) >= abs(d.y);
    int major = xMajor ? d.x : d.y;
    int minor = xMajor ? d.y : d.x;
    int pm = xMajor ? p.x - h.x : p.y - h.y;
    int pn = xMajor ? p.y - h.y : p.x - h.x;
    steps = abs(major);
    int s = major < 0 ? -1 : 1;
    along = pm * s;
    if (along < 0 || along > steps)
        return false;
    int expected = steps == 0 ? 0 : pixelFloorDiv(2 * along * minor + steps, 2 * steps);
    int lo = -((width - 1) / 2);
    int hi = width / 2;
    return pn - expected >= lo && pn - expected <= hi;
}

// particlesFsParams.pixelShape.w: 1 Disc's Rim, 2 Dot's Shade Bottom
int pixelShapeFlags(){
    return int(particlesFsParams.pixelShape.w + 0.5);
}

// a disc of radius r around c holds the pixel's centre q
bool pixelInDisc(vec2 q, vec2 c, float r){
    vec2 d = q - c;
    return d.x * d.x + d.y * d.y <= r * r;
}

// The shape at pixel p: the top tone and coverage, and the tone and coverage under it (Puff's
// cap over its core); false where the shape has no pixel. rampCount 0: no ramp.
bool pixelShape(int shape, ivec2 p, vec2 box, float n, float angle, vec4 pxLine, float age, int rampCount,
                out int tone, out float cover, out int tone2, out float cover2){
    tone = 0;
    cover = 1.0;
    tone2 = -1;
    cover2 = 0.0;
    int last = rampCount - 1;
    vec2 q = vec2(p) + 0.5;
    if (shape == PIXEL_DOT){
        // pixelfx GRIT: the bottom row in the secondary colour from 2 px
        if ((pixelShapeFlags() & 2) != 0 && p.y == 0 && n >= 2.0)
            tone = 1;
        return true;
    }
    if (shape == PIXEL_CROSS){
        // pixelfx sparkle: arms of floor(N / 2), tips beyond 0.6 of the arm, diagonals from 3
        // and 6, the heart the ramp's end
        int size = int(box.x);
        int c = (size - 1) / 2;
        int a = c;
        int dx = abs(p.x - c);
        int dy = abs(p.y - c);
        if (dx == 0 && dy == 0){
            tone = rampCount > 0 ? 2 + last : 0;
            return true;
        }
        if (dx == 0 || dy == 0){
            int d = max(dx, dy);
            tone = float(d) > 0.6 * float(a) ? 1 : 0;
            return true;
        }
        if (a >= 3 && dx == 1 && dy == 1)
            return true;
        if (a >= 6 && dx == 2 && dy == 2){
            tone = 1;
            return true;
        }
        return false;
    }
    if (shape == PIXEL_DISC || shape == PIXEL_RING || shape == PIXEL_GLOW){
        float r = n * 0.5;
        vec2 c = box * 0.5;
        if (!pixelInDisc(q, c, r))
            return false;
        if (shape == PIXEL_RING){
            float inner = r - particlesFsParams.pixelShape.y;
            return inner <= 0.0 || !pixelInDisc(q, c, inner);
        }
        if (shape == PIXEL_GLOW){
            // band i of `bands` concentric rings at coverage 1 - i / bands (pixelfx Lights)
            int bands = int(particlesFsParams.pixelShape.z + 0.5);
            vec2 d = q - c;
            float d2 = (d.x * d.x + d.y * d.y) * float(bands * bands);
            int band = bands - 1;
            for (int i = 0; i < 4; i++){
                if (i >= bands)
                    break;
                float edge = r * float(i + 1);
                if (d2 <= edge * edge){
                    band = i;
                    break;
                }
            }
            cover = 1.0 - float(band) / float(bands);
            return true;
        }
        // Rim: the outer pixel ring (a 4-neighbour outside the disc)
        if ((pixelShapeFlags() & 1) != 0){
            if (!pixelInDisc(q + vec2(1.0, 0.0), c, r) || !pixelInDisc(q - vec2(1.0, 0.0), c, r) ||
                !pixelInDisc(q + vec2(0.0, 1.0), c, r) || !pixelInDisc(q - vec2(0.0, 1.0), c, r))
                tone = 1;
        }
        return true;
    }
    if (shape == PIXEL_STREAK || shape == PIXEL_SHARD){
        int along, steps;
        int width = shape == PIXEL_STREAK ? int(particlesFsParams.pixelStreak.y + 0.5) : 1;
        if (!pixelOnLine(p, ivec2(pxLine.xy), ivec2(pxLine.zw), width, along, steps))
            return false;
        if (shape == PIXEL_STREAK){
            // the first headFraction of the length from the head is primary (pixelfx's spark split)
            tone = along <= int(floor(particlesFsParams.pixelStreak.x * float(steps))) ? 0 : 1;
        }else{
            // the shard's tip: its last pixel (pixelfx's white tip)
            tone = along == steps ? 1 : 0;
        }
        return true;
    }
    if (shape == PIXEL_PUFF){
        // pixelfx SMOKE: a body, a denser core sitting low, a lit cap while young (y up)
        float r = n * 0.5;
        vec2 c = box * 0.5;
        bool body = pixelInDisc(q, c, r);
        bool core = pixelInDisc(q, c + vec2(0.05 * r, -0.06 * r), 0.74 * r);
        bool cap = age < 0.5 && pixelInDisc(q, c + vec2(-0.22 * r, 0.3 * r), 0.5 * r);
        if (!body && !core && !cap)
            return false;
        float under = core ? 0.62 : (body ? 0.45 : 0.0);
        if (cap){
            tone = 1;
            cover = 0.4 * (1.0 - 2.0 * age);
            tone2 = under > 0.0 ? 0 : -1;
            cover2 = under;
        }else{
            cover = under;
        }
        return true;
    }
    if (shape == PIXEL_CHUNK){
        // pixelfx CHUNK: |u| / s + |v| / (0.62 s) <= 1.3, a rim past 1, quadrants lit top-left;
        // compared without a division (0.62 |u| + |v| against 0.62 s and 0.806 s)
        float s = n * 0.5;
        vec2 d = q - box * 0.5;
        float ca = cos(angle);
        float sa = sin(angle);
        // the same quantised axis as the vertex stage and the CPU (1 / 65536)
        ca = floor(ca * 65536.0 + 0.5) / 65536.0;
        sa = floor(sa * 65536.0 + 0.5) / 65536.0;
        float u = d.x * ca + d.y * sa;
        float v = -d.x * sa + d.y * ca;
        float m = 0.62 * abs(u) + abs(v);
        if (m > 0.806 * s)
            return false;
        bool rim = m > 0.62 * s;
        if (rampCount > 0){
            if (rim){
                tone = 2;
            }else if (v > 0.0){
                tone = 2 + (u < 0.0 ? last : max(last - 1, 0));
            }else{
                tone = 2 + (u < 0.0 ? max(last - 2, 0) : max(min(1, last), last - 3));
            }
        }else{
            tone = rim ? 1 : 0;
        }
        return true;
    }
    return true;
}

// A ramp step's colour, linear (the same decode as the instance colour: exact sRGB8 on screen)
vec3 pixelRampColor(int k){
    int count = int(particlesFsParams.pixelInfo.x + 0.5);
    k = clamp(k, 0, max(count - 1, 0));
    return sRGBToLinear(particlesFsParams.pixelRamp[k].rgb);
}

// R6: the secondary colour, a ramp offset from the particle's step or the fixed colour
vec3 pixelSecondaryColor(){
    int count = int(particlesFsParams.pixelInfo.x + 0.5);
    int primaryStep = int(v_pixelData.x + 0.5);
    if (particlesFsParams.pixelFade.w < 0.5 && count > 0 && v_pixelData.x >= 0.0)
        return pixelRampColor(primaryStep + int(particlesFsParams.pixelSecondary.w));
    return sRGBToLinear(particlesFsParams.pixelSecondary.rgb);
}

vec3 pixelToneColor(int tone, vec3 primary){
    if (tone == 0)
        return primary;
    if (tone == 1)
        return pixelSecondaryColor();
    return pixelRampColor(tone - 2);
}

// R4: whether a pixel of coverage a is drawn (Dither against the Bayer cell, Cutout at a half);
// Alpha draws everything (blended by the output)
bool pixelPasses(float a){
    float fade = particlesFsParams.pixelFade.x;
    if (fade > 1.5)
        return a > 0.0;
    if (fade > 0.5)
        return a >= 0.5;
    int size = int(particlesFsParams.pixelFade.y + 0.5);
    // Screen: the cell of the grid (the pattern holds still); Particle: of the box. Kept positive
    // for the table's % (a multiple of 8 added)
    vec2 at = particlesFsParams.pixelFade.z > 0.5 ? v_pixelLocal : v_pixelGrid;
    ivec2 cell = ivec2(floor(at)) + ivec2(65536);
    return a > bayerThreshold(size, cell);
}

// The fragment of a Pixel-mode shape, linear and straight alpha: discards where the shape has
// no pixel or the fade hides it; alpha 1 under Dither and Cutout
vec4 pixelShapeColor(vec3 primary){
    ivec2 p = ivec2(floor(v_pixelLocal));
    int tone, tone2;
    float cover, cover2;
    int rampCount = int(particlesFsParams.pixelInfo.x + 0.5);
    if (!pixelShape(int(particlesFsParams.pixelShape.x + 0.5), p, v_pixelBox.xy, v_pixelBox.z, v_pixelBox.w, v_pixelLine,
                    v_pixelData.y, rampCount, tone, cover, tone2, cover2))
        discard;
    float a = v_pixelData.z;
    bool alphaFade = particlesFsParams.pixelFade.x > 1.5;
    if (pixelPasses(a * cover))
        return vec4(pixelToneColor(tone, primary), alphaFade ? a * cover : 1.0);
    if (tone2 >= 0 && pixelPasses(a * cover2))
        return vec4(pixelToneColor(tone2, primary), alphaFade ? a * cover2 : 1.0);
    discard;
    return vec4(0.0);
}

// A textured billboard of the Pixel Art module: its texel's coverage through the same fade
vec4 pixelTexelColor(vec4 color){
    float a = v_pixelData.z * color.a;
    if (!pixelPasses(a))
        discard;
    return vec4(color.rgb, particlesFsParams.pixelFade.x > 1.5 ? a : 1.0);
}

#endif
