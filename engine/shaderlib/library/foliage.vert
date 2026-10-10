#version 450

// library:foliage: the shared vertex path with the wind of library/foliage.glsl. The
// vertex colour is a wind mask with WIND_MASK_VERTEX_COLOR (and comes along in v_color for
// its alpha); with WIND_MASK_HEIGHT v_color is the instance colour.
#ifdef WIND_MASK_HEIGHT
    #define SURFACE_NO_VERTEX_COLOR
#endif

#define SURFACE_DISPLACE
#include "includes/surface_vs.glsl"
#include "library/foliage.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return foliageSway(objectPosition, color, pbrParams.modelMatrix);
}
