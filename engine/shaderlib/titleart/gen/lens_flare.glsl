// Lens Flare, ported from Pachislop src/generators/light/lensFlare.ts by SafEight, used with the
// author's permission: an anamorphic flare drawn as light (set the layer to Add): a hot core in a
// Lorentzian bloom, the cusped horizontal streak, a starburst from noise sampled on a circle, a
// chromatic halo ring and six ghosts on the axis through the canvas centre.
//
// p6 = (the flare's centre x, y in the drawing space, its size, streak length in target px);
// p7 = (intensity, streak angle in render px radians counter-clockwise, starburst 0..2, seed);
// p8 = (linear colour, ghosts 0/1); p9 = (the canvas centre minus the flare's, target px +y up, 0, 0).

#ifndef TITLEART_GEN_LENS_FLARE_GLSL
#define TITLEART_GEN_LENS_FLARE_GLSL

#include "titleart/gen/tfx_anamorphic.glsl"

vec4 gen_lensFlare() {
    vec2 pos = ta.p[6].xy;
    float size = ta.p[6].z, streak = ta.p[6].w;
    float intensity = ta.p[7].x, rot = ta.p[7].y, rays = ta.p[7].z, seed = ta.p[7].w;
    vec3 col = ta.p[8].rgb;
    vec2 axis = ta.p[9].xy;
    // Pachislop's render px from the flare (+y up): the drawing-space offset in target px, rows flipped
    vec2 v = gen_dirToTarget(gen_spacePos() - pos) * vec2(1.0, -1.0);
    float c = cos(rot), sn = sin(rot);
    vec2 lv = vec2(c * v.x + sn * v.y, -sn * v.x + c * v.y);
    float r = length(v) / size;
    vec3 L = vec3(0.0);
    // hot core + soft bloom
    L += vec3(1.0) * (exp(-r * r * 18.0) * 2.0 + 0.25 / (1.0 + r * r * 6.0));
    L += col * 0.35 / (1.0 + r * r * 1.5);
    L += anamorphicStreak(lv, size, streak, col);
    // starburst spikes (atan is undefined at the exact centre, where they are faded out anyway)
    float ang = v.x == 0.0 && v.y == 0.0 ? 0.0 : atan(v.y, v.x);
    vec2 cd = vec2(cos(ang), sin(ang));
    float sp = pow(vnoise(cd * 9.0 + seed), 3.0) * 2.5 + pow(vnoise(cd * 23.0 + seed * 2.0), 6.0) * 4.0;
    L += mix(col, vec3(1.0), 0.6) * sp * rays * 0.5 / (1.0 + r * r * 2.5) * smoothstep(0.0, 0.15, r);
    // chromatic halo ring: Pachislop's pow(x, 2.0) as squares (pow is undefined for a negative base)
    vec3 ringD = (vec3(r) - vec3(2.35, 2.25, 2.15)) / 0.07;
    L += exp(-ringD * ringD) * 0.18;
    if (ta.p[8].w > 0.5) {
        for (int i = 0; i < 6; i++) {
            float fi = float(i);
            float k = 0.35 + fi * 0.33 + (hash12(vec2(fi, seed)) - 0.5) * 0.2;
            float gs = size * (0.25 + 0.9 * hash12(vec2(fi + 9.0, seed)));
            float d = length(v - axis * k * 2.0) / gs;
            float disc = (1.0 - smoothstep(0.9, 1.0, d)) * (0.35 + 0.65 * d * d);
            vec3 tint = 0.5 + 0.5 * cos(6.2831 * (fi * 0.17 + vec3(0.0, 0.33, 0.67) + seed * 0.1));
            L += tint * mix(col, vec3(1.0), 0.3) * disc * 0.16;
        }
    }
    return gen_lightPremul(L * intensity);
}

#endif // TITLEART_GEN_LENS_FLARE_GLSL
