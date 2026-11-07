#version 450

// G-buffer geometry pass (fragment), MRT output:
//   color[0] = packed view depth (identical to depth.frag, so SSAO/SSR depth is unchanged)
//   color[1] = view-space normal (octahedral .rg) + roughness (.b) + metallic (.a)
//   color[2] = linear base color (.rgb) + the reflection source code / 255 (.a): what the
//              colour pass adds as environment specular, which the SSR composite swaps
//              (includes/ssr_source.glsl)
//   color[3] = the occlusion the colour pass weighs that specular by (.r, R8): the
//              material's occlusion map times a painted terrain's occlusion
//
// HAS_TERRAIN_PBR evaluates the painted layers here too, so what this writes matches
// what the color pass shades. The maps are read on the UV set the material names, through
// Stochastic Tiling's lattice of that set when the material takes it (includes/pbr.glsl).

layout(location = 0) out vec4 frag_depth;
layout(location = 1) out vec4 frag_gbuffer;
layout(location = 2) out vec4 frag_albedo;
layout(location = 3) out vec4 frag_occlusion;

in vec2 v_projZW;
in vec3 v_normal;

#if defined(HAS_BASECOLOR_TEXTURE) || defined(HAS_METALLICROUGHNESS_TEXTURE) || defined(HAS_OCCLUSION_TEXTURE)
    #define GBUFFER_UV
#endif

// Stochastic Tiling (USE_STOCHASTIC_TILING): the maps read through the same lattices, taps and
// weights as Lit's colour pass (includes/pbr.glsl)
#if defined(USE_STOCHASTIC_TILING) && defined(GBUFFER_UV)
    #define GBUFFER_STOCHASTIC
#endif

uniform u_fs_gbufferMaterial {
    vec4 params;        // x = roughness, y = metallic, z = reflection source code / 255, w = alpha cutoff (-1 = disabled)
    vec4 baseColorFactor;
    #ifdef GBUFFER_STOCHASTIC
        vec4 stochastic; // the material's cell size, blend sharpness, contrast, rotation
    #elif defined(HAS_UV_SET2)
        vec4 stochasticPadding; // keeps texCoordSets at the CPU block's offset
    #endif
    #ifdef HAS_UV_SET2
        vec4 texCoordSets; // the UV set (0 = UV1, 1 = UV2) of x base colour, y metallic-roughness, z occlusion
    #endif
} gbufferMaterial;

#ifdef GBUFFER_UV
    in vec2 v_uv1;
    #ifdef HAS_UV_SET2
        in vec2 v_uv2;
    #endif
#endif
#if defined(HAS_BASECOLOR_TEXTURE)
    uniform texture2D u_baseColorTexture;
    uniform sampler u_baseColor_smp;
#endif
#if defined(HAS_METALLICROUGHNESS_TEXTURE)
    uniform texture2D u_metallicRoughnessTexture;
    uniform sampler u_metallicRoughness_smp;
#endif
#if defined(HAS_OCCLUSION_TEXTURE)
    uniform texture2D u_occlusionTexture;
    uniform sampler u_occlusion_smp;
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
#ifdef TERRAIN_HOLES
    #include "includes/terrain_holes_fs.glsl"
#endif

#ifdef GBUFFER_UV
    // the UV set a map is read on: 0 = UV1, 1 = UV2 (as includes/pbr.glsl selectUV)
    bool onUV2(float set){
        #ifdef HAS_UV_SET2
            return set > 0.5;
        #else
            return false;
        #endif
    }
    vec2 selectUV(float set){
        #ifdef HAS_UV_SET2
            if (onUV2(set))
                return v_uv2;
        #endif
        return v_uv1;
    }
    #ifdef HAS_UV_SET2
        #define GBUFFER_SET_BASECOLOR gbufferMaterial.texCoordSets.x
        #define GBUFFER_SET_METALLICROUGHNESS gbufferMaterial.texCoordSets.y
        #define GBUFFER_SET_OCCLUSION gbufferMaterial.texCoordSets.z
    #else
        #define GBUFFER_SET_BASECOLOR 0.0
        #define GBUFFER_SET_METALLICROUGHNESS 0.0
        #define GBUFFER_SET_OCCLUSION 0.0
    #endif
