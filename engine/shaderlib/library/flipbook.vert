#version 450

// library:flipbook: the shared vertex path, with the frame's UVs (library/flipbook.glsl)
// in place of v_uv1. The vertex colour reaches v_color only with VERTEX_COLOR (the
// instance colour always).
#ifndef VERTEX_COLOR
    #define SURFACE_NO_VERTEX_COLOR
#endif
#undef SURFACE_UV1

#define SURFACE_DISPLACE
#include "includes/surface_vs.glsl"
#include "library/flipbook.glsl"

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    flipbookVaryings(uv);
    return objectPosition;
}
