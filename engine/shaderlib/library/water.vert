#version 450

// library:water: the shared vertex path, moved by the waves with WAVES (library/water_vs.glsl);
// water.frag bends the normal to match (with GERSTNER at the rest position passed in v_waveRest).
// the waves on the scene clock (library/water_vs.glsl)
#define WATER_SCENE_CLOCK

#ifdef WAVES
    #define SURFACE_DISPLACE
    #ifdef GERSTNER
        // the rest XZ goes to the fragment stage, which lights the Gerstner surface there
        #define SURFACE_VERTEX
    #endif
#endif

#include "includes/surface_vs.glsl"

#ifdef WAVES
#include "library/water_vs.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return waterWavePosition(objectPosition, pbrParams.modelMatrix);
}

#ifdef GERSTNER
out vec2 v_waveRest;

void surfaceVertex(vec3 worldPosition, vec3 worldNormal, vec4 color, vec2 uv){
    v_waveRest = waterRestXZ;
}
#endif
#endif
