#version 450

// @shader mesh
// @name "Unlit"
// @category "Basic"
// @description "Base colour times base map, optionally times vertex colour, without lighting; optional alpha clip, whose shadows are cut out too."
// @require uv1 color
// @state blend=opaque
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property alphaCutoff range(0, 1) 0.5 "Alpha cutoff" feature=ALPHA_CLIP
// @texture u_baseTexture white "Base map"
// @feature VERTEX_COLOR "Vertex colour"
// @feature ALPHA_CLIP "Alpha clip"
// @depth

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
    float alphaCutoff;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

in vec2 v_uv1;
in vec4 v_color; // the instance colour, times the vertex colour with VERTEX_COLOR

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    vec4 color = customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;

    #ifdef ALPHA_CLIP
        if (color.a < customParams.alphaCutoff){
            discard;
        }
    #endif

    g_finalColor = finalizeColor(color);
}
