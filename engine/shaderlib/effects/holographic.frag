#version 450
// @effect     holographic
// @kind       paint
// @category   light
// @summary    Iridescent holographic foil with a drifting rainbow sheen and glitter flakes
// @source     shaders.com Holographic (adapted: shape = element box; tilt follows the pointer)
// @cost       medium
// @time       speed
// @pointer    position
// @suggest-for foil holo iridescent
// @param      hue-shift angle 0deg 0deg 360deg -- Turns the rainbow spectrum across the foil
// @param      foil-scale number 1.5 0.3 6 -- Size of the colour patches (higher gives smaller, busier patches)
// @param      saturation number 0.75 0 1 -- Rainbow saturation (0 plain silver foil, 1 fully spectral)
// @param      roughness number 0.25 0 1 -- Matte laminate grain that softens the gloss
// @param      speed number 1 0 2 -- Speed of the spectrum drifting across the foil (0 pauses)
// @param      crinkle number 0.5 0 1 -- Laminate wrinkles that make the sheen swirl locally
// @param      crinkle-scale number 1 0.3 6 -- Size of the wrinkles (higher gives smaller, denser wrinkles)
// @param      sparkle number 0.4 0 1 -- Glitter flakes that twinkle in a shifted hue
// @param      tilt-follows-pointer boolean true -- The foil tilts toward the pointer while it is over the element

// Ported from shaders.com Holographic (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (the flat-shape path: the pillow normal from the field's slope, the laminate wrinkles, the
// diffraction hue through the cosine rainbow, the fixed key light's sheen and specular, the
// glitter flakes and the roughness grain). Adapted: the shape is the element's box (main_paint
// clips it to its rounded corners), whose shorter side spans the source's default shape (0.7
// units) so the patches scale with the element; the pattern coordinates are centred on it. While
// the pointer is over the element (eased by presence), the foil tilts toward it like a card in
// the hand: the pointer's offset from the centre leans the surface normal, which turns the
// spectrum and moves the highlight.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/jfx_sdf.glsl"

uniform u_fs_effectParams {
    float hueShift;
    float foilScale;
    float saturation;
    float roughness;
    float speed;
    float crinkle;
    float crinkleScale;
    float sparkle;
    float tiltFollowsPointer;
} params;

// the fixed studio light (300deg) and its pre-folded key-light vector
#define HOLO_LIGHT_X 0.5000000000000001
#define HOLO_LIGHT_Y -0.8660254037844386
#define HOLO_L vec3(0.37164708971977234, -0.6437116265296936, -0.6689647436141968)

// the cosine rainbow with exact thirds
vec3 holo_rainbow(float t){
    return vec3(cos(t * JFX_TAU) * 0.5 + 0.5, cos((t + 1.0 / 3.0) * JFX_TAU) * 0.5 + 0.5, cos((t + 2.0 / 3.0) * JFX_TAU) * 0.5 + 0.5);
}

