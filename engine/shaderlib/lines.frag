#version 450

// A fork may declare a u_fs_customParams block: its members are filled by name from the
// component's shader uniforms (Properties window, setShaderUniform). Three names are
// written by the engine instead: "time" (seconds since startup), "sceneTime" (the scene
// clock: scaled game seconds, held while paused) and "resolution" (xy = render target
// size, zw = 1 / size). Do not mix int and float members: GL uploads the block typed
// after its first member.
//
//   uniform u_fs_customParams {
//       float time;
//       vec4 tint;
//   } customParams;
//
// HAS_TEXTURE ('Tex', a ribbon with a texture): the vertex colour times u_baseTexture at the
// ribbon's texture coordinate (includes/lines_fs.glsl lineUV).

out vec4 g_finalColor;

#include "includes/lines_fs.glsl"
// the post-processing colour (USE_POST_COLOR)
#include "includes/post_color.glsl"

#ifdef HAS_TEXTURE
    uniform texture2D u_baseTexture;
    uniform sampler u_base_smp;
#endif

vec4 getLineColor(){
    vec4 color = lineVertexColor();

    #ifdef HAS_TEXTURE
        color *= sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), lineUV()));
    #endif

    return color;
}

void main() {
    vec4 color = getLineColor();

    #ifdef USE_POST_COLOR
        g_finalColor = postColorEncodeBlended(color);
    #else
        g_finalColor = (vec4(linearTosRGB(color.rgb), color.a));
    #endif
}
