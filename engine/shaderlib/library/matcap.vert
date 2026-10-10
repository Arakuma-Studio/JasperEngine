#version 450

// The vertex colour stays out of v_color (the instance colour still applies); REPLACE
// does not read the base map, so it has no UVs.
#define SURFACE_NO_VERTEX_COLOR
#ifdef MATCAP_MODE_REPLACE
    #undef SURFACE_UV1
    #undef HAS_UV_SET1
#endif

#include "includes/surface_vs.glsl"
