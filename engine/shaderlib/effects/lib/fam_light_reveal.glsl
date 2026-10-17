// HTML UI effects, light family (p5-fx-light): the transitions' reveal tail and coverage
// coordinates. Ported from the shaders.com kit reveal.ts (MIT, Copyright (c) 2026 Shader Effects
// Inc.; see effects/LICENSE.shaders-com.txt): revealMask, directionalCoord, radialCornerNormCoord,
// angularCoord, cellGrid with cellDiamondCoord and cellShuffleCoord, tilePhase, bandMidpointCoord
// and fbmCoverageCoord, with their constants.
//
// A coverage is a number per pixel from 0 (goes first) to 1 (goes last). Positions arrive
// authored (0-1, y down): the kit's `1 - center.y` recovers the authored y from its flipped
// storage, so these read the centre as it comes. Angles arrive in radians.
#ifndef FAM_LIGHT_REVEAL_GLSL
#define FAM_LIGHT_REVEAL_GLSL

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_hash.glsl"
#include "effects/lib/jfx_noise.glsl"

// revealMask: the alpha multiplier of a coverage at a progress, the front remapped by the feather
// so progress 0 leaves everything and 1 clears everything. The transitions' contract (phase-5
// spec A.3): invert swaps the two ends. It is the kit's invert (the coverage flipped) with the
// progress counted from the other end, which is exactly the complement of the plain mask: the
// content comes back in the order the plain transition takes it away.
float fam_reveal_mask(float coord, float progress, float feather, float invert){
    bool inverted = invert > 0.5;
    float c = inverted ? 1.0 - coord : coord;
    float p = inverted ? 1.0 - progress : progress;
    float f = max(feather, 0.0001);
    float front = p * (1.0 + 2.0 * f) - f;
    return smoothstep(front - f, front + f, c);
}

// directionalCoord: 0 -> 1 along the angle, aspect corrected, both ends of the area reached
float fam_directional_coord(vec2 uv, float aspect, float angle){
    vec2 dir = vec2(cos(angle), sin(angle));
    vec2 p = vec2((uv.x - 0.5) * aspect, uv.y - 0.5);
    float grad = dot(p, dir);
    float ext = 0.5 * (aspect * abs(dir.x) + abs(dir.y));
    return grad / (2.0 * ext) + 0.5;
}

// radialCornerNormCoord: the distance from the centre over the distance to the farthest corner
float fam_radial_corner_coord(vec2 uv, float aspect, vec2 center){
    float cx = center.x * aspect;
    float cy = center.y;
    vec2 rel = vec2(uv.x * aspect - cx, uv.y - cy);
    float dist = length(rel);
    float dx = max(cx, aspect - cx);
    float dy = max(cy, 1.0 - cy);
    float maxDist = length(vec2(dx, dy));
    return dist / max(maxDist, 0.0001);
}

// angularCoord: a clock sweep about the centre from the start angle; mode 0 cw, 1 ccw, 2 both
float fam_angular_coord(vec2 uv, float aspect, vec2 center, float start, float mode){
    vec2 rel = vec2((uv.x - center.x) * aspect, uv.y - center.y);
    float ang = atan(rel.y, rel.x);
    float frac = (ang - start) / JFX_TAU;
    frac = frac - floor(frac);
    float ccwCoord = 1.0 - frac;
    float bothCoord = min(frac, 1.0 - frac) * 2.0;
    float coord = mode > 0.5 ? ccwCoord : frac;
    return mode > 1.5 ? bothCoord : coord;
}

// cellGrid: square-ish cells `size` wide (a share of the area's width); xy the cell, zw the
// position in it
vec4 fam_cell_grid(vec2 uv, float aspect, float size){
    float gridX = 1.0 / max(size, 0.001);
    float gridY = max(gridX / aspect, 1.0);
    vec2 s = vec2(uv.x * gridX, uv.y * gridY);
    return vec4(floor(s), fract(s));
}

// cellDiamondCoord: the L1 distance from the cell's centre (diamonds that tile the area)
float fam_cell_diamond_coord(vec2 uv, float aspect, float size){
    vec4 g = fam_cell_grid(uv, aspect, size);
    return abs(g.z - 0.5) + abs(g.w - 0.5);
}

// cellShuffleCoord: a stable random coverage per cell
float fam_cell_shuffle_coord(vec2 uv, float aspect, float size){
    return jfx_hash12(fam_cell_grid(uv, aspect, size).xy);
}

// tilePhase: the coverage repeated count times
float fam_tile_phase(float t, float count){
    return fract(t * count);
}

// bandMidpointCoord: the coverage cut into count bands, each at its midpoint
float fam_band_midpoint_coord(float t, float count){
    float n = max(count, 1.0);
    return clamp((floor(t * n) + 0.5) / n, 0.0, 1.0);
}

// fbmCoverageCoord: a stable 3-octave MaterialX perlin fbm recentred onto 0..1
float fam_fbm_coverage_coord(vec2 uv, float aspect, float scale, float seed){
    vec2 p = vec2(uv.x * aspect * scale + seed * 13.7, uv.y * scale + seed * 7.9);
    float n1 = jfx_mx_noise2(p);
    float n2 = jfx_mx_noise2(vec2(p.x * 2.0 + 17.3 + seed * 3.1, p.y * 2.0 + 9.1)) * 0.5;
    float n3 = jfx_mx_noise2(vec2(p.x * 4.0 + 41.7, p.y * 4.0 + 27.9 + seed * 5.3)) * 0.25;
    float n = (n1 + n2 + n3) / 1.75;
    return clamp(0.5 + n * 0.9, 0.0, 1.0);
}

#ifdef JFX_FILTER_FORM
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_sample.glsl"

// applyReveal on the exact texel of this fragment: the alpha scaled, the colour kept
vec4 fam_reveal_here(float mask){
    vec4 s = jfx_decode(jfx_src_premul_here());
    return vec4(s.rgb, s.a * mask);
}
#endif

#endif // FAM_LIGHT_REVEAL_GLSL
