#version 450

// Depth program of library:unlit: drops what the colour pass clips (ALPHA_CLIP)

#ifdef ALPHA_CLIP
    #define SURFACE_DEPTH_DISCARD

    uniform u_fs_customParams {
        vec4 baseColor;
        vec4 tiling;
        float alphaCutoff;
        #ifdef STOCHASTIC
            float stochasticCellSize;
            float stochasticBlendSharpness;
            float stochasticContrast;
            float stochasticRotation;
        #endif
    } customParams;

    uniform texture2D u_baseTexture;
    uniform sampler u_base_smp;
#endif

#include "includes/surface_depth_fs.glsl"

#if defined(ALPHA_CLIP) && defined(STOCHASTIC)
    #include "library/stochastic_surface.glsl"
#endif

#ifdef ALPHA_CLIP
bool surfaceDepthDiscard(vec2 uv, vec4 color){
    vec2 tiled = uv * customParams.tiling.xy + customParams.tiling.zw;
    #ifdef STOCHASTIC
        // as the colour pass: the luminance weights of the same taps, so the same alpha
        StochasticTiling stochastic = stochasticSetup(tiled, stochasticCustomParams(), 0u);
        float alpha = customParams.baseColor.a * color.a * stochasticTextureLuminance(u_baseTexture, u_base_smp, stochastic).a;
    #else
        float alpha = customParams.baseColor.a * color.a * texture(sampler2D(u_baseTexture, u_base_smp), tiled).a;
    #endif
    return alpha < customParams.alphaCutoff;
}
#endif
