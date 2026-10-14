#version 450

// Depth program of library:foliage: drops what the colour pass cuts out (ALPHA_CLIP)

#ifdef ALPHA_CLIP
    #define SURFACE_DEPTH_DISCARD

    uniform u_fs_customParams {
        vec4 baseColor;
        vec4 tiling;
        float alphaCutoff;
    } customParams;

    uniform texture2D u_baseTexture;
    uniform sampler u_base_smp;
#endif

#include "includes/surface_depth_fs.glsl"

#ifdef ALPHA_CLIP
bool surfaceDepthDiscard(vec2 uv, vec4 color){
    vec2 tiled = uv * customParams.tiling.xy + customParams.tiling.zw;
    float alpha = customParams.baseColor.a * color.a * texture(sampler2D(u_baseTexture, u_base_smp), tiled).a;
    return alpha < customParams.alphaCutoff;
}
#endif
