#version 450
// @effect     bulge
// @kind       filter
// @category   distort
// @summary    Magnifies (or pinches) the content in a disc, optionally following the pointer
// @source     shaders.com Bulge (+ original `follow-pointer`)
// @cost       low
// @pointer    position
// @suggest-for magnify fisheye pinch
// @param      center position "50% 50%" -- Centre of the bulge
// @param      strength number 1 -1 1 -- Magnification (negative pinches)
// @param      radius number 1 0 5 -- Size of the bulge (the disc is half this, in area heights)
// @param      falloff number 0.5 0 1 -- Softness of the bulge's rim
// @param      follow-pointer boolean false -- The bulge moves to the pointer while it is over the element
// @param      edges enum stretch stretch|transparent|mirror|wrap -- What shows where the bulge reads beyond the element

// Ported from shaders.com Bulge (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/warps.ts bulge: warpMaps.ts bulgeUV, the aspect-corrected offset from the centre scaled
// by 1 - strength x a smooth quadratic falloff; read through edges.ts sampleRemappedExpr:
// Catmull-Rom, bilinear in the mirror mode). Original: with `follow-pointer`, the centre eases to
// the smoothed pointer by its presence.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    vec2 center;
    float strength;
    float radius;
    float falloff;
    float followPointer;
    float edges;
} params;

vec4 jfx_filter(JfxContext c){
    vec2 center = params.center;
    if (params.followPointer > 0.5)
        center = mix(center, c.pointerSmooth * frame.size.zw, c.pointerPresence);
    vec2 centerPos = vec2(center.x * c.aspect, center.y);
    vec2 delta = vec2(c.uv.x * c.aspect, c.uv.y) - centerPos;
    float distance = length(delta);
    float effectRadius = params.radius * 0.5;
    float innerRadius = effectRadius * max(1.0 - params.falloff - 0.001, 0.0);
    float smoothFalloff = 1.0 - smoothstep(innerRadius, max(effectRadius, innerRadius + 1e-6), distance);
    float normalizedDist = distance / max(effectRadius, 1e-6);
    float quadraticFalloff = max(0.0, 1.0 - normalizedDist * normalizedDist);
    float scaleFactor = 1.0 - params.strength * smoothFalloff * quadraticFalloff;
    vec2 bulged = centerPos + delta * scaleFactor;
    return fam_src_remapped(vec2(bulged.x / c.aspect, bulged.y) * c.size, params.edges);
}

#include "effects/lib/main_filter.glsl"
