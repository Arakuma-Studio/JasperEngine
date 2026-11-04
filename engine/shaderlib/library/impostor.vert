#version 450

// library:impostor: the shared vertex path, with the quad placed by library/impostor.glsl. The
// whole-tree wind applies; the impostor has no leaves to flutter (T5 R19).
#define SURFACE_NO_VERTEX_COLOR
#define SURFACE_DISPLACE
#define FOLIAGE_IMPOSTOR
#include "includes/surface_vs.glsl"
#include "library/impostor.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return impostorPlace(a_position.xy, a_texcoord1);
}