vec4 jfx_paint(JfxContext c){
    float t = c.anim.x;
    float minSide = max(min(c.size.x, c.size.y), 1.0);
    float unitPx = minSide / 0.7;
    // the pattern coordinates: centred, in source units
    vec2 pd = (c.px - c.size * 0.5) / unitPx;
    float sdf = jfx_box_sdf(c.px, c.size, c.radii) / unitPx;

    // the pillow normal: the field's slope over the last 0.06 units inside the edge
    vec2 grad = jfx_box_normal(c.px, c.size, c.radii);
    float edgeT = clamp(-sdf / 0.06, 0.0, 1.0);
    float domeSlope = (1.0 - edgeT) * 0.35;
    vec2 baseN = grad * domeSlope;
    // the tilt toward the pointer
    vec2 tilt = clamp((c.pointerSmooth - c.size * 0.5) / max(c.size * 0.5, vec2(1.0)), -1.0, 1.0);
    baseN += tilt * (0.5 * c.pointerPresence * step(0.5, params.tiltFollowsPointer));

    // laminate wrinkles
    float crScale = params.crinkleScale * 1.35;
    float crAmp = params.crinkle * 0.8;
    const float neps = 0.02;
    float w0 = jfx_mx_noise2(vec2(pd.x * crScale + 2.7, pd.y * crScale + 11.3));
    float wx = (jfx_mx_noise2(vec2((pd.x + neps) * crScale + 2.7, pd.y * crScale + 11.3)) - w0) / neps;
    float wy = (jfx_mx_noise2(vec2(pd.x * crScale + 2.7, (pd.y + neps) * crScale + 11.3)) - w0) / neps;
    vec3 n = normalize(vec3(baseN.x + wx * crAmp * 0.4, baseN.y + wy * crAmp * 0.4, -1.0));

    // the diffraction hue
    float fScale = params.foilScale * 2.2;
    float drift = t * 0.06;
    float blotch = jfx_mx_noise2(vec2(pd.x * fScale + drift * HOLO_LIGHT_X, pd.y * fScale + drift * HOLO_LIGHT_Y)) +
                   jfx_mx_noise2(vec2(pd.x * fScale * 2.1 + 5.2 - drift * HOLO_LIGHT_X * 1.6, pd.y * fScale * 2.1 + 9.4 - drift * HOLO_LIGHT_Y * 1.6)) * 0.4;
    float normalTilt = n.x * HOLO_LIGHT_X + n.y * HOLO_LIGHT_Y;
    float ramp = (pd.x * HOLO_LIGHT_X + pd.y * HOLO_LIGHT_Y) * 0.8;
    float hue = blotch * 0.45 + ramp + normalTilt * 1.4 + t * 0.18 + params.hueShift / JFX_TAU;
    vec3 spectral = holo_rainbow(hue);
    vec3 foilTint = mix(vec3(0.72, 0.73, 0.76), spectral, vec3(clamp(params.saturation, 0.0, 1.0) * 0.85));

    // the key light on the foil
    float roughnessC = clamp(params.roughness, 0.0, 1.0);
    vec3 L = HOLO_L;
    vec3 H = normalize(L + vec3(0.0, 0.0, -1.0));
    float ndl = clamp(dot(n, L) * 0.5 + 0.5, 0.0, 1.0);
    float ndh = clamp(dot(n, H), 0.0, 1.0);
    float sheen = pow(ndh, 7.0) * 0.45 * (1.0 - roughnessC * 0.35);
    float specCore = pow(ndh, mix(42.0, 12.0, roughnessC)) * 0.85 * (1.0 - roughnessC * 0.55);
    vec3 specRgb = foilTint * sheen + mix(foilTint, vec3(1.0), vec3(0.6)) * specCore;

    // glitter flakes
    float flakeN = jfx_mx_noise2(vec2(pd.x * 190.0, pd.y * 190.0));
    float flakePhase = jfx_mx_noise2(vec2(pd.x * 190.0 + 47.3, pd.y * 190.0 - 31.1));
    float twinkle = sin(t * 2.2 + flakePhase * 12.6) * 0.5 + 0.5;
    float flakeMask = smoothstep(0.52, 0.62, flakeN * 0.75 + twinkle * 0.3) * clamp(params.sparkle, 0.0, 1.0);
    vec3 flakeRgb = holo_rainbow(hue + 0.33 + flakePhase * 0.5) * (0.85 + twinkle * 0.55) * (ndl * 0.6 + 0.7);

    // roughness grain
    float grain = jfx_mx_noise2(vec2(pd.x * 420.0, pd.y * 420.0));
    float grainMul = 1.0 + grain * roughnessC * 0.28;

    vec3 foilBase = foilTint * (ndl * 0.5 + 0.62) + specRgb;
    vec3 foilRgb = mix(foilBase, flakeRgb, vec3(flakeMask)) * grainMul;
    return vec4(foilRgb, 1.0);
}

#include "effects/lib/main_paint.glsl"
