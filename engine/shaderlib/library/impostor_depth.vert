#version 450

// Depth program of library:impostor: the same quad, and for the cutout the UV and instance colour
#define SURFACE_DEPTH_VARYINGS
#define SURFACE_NO_VERTEX_COLOR
#define SURFACE_DISPLACE
#define FOLIAGE_IMPOSTOR
#include "includes/surface_depth_vs.glsl"
#include "library/impostor.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return impostorPlace(a_position.xy, a_texcoord1);
}
