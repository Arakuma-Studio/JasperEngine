#version 450

// Depth program of library:unlit: the UV and colour reach the fragment stage for the
// alpha clip only
#ifdef ALPHA_CLIP
    #define SURFACE_DEPTH_VARYINGS
#endif
#ifndef VERTEX_COLOR
    #define SURFACE_NO_VERTEX_COLOR
#endif

#include "includes/surface_depth_vs.glsl"
