#version 450

// The vertex colour reaches v_color only with VERTEX_COLOR (the instance colour always).
// The lighting varyings (v_position, v_normal, v_vertexLight, v_lightProjPos) follow the
// variant's lighting mode, see includes/surface_vs.glsl.
#ifndef VERTEX_COLOR
    #define SURFACE_NO_VERTEX_COLOR
#endif

#include "includes/surface_vs.glsl"
