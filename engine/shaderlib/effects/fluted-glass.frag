#version 450
// @effect     fluted-glass
// @kind       filter
// @category   glass
// @summary    Reeded glass: what is behind refracted through rows of cylindrical flutes
// @source     shaders.com FlutedGlass
// @cost       medium
// @backdrop   yes
// @time       speed
// @suggest-for reeded-glass ribbed-glass
// @param      shape enum bars bars|rounded|waves -- Cross-section of a flute
// @param      angle angle 0deg 0deg 360deg -- Direction of the flutes (0deg vertical bars)
// @param      frequency number 10 1 20 -- Flutes per distance of the area height
// @param      softness number 0.5 0 1 -- From flat middles and sharp seams (0) to a gentle curve (1)
// @param      wave-amplitude number 0.06 0 0.5 -- How far a flute sways (waves)
// @param      wave-frequency number 1.5 0.1 10 -- Sways along a flute (waves)
// @param      speed number 0 -1 1 -- Drift of the flutes and their waves
// @param      refraction number 1.5 0 4 -- How hard a flute bends what is behind
// @param      aberration number 0.2 0 1 -- Colour split at the seams
// @param      light-angle angle 30deg -90deg 90deg -- Light direction (0deg head-on, 90deg grazing)
// @param      highlight number 0.2 0 2 -- Strength of the specular streak on each flute
// @param      highlight-softness number 0.3 0 1 -- Spread of the streak
// @param      highlight-color color #ffffff -- Colour of the streak
// @param      edges enum mirror mirror|stretch|transparent|wrap -- What shows where a flute reads beyond the element

// Ported from shaders.com FlutedGlass (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/blurs.ts fluteFrame, refractedTaps, fluteHighlight: warpMaps.ts flutedGlassGeom,
// edgeClipSample, rgbSplitTaps, blinnHighlight). The clock is negated so a positive speed drifts
// right at 0deg.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_sample.glsl"
#include "effects/lib/fam_filters_glass.glsl"

uniform u_fs_effectParams {
    vec4 highlightColor;
    float shape;
    float angle;
    float frequency;
    float softness;
    float waveAmplitude;
    float waveFrequency;
    float speed;
    float refraction;
    float aberration;
    float lightAngle;
    float highlight;
    float highlightSoftness;
    float edges;
} params;

vec4 jfx_filter(JfxContext c){
    FamFluteGeom g = fam_flute_geom(c.uv, c.aspect, -c.anim.x, params.angle, params.frequency, params.softness, params.waveAmplitude,
                                    params.waveFrequency, params.refraction, params.aberration, params.shape);
    float mode = fam_edge_mode_mirror_first(params.edges);
    // rgbSplitTaps over the edge-clipped premultiplied taps
    vec4 gTap = jfx_premultiply(fam_src_edge_clip(g.refractedUV * c.size, mode));
    float r = jfx_premultiply(fam_src_edge_clip((g.refractedUV + g.chrOff) * c.size, mode)).r;
    float b = jfx_premultiply(fam_src_edge_clip((g.refractedUV - g.chrOff) * c.size, mode)).b;
    vec4 color = jfx_unpremultiply(vec4(r, gTap.g, b, gTap.a));
    // fluteHighlight, weighted by the alpha
    float spec = fam_blinn_highlight(g.slope, params.lightAngle, params.highlight, params.highlightSoftness);
    return vec4(color.rgb + params.highlightColor.rgb * (spec * color.a), color.a);
}

#include "effects/lib/main_filter.glsl"
