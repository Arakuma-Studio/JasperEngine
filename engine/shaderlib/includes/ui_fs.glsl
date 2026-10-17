// The colour of a UI element for library UI shaders (@shader ui), as the built-in ui.frag
// computes it: the element's colour (u_fs_uiParams, sRGB-edited, uploaded linear) times
// its texture (HAS_TEXTURE, an Image) or the font atlas coverage (HAS_FONTATLAS_TEXTURE,
// Text: alpha from the red channel), times the vertex colour. Declares the inputs
// includes/ui_vs.glsl writes (except v_localPosition) and u_uiTexture, which the engine
// binds by name. Fragment stage.

#ifndef UI_FS_GLSL
#define UI_FS_GLSL

#include "includes/srgb.glsl"

uniform u_fs_uiParams {
    vec4 color;
} uiParams;

#ifdef HAS_VERTEX_COLOR_VEC3
    in vec3 v_color;
#endif
#ifdef HAS_VERTEX_COLOR_VEC4
    in vec4 v_color;
#endif

#if defined(HAS_TEXTURE) || defined(HAS_FONTATLAS_TEXTURE)
    uniform texture2D u_uiTexture;
    uniform sampler u_ui_smp;
    in vec2 v_uv1;
#endif

// linear colour, straight alpha
vec4 uiBaseColor(){
    vec4 color = uiParams.color;
    #ifdef HAS_TEXTURE
        color *= sRGBToLinear(texture(sampler2D(u_uiTexture, u_ui_smp), v_uv1));
    #endif
    #ifdef HAS_FONTATLAS_TEXTURE
        color.a *= texture(sampler2D(u_uiTexture, u_ui_smp), v_uv1).r;
    #endif
    #ifdef HAS_VERTEX_COLOR_VEC3
        color.rgb *= v_color;
    #endif
    #ifdef HAS_VERTEX_COLOR_VEC4
        color *= v_color;
    #endif
    return color;
}

#endif
