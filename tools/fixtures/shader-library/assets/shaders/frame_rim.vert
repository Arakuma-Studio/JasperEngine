#version 450

// A project fork on the shared library vertex path: the frame block in the vertex stage
// (time) moves the vertices along their normal through the SURFACE_DISPLACE hook

#define FRAME_VS
#include "includes/frame.glsl"

uniform u_vs_customParams {
    float wobble;
} customParams;

#define SURFACE_DISPLACE
#include "includes/surface_vs.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return objectPosition + objectNormal * customParams.wobble * sin(frame.time.x * 3.0 + objectPosition.y * 6.0);
}
