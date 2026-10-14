#version 450

// Depth program of library:foliage: the same wind (library/foliage.glsl), and for the
// cutout (ALPHA_CLIP, foliage_depth.frag) the UV and the instance colour, as the colour
// pass (foliage.vert: the vertex colour is only the wind mask). Inputs nothing reads stay
// undeclared.
#ifdef ALPHA_CLIP
    #define SURFACE_DEPTH_VARYINGS
#else
    // no cutout: nothing reads the UV or v_color (nor the instance colour)
    #undef HAS_UV_SET1
    #undef SURFACE_COLOR
#endif
#define SURFACE_NO_VERTEX_COLOR
#ifdef WIND_MASK_VERTEX_COLOR
    #define SURFACE_DISPLACE_COLOR
#endif

#define SURFACE_DISPLACE
#include "includes/surface_depth_vs.glsl"
#include "library/foliage.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return foliageSway(objectPosition, color, depthParams.modelMatrix);
}
