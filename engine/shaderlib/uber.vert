#version 450

// The uber pass (shader library phase 4, tier T1): one fullscreen triangle reading the scene
// colour and writing the destination, the blit's job (fixed resolution, render scale) with the
// post-processing effects its variant compiles in (uber.frag, RenderSystem::UberFeature). The
// vertex stage computes the tap coordinates it can: the source UV, the output position, and
// the four corner taps FXAA, sharpen and edge detect share (half a source texel off the
// centre, so each bilinear tap averages 2x2 texels), unless the fragment stage warps the UV
// first (pixelate, CRT curvature).

#if defined(UBER_FXAA) || defined(UBER_SHARPEN) || defined(UBER_EDGE)
    #define UBER_CORNERS
#endif
#if defined(UBER_PIXELATE) || defined(UBER_CRT)
    #define UBER_WARP
#endif

uniform u_vs_uberParams {
    vec4 uvScaleOffset; // source UV = position * xz + yw (the blit's convention)
    vec4 sourceTexel;   // xy 1 / source size, zw source size
} uberVS;

// the source UV, and the output position 0..1 (y down on every backend but GL, as v_texcoord)
out highp vec2 v_texcoord;
out highp vec2 v_screen;
#if defined(UBER_CORNERS) && !defined(UBER_WARP)
    // xy north-west, zw south-east corner of the centre texel
    out highp vec4 v_corners;
#endif

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifndef IS_GLSL
        // only GL stores destination row 0 at clip y = -1 (fullscreen.vert)
        uv.y = 1.0 - uv.y;
    #endif
    v_screen = uv;
    v_texcoord = uv * uberVS.uvScaleOffset.xz + uberVS.uvScaleOffset.yw;
    #if defined(UBER_CORNERS) && !defined(UBER_WARP)
        vec2 halfTexel = 0.5 * uberVS.sourceTexel.xy;
        v_corners = vec4(v_texcoord - halfTexel, v_texcoord + halfTexel);
    #endif
    #ifdef IS_VULKAN
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
