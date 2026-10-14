#version 450

// Depth program of library:flipbook: drops what the colour pass clips (ALPHA_CLIP), from
// the same frame (and crossfade) of the sheet

#undef SURFACE_UV1
#ifdef ALPHA_CLIP
    #define SURFACE_DEPTH_DISCARD

    uniform u_fs_customParams {
        vec4 tint;
        float alphaCutoff;
    } customParams;

    uniform texture2D u_sheetTexture;
    uniform sampler u_sheet_smp;

    in vec4 v_frameUV;
    #ifdef FRAME_BLEND
        in float v_frameBlend;
    #endif
#endif

#include "includes/surface_depth_fs.glsl"

#ifdef ALPHA_CLIP
bool surfaceDepthDiscard(vec2 uv, vec4 color){
    float alpha = texture(sampler2D(u_sheetTexture, u_sheet_smp), v_frameUV.xy).a;
    #ifdef FRAME_BLEND
        alpha = mix(alpha, texture(sampler2D(u_sheetTexture, u_sheet_smp), v_frameUV.zw).a, v_frameBlend);
    #endif
    return customParams.tint.a * color.a * alpha < customParams.alphaCutoff;
}
#endif
