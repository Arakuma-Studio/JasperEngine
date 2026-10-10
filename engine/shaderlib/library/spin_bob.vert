#version 450

// library:spin_bob: the shared vertex path with the spin and bob of library/spin_bob.glsl
// (positions and normals). The vertex colour reaches v_color only with VERTEX_COLOR (the
// instance colour always).
#ifndef VERTEX_COLOR
    #define SURFACE_NO_VERTEX_COLOR
#endif

#define SURFACE_DISPLACE
#define SURFACE_DISPLACE_NORMAL
#include "includes/surface_vs.glsl"
#include "library/spin_bob.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return spinBobPosition(objectPosition, pbrParams.modelMatrix);
}

vec3 surfaceDisplaceNormal(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    return spinBobNormal(objectNormal, pbrParams.modelMatrix);
}
