#version 450
// @effect     ripple-rings
// @kind       paint
// @category   pattern
// @summary    Concentric rings rippling out from a centre
// @source     shaders.com Ripples
// @cost       low
// @time       speed
// @suggest-for ripples rings
// @param      center position "50% 50%" -- Centre of the rings
// @param      color-a color #ffffff -- Colour of the rings
// @param      color-b color #000000 -- Colour between the rings
// @param      speed number 1 -5 5 -- Speed of the rings moving outward (negative moves inward)
// @param      frequency number 20 1 80 -- Ring density
// @param      softness number 0 0 3 -- Edge softness (0 is crisp)
// @param      thickness number 0.5 0 1 -- Share of each period the ring fills
// @param      phase number 0 0 6.28 -- Phase offset of the rings (radians)

// Ported from shaders.com Ripples (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (strokeOver(colorB, colorA, ringWaves): ripplesMask, the alpha-weighted linear mix).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"
#include "effects/lib/fam_paint_pattern.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    vec2 center;
    float speed;
    float frequency;
    float softness;
    float thickness;
    float phase;
} params;

vec4 jfx_paint(JfxContext c){
    float dist = length(fam_centered_delta(c.uv, params.center, c.aspect));
    float baseWave = sin(dist * params.frequency - c.anim.x + params.phase);
    // thickness 0..1 to a threshold in -1..1, banded by the softness; the ring reads 1
    float thicknessThreshold = params.thickness * 2.0 - 1.0;
    float mask = 1.0 - fam_smoothstep(thicknessThreshold - params.softness, thicknessThreshold + params.softness, baseWave);
    return jfx_mix_colors(params.colorB, params.colorA, mask, 0.0);
}

#include "effects/lib/main_paint.glsl"
