// Particle shapes (docs/specs/S83-title-effects-generators.md §6.7.1, D6), ported from Pachislop
// src/generators/particles.ts (PARTICLE_FRAG, GLINT_FRAG) by SafEight, used with the author's
// permission. sdStar5 and sdHeart are Inigo Quilez's signed distance functions (MIT, from his
// published 2D distance articles; D15). The glint shapes are the glint star (tfx_glint_star.glsl).
//
// Standalone: no block, texture or title include, so S20's particle renderer can adopt it. A
// shape is drawn on a quad: `quad` -1..1 along the quad's own axes (x along its length), `fwq`
// = fwidth(quad) taken by the caller in uniform control flow, `color` premultiplied linear,
// `extra` = (defocus in particle radii, aspect = length / width, tumble phase, the glint's
// smallest core half-width in half quad heights). Light shapes return light (rgb may exceed 1,
// alpha = the brightest channel, for additive blending); solid shapes (confetti, petal, heart)
// premultiplied coverage for "over".

#ifndef TITLEART_GEN_TFX_PARTICLE_SHAPES_GLSL
#define TITLEART_GEN_TFX_PARTICLE_SHAPES_GLSL

#include "titleart/gen/tfx_glint_star.glsl"

const int TFX_PARTICLE_DOT = 0;
const int TFX_PARTICLE_GLINT4 = 1;
const int TFX_PARTICLE_GLINT6 = 2;
const int TFX_PARTICLE_EMBER = 3;
const int TFX_PARTICLE_SQUARE = 4;
const int TFX_PARTICLE_CONFETTI = 5;
const int TFX_PARTICLE_PETAL = 6;
const int TFX_PARTICLE_STAR = 7;
const int TFX_PARTICLE_HEART = 8;
const int TFX_PARTICLE_BOKEH = 9;
const int TFX_PARTICLE_RAIN = 10;

float tfx_particleMax3(vec3 c) {
    return max(c.r, max(c.g, c.b));
}

float tfx_particleCover(float d, float e) {
    return 1.0 - smoothstep(-e, e, d);
}

// Inigo Quilez's 5-point star pointing up: outer radius r, inner ratio rf
float tfx_sdStar5(vec2 p, float r, float rf) {
    const vec2 k1 = vec2(0.809016994375, -0.587785252292);
    const vec2 k2 = vec2(-k1.x, k1.y);
    p.x = abs(p.x);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= r;
    vec2 ba = rf * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
    float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
    return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}

float tfx_particleDot2(vec2 v) {
    return dot(v, v);
}

// Inigo Quilez's heart with its tip at the origin
float tfx_sdHeart(vec2 p) {
    p.x = abs(p.x);
    if (p.y + p.x > 1.0) return sqrt(tfx_particleDot2(p - vec2(0.25, 0.75))) - sqrt(2.0) / 4.0;
    return sqrt(min(tfx_particleDot2(p - vec2(0.0, 1.0)), tfx_particleDot2(p - 0.5 * max(p.x + p.y, 0.0)))) * sign(p.x - p.y);
}

// the 4- or 6-point glint as light: the glint star in its quad (glints are round, so the quad
// position is the position in glint radii, defocus pad included)
vec4 tfx_particleGlint(int shape, vec2 quad, vec2 fwq, vec4 color, vec4 extra) {
    float b = extra.x;
    // the colour unpremultiplied (it saturates toward the ray tips), the light x opacity
    float a = max(color.a, 1e-4);
    vec3 c = glintStar(quad, shape == TFX_PARTICLE_GLINT4 ? 4 : 6, 0.035 * (1.0 + 3.0 * b), extra.w, max(fwq.x, fwq.y), color.rgb / a,
                       false) *
             (color.a / (1.0 + b));
    return vec4(c, min(1.0, tfx_particleMax3(c)));
}

