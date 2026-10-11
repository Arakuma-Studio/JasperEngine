#version 450

// library:foliage: the shared vertex path with the wind of library/foliage.glsl. The
// vertex colour never tints (v_color is the instance colour); with WIND_MASK_VERTEX_COLOR
// the wind hook reads it as the mask (SURFACE_DISPLACE_COLOR: the raw vertex colour, which
// an instance colour does not scale, so instanced plants sway evenly whatever their tint).
#define SURFACE_NO_VERTEX_COLOR
#ifdef WIND_MASK_VERTEX_COLOR
    #define SURFACE_DISPLACE_COLOR
#endif

#define SURFACE_DISPLACE
#include "includes/surface_vs.glsl"
#include "library/foliage.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return foliageSway(objectPosition, color, pbrParams.modelMatrix);
}
