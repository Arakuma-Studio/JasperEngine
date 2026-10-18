#version 450

// library:dissolve: the shared vertex path, plus the world noise varyings (WORLD_NOISE,
// library/dissolve_vs.glsl). The vertex colour reaches v_color only with VERTEX_COLOR (the
// instance colour always).
#ifndef VERTEX_COLOR
    #define SURFACE_NO_VERTEX_COLOR
#endif

// the world noise reads the normal attribute (@require normals feature=WORLD_NOISE) in its
// own varyings; v_normal is for the pixel lights only
#ifndef USE_PUNCTUAL
    #undef SURFACE_NORMALS
#endif

#ifdef WORLD_NOISE
    #define SURFACE_DISPLACE
#endif

#include "includes/surface_vs.glsl"
#include "library/dissolve_vs.glsl"

#ifdef WORLD_NOISE
vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    dissolveVaryings(objectPosition, objectNormal, pbrParams.modelMatrix);
    return objectPosition;
}
#endif