// any shape: light (light = true: the light shapes' additive output) or solid premultiplied
// coverage; ring = the bokeh rim 0..1, sheen = the confetti foil's glare 0..1
vec4 tfx_particleShade(int shape, bool light, vec2 quad, vec2 fwq, vec4 color, vec4 extra, float ring, float sheen) {
    if (shape == TFX_PARTICLE_GLINT4 || shape == TFX_PARTICLE_GLINT6) return tfx_particleGlint(shape, quad, fwq, color, extra);
    float b = extra.x, A = extra.y, phase = extra.z;
    // local position in particle radii: x along the length (-A..A), y across (-1..1), plus the
    // defocus pad; its fwidth is the quad's scaled (A and b are the quad's constants)
    vec2 scale = vec2(A + b, 1.0 + b);
    vec2 l = quad * scale;
    vec2 fw = fwq * abs(scale);
    float e = max(0.7 * (fw.x + fw.y), 0.5 * b);
    float r = length(l);
    float s = 1.0 + b;
    float I = 0.0;      // light intensity, or coverage of solids
    float hot = 0.0;    // white-hot core of light shapes
    vec3 tint = vec3(1.0);
    if (shape == TFX_PARTICLE_DOT) {
        I = exp(-2.6 * r * r / (s * s)) / s;
        hot = 0.5 * exp(-18.0 * r * r) / (1.0 + 4.0 * b);
    } else if (shape == TFX_PARTICLE_EMBER) {
        // a dot dragged along its motion: dim tail at -x, hot head at +x
        float hl = A - 1.0;
        vec2 q = vec2(l.x - clamp(l.x, -hl, hl), l.y);
        float h = clamp((l.x + A) / (2.0 * A), 0.0, 1.0);
        I = exp(-3.0 * dot(q, q) / (s * s)) * mix(0.15, 1.0, h * h) / s;
        vec2 hp = vec2(l.x - hl, l.y);
        hot = 0.7 * exp(-10.0 * dot(hp, hp) / (s * s)) / s;
    } else if (shape == TFX_PARTICLE_RAIN) {
        float h = clamp((l.x + A) / (2.0 * A), 0.0, 1.0);
        I = exp(-3.0 * l.y * l.y / (s * s)) * smoothstep(0.0, 0.3, h) * (1.0 - smoothstep(0.92, 1.0, h)) * mix(0.35, 1.0, h) / s;
    } else if (shape == TFX_PARTICLE_BOKEH) {
        // a defocused highlight: a soft-edged disc, a little dimmer in the middle, with an
        // optional bright rim (onion ring)
        float cov = tfx_particleCover(r - 0.94, max(e, 0.06 + 0.5 * b));
        I = cov * (mix(0.8, 0.4, ring) * (1.0 - 0.2 * r * r) + 1.1 * ring * smoothstep(0.6, 0.95, r));
    } else if (shape == TFX_PARTICLE_STAR) {
        I = tfx_particleCover(tfx_sdStar5(l, 1.0, 0.45), e) * (0.75 + 0.6 * exp(-6.0 * r * r));
        hot = 0.35 * exp(-20.0 * r * r);
    } else if (shape == TFX_PARTICLE_SQUARE) {
        I = tfx_particleCover(max(abs(l.x), abs(l.y)) - 1.0, e);
    } else if (shape == TFX_PARTICLE_CONFETTI) {
        I = tfx_particleCover(max(abs(l.x) - A, abs(l.y) - 1.0), e);
        // foil sheen: a diagonal glare band that slides along the strip as it turns
        float band = (l.x / A) * 0.8 + l.y * 0.35 - 1.6 * sin(phase);
        tint += sheen * 1.6 * exp(-band * band / 0.05);
    } else if (shape == TFX_PARTICLE_PETAL) {
        // base at -x, notched tip at +x, widest near the tip
        float u = (l.x / A + 1.0) * 0.5;
        float uc = clamp(u, 0.0, 1.0);
        float w = 0.95 * pow(uc, 0.6) * sqrt(max(0.0, 1.0 - pow(uc, 6.0)));
        float d = max(abs(l.y) - w, abs(l.x) - A);
        d = max(d, (u - 0.86) * 2.4 * A - abs(l.y));
        I = tfx_particleCover(d, e);
        tint *= mix(1.12, 0.94, uc);
    } else {
        vec2 p = l * 0.55 + vec2(0.0, 0.5);
        I = tfx_particleCover(tfx_sdHeart(p) / 0.55, e);
        vec2 hl = l - vec2(-0.35, 0.35);
        tint *= 1.0 + 0.25 * exp(-8.0 * dot(hl, hl));
    }
    if (light) {
        vec3 c = color.rgb * I + color.a * hot;
        return vec4(c, min(1.0, tfx_particleMax3(c)));
    }
    return vec4(color.rgb * tint, color.a) * (I / (1.0 + 0.6 * b));
}

#endif // TITLEART_GEN_TFX_PARTICLE_SHAPES_GLSL
