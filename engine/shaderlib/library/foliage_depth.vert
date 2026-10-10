#version 450

// Depth program of library:foliage: the same wind (library/foliage.glsl), and for the
// cutout (ALPHA_CLIP, foliage_depth.frag) the UV and colour. Inputs nothing reads stay
// undeclared.
#ifdef ALPHA_CLIP
    #define SURFACE_DEPTH_VARYINGS
#else
    #undef HAS_UV_SET1
    #ifdef WIND_MASK_HEIGHT
        #undef SURFACE_COLOR
    #endif
#endif
#ifdef WIND_MASK_HEIGHT
    #define SURFACE_NO_VERTEX_COLOR
#endif
#undef HAS_NORMALS

#define SURFACE_DISPLACE
#include "includes/surface_depth_vs.glsl"
#include "library/foliage.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return foliageSway(objectPosition, color, depthParams.modelMatrix);
}
