#version 450

// fake_lit's inputs follow its features (@require ... feature=): the second UV set only for
// SHADOW_TEXTURE, and only on a mesh that has one (otherwise the fragment stage falls back
// to the first); the raw vertex colour (v_rawColor) only for VERTEX_AO. v_color is the
// instance colour alone: the vertex colour never tints. The vertex lights come without the
// SH ambient: the fake shading stands in for it.
#ifndef HAS_UV_SET2
    #undef SURFACE_UV2
#endif
#define SURFACE_NO_VERTEX_COLOR
#define SURFACE_NO_VERTEX_AMBIENT

#include "includes/surface_vs.glsl"
