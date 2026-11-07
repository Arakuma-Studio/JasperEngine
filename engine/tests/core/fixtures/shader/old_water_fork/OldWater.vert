#version 450

// library:water: the shared vertex path, raised by the waves with WAVES
// (library/water_vs.glsl); water.frag bends the normal to match.
#ifdef WAVES
    #define SURFACE_DISPLACE
#endif

#include "includes/surface_vs.glsl"

#ifdef WAVES
#include "library/water_vs.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return waterWavePosition(objectPosition, pbrParams.modelMatrix);
}
#endif
