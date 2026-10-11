// Vertex path of the fixture's hull shaders (outline.vert, lit_outline.vert): the library's
// shared vertex path with the mesh pushed out along its smoothed normal (RAW_NORMALS: along
// its own normals, which split at hard edges and leave gaps), then shifted by offset.

#define SURFACE_DISPLACE
#include "includes/surface_vs.glsl"

uniform u_vs_customParams {
    vec4 offset; // xyz in object units
    float width; // object units
} hullParams;

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    #ifdef RAW_NORMALS
        vec3 direction = objectNormal;
    #else
        vec3 direction = surfaceSmoothNormal;
    #endif
    return objectPosition + direction * hullParams.width + hullParams.offset.xyz;
}
