#version 450

// Depth program of library:flipbook: for the alpha clip (ALPHA_CLIP, flipbook_depth.frag)
// the same frame UVs as the colour pass (library/flipbook.glsl) and the colour; without
// it, the plain depth program. The frame UVs take the place of v_uv1, which nothing reads.
#undef SURFACE_UV1
#ifdef ALPHA_CLIP
    #define SURFACE_DEPTH_VARYINGS
    #define SURFACE_DISPLACE
    #ifndef VERTEX_COLOR
        #define SURFACE_NO_VERTEX_COLOR
    #endif
#endif

#include "includes/surface_depth_vs.glsl"

#ifdef ALPHA_CLIP
#include "library/flipbook.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    flipbookVaryings(uv);
    return objectPosition;
}
#endif
