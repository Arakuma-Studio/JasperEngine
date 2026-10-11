// The built-in Lit shader's per-camera block (fragment stage, lit variants): the members of
// the engine frame block Lit reads, packed into at most 6 vectors (no matrices, time or
// main light). A variant declares the prefix it reads, so a draw uploads only that:
// 2 vectors (camera position, ambient), 3 with fog, 4 with shadows, 5 with SSAO, 6 with
// IBL; the engine uploads the reflected size. Filled with the frame block by
// RenderSystem::updatePassFrame (lit_frame_t in RenderSystem.h). Engine-internal: library
// shaders use includes/frame.glsl.
//
// includes/lights.glsl (shadows), includes/shadows.glsl, includes/ibl.glsl and
// includes/fog.glsl read it when LIT_FRAME_DECLARED is defined, the frame block otherwise.

#ifndef LIT_FRAME_DECLARED
#define LIT_FRAME_DECLARED

uniform u_fs_litFrame {
    vec4 cameraPosition_fogEnd;     // xyz camera world position, w fog linear end
    vec4 ambient_fogType;           // rgb scene ambient, w fog type (0 linear, 1 exp, 2 exp2)
    #if defined(HAS_FOG) || defined(USE_SHADOWS) || defined(USE_SSAO) || defined(USE_IBL)
        vec4 fogColor_param;        // rgb fog colour, w fog linear start (linear) or density
    #endif
    #if defined(USE_SHADOWS) || defined(USE_SSAO) || defined(USE_IBL)
        vec4 cameraForward_pcf;     // xyz camera world forward, w 3D shadow PCF tap radius
    #endif
    #if defined(USE_SSAO) || defined(USE_IBL)
        vec4 viewport_ssao_envRotation; // xy 1 / viewport size, z SSAO flags (1 debug view, 2 Y flip), w environment rotation (radians)
    #endif
    #if defined(USE_IBL)
        vec4 environmentColor;      // rgb environment colour (linear)
    #endif
} litFrame;

#endif
