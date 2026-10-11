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

#ifdef VERTEX_COLOR_WIDTH
    // the vertex colour the width was scaled by (@require rawcolor: the hook's colour needs
    // the attribute, which only a required colour input declares)
    in vec4 v_rawColor;
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

void main(){
    #ifdef VERTEX_COLOR_WIDTH
        // no width there: nothing to outline (a hull lying on the surface would z-fight it)
        if (v_rawColor.r <= 0.0)
            discard;
    #endif
    vec4 color = customParams.outlineColor;
    #ifdef TEXTURE
        vec2 uv = v_uv1 * customParams.textureTiling.xy + customParams.textureTiling.zw;
        color *= sRGBToLinear(texture(sampler2D(u_outlineTexture, u_outline_smp), uv));
    #endif
    g_finalColor = finalizeStraightColor(color);
}
