#version 450
// @effect     voronoi
// @kind       paint
// @category   noise
// @summary    Drifting Voronoi cells with a gradient fill and border lines
// @source     shaders.com Voronoi
// @cost       medium
// @time       speed
// @suggest-for cells worley
// @param      color-a color #3186cf -- Colour at the cell centres
// @param      color-b color #fc02dd -- Colour towards the cell edges
// @param      color-border color #000000 -- Colour of the border lines
// @param      scale number 6 1 20 -- Number of cells across the area's height
// @param      speed number 0.5 0 5 -- Speed of the drifting cell points
// @param      seed number 0 0 100 -- Random seed for the cell layout
// @param      edge-intensity number 0.5 0 1 -- How far the edge colour reaches into each cell
// @param      edge-softness number 0.05 0 0.4 -- Width and softness of the border lines (0 none)
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the fill

// Ported from shaders.com Voronoi (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (borderOverlay(rampOver(cellFill(cellDistances)), cellBorders): aspectPlane, the drifting
// nearest-2 fold (voronoiCells), cellRatio, cellEdgeMask).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_noise.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    vec4 colorBorder;
    float scale;
    float speed;
    float seed;
    float edgeIntensity;
    float edgeSoftness;
    float colorSpace;
} params;

vec4 jfx_paint(JfxContext c){
    vec2 pos = vec2(c.uv.x * c.aspect, c.uv.y) * params.scale;
    vec2 dd = fam_voronoi_nearest2(pos, c.anim.x, params.seed);
    // cellFill: the F1/F2 ratio, its exponent from the edge intensity
    float safeSum = max(dd.x + dd.y, 0.0001);
    float gradientPower = 4.0 - params.edgeIntensity * 3.0;
    float fill = clamp(pow(clamp(dd.x * 2.0 / safeSum, 0.0, 1.0), gradientPower), 0.0, 1.0);
    vec4 base = jfx_mix_colors(params.colorA, params.colorB, fill, params.colorSpace);
    // cellBorders: the line mask, its softness compensated for the cell count (0 on a border)
    float edgeMetric = (dd.y - dd.x) / safeSum;
    float mask = smoothstep(0.0, params.edgeSoftness * params.scale / 6.0 + 0.001, edgeMetric);
    return vec4(mix(params.colorBorder.rgb, base.rgb, mask), base.a);
}

#include "effects/lib/main_paint.glsl"
