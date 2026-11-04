#version 450

// G-buffer geometry pass (fragment), MRT output:
//   color[0] = packed view depth (identical to depth.frag, so SSAO/SSR depth is unchanged)
//   color[1] = view-space normal (octahedral .rg) + roughness (.b) + metallic (.a)
//   color[2] = linear base color (.rgb) + IBL source (.a):
//              0 = none, 0.5 = sky, 1 = local reflection probe
//
// HAS_TERRAIN_PBR evaluates the painted layers here too, so what this writes matches
// what the color pass shades.

layout(location = 0) out vec4 frag_depth;
layout(location = 1) out vec4 frag_gbuffer;
layout(location = 2) out vec4 frag_albedo;

in vec2 v_projZW;
in vec3 v_normal;

// Stochastic Tiling (USE_STOCHASTIC_TILING): base colour and metallic-roughness read through
// the same lattice, taps and weights as Lit's colour pass (includes/pbr.glsl, UV set 1)
#if defined(USE_STOCHASTIC_TILING) && (defined(HAS_BASECOLOR_TEXTURE) || defined(HAS_METALLICROUGHNESS_TEXTURE))
    #define GBUFFER_STOCHASTIC
#endif

uniform u_fs_gbufferMaterial {
    vec4 params;        // x = roughness, y = metallic, z = IBL source, w = alpha cutoff (-1 = disabled)
    vec4 baseColorFactor;
    #ifdef GBUFFER_STOCHASTIC
        vec4 stochastic; // the material's cell size, blend sharpness, contrast, rotation
    #endif
} gbufferMaterial;

#if defined(HAS_BASECOLOR_TEXTURE) || defined(HAS_METALLICROUGHNESS_TEXTURE)
    in vec2 v_uv1;
#endif
#if defined(HAS_BASECOLOR_TEXTURE)
    uniform texture2D u_baseColorTexture;
    uniform sampler u_baseColor_smp;
#endif
#if defined(HAS_METALLICROUGHNESS_TEXTURE)
    uniform texture2D u_metallicRoughnessTexture;
    uniform sampler u_metallicRoughness_smp;
#endif

#include "includes/depth_util.glsl"
#include "includes/octahedral.glsl"
#include "includes/srgb.glsl"
#ifdef HAS_TERRAIN_PBR
    #include "includes/terrain_fs.glsl"
#endif
#ifdef GBUFFER_STOCHASTIC
    #include "includes/stochastic.glsl"
#endif

#include "includes/foliage_dither.glsl"

void main() {
    #ifdef USE_FOLIAGE_DITHER
        foliageDitherDiscard();
    #endif
    #ifdef GBUFFER_STOCHASTIC
        // the base colour's luminance weights, then the metallic-roughness taps with them (as
        // geometric without a base colour texture)
        StochasticTiling stochastic = stochasticSetup(v_uv1, stochasticParams(gbufferMaterial.stochastic.x,
            gbufferMaterial.stochastic.y, gbufferMaterial.stochastic.z, gbufferMaterial.stochastic.w), 0u);
    #endif

    vec4 baseColor = gbufferMaterial.baseColorFactor;
    #if defined(HAS_BASECOLOR_TEXTURE) && defined(GBUFFER_STOCHASTIC)
        baseColor *= sRGBToLinear(stochasticTextureLuminance(u_baseColorTexture, u_baseColor_smp, stochastic));
    #elif defined(HAS_BASECOLOR_TEXTURE)
        vec4 texColor = texture(sampler2D(u_baseColorTexture, u_baseColor_smp), v_uv1);
        baseColor *= sRGBToLinear(texColor);
    #endif
    if (gbufferMaterial.params.w >= 0.0 && baseColor.a < gbufferMaterial.params.w) {
        discard;
    }

    float roughness = gbufferMaterial.params.x;
    float metallic  = gbufferMaterial.params.y;
    #if defined(HAS_METALLICROUGHNESS_TEXTURE)
        // glTF convention: green = roughness, blue = metallic
        #ifdef GBUFFER_STOCHASTIC
            vec4 mr = stochasticTexture(u_metallicRoughnessTexture, u_metallicRoughness_smp, stochastic);
        #else
            vec4 mr = texture(sampler2D(u_metallicRoughnessTexture, u_metallicRoughness_smp), v_uv1);
        #endif
        roughness *= mr.g;
        metallic  *= mr.b;
    #endif

    // Higher precision equivalent of gl_FragCoord.z (see depth.frag).
    frag_depth = encodeDepth(0.5 * v_projZW[0] / v_projZW[1] + 0.5);

    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;

    #ifdef HAS_TERRAIN_PBR
        // the same evaluation the color pass runs, so SSR sees the surface it shades
        TerrainSurface terrainSurface = getTerrainSurface(baseColor, n, roughness, metallic);
        baseColor = terrainSurface.color;
        roughness = terrainSurface.roughness;
        metallic = terrainSurface.metallic;
        n = terrainSurface.normal;
    #endif

    frag_gbuffer = vec4(octEncode(n), clamp(roughness, 0.0, 1.0), clamp(metallic, 0.0, 1.0));

    frag_albedo = vec4(baseColor.rgb, gbufferMaterial.params.z);
}
