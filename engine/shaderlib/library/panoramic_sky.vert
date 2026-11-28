#version 450

// Per-Vertex Mapping: the longitude and latitude come from the engine's sphere grid
// (includes/sky_vs.glsl SKY_SPHERE_UV)
#ifdef VERTEX_MAPPING
    #define SKY_SPHERE_UV
#endif

#include "includes/sky_vs.glsl"
