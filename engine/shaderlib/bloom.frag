#version 450

// The bloom and blur chain (shader library phase 4, tier T2), see bloom.vert. Every target of
// the chain is SRGB8A8: the hardware decodes on sampling (before filtering) and encodes on
// write, so the chain filters in linear light at 8-bit perceptual precision for no shader
// cost. The source of the prefilter is the scene colour (RGBA8, display-encoded); its taps
// are decoded with the gamma-2 approximation c * c, which the uber pass's composites invert
// (bloom: sqrt(c * c + bloom), blur: sqrt(blur)), so a blur of a flat colour is that colour.
//
//   BLOOM_PREFILTER   4 taps of the scene, decoded and averaged (a 4x4 box to 1/4 size)
//   BLOOM_THRESHOLD   with it: only what is brighter than the threshold, with a soft knee
//                     (on the brightest channel of the linear colour)
//   (none)            dual-filter downsample: centre x 4 + 4 corners, / 8
//   BLOOM_UPSAMPLE    dual-filter upsample: 4 axis taps + 4 diagonal taps x 2, / 12
//   BLOOM_MIX         with it: mixed with the same-size level of the way down by the scatter
//                     (0: that level alone, a tight glow; 1: the whole wide blur)
//
// Colour maths is mediump (fp16 where the GPU has it); UVs are highp.

#if !defined(BLOOM_PREFILTER) && (!defined(BLOOM_UPSAMPLE) || defined(BLOOM_MIX))
    #define BLOOM_CENTER
    in highp vec2 v_texcoord;
#endif
in highp vec4 v_taps0;
in highp vec4 v_taps1;
#ifdef BLOOM_UPSAMPLE
    in highp vec4 v_taps2;
    in highp vec4 v_taps3;
#endif

out vec4 frag_color;

uniform texture2D u_sourceTexture;
uniform sampler u_source_smp;
#ifdef BLOOM_MIX
    uniform texture2D u_levelTexture;
    uniform sampler u_level_smp;
#endif

#if (defined(BLOOM_PREFILTER) && defined(BLOOM_THRESHOLD)) || defined(BLOOM_MIX)
    uniform u_fs_bloomParams {
        vec4 curve;  // threshold soft knee: x threshold - knee, y 2 knee, z 0.25 / knee, w threshold (linear)
        vec4 params; // x scatter
    } bloom;
#endif

mediump vec3 tap(highp vec2 uv){
    return texture(sampler2D(u_sourceTexture, u_source_smp), uv).rgb;
}

void main(){
    #ifdef BLOOM_PREFILTER
        mediump vec3 a = tap(v_taps0.xy);
        mediump vec3 b = tap(v_taps0.zw);
        mediump vec3 c = tap(v_taps1.xy);
        mediump vec3 d = tap(v_taps1.zw);
        mediump vec3 color = (a * a + b * b + c * c + d * d) * 0.25;
        #ifdef BLOOM_THRESHOLD
            mediump float brightness = max(color.r, max(color.g, color.b));
            mediump float soft = clamp(brightness - bloom.curve.x, 0.0, bloom.curve.y);
            soft = soft * soft * bloom.curve.z;
            color *= max(soft, brightness - bloom.curve.w) / max(brightness, 1e-4);
        #endif
    #elif defined(BLOOM_UPSAMPLE)
        mediump vec3 color = tap(v_taps0.xy) + tap(v_taps0.zw) + tap(v_taps1.xy) + tap(v_taps1.zw);
        color += (tap(v_taps2.xy) + tap(v_taps2.zw) + tap(v_taps3.xy) + tap(v_taps3.zw)) * 2.0;
        color *= 1.0 / 12.0;
        #ifdef BLOOM_MIX
            mediump vec3 level = texture(sampler2D(u_levelTexture, u_level_smp), v_texcoord).rgb;
            color = mix(level, color, bloom.params.x);
        #endif
    #else
        mediump vec3 color = tap(v_texcoord) * 4.0;
        color += tap(v_taps0.xy) + tap(v_taps0.zw) + tap(v_taps1.xy) + tap(v_taps1.zw);
        color *= 0.125;
    #endif
    frag_color = vec4(color, 1.0);
}
