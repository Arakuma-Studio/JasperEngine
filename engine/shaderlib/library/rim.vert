#version 450

// library:rim: the shared vertex path; the UVs reach the fragment stage only for the
// pattern (TEXTURE)
#ifndef TEXTURE
    #undef SURFACE_UV1
    #undef HAS_UV_SET1
#endif

#include "includes/surface_vs.glsl"
