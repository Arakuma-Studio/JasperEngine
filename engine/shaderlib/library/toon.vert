#version 450

// The vertex colour stays out of v_color (the instance colour still applies). In vertex
// mode the SH ambient comes apart from the vertex lights (v_vertexAmbient), so the lights
// alone are banded. See includes/surface_vs.glsl.
#define SURFACE_NO_VERTEX_COLOR
#define SURFACE_VERTEX_AMBIENT
// the world position and normal reach the fragment stage only for pixel lights and the rim
#if !defined(USE_PUNCTUAL) && !defined(RIM)
    #undef SURFACE_POSITION
    #undef SURFACE_NORMALS
#endif

#include "includes/surface_vs.glsl"
