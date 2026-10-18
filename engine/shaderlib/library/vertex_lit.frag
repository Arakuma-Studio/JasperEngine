#version 450

// @shader mesh
// @name "Vertex Lit"
// @category "Lit"
// @description "Base colour times base map, lit per vertex: the ambient plus the diffuse of the object's lights. The cheapest lit shader."
// @lighting vertex
// @require uv1 color
// @state blend=opaque
// @property baseColor color (1, 1, 1, 1) "Base colour"
// @property tiling vector4 (1, 1, 0, 0) "Tiling (xy) and offset (zw)"
// @property emissionColor hdrcolor (1, 1, 1, 1) "Emission colour" feature=EMISSION
// @texture u_baseTexture white "Base map"
// @texture u_emissionTexture white "Emission map" feature=EMISSION
// @feature VERTEX_COLOR "Vertex colour"
// @feature EMISSION "Emission"

// Every light of the object is evaluated per vertex (Lambert diffuse with its range and
// cone, no specular, shadows or spot masks); the lights over the cap and the scene ambient
// come in through the SH ambient. Without scene lights the variant is unlit (base colour
// and emission), as the built-in Lit. Light units: includes/lights.glsl.

uniform u_fs_customParams {
    vec4 baseColor;
    vec4 tiling;
    vec4 emissionColor;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color; // the instance colour, times the vertex colour with VERTEX_COLOR

#ifdef USE_VERTEX_LIGHTS
    // the ambient plus the vertex lights' diffuse (includes/surface_vs.glsl)
    in vec3 v_vertexLight;
#endif

#ifdef EMISSION
    uniform texture2D u_emissionTexture;
    uniform sampler u_emission_smp;
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    vec2 uv = v_uv1 * customParams.tiling.xy + customParams.tiling.zw;
    vec4 base = customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), uv)) * v_color;
    vec3 color = base.rgb;

    #ifdef USE_VERTEX_LIGHTS
        color *= v_vertexLight;
    #endif

    #ifdef EMISSION
        color += customParams.emissionColor.rgb * sRGBToLinear(texture(sampler2D(u_emissionTexture, u_emission_smp), uv).rgb);
    #endif

    g_finalColor = finalizeColor(vec4(color, base.a));
}
