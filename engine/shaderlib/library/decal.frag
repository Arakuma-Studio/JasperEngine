#version 450

// @shader mesh
// @name "Decal"
// @category "Effects"
// @description "Mesh decal: texture times tint on a quad laid on a surface, alpha blended, pulled towards the camera by a depth offset so it does not z-fight. DECAL_BLEND MULTIPLY darkens what is under it (dirt, fake shadows): set the material's blend mode to Multiply."
// @require uv1 color
// @state blend=alpha zwrite=off offset=-1,-1
// @property tint color (1, 1, 1, 1) "Tint"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property opacity range(0, 1) 1 "Opacity"
// @texture u_baseTexture white "Decal"
// @feature VERTEX_COLOR "Vertex colour"
// @keywords DECAL_BLEND "Blend" ALPHA MULTIPLY

// ALPHA writes texture * tint with its alpha (times opacity) for the alpha blend. MULTIPLY
// writes the colour faded to white by that alpha, for the Multiply blend mode (the
// material sets it: the default state cannot follow the keyword), so the transparent parts
// leave the surface as it is; the fog fades it to white too. Unlit: an ALPHA decal does
// not darken in shadow, a MULTIPLY one takes the surface's lighting. The depth offset
// (-1, -1: slope and constant) is the material's to tune for distant or steep decals.

uniform u_fs_customParams {
    vec4 tint;
    vec4 tiling;
    float opacity;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

in vec2 v_uv1;
in vec4 v_color; // the instance colour, times the vertex colour with VERTEX_COLOR

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    vec4 color = customParams.tint * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;
    color.a *= customParams.opacity;

    #ifdef DECAL_BLEND_MULTIPLY
        g_finalColor = finalizeBlendedColor(vec4(mix(vec3(1.0), color.rgb, color.a), 1.0), vec3(1.0));
    #else
        g_finalColor = finalizeColor(color);
    #endif
}
