#version 450

// Depth program of library:spin_bob: the same motion as the colour pass (library/
// spin_bob.glsl). The hook reads only the position: the other inputs stay undeclared.
#undef HAS_NORMALS
#undef HAS_UV_SET1
#undef HAS_VERTEX_COLOR_VEC3
#undef HAS_VERTEX_COLOR_VEC4
#undef SURFACE_COLOR

#define SURFACE_DISPLACE
#include "includes/surface_depth_vs.glsl"
#include "library/spin_bob.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return spinBobPosition(objectPosition, depthParams.modelMatrix);
}
