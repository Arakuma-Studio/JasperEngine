// HTML UI effects, filter family (p5-fx-filters): how the filters read the content. Ported from
// the shaders.com kit (MIT, Copyright (c) 2026 Shader Effects Inc.; see
// effects/LICENSE.shaders-com.txt): sampling.ts (catmullRomAxisPositions/Weights and the 9-tap
// sampleCatmullRomExpr), edges.ts (sampleRemappedExpr: Catmull-Rom for the stretch, transparent
// and wrap modes, bilinear for mirror) and the premultiplied tap sums of the gathers
// (blur.ts unrolledTapGather).
//
// Positions are CSS px of the effect area; results are straight-alpha linear sRGB like
// jfx_src. The Catmull-Rom footprint is laid on the layer's texels (device px) and its taps stay
// inside the area and the valid source region, as the kit's clamp sampler keeps them on the canvas.
#ifndef FAM_FILTERS_SAMPLE_GLSL
#define FAM_FILTERS_SAMPLE_GLSL

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"
#include "effects/lib/jfx_geom.glsl"
#include "effects/lib/jfx_sample.glsl"

// edge modes as jfx_edge_uv numbers them
#define FAM_EDGE_STRETCH 0.0
#define FAM_EDGE_TRANSPARENT 1.0
#define FAM_EDGE_MIRROR 2.0
#define FAM_EDGE_WRAP 3.0

// an `edges` enum declared mirror|stretch|transparent|wrap (fluted-glass, flow-field: the source's
// default first) as jfx_edge_uv's mode
float fam_edge_mode_mirror_first(float option){
    if (option < 0.5)
        return FAM_EDGE_MIRROR;
    if (option < 1.5)
        return FAM_EDGE_STRETCH;
    if (option < 2.5)
        return FAM_EDGE_TRANSPARENT;
    return FAM_EDGE_WRAP;
}

// a position held half a device pixel inside the area (the clamp sampler's last texel centre)
vec2 fam_clamp_area(vec2 localPx){
    vec2 inset = vec2(0.5 / max(frame.time.z, 1e-3));
    return clamp(localPx, min(inset, frame.size.xy * 0.5), max(frame.size.xy - inset, frame.size.xy * 0.5));
}

// the source clamped to the area (the kit's linearClamp sampler at the canvas border)
vec4 fam_src_clamped(vec2 localPx){
    return jfx_src(fam_clamp_area(localPx));
}

// a premultiplied linear tap clamped to the area: the gathers sum these (the kit's RTT holds
// premultiplied linear colour) and unpremultiply the sum
vec4 fam_src_premul(vec2 localPx){
    return jfx_premultiply(fam_src_clamped(localPx));
}

// ---- Catmull-Rom (sampling.ts)

// catmullRomAxisPositions: (pos0, pos12, pos3) in uv of one axis of texSize texels
vec3 fam_cr_positions(float uv, float texSize){
    float samplePos = uv * texSize;
    float texPos1 = floor(samplePos - 0.5) + 0.5;
    float f = samplePos - texPos1;
    float w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float w12 = max(w1 + w2, 1e-6);
    float offset12 = w2 / w12;
    return vec3(texPos1 - 1.0, texPos1 + offset12, texPos1 + 2.0) / texSize;
}

// catmullRomAxisWeights: (w0, w1 + w2, w3)
vec3 fam_cr_weights(float uv, float texSize){
    float samplePos = uv * texSize;
    float texPos1 = floor(samplePos - 0.5) + 0.5;
    float f = samplePos - texPos1;
    float w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float w3 = f * f * (-0.5 + 0.5 * f);
    return vec3(w0, w1 + w2, w3);
}

// the layer uv rectangle the taps may read: the area's bounds in the layer, within the valid source
vec4 fam_cr_tap_rect(){
    vec2 a = jfx_local_to_layer_uv(vec2(0.0));
    vec2 b = jfx_local_to_layer_uv(vec2(frame.size.x, 0.0));
    vec2 c = jfx_local_to_layer_uv(vec2(0.0, frame.size.y));
    vec2 d = jfx_local_to_layer_uv(frame.size.xy);
    vec2 inset = 0.5 * frame.layer.zw;
    vec2 lo = max(min(min(a, b), min(c, d)) + inset, frame.srcRect.xy);
    vec2 hi = min(max(max(a, b), max(c, d)) - inset, frame.srcRect.zw);
    return vec4(lo, max(lo, hi));
}

// sampleCatmullRomExpr ('positive' range): 9 bilinear taps of the layer's premultiplied texels
// around the position, the undershoot cut at 0, then decoded
vec4 fam_src_cr(vec2 localPx){
    vec2 uv = jfx_local_to_layer_uv(localPx);
    vec2 texSize = frame.layer.xy;
    vec3 px = fam_cr_positions(uv.x, texSize.x);
    vec3 py = fam_cr_positions(uv.y, texSize.y);
    vec3 wx = fam_cr_weights(uv.x, texSize.x);
    vec3 wy = fam_cr_weights(uv.y, texSize.y);
    vec4 rect = fam_cr_tap_rect();
    vec4 sum = vec4(0.0);
    for (int yi = 0; yi < 3; yi++){
        float ty = yi == 0 ? py.x : (yi == 1 ? py.y : py.z);
        float wty = yi == 0 ? wy.x : (yi == 1 ? wy.y : wy.z);
        for (int xi = 0; xi < 3; xi++){
            float tx = xi == 0 ? px.x : (xi == 1 ? px.y : px.z);
            float wtx = xi == 0 ? wx.x : (xi == 1 ? wx.y : wx.z);
            vec2 tap = clamp(vec2(tx, ty), rect.xy, rect.zw);
            sum += texture(sampler2D(u_effectSource, u_effectSource_smp), tap) * (wtx * wty);
        }
    }
    return jfx_decode(max(sum, vec4(0.0)));
}

// sampleRemappedExpr: a remapped position read with an edge mode (FAM_EDGE_*), Catmull-Rom
// except in the mirror mode (bilinear, as the kit has no mirror-repeat sampler)
vec4 fam_src_remapped(vec2 localPx, float mode){
    if (mode > 1.5 && mode < 2.5)
        return jfx_src_edge(localPx, mode);
    vec2 uv = localPx * frame.size.zw;
    vec4 s = fam_src_cr(jfx_edge_uv(uv, mode) * frame.size.xy);
    s.a *= jfx_edge_alpha(uv, mode);
    return s;
}

// a bilinear tap with an edge mode that clips on the transparent mode (warpMaps.ts
// edgeClipSample: out of the area the whole tap is cleared, so a channel recombination never
// picks up a clamped colour)
vec4 fam_src_edge_clip(vec2 localPx, float mode){
    vec4 s = jfx_src_edge(localPx, mode);
    return s.a > 0.0 ? s : vec4(0.0);
}

#endif // FAM_FILTERS_SAMPLE_GLSL
