#version 450
// @effect     chromatic-aberration
// @kind       filter
// @category   stylize
// @summary    Red, green and blue copies of the content pushed apart along an angle
// @source     shaders.com ChromaticAberration
// @cost       low
// @suggest-for rgb-split rgb-shift chroma
// @param      strength number 0.2 0 1 -- Overall shift (1 moves a channel a tenth of the area)
// @param      angle angle 0deg 0deg 360deg -- Direction the channels are pushed (0deg along x)
// @param      red-offset number -1 -2 2 -- Red channel's share of the shift
// @param      green-offset number 0 -2 2 -- Green channel's share of the shift (it also carries the alpha)
// @param      blue-offset number 1 -2 2 -- Blue channel's share of the shift

// Ported from shaders.com ChromaticAberration (MIT, Copyright (c) 2026 Shader Effects Inc.); see effects/LICENSE.shaders-com.txt
// (std/effects/lens.ts chromaticFan: three chromaticOffsetUV taps recombined per channel, the
// alpha from the green tap, premultiplied as the kit's RTT; taps beyond the area repeat its
// border as the kit's clamp sampler does).

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_sample.glsl"
#include "effects/lib/fam_filters_common.glsl"
#include "effects/lib/fam_filters_sample.glsl"

uniform u_fs_effectParams {
    float strength;
    float angle;
    float redOffset;
    float greenOffset;
    float blueOffset;
} params;

vec4 jfx_filter(JfxContext c){
    vec4 r = fam_src_premul(fam_chromatic_offset_uv(c.uv, params.angle, params.strength, c.aspect, params.redOffset) * c.size);
    vec4 g = fam_src_premul(fam_chromatic_offset_uv(c.uv, params.angle, params.strength, c.aspect, params.greenOffset) * c.size);
    vec4 b = fam_src_premul(fam_chromatic_offset_uv(c.uv, params.angle, params.strength, c.aspect, params.blueOffset) * c.size);
    return jfx_unpremultiply(vec4(r.r, g.g, b.b, g.a));
}

#include "effects/lib/main_filter.glsl"
