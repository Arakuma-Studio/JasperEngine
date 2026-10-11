// The fragment side of library:outline and library:outline_stencil (their .frag include
// only this, after their manifests): one colour (HDR, straight alpha), optionally times a
// texture (TEXTURE) at the mesh's first UV set. No lighting: a flat outline.

uniform u_fs_customParams {
    vec4 outlineColor;
    #ifdef TEXTURE
        vec4 textureTiling;
    #endif
} customParams;

#ifdef TEXTURE
    uniform texture2D u_outlineTexture;
    uniform sampler u_outline_smp;

    in vec2 v_uv1;
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

void main(){
    vec4 color = customParams.outlineColor;
    #ifdef TEXTURE
        vec2 uv = v_uv1 * customParams.textureTiling.xy + customParams.textureTiling.zw;
        color *= sRGBToLinear(texture(sampler2D(u_outlineTexture, u_outline_smp), uv));
    #endif
    g_finalColor = finalizeStraightColor(color);
}
