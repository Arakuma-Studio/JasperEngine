#version 450

// HTML UI effects: the internal bright pass of a "@prepass blur ... threshold <param>" (not an
// effect: no manifest, not listed). What lies above the threshold of the source's brightness
// (its largest linear channel), with a soft knee; premultiplied sRGB in and out. Drawn by the
// engine (EffectRenderer) as library id @effects/_bright, filter form only.
#ifndef JFX_FILTER_FORM
    #error "_bright.frag is a filter-form pass"
#endif

#include "effects/lib/jfx_common.glsl"
#include "effects/lib/jfx_color.glsl"

uniform u_fs_effectParams {
    float threshold;    // 0..1, linear brightness
    float knee;         // soft knee width
} params;

void main(){
    vec4 src = texture(sampler2D(u_effectSource, u_effectSource_smp), v_texcoord);
    vec4 s = jfx_decode(src);
    float brightness = max(max(s.r, s.g), s.b);
    float knee = max(params.knee, 1e-4);
    float soft = clamp(brightness - params.threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contribution = max(soft, brightness - params.threshold) / max(brightness, 1e-4);
    frag_color = jfx_encode(vec4(s.rgb * clamp(contribution, 0.0, 1.0), s.a));
}
