#version 450

// fake_lit's inputs follow its features: the second UV set only for SHADOW_TEXTURE on a
// mesh that has one (otherwise the fragment stage falls back to the first), the vertex
// colour only for VERTEX_AO (the instance colour always). The vertex lights come without
// the SH ambient: the fake shading stands in for it.
#ifndef SHADOW_TEXTURE
    #undef HAS_UV_SET2
#endif
#if !defined(SHADOW_TEXTURE) || !defined(HAS_UV_SET2)
    #undef SURFACE_UV2
#endif
#ifndef VERTEX_AO
    #define SURFACE_NO_VERTEX_COLOR
#endif
#define SURFACE_NO_VERTEX_AMBIENT

#include "includes/surface_vs.glsl"
