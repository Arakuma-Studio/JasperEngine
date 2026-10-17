#version 450

// The bloom and blur chain (shader library phase 4, tier T2): fullscreen passes between small
// SRGB8A8 targets (RenderSystem::renderBloomChain). The vertex stage computes every tap UV of
// the pass, so the fragment stage makes no dependent reads:
//
//   prefilter (BLOOM_PREFILTER): the scene to 1/4 of its size, 4 taps at the centres of the
//     four 2x2 quadrants of the 4x4 source texels each output texel covers (a 4x4 box)
//   downsample (no define): dual filter, the centre and the 4 diagonal corners
//   upsample (BLOOM_UPSAMPLE): dual filter, 4 taps on the axes and 4 on the diagonals
//
// offset.xy is the tap distance in source UV (the engine's: one texel of the larger image of
// the pass, times the radius), so each bilinear tap averages a 2x2 block.

// the centre UV: the downsample's centre tap and the mixed level's (the outputs match the
// fragment stage's inputs)
#if !defined(BLOOM_PREFILTER) && (!defined(BLOOM_UPSAMPLE) || defined(BLOOM_MIX))
    #define BLOOM_CENTER
#endif

uniform u_vs_bloomParams {
    vec4 offset; // xy tap distance in source UV
} bloomVS;

#ifdef BLOOM_CENTER
    out highp vec2 v_texcoord;
#endif
out highp vec4 v_taps0;
out highp vec4 v_taps1;
#ifdef BLOOM_UPSAMPLE
    out highp vec4 v_taps2;
    out highp vec4 v_taps3;
#endif

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifndef IS_GLSL
        // only GL stores destination row 0 at clip y = -1 (fullscreen.vert)
        uv.y = 1.0 - uv.y;
    #endif
    #ifdef BLOOM_CENTER
        v_texcoord = uv;
    #endif
    vec2 o = bloomVS.offset.xy;
    #ifdef BLOOM_UPSAMPLE
        // the axes at twice the distance, the diagonals at it (weights 1 and 2 in the fragment)
        v_taps0 = vec4(uv + vec2(-2.0 * o.x, 0.0), uv + vec2(2.0 * o.x, 0.0));
        v_taps1 = vec4(uv + vec2(0.0, -2.0 * o.y), uv + vec2(0.0, 2.0 * o.y));
        v_taps2 = vec4(uv + vec2(-o.x, -o.y), uv + vec2(o.x, o.y));
        v_taps3 = vec4(uv + vec2(-o.x, o.y), uv + vec2(o.x, -o.y));
    #else
        // the four diagonal corners (the prefilter's quadrant centres)
        v_taps0 = vec4(uv - o, uv + o);
        v_taps1 = vec4(uv + vec2(-o.x, o.y), uv + vec2(o.x, -o.y));
    #endif
    #ifdef IS_VULKAN
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
