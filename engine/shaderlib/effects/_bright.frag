#version 450

// HTML UI effects: the internal bright pass of a "@prepass blur ... threshold <param>" (not an
// effect: no manifest, not listed). The shaders.com kit's bloomExtractSoftKnee (MIT, Copyright
// (c) 2026 Shader Effects Inc.; see effects/LICENSE.shaders-com.txt): the source premultiplied in
// linear light, scaled by the soft-knee mask of its Rec. 709 luminance above the threshold, alpha
// included (what is below the threshold contributes nothing, not a dark halo); premultiplied sRGB
// in and out. Drawn by the engine (EffectRenderer, knee = threshold / 2) as library id
// @effects/_bright, filter form only.
#ifndef JFX_FILTER_FORM
    #error "_bright.frag is a filter-form pass"
#endif

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"

uniform u_fs_effectParams {
    float threshold;    // 0..1, linear luminance
    float knee;         // soft knee width
} params;

void main(){
    vec4 src = texture(sampler2D(u_effectSource, u_effectSource_smp), v_texcoord);
    vec4 s = jfx_decode(src);
    float knee = max(params.knee, 1e-4);
    float lum = jfx_luma709(s.rgb * s.a);
    float soft = clamp(lum - params.threshold + knee, 0.0, 2.0 * knee);
    float softCurve = soft * soft / (4.0 * knee);
    float mask = max(softCurve, lum - params.threshold) / max(lum, 1e-4);
    frag_color = jfx_encode(vec4(s.rgb, s.a * clamp(mask, 0.0, 1.0)));
}
