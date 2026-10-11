#version 450

// The vertex colour stays out of v_color (the instance colour still applies). REPLACE
// does not read the base map, so it has no UVs (@require uv1 feature=!MATCAP_MODE_REPLACE).
#define SURFACE_NO_VERTEX_COLOR

#include "includes/surface_vs.glsl"
