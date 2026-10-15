// HTML UI effects, paint family (p5-fx-paint): helpers the paint effects share. Ported from the
// shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see effects/LICENSE.shaders-com.txt):
// geom.ts (the centred deltas), std/math.ts hashPhase, aa.ts footprint1, colorMixing.ts
// mixPreconvertedInSpace, gradientPaints.ts (the edge modes, the repeat parts, rampWrap), tone.ts
// (the toneRemap variants, rgbDither).
//
// Positions arrive authored (0-1, y down): the kit stores them as (x, 1 - y) and its bodies flip
// them back (unflipPosition, `1 - center.y`), so the ports read them as they come.
#ifndef FAM_PAINT_COMMON_GLSL
#define FAM_PAINT_COMMON_GLSL

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_hash.glsl"

// std/math.ts hashPhase: a stable angle 0..2pi from a seed and a (k, big) salt
float fam_hash_phase(float seed, float k, float big){
    return fract(sin(seed * k) * big) * 6.283185307179586;
}

// geom.ts rotate2 by a precomputed cos/sin
vec2 fam_rotate2(vec2 p, float c, float s){
    return vec2(p.x * c - p.y * s, p.x * s + p.y * c);
}

// geom.ts aspectCenteredDelta: uv to an authored centre, both aspect-corrected (x scaled)
vec2 fam_centered_delta(vec2 uv, vec2 center, float aspect){
    return vec2(uv.x * aspect - center.x * aspect, uv.y - center.y);
}

// geom.ts canvasCentredDelta: uv to the area's centre, aspect-corrected
vec2 fam_canvas_centred(vec2 uv, float aspect){
    return vec2(uv.x * aspect - aspect * 0.5, uv.y - 0.5);
}

// geom.ts rotateAboutCanvasCentre: an aspect-corrected point turned about (aspect / 2, 1 / 2)
vec2 fam_rotate_about_centre(vec2 p, float aspect, float c, float s){
    float cx = p.x - aspect * 0.5;
    float cy = p.y - 0.5;
    return vec2(cx * c - cy * s + aspect * 0.5, cx * s + cy * c + 0.5);
}

// aa.ts footprint1: the screen-space footprint of x widened by softness
float fam_footprint1(float x, float softness){
    float baseW = max(abs(dFdx(x)), abs(dFdy(x)));
    return max(baseW + softness, 0.00001);
}

// ---- colour in a working space (colorMixing.ts mixPreconvertedInSpace and its back-conversion):
// the kit chains mixes in the working space and converts back once

// one alpha-weighted mix of two working-space colours: (conv, alpha)
vec4 fam_mix_in_space(vec4 a, vec4 b, float t){
    float weightA = a.a * (1.0 - t);
    float weightB = b.a * t;
    float total = weightA + weightB;
    return vec4((a.rgb * weightA + b.rgb * weightB) / max(total, 0.001), total);
}

// a straight linear colour into a working space (alpha kept)
vec4 fam_to_space(vec4 c, float space){
    return vec4(jfx_to_space(c.rgb, space), c.a);
}

vec4 fam_from_space(vec4 c, float space){
    return vec4(jfx_from_space(c.rgb, space), c.a);
}

// ---- gradient parameter edge modes and repeats (gradientPaints.ts)

// gradientEdgeMirror: reflect at the ends (WGSL mod of a positive value)
float fam_edge_mirror(float t){
    float m = mod(abs(t), 2.0);
    return m > 1.0 ? 2.0 - m : m;
}

// repeatRings: clamped at repeat 1 or below, else fract-tiled
float fam_repeat_rings(float t, float repeatCount){
    float s = t * repeatCount;
    return repeatCount > 1.0001 ? fract(s) : clamp(s, 0.0, 1.0);
}

// repeatWrap: always wrapped
float fam_repeat_wrap(float t, float repeatCount){
    return fract(t * repeatCount);
}

// rampWrap: t folded back and forth `cycles` half-cycles, fading to the middle where the bands
// get thinner than a pixel (identity at cycles 1)
float fam_ramp_wrap(float t, float cycles){
    float s = t * cycles;
    float m = fract(s * 0.5) * 2.0;
    float band = 1.0 - abs(m - 1.0);
    float fp = fam_footprint1(t, 0.0) * cycles;
    return mix(band, 0.5, smoothstep(0.5, 1.5, fp));
}

// ---- tone tails (tone.ts toneRemap variants)

// toneUnitPivotInverted (noiseColor.ts noiseToneKColor): additive contrast about mid grey, balance
// shift, inverted
float fam_tone_unit_inverted(float noise01, float contrast, float balance){
    float k = clamp((noise01 - 0.5) * (contrast + 1.0) + 0.5 + balance, 0.0, 1.0);
    return 1.0 - k;
}

// toneSignedInverted: contrast and balance on the signed field before the squash, inverted
float fam_tone_signed_inverted(float v, float contrast, float balance){
    float adjusted = v * (contrast + 1.0) + balance;
    return 1.0 - clamp(adjusted * 0.5 + 0.5, 0.0, 1.0);
}

// toneGlowInverted: pow(v, 5 / glow), multiplicative contrast, percent balance (50 neutral), inverted
float fam_tone_glow_inverted(float v, float glow, float contrast, float balance){
    float spread = pow(v, 5.0 / glow);
    float contrasted = clamp((spread - 0.5) * contrast + 0.5, 0.0, 1.0);
    float balanced = clamp(contrasted + (balance / 100.0 - 0.5), 0.0, 1.0);
    return 1.0 - balanced;
}

// tone.ts rgbDither: a +-0.002 hash dither of a final colour (px: the pixel position; the kit's
// uv * viewport), drifting with the clock
vec4 fam_rgb_dither(vec4 base, vec2 px, float t){
    float n = jfx_hash12(px + vec2(t, t * 1.618));
    return vec4(clamp(base.rgb + (n - 0.5) * 0.004, 0.0, 1.0), base.a);
}

#endif // FAM_PAINT_COMMON_GLSL
