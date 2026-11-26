#version 450

// The vertex colour weighs the layers (v_rawColor): it stays out of v_color, which is the
// instance colour alone (library/vertex_blend.frag).
#define SURFACE_NO_VERTEX_COLOR

#include "includes/surface_vs.glsl"
