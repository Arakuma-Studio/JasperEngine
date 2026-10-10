#version 450

// @shader mesh
// @name "Directional Tint"
// @category "Stylized"
// @description "Kenney's directional tint: one colour per world axis, blended by the world normal, times the base map. Unlit; set all three colours to white for no tint."
// @require normals uv1
// @state blend=opaque
// @property color color (1, 1, 1, 1) "Colour (multiplies all three)"
// @property colorA color (1, 0.78, 0.25, 1) "Colour A (faces along Y: top, bottom)"
// @property colorB color (0.56, 0.33, 0.86, 1) "Colour B (faces along X)"
// @property colorC color (0.92, 0.26, 0.3, 1) "Colour C (faces along Z)"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @texture u_baseTexture white "Base map"

// Kenney's "Directional Tint" (Unlit/Directional Tint), the same maths: with w the squared
// components of the normalised world normal, the tint is
//     mix(mix(mix(A, B, w.x), A, w.y), C, w.z)
// where A = color * colorA, B = color * colorB and C = color * colorC; the output is the
// base map times the tint, alpha 1. White in every colour leaves the base map unchanged.

uniform u_fs_customParams {
    vec4 color;
    vec4 colorA;
    vec4 colorB;
    vec4 colorC;
    vec4 tiling;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

in vec3 v_normal;
in vec2 v_uv1;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    vec3 n = normalize(v_normal);
    vec3 w = n * n;

    vec4 a = customParams.color * customParams.colorA;
    vec4 b = customParams.color * customParams.colorB;
    vec4 c = customParams.color * customParams.colorC;
    vec4 tint = mix(mix(mix(a, b, w.x), a, w.y), c, w.z);

    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    vec4 base = sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv));

    g_finalColor = finalizeColor(vec4((base * tint).rgb, 1.0));
}
