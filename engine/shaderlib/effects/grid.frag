#version 450
// @effect     grid
// @kind       paint
// @category   pattern
// @summary    Square grid lines, with an optional per-cell fill
// @source     shaders.com Grid
// @cost       low
// @param      color color #ffffff -- Colour of the lines
// @param      cell-color color transparent -- Colour of the cells (transparent: lines only)
// @param      cells number 10 1 50 -- Cells along the area's height
// @param      thickness number 1 0 20 -- Line thickness (1 is about 2% of a cell)
// @param      rotation angle 0deg 0deg 360deg -- Rotation of the grid about the centre
// @param      softness number 0 0 1 -- Blurs the lines
// @param      variation number 0 0 1 -- How much the cells' brightness varies
// @param      color-space enum linear linear|oklch|oklab|hsl|hsv|lch -- Colour space of the line/cell mix

// Ported from shaders.com Grid (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (strokeOver(vary(cellColor), color, gridLines(cellFrame flippedY))): latticeFrameFlipY,
// gridField (Quilez's line filter over the derivative footprint), cellVariation).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/fam_paint_common.glsl"
#include "effects/lib/fam_paint_pattern.glsl"

uniform u_fs_effectParams {
    vec4 color;
    vec4 cellColor;
    float cells;
    float thickness;
    float rotation;
    float softness;
    float variation;
    float colorSpace;
} params;

vec4 jfx_paint(JfxContext c){
    // latticeFrameFlipY: aspect-corrected, y up, turned about the centre, scaled to the cells
    vec2 correctedUV = vec2(c.uv.x * c.aspect, 1.0 - c.uv.y);
    vec2 gridUV = fam_rotate_about_centre(correctedUV, c.aspect, cos(params.rotation), sin(params.rotation)) * params.cells;

    // gridField: lines straddle each cell boundary (lineWidth per side)
    float lineWidth = params.thickness * 0.01;
    float safeLine = max(lineWidth, 0.0001);
    float N = 1.0 / (safeLine * 2.0);
    vec2 p = gridUV + safeLine;
    vec2 dpx = dFdx(p);
    vec2 dpy = dFdy(p);
    float wx = max(max(abs(dpx.x), abs(dpy.x)), 0.00001) + params.softness;
    float wy = max(max(abs(dpx.y), abs(dpy.y)), 0.00001) + params.softness;
    float iX = fam_quilez_line_axis(p.x, wx, N);
    float iY = fam_quilez_line_axis(p.y, wy, N);
    float gridMask = (1.0 - (1.0 - iX) * (1.0 - iY)) * step(0.0001, lineWidth);
    float varied = fam_cell_variation(floor(gridUV), params.variation);

    vec4 fill = vec4(params.cellColor.rgb * varied, params.cellColor.a);
    return jfx_mix_colors(fill, params.color, gridMask, params.colorSpace);
}

#include "effects/lib/main_paint.glsl"
