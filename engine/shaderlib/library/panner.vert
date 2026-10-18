#version 450

// library:panner: the shared vertex path. The vertex colour reaches v_color only with
// VERTEX_COLOR (the instance colour always).
#ifndef VERTEX_COLOR
    #define SURFACE_NO_VERTEX_COLOR
#endif

#include "includes/surface_vs.glsl"
