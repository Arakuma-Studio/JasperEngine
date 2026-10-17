#version 450
// @effect     glass-tiles
// @kind       filter
// @category   glass
// @summary    A grid of glass tiles, each refracting its own slice of what is behind
// @source     shaders.com GlassTiles
// @cost       low
// @backdrop   yes
// @param      intensity number 2 0 10 -- Strength of each tile's refraction
// @param      tile-count number 20 5 50 -- Tiles along the longer side
// @param      rotation angle 0deg 0deg 360deg -- Rotation of the tile grid
// @param      roundness number 0 0 1 -- Fades the refraction toward the tile corners, making round lenses

// Ported from shaders.com GlassTiles (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (motionBlur.ts glassTilesUV and glassTilesFragment: the tile-distorted position read with
// Catmull-Rom reconstruction, clamped to the area).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_sample.glsl"
#include "effects/lib/fam_filters_glass.glsl"

uniform u_fs_effectParams {
    float intensity;
    float tileCount;
    float rotation;
    float roundness;
} params;

vec4 jfx_filter(JfxContext c){
    vec2 uv = fam_glass_tiles_uv(c.uv, c.aspect, params.intensity, params.tileCount, params.rotation, params.roundness);
    return fam_src_remapped(uv * c.size, FAM_EDGE_STRETCH);
}

#include "effects/lib/main_filter.glsl"
