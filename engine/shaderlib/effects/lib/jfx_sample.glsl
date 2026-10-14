// HTML UI effects: reading the filtered pixels (filter form only). Positions are CSS px in the
// effect area; the samples are straight-alpha linear sRGB, clamped to the valid source region.
#ifndef JFX_SAMPLE_GLSL
#define JFX_SAMPLE_GLSL

#ifndef JFX_FILTER_FORM
    #error "effects/lib/jfx_sample.glsl reads the filtered pixels: filter form only"
#endif

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_geom.glsl"

// the exact texel at this fragment, premultiplied sRGB as the layer holds it
vec4 jfx_src_premul_here(){
    return texture(sampler2D(u_effectSource, u_effectSource_smp), v_texcoord);
}

vec2 jfx_source_uv(vec2 localPx){
    return clamp(jfx_local_to_layer_uv(localPx), frame.srcRect.xy, frame.srcRect.zw);
}

// the source at a position of the area (straight linear)
vec4 jfx_src(vec2 localPx){
    return jfx_decode(texture(sampler2D(u_effectSource, u_effectSource_smp), jfx_source_uv(localPx)));
}

// the source with an edge mode relative to the effect area (jfx_edge_uv)
vec4 jfx_src_edge(vec2 localPx, float mode){
    vec2 uv = localPx * frame.size.zw;
    vec4 s = jfx_src(jfx_edge_uv(uv, mode) * frame.size.xy);
    s.a *= jfx_edge_alpha(uv, mode);
    return s;
}

// the blurred copy of the source (@prepass blur; the source itself without a prepass)
vec4 jfx_blurred(vec2 localPx){
    return jfx_decode(texture(sampler2D(u_effectBlurred, u_effectBlurred_smp), jfx_source_uv(localPx)));
}

#endif // JFX_SAMPLE_GLSL
