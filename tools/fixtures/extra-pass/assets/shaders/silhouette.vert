#version 450

// The library's shared vertex path, shifted by offset (a drop shadow)
#define SURFACE_DISPLACE
#include "includes/surface_vs.glsl"

uniform u_vs_customParams {
    vec3 offset; // object units
} silhouetteParams;

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return objectPosition + silhouetteParams.offset;
}
