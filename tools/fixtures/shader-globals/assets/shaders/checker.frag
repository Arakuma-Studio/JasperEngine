#version 450

// @shader mesh
// @name "Global texture"
// @description "Shader globals fixture: samples the _CheckerTex texture global."
// @require uv1

uniform texture2D _CheckerTex;
uniform sampler _CheckerTex_smp;

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    g_finalColor = finalizeColor(sRGBToLinear(texture(sampler2D(_CheckerTex, _CheckerTex_smp), v_uv1)));
}
