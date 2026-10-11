#version 450

// @shader mesh
// @name "Decal"
// @category "Effects"
// @description "Mesh decal: texture times tint on a quad laid on a surface, alpha blended, pulled towards the camera by a depth offset so it does not z-fight; no shadows. Default blend Multiply darkens what is under it instead (dirt, fake shadows)."
// @require uv1 color
// @state blend=alpha zwrite=off offset=-1,-1 shadows=off
// @state[DECAL_BLEND_MULTIPLY] blend=multiply
// @property tint color (1, 1, 1, 1) "Tint"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property opacity range(0, 1) 1 "Opacity"
// @texture u_baseTexture white "Decal"
// @feature VERTEX_COLOR "Vertex colour"
// @keywords DECAL_BLEND "Default blend" ALPHA MULTIPLY

// texture * tint, its alpha times opacity, in whatever blend mode the variant draws with
// (finalizeColor): alpha blends it over the surface; multiply (the default with
// DECAL_BLEND_MULTIPLY, @state[...]) writes the colour faded to white by that alpha, so the
// transparent parts leave the surface as it is, and the fog fades it to white too. Unlit:
// an alpha decal does not darken in shadow, a multiply one takes the surface's lighting.
// The depth offset (-1, -1: slope and constant) is the material's to tune for distant or
// steep decals. No shadows: it lies on a surface that casts its own.

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

    g_finalColor = finalizeColor(color);
}
