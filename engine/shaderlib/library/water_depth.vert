#version 450

// Depth program of library:water: raised by the same waves as the colour pass (WAVES,
// library/water_vs.glsl). The hook reads only the position: the other inputs stay
// undeclared.
#undef HAS_NORMALS
#undef HAS_UV_SET1
// (a lattice's vertex colour mask still reads the colour, includes/lattice.glsl)
#ifndef HAS_LATTICE
    #undef HAS_VERTEX_COLOR_VEC3
    #undef HAS_VERTEX_COLOR_VEC4
#endif
#undef SURFACE_COLOR

// the waves on the scene clock (library/water_vs.glsl)
#define WATER_SCENE_CLOCK

#ifdef WAVES
    #define SURFACE_DISPLACE
#endif

#include "includes/surface_depth_vs.glsl"

#ifdef WAVES
#include "library/water_vs.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return waterWavePosition(objectPosition, depthParams.modelMatrix);
}
#endif
