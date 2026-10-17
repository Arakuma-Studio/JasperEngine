#version 450
// @effect     hex-grid
// @kind       paint
// @category   pattern
// @summary    Honeycomb lines (pointy-top hexagons) over a cell colour
// @source     shaders.com HexGrid
// @cost       low
// @suggest-for honeycomb hexagons
// @param      color-a color #000000 -- Colour of the cells
// @param      color-b color #ffffff -- Colour of the lines
// @param      cells number 8 1 40 -- Cells along the area's height
// @param      thickness number 1 0 10 -- Line thickness (1 is about 2% of a cell)
// @param      rotation angle 0deg 0deg 360deg -- Rotation of the grid about the centre (clockwise)
// @param      softness number 0 0 1 -- Blurs the lines
// @param      variation number 0 0 1 -- How much the cells' brightness varies
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the line/cell mix

// Ported from shaders.com HexGrid (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (strokeOver(vary(colorA), colorB, hexLines(cellFrame clockwise))): latticeFrameCW, hexTiling,
// hexGridField, strokeBandFloored, cellVariation).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_geom.glsl"
#include "effects/lib/fam_paint_common.glsl"
#include "effects/lib/fam_paint_pattern.glsl"

uniform u_fs_effectParams {
    vec4 colorA;
    vec4 colorB;
    float cells;
    float thickness;
    float rotation;
    float softness;
    float variation;
    float colorSpace;
} params;

#define SQRT3 1.7320508075688772
#define SQRT3_2 0.8660254037844386
#define INV_SQRT3_2 1.1547005383792515

vec4 jfx_paint(JfxContext c){
    // latticeFrameCW: aspect-corrected, y down, turned clockwise about the centre, scaled
    vec2 correctedUV = vec2(c.uv.x * c.aspect, c.uv.y);
    float ang = -params.rotation;
    vec2 scaledUV = fam_rotate_about_centre(correctedUV, c.aspect, cos(ang), sin(ang)) * params.cells;

    // hexTiling, period (sqrt 3, 1): the nearer of the two offset lattices' centres
    vec2 s = vec2(SQRT3, 1.0);
    vec2 sh = s * 0.5;
    vec2 a = jfx_fmod(scaledUV, s) - sh;
    vec2 b = jfx_fmod(scaledUV - sh, s) - sh;
    vec2 gv = dot(a, a) < dot(b, b) ? a : b;
    vec2 id = scaledUV - gv;

    // per-cell variation of the integer cell indices (WGSL round: half to even)
    float varied = fam_cell_variation(vec2(roundEven(id.x * INV_SQRT3_2), roundEven(id.y * 2.0)), params.variation);

    // the hex edge distance and strokeBandFloored
    vec2 pAbs = abs(gv);
    float hexDist = max(dot(pAbs, vec2(SQRT3_2, 0.5)), pAbs.y);
    float edgeDist = 0.5 - hexDist;
    float lineWidth = params.thickness * 0.01;
    float pixel = fwidth(scaledUV.x) * 0.5;
    float soft = params.softness * 0.5;
    float lineMask = fam_line_mask(edgeDist, lineWidth, max(lineWidth - soft, 0.0), lineWidth + pixel + soft);

    vec4 fill = vec4(params.colorA.rgb * varied, params.colorA.a);
    return jfx_mix_colors(fill, params.colorB, lineMask, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
