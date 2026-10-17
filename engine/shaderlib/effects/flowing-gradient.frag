#version 450
// @effect     flowing-gradient
// @kind       paint
// @category   gradient
// @summary    Four colours folded through drifting domain warps, like liquid silk
// @source     shaders.com FlowingGradient
// @cost       medium
// @time       speed
// @suggest-for liquid-gradient silk
// @param      color-a color #0a0015 -- First colour
// @param      color-b color #6b17e6 -- Second colour
// @param      color-c color #ff4d6a -- Third colour
// @param      color-d color #ff6b35 -- Fourth colour
// @param      color-space enum oklch linear|oklch|oklab|hsl|hsv|lch -- Colour space of the colour mix
// @param      speed number 1 0 10 -- Animation speed
// @param      distortion number 0.5 0 2 -- Strength of the folds and warps
// @param      seed number 0 0 100 -- Random seed for variation

// Ported from shaders.com FlowingGradient (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (FlowingGradient/index.ts paint: the noise rotation, the sine shear, two chainedWarpStep
// levels, the hard/soft zone edges, quadBlend in the working colour space, the fold lighting).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_noise.glsl"
#include "effects/lib/fam_paint_common.glsl"
#include "effects/lib/fam_paint_noise.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    vec4 colorC;
    vec4 colorD;
    float colorSpace;
    float speed;
    float distortion;
    float seed;
} params;

#define FOUR_PI 12.566370614359172

float flowing_fold(float blend){
    return 1.0 - abs(blend * 2.0 - 1.0);
}

vec4 jfx_paint(JfxContext c){
    float t = c.anim.x * 0.075;
    float seedOff = params.seed * 0.17;
    float strength = params.distortion;
    vec2 centred = c.uv - 0.5;

    // organic rotation of the aspect-squashed area by one noise read
    float spinNoise = jfx_mx_noise3(vec3(centred.x * 0.8 + t * 0.4, centred.y * 0.8 - t * 0.25, seedOff));
    float angle = spinNoise * FOUR_PI * strength;
    vec2 squashed = vec2(centred.x, centred.y / c.aspect);
    vec2 spun = fam_rotate2(squashed, cos(angle), sin(angle));
    vec2 rot = vec2(spun.x, spun.y * c.aspect);

    // the sine shear: the y wave reads the updated x
    float shearT = t * 2.0;
    float shearX = rot.x + sin(rot.y * 5.0 + shearT) / 50.0 * strength;
    float shearY = rot.y + sin(shearX * 7.5 + shearT) / 25.0 * strength;

    // two chained warp levels (WARP_LEVELS)
    vec4 level1 = fam_chained_warp_step(vec2(shearX, shearY), 2.0, t * 0.5 + seedOff, vec2(5.2, 1.3), vec2(1.7, 9.2), 0.25, strength);
    float tz = t * 0.3 + seedOff;
    vec4 level2 = fam_chained_warp_step(level1.xy, 1.5, tz, vec2(8.3, 2.8), vec2(3.1, 7.4), 0.2, strength);

    // colour zones with variable hard/soft edges
    float zoneNoise = jfx_mx_noise3(vec3(level2.x * 1.8, level2.y * 1.8, tz));
    float edgeHardness = pow(max(level1.z * 0.5 + 0.5, 0.0), 1.5);
    float edgeWidth = mix(0.5, 0.008, edgeHardness);
    float t1 = smoothstep(edgeWidth * -1.0, edgeWidth, zoneNoise);
    float t2 = smoothstep(edgeWidth * -1.0, edgeWidth, level1.w);

    // quadBlend: A -> B and C -> D by t1, then the rows by t2, in the working space
    float space = params.colorSpace;
    vec4 row1 = fam_mix_in_space(fam_to_space(params.colorA, space), fam_to_space(params.colorB, space), t1);
    vec4 row2 = fam_mix_in_space(fam_to_space(params.colorC, space), fam_to_space(params.colorD, space), t1);
    vec4 base = fam_from_space(fam_mix_in_space(row1, row2, t2), space);

    // fold lighting: peaks where the colour flips fastest, plus surface curvature
    float foldHighlight = max(flowing_fold(t1), flowing_fold(t2)) * edgeHardness;
    float surfaceLight = (level2.z * 0.5 + 0.5) * 0.12 + 0.94;
    float litScale = surfaceLight * (1.0 + foldHighlight * 0.35);
    return vec4(clamp(base.rgb * litScale, 0.0, 1.0), base.a);
}

#include "effects/lib/main_paint.glsl"
