#version 450

// The UVs reach the fragment stage only with TEXTURE. The lighting varyings follow the
// variant's lighting mode, see includes/surface_vs.glsl.
#ifndef TEXTURE
    #undef SURFACE_UV1
    #undef HAS_UV_SET1
#endif

#include "includes/surface_vs.glsl"
