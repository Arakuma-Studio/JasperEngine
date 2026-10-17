#version 450
// @effect     water-ripple
// @kind       filter
// @category   distort
// @summary    Concentric water ripples spreading from a point through the content
// @source     original (Ripples' ring math as a displacement; CursorRipples' chromatic split)
// @cost       low
// @time       speed
// @suggest-for ripple water
// @param      center position "50% 50%" -- Point the ripples spread from
// @param      amplitude length 6px 0px 64px -- Largest push of a ripple
// @param      wavelength length 40px 4px 400px -- Distance between two crests
// @param      speed number 1 -5 5 -- Wavelengths per second the ripples travel outward (inward when negative)
// @param      decay number 0.5 0 5 -- How fast the ripples fade with distance (per 100 CSS px)
// @param      chromatic-split number 0 0 3 -- Colour separation along the ripples
// @param      edges enum stretch stretch|transparent|mirror|wrap -- What shows where the ripples read beyond the element

// After the shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): Ripples' rings, sin(distance x frequency - clock), here
// travelling outward a wavelength per second times `speed` on the integrated clock, as a surface
// whose radial slope pushes the content (amplitude px at a crest's slope), fading as
// exp(-decay x distance / 100 px); CursorRipples' chromaticDisplaceUVs splits red and blue
// along the push (split x 0.1). One tap reads Catmull-Rom (bilinear when mirrored), the split's
// three bilinear, as the kit's samplers.

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    vec2 center;
    float amplitude;
    float wavelength;
    float speed;
    float decay;
    float chromaticSplit;
    float edges;
} params;

vec4 jfx_filter(JfxContext c){
    vec2 d = c.px - params.center * c.size;
    float r = length(d);
    float wavelength = max(params.wavelength, 1.0);
    float slope = cos(JFX_TAU * (r / wavelength - c.anim.x));
    vec2 disp = (r > 1e-3 ? d / r : vec2(0.0)) * (params.amplitude * slope * exp(-params.decay * r / 100.0));
    if (params.chromaticSplit <= 0.0)
        return fam_src_remapped(c.px - disp, params.edges);
    float chromaticScale = params.chromaticSplit * 0.1;
    vec4 red = fam_src_edge_clip(c.px - disp * (1.0 + chromaticScale), params.edges);
    vec4 green = fam_src_edge_clip(c.px - disp, params.edges);
    vec4 blue = fam_src_edge_clip(c.px - disp * (1.0 - chromaticScale), params.edges);
    return vec4(red.r, green.g, blue.b, green.a);
}

#include "effects/lib/main_filter.glsl"
