#version 450

// Depth program of library:dissolve: the UV (or, with WORLD_NOISE, the world position and
// normal) the noise is sampled from, as in the colour pass; nothing moves.
#undef SURFACE_COLOR
// (a lattice's vertex colour mask still reads the colour, includes/lattice.glsl)
#ifndef HAS_LATTICE
    #undef HAS_VERTEX_COLOR_VEC3
    #undef HAS_VERTEX_COLOR_VEC4
#endif

#ifdef WORLD_NOISE
    #define SURFACE_DISPLACE
    #undef HAS_UV_SET1
#else
    #define SURFACE_DEPTH_VARYINGS
#endif

#include "includes/surface_depth_vs.glsl"
#include "library/dissolve_vs.glsl"

#ifdef WORLD_NOISE
vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    dissolveVaryings(objectPosition, objectNormal, depthParams.modelMatrix);
    return objectPosition;
}
#endif
