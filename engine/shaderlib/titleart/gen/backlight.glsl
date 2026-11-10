// Backlight, ported from Pachislop src/generators/light/backlight.ts by SafEight, used with the
// author's permission: god rays streaming out from behind the followed layer's own silhouette.
// Pachislop's ray source (alpha x noise on a circle) and its two radial blurs run at half
// resolution; the source is folded into the first blur, whose taps all lie on the ray from the
// pixel to the centre, so the noise (a function of that ray's angle) is one factor of the sum.
//
// BacklightSource (half res): TA_TEX0 the followed layer's content; BacklightRadial (half res):
// TA_TEX0 the first blur. Both: p6 = (the centre x, y in full target px, rows down; the centre
// x, y in the drawing space), p7 = (the full target's width, height, ray length in target px, 0);
// BacklightSource also p8 = (ray count, noise 0..1, rotation in radians counter-clockwise, seed).
// Positions are full-target px, so the half-resolution targets sample exactly what Pachislop's
// full-resolution passes would there. The blurs store their value / BL_HEADROOM, which keeps
// rays above 1 on the RGBA8 path.
// BacklightCompose (full res): TA_TEX0 the second blur; p6.x = intensity, p7 = inner linear
// colour, p8 = outer linear colour.

#ifndef TITLEART_GEN_BACKLIGHT_GLSL
#define TITLEART_GEN_BACKLIGHT_GLSL

const float BL_HEADROOM = 4.0;
const int BL_TAPS = 40;

// Pachislop's radial blur toward the centre over `len` (radialFrag): taps of TA_TEX0's alpha
// (the source) or of its stored rays, weighted toward the pixel with `falloff`
float bl_radial(float len, bool falloff, bool fromSource) {
    vec2 size = ta.p[7].xy;
    vec2 p = ta_uv() * size;
    vec2 toC = ta.p[6].xy - p;
    float dist = length(toC);
    float span = min(dist, len);
    vec2 dir = toC / max(dist, 1e-3);
    float acc = 0.0, wsum = 0.0;
    for (int i = 0; i < BL_TAPS; i++) {
        float f = (float(i) + 0.5) / float(BL_TAPS);
        float w = falloff ? pow(1.0 - f, 1.3) : 1.0;
        vec4 s = textureLod(TA_TEX0, (p + dir * span * f) / size, 0.0);
        acc += (fromSource ? s.a : s.r * BL_HEADROOM) * w;
        wsum += w;
    }
    float v = acc / wsum;
    // rays stop at len from their source edge rather than averaging out to nothing
    return falloff ? v * span / max(len, 1.0) * 2.0 : v;
}

vec4 gen_backlightSource() {
    // the ray mask: noise on a circle around the centre (raySrcFrag), in the drawing space (+y up)
    vec2 d = gen_spacePos() - ta.p[6].zw;
    float ang = (d.x == 0.0 && d.y == 0.0 ? 0.0 : atan(d.y, d.x)) + ta.p[8].z;
    vec2 cdir = vec2(cos(ang), sin(ang));
    float rad = ta.p[8].x / 6.2831853;
    float seed = ta.p[8].w;
    float n = 0.6 * vnoise(cdir * rad + seed) + 0.4 * vnoise(cdir * rad * 2.7 + seed * 1.7);
    float mask = mix(1.0, pow(smoothstep(0.25, 0.85, n), 1.5) * 1.8, ta.p[8].y);
    return vec4(bl_radial(ta.p[7].z, true, true) * mask / BL_HEADROOM, 0.0, 0.0, 1.0);
}

vec4 gen_backlightRadial() {
    return vec4(bl_radial(ta.p[7].z, false, false) / BL_HEADROOM, 0.0, 0.0, 1.0);
}

vec4 gen_backlightCompose() {
    float r = texture(TA_TEX0, ta_uv()).r * BL_HEADROOM * ta.p[6].x;
    return gen_lightPremul(mix(ta.p[8].rgb, ta.p[7].rgb, clamp(r, 0.0, 1.0)) * r);
}

#endif // TITLEART_GEN_BACKLIGHT_GLSL
