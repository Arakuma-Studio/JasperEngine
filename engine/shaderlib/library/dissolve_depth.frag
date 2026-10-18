#version 450

// Depth program of library:dissolve: drops what the colour pass dissolves
// (library/dissolve.glsl), so the shadow has the same holes

#define SURFACE_DEPTH_DISCARD
#undef SURFACE_COLOR
#ifdef WORLD_NOISE
    #undef SURFACE_UV1
#endif

#include "library/dissolve.glsl"
#include "includes/surface_depth_fs.glsl"

bool surfaceDepthDiscard(vec2 uv, vec4 color){
    return dissolveNoise(uv) < dissolveClipLevel();
}