#endif

#ifdef GBUFFER_STOCHASTIC
    // one lattice per UV set, seeded as Lit's (0 for UV1, 1 for UV2); the base colour's taps
    // give their set's weights to every other map of that set
    StochasticTiling gbufferStochastic1;
    #ifdef HAS_UV_SET2
        StochasticTiling gbufferStochastic2;
    #endif
#endif

void main() {
    #ifdef TERRAIN_HOLES
        terrainDiscardHole();
    #endif
    #ifdef GBUFFER_STOCHASTIC
        {
            vec4 params = stochasticParams(gbufferMaterial.stochastic.x, gbufferMaterial.stochastic.y,
                gbufferMaterial.stochastic.z, gbufferMaterial.stochastic.w);
            // in uniform control flow (the gradients), both sets when there are two, as Lit
            gbufferStochastic1 = stochasticSetup(v_uv1, dFdx(v_uv1), dFdy(v_uv1), params, 0u);
            #ifdef HAS_UV_SET2
                gbufferStochastic2 = stochasticSetup(v_uv2, dFdx(v_uv2), dFdy(v_uv2), params, 1u);
            #endif
        }
    #endif

    vec4 baseColor = gbufferMaterial.baseColorFactor;
    #if defined(HAS_BASECOLOR_TEXTURE) && defined(GBUFFER_STOCHASTIC)
        #ifdef HAS_UV_SET2
            if (onUV2(GBUFFER_SET_BASECOLOR))
                baseColor *= sRGBToLinear(stochasticTextureLuminance(u_baseColorTexture, u_baseColor_smp, gbufferStochastic2));
            else
        #endif
        baseColor *= sRGBToLinear(stochasticTextureLuminance(u_baseColorTexture, u_baseColor_smp, gbufferStochastic1));
    #elif defined(HAS_BASECOLOR_TEXTURE)
        vec4 texColor = texture(sampler2D(u_baseColorTexture, u_baseColor_smp), selectUV(GBUFFER_SET_BASECOLOR));
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
            #ifdef HAS_UV_SET2
                vec4 mr = onUV2(GBUFFER_SET_METALLICROUGHNESS)
                    ? stochasticTexture(u_metallicRoughnessTexture, u_metallicRoughness_smp, gbufferStochastic2)
                    : stochasticTexture(u_metallicRoughnessTexture, u_metallicRoughness_smp, gbufferStochastic1);
            #else
                vec4 mr = stochasticTexture(u_metallicRoughnessTexture, u_metallicRoughness_smp, gbufferStochastic1);
            #endif
        #else
            vec4 mr = texture(sampler2D(u_metallicRoughnessTexture, u_metallicRoughness_smp), selectUV(GBUFFER_SET_METALLICROUGHNESS));
        #endif
        roughness *= mr.g;
        metallic  *= mr.b;
    #endif

    // the occlusion map's red channel, as Lit's getOcclusionTexture() (its strength is 1)
    float occlusion = 1.0;
    #if defined(HAS_OCCLUSION_TEXTURE)
        #ifdef GBUFFER_STOCHASTIC
            #ifdef HAS_UV_SET2
                occlusion = onUV2(GBUFFER_SET_OCCLUSION)
                    ? stochasticTexture(u_occlusionTexture, u_occlusion_smp, gbufferStochastic2).r
                    : stochasticTexture(u_occlusionTexture, u_occlusion_smp, gbufferStochastic1).r;
            #else
                occlusion = stochasticTexture(u_occlusionTexture, u_occlusion_smp, gbufferStochastic1).r;
            #endif
        #else
            occlusion = texture(sampler2D(u_occlusionTexture, u_occlusion_smp), selectUV(GBUFFER_SET_OCCLUSION)).r;
        #endif
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
        occlusion *= terrainSurface.occlusion;
    #endif

    frag_gbuffer = vec4(octEncode(n), clamp(roughness, 0.0, 1.0), clamp(metallic, 0.0, 1.0));

    frag_albedo = vec4(baseColor.rgb, gbufferMaterial.params.z);

    frag_occlusion = vec4(clamp(occlusion, 0.0, 1.0), 0.0, 0.0, 1.0);
}
